//See readme file for notes.
#define __USE_POSIX //for strtok_r
#include <rapophenia.h>
#include <stdbool.h>
#include <stdlib.h>
#include "internal.h"
#include "em_weight.h"
extern char *datatab;
void qxprintf(char **q, char *format, ...); //bridge.c
char *process_string(char *inquery, char **typestring); //parse_sql.c
void xprintf(char **q, char *format, ...); //parse_sql.c
char *strip (const char*); //peptalk.y
#define XN(in) ((in) ? (in) : "")

apop_model *relmodel; //impute/rel.c

data_frame_from_apop_data_type *rapop_df_from_ad;//alloced in PEPedits.c:R_init_tea


typedef struct {
	apop_model *base_model, *fitted_model;
	char * depvar, **allvars, *vartypes, *selectclause;
	int position, allvars_ct, error;
	apop_data *isnan, *notnan;
    bool is_bounds_checkable, is_hotdeck, textdep, is_em, is_regression, allow_near_misses;
} impustruct;


////////////////////////

/* This function is a hack under time pressure by BK, intended 
    to allow raking to use the fingerprinting output. I just blank
    out the results from the fingerprinting and do the imputation. 
    In an effort to be conservative, I blank out only the most 
    implicated field in each record.
    So you'll want to run a for loop that re-checks the vflags.
    I'll have to see later if that creates problems...

    Input is **datatab for the convenience of R.

*/
int blank_fingerprints(char const **datatab){
    if (!apop_table_exists("vflags")) return 0;
    char *id = get_key_word(NULL, "id");
    
    sprintf(apop_opts.db_name_column, "%s", id);
    apop_data *prints = apop_query_to_data("select * from vflags");
    Apop_stopif(!prints || prints->error, return -1, 0, 
            "vflags exists, but select * from vflags failed.");
    for (int i=0; i< prints->matrix->size1; i++){
        //mm is a matrix with one row; v is the vector view.
        Apop_submatrix(prints->matrix, i, 0, 1, prints->matrix->size2-2, mm);
        Apop_matrix_row(mm, 0, v);

        apop_query("update %s set %s = null where %s+0.0 = %s",
                *datatab, 
                prints->names->col[gsl_vector_max_index(v)],
                id, prints->names->row[i]
                );
    }
    return 0;
}

////////////////////////


void index_cats(char const *tab, apop_data const *category_matrix){
    if (!category_matrix||!*category_matrix->textsize) return;
    char *cats = apop_text_paste(category_matrix, .between=", ");
    char *c2 = apop_text_paste(category_matrix, .between="_");
    apop_query("create index idx_%s_%s on %s(%s)", tab, c2, tab, cats);
}



///// second method: via raking

/* Zero out the weights of those rows that don't match. 
 * Thanks to copy_by_name, we know that the two input data sets match.
 */
double cull(apop_data *onerow, void *subject_row_in){
    apop_data *subject_row = subject_row_in;
    for (int i=0; i< subject_row->matrix->size2; i++){
        double this = apop_data_get(subject_row, .col=i);
        if (isnan(this)) continue;
        /* If match or no information, then continue
         * If not a match, then reweight by 1/distance? */
        double dist = fabs(apop_data_get(onerow, .col=i) - this);
        if (dist< 1e-5) continue;
        else {
            if (!subject_row->more) *onerow->weights->data = 0;
            else {
                char type = subject_row->more->text[i][0][0];
                *onerow->weights->data *= (type=='r') ? 1/(1+dist): 0;
            }
        }
    }
    return 0;
}

/* OK, exact matching didn't work? Use this function to find
 * the difference count between the reference record and each row in the raked table. */
double count_diffs(apop_data *onerow, void *subject_row_in){
    apop_data *subject_row = subject_row_in;
    int diffs=0;
    for (int i=0; i< subject_row->matrix->size2; i++){
        double this = apop_data_get(subject_row, .col=i);
        if (isnan(this)) continue;
        if (apop_data_get(onerow, .col=i) != this) {
            diffs++;
        }
    }
    return diffs;
}


/* We're depending on the data columns and the draw columns to
   be the same fields, and we aren't guaranteed that the raking
   gave us data in the right format(BK: check this?). Copy the 
   raked output to explicitly fit the data format. */
apop_data *copy_by_name(apop_data *data, apop_data const *form){
    apop_data *out= apop_data_alloc(data->matrix->size1, form->matrix->size2);
    apop_name_stack(out->names, data->names, 'r');
    apop_name_stack(out->names, form->names, 'c');
    for (int i=0; i< out->matrix->size2; i++){
        int corresponding_col = apop_name_find(out->names, data->names->col[i], 'c');
        Apop_col_v(data, i, source);
        if (corresponding_col!=-2) {
            Apop_matrix_col(out->matrix, corresponding_col, dest);
            gsl_vector_memcpy(dest, source);
        }
    }
    return out;
}

double en(apop_data *in){ 
    double w = gsl_vector_get(in->weights, 0);
    return w*log2(w);
}

/* We have raked output, and will soon be making draws from it. 
   So it needs to be a PMF of the right format to match the data point
   we're trying to match.  
   
   It may be that the row in the data has no match in the raked output, due to structural
   zeros and the inputs. E.g., if a person is married and has missing age, and everybody
   else in the data set is under 15, and (age <15 && married) is an edit failure, there will
   be no entries in the rake table with married status. In this case, blank out the existing married status.
   
   */
apop_model *prep_the_draws(apop_data *raked, apop_data *fin, gsl_vector const *orig,  int cutctr){
    Apop_stopif(!raked, return NULL, 0, "NULL raking results.");
    double s = apop_sum(raked->weights);
    Apop_stopif(isnan(s), return NULL, 0, "NaNs in raking results.");
    Apop_stopif(!s, return NULL, 0, "No weights in raking results.");
    bool done = false;
    apop_map(raked, .fn_rp=cull, .param=fin, .inplace='v');
    done = apop_sum(raked->weights);
    Apop_stopif(!done, return NULL, 0, "Still couldn't find something to draw.");
    return apop_estimate(raked, apop_pmf);
}

//An edit that uses variables not in our current raking table will cause SQL failures.
bool vars_in_edit_are_in_rake_table(used_var_t* vars_used, size_t ct, apop_data rake_vars){
    for (int i=0; i< ct; i++){
        bool pass=false;
        for (int j=0; j< rake_vars.textsize[1] && !pass; j++)
            if (!strcmp(vars_used[i].name, rake_vars.text[0][j])) pass=true;
        if (!pass) return false;
    }
    return true;
}

apop_data *get_data_for_em(const char *datatab, char *catlist, const char *id_col, 
                           char const *weight_col, char *previous_filltab, impustruct is){
    apop_data as_data = (apop_data){.textsize={1,is.allvars_ct}, .text=&is.allvars};
    char *varlist = apop_text_paste(&as_data, .between=", ");
    apop_data *d = apop_query_to_data("select %s, %s %c %s from %s %s %s", id_col, varlist, 
                    weight_col ? ',' : ' ',
                    XN(weight_col), /*previous_filltab ? dt :*/datatab, catlist ? "where": " ", XN(catlist));
    free(varlist);
    Apop_stopif(!d || d->error, return d, 0, "Query trouble.");
    if (!d->weights) {
        if (weight_col){
            Apop_col_tv(d, weight_col, wc);
            d->weights = apop_vector_copy(wc);
            gsl_vector_set_all(wc, 0); //debris.
        } else {
            d->weights = gsl_vector_alloc(d->matrix->size1);
            gsl_vector_set_all(d->weights, 1);
        }
    }
    return d;
}

void writeout(apop_data *fillins, gsl_vector *cp_to_fill, apop_data const *focus, int *ctr, int drawno){
    for (size_t j=0; j< focus->matrix->size2; j++)  
        if (isnan(apop_data_get(focus, .col=j))){
            apop_text_add(fillins, *ctr, 0, *focus->names->row);
            apop_text_add(fillins, *ctr, 1, focus->names->col[j]);
            gsl_matrix_set(fillins->matrix, *ctr, 0, drawno);
            gsl_matrix_set(fillins->matrix, (*ctr)++, 1, cp_to_fill->data[j]);
        }
}

double nnn (double in){return isnan(in);}

void get_types(apop_data *raked){
    apop_data *names = apop_data_add_page(raked, 
                       apop_text_alloc(NULL, raked->names->colct, 1), "types");
    for (int i=0; i< raked->names->colct; i++)
        apop_text_add(names, i, 0, "%c", get_coltype(raked->names->col[i]));
}

static void rake_to_completion(char const *datatab, char const *underlying,
        impustruct is, int min_group_size, gsl_rng *r,
        int draw_count, char *catlist,
        apop_data const *fingerprint_vars, char const *id_col,
        char const *weight_col, char const *fill_tab, char const *margintab,
        char *previous_filltab){

    //char *dt="tea_co", *dxx=(char*)datatab;
    //int zero=0;
    //if (previous_filltab) check_out_impute(&dxx, /*&dt*/ NULL, &zero, NULL, &previous_filltab);

    apop_data *d = get_data_for_em(datatab, catlist, id_col, weight_col, previous_filltab, is);
    Apop_stopif(!d, return, 0, "Query for appropriate data returned no elements. Nothing to do.");
    Apop_stopif(d->error, return, 0, "query error.");
    int count_of_nans = apop_map_sum(d, .fn_d=nnn);
    if (!count_of_nans) return;
    
    apop_data *raked = em_weight(d, .tolerance=1e-3);
    Apop_stopif(!raked, return, 0, "Raking returned a blank table. This shouldn't happen.");
    Apop_stopif(raked->error, return, 0, "Error (%c) in raking.", raked->error);


    /*
apop_data *dcp = apop_data_sort(apop_data_pmf_compress(apop_data_copy(d)));
asprintf(&dcp->names->title, "<<<<<<<<<<<<<<<<<<<<<<<<<<<");
asprintf(&raked->names->title, ">>>>>>>>>>>>>>>>>>>>>>>>>>>");
apop_data_pmf_compress(raked);
apop_data_sort(raked);
apop_data_print(dcp, .output_name="ooo", .output_append='a');
apop_data_print(raked, .output_name="ooo", .output_append='a');
apop_data_free(dcp);
*/

    gsl_vector *original_weights = apop_vector_copy(raked->weights);
    gsl_vector *cp_to_fill = gsl_vector_alloc(d->matrix->size2);
    Apop_row(raked, 0, firstrow);
    apop_data *name_sorted_cp = copy_by_name(d, firstrow);
    if (is.allow_near_misses) get_types(name_sorted_cp); //add a list of types as raked->more.
    apop_data *fillins = apop_text_alloc(apop_data_alloc(count_of_nans*draw_count, 2), count_of_nans*draw_count, 2);
    int ctr = 0;
    for (size_t i=0; i< d->matrix->size1; i++){
        Apop_matrix_row(d->matrix, i, focusv);    //as vector
        if (!isnan(apop_sum(focusv))) continue;
        Apop_row(d, i, focus);               //as data set w/names
        Apop_row(name_sorted_cp, i, focusn); //as data set w/names, arranged to match rake
        focusn->more = name_sorted_cp->more;

        //draw the entire row at once, but write only the NaN elmts to the filled tab.
        apop_model *m = prep_the_draws(raked, focusn, original_weights, 0); 
        if (!m || m->error) goto end; //get it on the next go `round.
        for (int drawno=0; drawno< draw_count; drawno++){
            Apop_stopif(!focus->names, goto end, 0, "focus->names is NULL. This should never have happened.");
            apop_draw(cp_to_fill->data, r, m);
            writeout(fillins, cp_to_fill, focus, &ctr, drawno);
        }
        end:
        apop_model_free(m);
        gsl_vector_memcpy(raked->weights, original_weights);
    }
    apop_matrix_realloc(fillins->matrix, ctr, fillins->matrix->size2);
    apop_text_alloc(fillins, ctr, 2);
    apop_data_free(name_sorted_cp);
    begin_transaction();
    if (fillins->matrix) apop_data_print(fillins, .output_name=fill_tab, .output_type='d', .output_append='a');
    commit_transaction();
    apop_data_free(fillins);
    //apop_data_print(raked, .output_name="ooo", .output_append='a');
    apop_data_free(raked); //Thrown away.
    gsl_vector_free(cp_to_fill);
    gsl_vector_free(original_weights);
    apop_data_free(d);
}


	
static int lil_ols_draw(double *out, gsl_rng *r, apop_model *m){
    double temp_out[m->parameters->vector->size+1];
    m->draw = apop_ols->draw;
    Apop_stopif(apop_draw(temp_out, r, m), out[0]=NAN; return 1,
            0, "OLS draw failed.");
    m->draw = lil_ols_draw;
    *out = temp_out[0];
    return 0;
}

static char *construct_a_query(char const *datatab, char const *underlying, char const *varlist, 
                                apop_data const *category_matrix, char const *id_col, char const* ego_id, char *depvar){
/* Find out which constraints fit the given record, then join them into a query.
   The query ends in "and", because get_constrained_page will add one last condition.  */

	//These ad hoc indices help in sqlite. ---except they're commented out right
	//now, due to a rush project.
/*    if (categories_left){ //maybe make a subindex
        int catct=0;
	 	char *idxname =strdup("idx_"); 
        for (int i=0; i< category_matrix->textsize[0] && catct < categories_left; i++)
		//index only the active categories, of course, and only those that aren't a pain to parse.
	    if (active_cats[i] && !apop_regex(category_matrix->text[i][0], "[()<>+*!%]")){
		    catct++;
	   apop_data *test_for_index = apop_query_to_data("select * from sqlite_master where name='%s'", idxname);
	   if (!test_for_index){
           //create index idx_a_b_c on datatab(a, b, c);
           char *make_idx, comma =' ';
           asprintf(&make_idx, "create index %s on %s (", idxname, underlying);
           for (int i=0, catct=0; i< category_matrix->textsize[0] && catct < categories_left; i++)
               if (active_cats[i]){
                   catct++;
                   qxprintf(&make_idx, "%s%c %s", make_idx, comma, category_matrix->text[i][0]);
                   if (strchr(make_idx,'=')) *strchr(make_idx,'=')='\0'; //cat is prob. varname=x, cut string at =.
                   comma = ',';
               }
           apop_query("%s)", make_idx);
           apop_data_free(test_for_index);
	   }
    }
	}
*/
    char *q;
    asprintf(&q, "select %s from %s where ", varlist, datatab);
    if (!category_matrix) return q;
    //else
    for (int i=0; i< category_matrix->textsize[0]; i++){
        char *n = *category_matrix->text[i]; //short name
        if (strcmp(n, depvar)){ //if n==depvar, you're categorizing by the missing var.

            /* apop_data *category_str = apop_query_to_text("select %s from %s where %s is \
                      null limit 1", n, datatab, n);

            Apop_stopif(category_str == NULL, return, 0, "I returned a NULL apop_data \
                    ptr. Something is wrong here."); 

            Apop_stopif(strcmp(**category_str->text, "NaN"), return, 0, "You gave me a null \
                    category. You should either check your syntax to make sure that \
                    you're giving me the right value, or consider imputing the \
                    category you gave me at an earlier point in your spec file."); */

            qxprintf(&q, "%s (%s) = (select %s from %s where %s = %s) and\n",  
                           q,  n,           n,  datatab, id_col, ego_id);
        }
    }
    return q;
}

static char *construct_a_query_II(char const *id_col, const impustruct *is, apop_data const *fingerprint_vars){
    /* Do we need the clause that includes the requisite items from the vflags table? */
    if (!fingerprint_vars || !apop_table_exists("vflags")) return NULL;
    char *q2 = NULL;
    int is_fprint=0; 
    for (int i=0; !is_fprint && i< fingerprint_vars->textsize[0]; i++)
        if (!strcasecmp(fingerprint_vars->text[i][0], is->depvar))
            is_fprint++;
    if (is_fprint)
        asprintf(&q2, "select %s from vflags where %s > 0", is->selectclause, is->depvar);
    return q2;
}

void install_data_to_R(apop_data *d, apop_model *m){
    R_model_settings *settings = Apop_settings_get_group(m, R_model);
    if (!settings) return; //not an R model.

    SEXP env;
    if (!settings->env){
        PROTECT(env = allocSExp(ENVSXP));
        SET_ENCLOS(env, R_BaseEnv);
        settings->env = env;
    } else {
        PROTECT(env = settings->env);
    }
    defineVar(mkChar("data"), rapop_df_from_ad(d), env);
    UNPROTECT(1);
}

apop_data * get_data_from_R(apop_model *m){
    R_model_settings *settings = Apop_settings_get_group(m, R_model);
    if (!settings) return NULL; //not an R model.

    static int is_inited;
    static apop_data_from_frame_type *rapop_ad_from_df;
    if (!is_inited)
        rapop_ad_from_df =  (void*) R_GetCCallable("Rapophenia", "apop_data_from_frame");

    SEXP env;
    PROTECT(env = settings->env);
    assert(TYPEOF(env) == ENVSXP);
    SEXP data_as_sexp = findVar(mkChar("data"), env);
    UNPROTECT(1);
    return rapop_ad_from_df(data_as_sexp);
}

apop_data *nans_no_constraints, *notnans_no_constraints;
char *last_no_constraint;

void verify(impustruct is){
    /*Apop_stopif(is.isnan && !is.notnan, return, 0, "Even with no constraints, I couldn't find any "
                "non-NaN records to use for fitting a model and drawing values for %s.", is.depvar);*/
    if (is.notnan){
        int v= apop_opts.verbose; apop_opts.verbose=0;
        Apop_stopif(gsl_isnan(apop_matrix_sum((is.notnan)->matrix)+apop_sum((is.notnan)->vector)), 
                return, 0, 
                "NULL values or infinities where there shouldn't be when fitting the model for %s.", is.depvar);
        apop_opts.verbose=v;
    }
    Apop_assert_c(is.isnan, , 2, "%s had no missing values. This sometimes happens when the fields used for sub-classes "
            "still has missing values. Perhaps do an imputation for these fields and add an 'earlier output table' line to "
            "this segment of the spec.", is.depvar);
}

/* Generate two tables: those that have Nulls and therefore need to be
   considered; those that don't have nulls, and therefore can be used to fit
   models.

   The generation here is almost symmetric, but usage is asymmetric. The isnans are mostly used only for
   the rowids of those who need filling in, and are read as text for the consistency_check system; the 
	notnans are a data set for model estimation, so rowids are irrelevant. 

   The information for both is there should you want to use it.

	Given a list of other flaws---consistency failures, disclosure---we've gotta decide 
	how we're gonna fit to those models. 

	Currently, one method involves hot-deck --> not really any ML anything
	OLS finds best fit using the rest of sample, not the item that needs
	subbing, and so it may pull further from the data than is useful; also, our
	version (probably most versions) goes one var. at a time --> leans toward one
	var over the others.
*/
static void get_nans_and_notnans(impustruct *is, char const* index, char const *datatab, 
        char const *underlying, int min_group_size, apop_data const *category_matrix, 
        apop_data const *fingerprint_vars, char const *id_col){
    is->isnan = NULL;
    char *q, *q2;
    if (!category_matrix || *category_matrix->textsize == 0){
        if (nans_no_constraints){//maybe recycle what we have.
            if(apop_strcmp(last_no_constraint, is->selectclause)){
                is->isnan = nans_no_constraints;
                is->notnan = notnans_no_constraints;
                return;
            } else { //this isn't the right thing; start over
                apop_data_free(nans_no_constraints);
                apop_data_free(notnans_no_constraints);
                is->isnan = is->notnan = NULL;
                last_no_constraint = strdup(is->selectclause);
            }
        }//else, carry on:
        asprintf(&q, "select %s from %s where 1 and ", is->selectclause, datatab);
    } else
        q = construct_a_query(datatab, underlying, is->selectclause, category_matrix, id_col, index, is->depvar);
    q2 = construct_a_query_II(id_col, is, fingerprint_vars);

    if (!strcmp(is->vartypes, "all numeric"))
        is->notnan = q2? apop_query_to_data("%s %s is not null except %s", q, is->depvar, q2) : apop_query_to_data("%s %s is not null", q, is->depvar);
    else is->notnan = q2? 
                 apop_query_to_mixed_data(is->vartypes, "%s %s is not null except %s", q, is->depvar, q2)
               : apop_query_to_mixed_data(is->vartypes, "%s %s is not null", q, is->depvar);
    apop_data_listwise_delete(is->notnan, .inplace='y');
    if (!strcmp(is->vartypes, "all numeric")){
         is->isnan = q2 ? 
              apop_query_to_data("%s %s is null union %s", q, is->depvar, q2)
            : apop_query_to_data("%s %s is null", q, is->depvar);
         //but we'll need the text for the consistency checking anyway.

         //Check whether isnan is null somewhere
         Apop_stopif(!is->isnan, return, 0, "query returned no data.");
         Apop_stopif(is->isnan->error, return, 0, "query failed.");
         apop_text_alloc(is->isnan, is->isnan->matrix->size1, is->isnan->matrix->size2);
         for (int i=0; i< is->isnan->matrix->size1; i++)
             for (int j=0; j< is->isnan->matrix->size2; j++)
                 apop_text_add(is->isnan, i, j, "%g", apop_data_get(is->isnan, i, j));
    } else is->isnan = q2 ? 
              apop_query_to_text("%s %s is null union %s", q, is->depvar, q2)
            : apop_query_to_text("%s %s is null", q, is->depvar);
    free(q); q=NULL;
    free(q2); q2=NULL;
    verify(*is);
}

//Primarily just call apop_estimate, but also make some minor tweaks.
static void model_est(impustruct *is, int *model_id){
    apop_data *notnan = is->notnan; //just an alias.
    assert(notnan);
    //maybe check here for constant columns in regression estimations.
    if (notnan->text && !apop_data_get_page(notnan,"<categories"))
        for (int i=0; i< notnan->textsize[1]; i++){
            if (!is->is_hotdeck && is->textdep && strcmp(notnan->names->col[i], is->depvar) )
                apop_data_to_dummies(notnan, i, .keep_first='y', .append='y');
            //the actual depvar got factor-ized in prep_for_draw.
//            apop_data_to_dummies(is->isnan, i, .keep_first='y', .append='y'); //presumably has the same structure.
        }
    Apop_stopif(notnan->vector && isnan(apop_vector_sum(notnan->vector)), return, 
            0, "NaNs in the not-NaN vector that I was going to use to estimate "
            "the imputation model. This shouldn't happen");
    Apop_stopif(notnan->matrix && isnan(apop_matrix_sum(notnan->matrix)), return,
            0, "NaNs in the not-NaN matrix that I was going to use to estimate "
            "the imputation model. This shouldn't happen");
    if (is->is_hotdeck) apop_data_pmf_compress(notnan);
	//Apop_model_add_group(&(is->base_model), apop_parts_wanted); //no extras like cov or log like.

    install_data_to_R(notnan, is->base_model); //no-op if not an R model.
	is->fitted_model = apop_estimate(notnan, is->base_model);
    Apop_stopif(!is->fitted_model, return, 0, "model fitting fail.");
    if (!strcmp(is->base_model->name, "multinomial"))
        apop_data_set(is->fitted_model->parameters, .row=0, .col=-1, .val=1);
    if (!strcmp(is->base_model->name, "Ordinary Least Squares"))
        is->fitted_model->draw=lil_ols_draw;
    if (verbose) apop_model_print(is->fitted_model, NULL);
    (*model_id)++;
    apop_query("insert into model_log values(%i, 'type', '%s');", *model_id, is->fitted_model->name);
    if (is->fitted_model->parameters && is->fitted_model->parameters->vector) //others are hot-deck or kde-type
        for (int i=0; i< is->fitted_model->parameters->vector->size; i++){
            char *param_name = NULL;
            if (is->fitted_model->parameters->names->rowct > i)
                param_name = is->fitted_model->parameters->names->row[i];
            else {
                free(param_name);
                asprintf(&param_name, "param %i", i);
            }
            apop_query("insert into model_log values(%i, '%s', %g);",
                    *model_id, param_name,
                    is->fitted_model->parameters->vector->data[i]);
        }
    apop_query("insert into model_log values(%i, 'subuniverse size', %i);"
                    , *model_id, (int) (is->fitted_model->data->matrix 
                                    ? is->fitted_model->data->matrix->size1
                                    : *is->fitted_model->data->textsize));
    /*if (is->fitted_model->parameters->vector)
        assert(!isnan(apop_sum(is->fitted_model->parameters->vector)));*/
}

static void prep_for_draw(apop_data *notnan, impustruct *is){
    apop_lm_settings *lms = apop_settings_get_group(is->fitted_model, apop_lm);
    if (is->textdep) apop_data_to_factors(is->notnan);
    if (lms){
        apop_data *p = apop_data_copy(notnan);
        p->vector=NULL;
        lms->input_distribution = apop_estimate(p, apop_pmf);
        is->fitted_model->dsize=1;
    }
}


/*Here, we check that the variable is within declared bounds. We have to do this
  because the edit system in consistency_check will assume that all variables are 
  within their declared range. By doing the checking here, we may even save some trips
  to the consistency checker.

Return: 1=input wasn't in the list, was modified to nearest value
        0=input was on the list, wasn't modified
        0=input variable has no associated checks.
  If function returns a 1, you get to decide whether to use the rounded value or re-draw.

  If text or real, there's no rounding to be done: it's valid or it ain't. If numeric, then we can do rounding as above.
  I only check for 't' for text; suggest 'n' for numeric but it's up to you.
*/

/* TeaKEY(checks, <<<This key is where the user specifies the parameters for the variables she declared in types. The parameters given here are checked during each round of imputation.>>>)
 */
int check_bounds(double *val, char const *var, char type){
//Hey, B: Check the SQL that is about only this variable. This will need to be modified for multi-variable models.
//After checking the SQL, check the declarations.

    char *val_as_text;
    asprintf(&val_as_text, "%g", *val);
    int rowid = ri_from_ext(var, val_as_text);
    free(val_as_text); 
    if (rowid >=0) return 0;
    //else, try to round it.
    if (type=='r' || type=='c') return 0;
/*    if (type =='c')
		return !!apop_query_to_float("select count(*) from %s where %s = '%s'",
var, var, val);*/
    else {//integer type
        double closest = find_nearest_val(var, *val);
        if (fabs(closest-*val) > 0){//may overdetect for some floats
            if (verbose) printf("rounding %g -> %g\n", *val , closest);
            *val = closest;
            return 1;
        }
    }
    return 0;
}

void R_check_bounds(double *val, char **var, int *fails){
    if (ri_from_ext(*var, "0") == -100) {//-100=var not found.
        *fails=0; 
        return;
    }
	*fails = check_bounds(val, *var, 'i');
}

    /* notes for check_records: 
		I have on hand all of the imputation models, so I can make new draws as desired.
      I expect all N replications to work together, so they all have to be tested.

      --for each NaN found, impute if there's a spec for it.
      --do{
          --run the consistency check, requesting failed var.s
          --if no failure, break.
          --generate a list of failed var.s with specs.
          --if empty, display a fault---I need a model to reconcile this---and break
          --randomly pick a failed var with a spec., impute.
     } while(1); //because the internal breaks are what will halt this.

      If we fill a record and find that it doesn't work, randomly select one variable to re-impute, 
      then try again. Max likelihood methods that would use the alternatives table that 
      consistency_check can produe are reserved for future work.
      */

static apop_data *get_all_nanvals(impustruct is, const char *id_col, const char *datatab){
    //db_name_column may be rowid, which would srsly screw us up.
    //first pass: get the full list of NaNs; no not-NaNs yet.
    apop_data *nanvals = apop_query_to_data("select distinct %s, %s from %s where %s is null "
                                            "order by %s",
									 id_col, is.depvar, datatab, is.depvar, id_col);
    if (!nanvals) return NULL; //query worked, nothing found.
    Apop_stopif(nanvals->error, return NULL, 0, "Error querying for missing values.");
    if (verbose){
        printf("For %s, the following NULLs:\n", is.depvar);
        apop_data_show(nanvals); //may print NULL.
    }
    return nanvals;
}

static int forsearch(const void *a, const void *b){return strcmp(a, *(char**)b);}
//static int forsearch(const void *a, const void *b){return atoi((char*)a) - atoi(*(char**)b);}

static int mark_an_id(const char *target, char * const *list, int len, char just_check){
    char **found = bsearch(target, list, len, sizeof(char*), forsearch);
    if(!found) return -1;
    if (just_check) return 0;
    asprintf(found, "%s.", *found);
    return 0;
}

typedef struct {
    char *textx;
    double pre_round;
    int is_fail;
} a_draw_struct;

/*This function is the inner loop cut out from impute(). As you can see from the list of
arguments, it really doesn't stand by itself.

The draw itself is one line---just call apop_draw. The hard part is in checking that the draw is OK.
This involves bounds-checking, if applicable, 
then generating a dummy version of the observation that is in an R-friendly format, 
then sending it to consistency_check for an up-down vote.

The parent function, make_a_draw, then either writes the imputation to the db or tries this fn again.
*/
static a_draw_struct onedraw(gsl_rng *r, impustruct *is, 
        char type, int id_number, int fail_id, 
        int model_id, apop_data *full_record, int col_of_interest){
    a_draw_struct out = { };
	static char const *const whattodo="passfail";
    double x;
    apop_draw(&x, r, is->fitted_model);
    Apop_stopif(isnan(x), return out, 0, "I drew NaN from the fitted model. Something is wrong.");
    apop_data *rd = get_data_from_R(is->fitted_model);
    if (rd) {
        x = rd->vector ? *rd->vector->data : *rd->matrix->data;
        apop_data_free(rd);
    }
    out.pre_round=x;
    if (type == '\0'){ // '\0' means not in the index of variables to check.
        asprintf(&out.textx, "%g", x);
    } else {
        if (!is->is_hotdeck && is->is_bounds_checkable) //inputs all valid ==> outputs all valid
            check_bounds(&x, is->depvar, type); // just use the rounded value.
        apop_data *f;
        if (is->textdep && (f = apop_data_get_factor_names(is->fitted_model->data, .type='t')))
             out.textx = strdup(*f->text[(int)x]);
        else asprintf(&out.textx, "%g", x);
/*        apop_query("insert into impute_log values(%i, %i, %i, %g, '%s', 'cc')",
                                id_number, fail_id, model_id, out.pre_round, out.textx);
*/
        //copy the new impute to full_record, for re-testing
        apop_text_add(full_record, 0, col_of_interest, "%s", out.textx);
        int size_as_int =*full_record->textsize;
        consistency_check((char *const *)(full_record->names->text ? full_record->names->text : full_record->names->col),
                          (char *const *)full_record->text[0],
                          &size_as_int,
                          &whattodo,
                          &id_number,
                          &out.is_fail,
                          NULL);//record_fails);
    }
    return out;
}

//a shell for do onedraw() while (!done).
static void make_a_draw(impustruct *is, gsl_rng *r, int fail_id,
                        int model_id, int draw, apop_data *nanvals, char *filltab){
    char type = get_coltype(is->depvar);
    int col_of_interest=apop_name_find(is->isnan->names, is->depvar, type !='c' && is->isnan->names->colct ? 'c' : 't');
    Apop_stopif(col_of_interest < -1, return, 0, "I couldn't find %s in the list of column names.", is->depvar);
    for (int rowindex=0; rowindex< is->isnan->names->rowct; rowindex++){
        char *name = is->isnan->names->row[rowindex];
        if (mark_an_id(name, nanvals->names->row, nanvals->names->rowct, 0)==-1){
            //Then this is already done. Verify in the main list; continue.
            char *pd; asprintf(&pd, "%s.", name);
            Apop_stopif(mark_an_id(pd, nanvals->names->row, nanvals->names->rowct, 'y')==-1,
                free(pd); continue, 0, "A sublist asked me to impute for ID %s, but I couldn't "
                    "find that ID in the main list of NaNs.", name);
            free(pd);
            continue;
        }
        int tryctr=0;
        int id_number = atoi(is->isnan->names->row[rowindex]);
        a_draw_struct drew;
        Apop_row(is->isnan, rowindex, full_record);
        do drew = onedraw(r, is, type, id_number, fail_id, 
                          model_id, full_record, col_of_interest);
        while (drew.is_fail && tryctr++ < 1000);
        Apop_stopif(drew.is_fail, , 0, "I just made a thousand attempts to find an "
            "imputed value that passes checks, and couldn't. "
            "Something's wrong that a computer can't fix.\n "
            "I'm at id %i.", id_number);

        char * final_value = (type=='c') 
				//Get external value from row ID
                                ? ext_from_ri(is->depvar, drew.pre_round+1)
                                : strdup(drew.textx); //I should save the numeric val.

	Apop_stopif(isnan(atof(final_value)), return, 0, "I drew a blank from the imputed column when I shouldn't have for record %i.", id_number);

        apop_query("insert into %s values(%i, '%s', '%s', '%s');",
                       filltab,  draw, final_value, is->isnan->names->row[rowindex], is->depvar);
        free(final_value);
        free(drew.textx);
    }
}

double still_is_nan(apop_data *in){return in->names->row[0][strlen(*in->names->row)-1]!= '.';}


/* As named, impute a single variable.
   We check bounds, because those can be done on a single-variable basis.
   But overall consistency gets done on the whole-record basis. 
   
    The main i-indexed loop is really just a search for representatives
    of every subgroup. The rowindex-indexed loop is where the filled table is actually filled.

    within the i loop:
        Get those in the category matching this guy, split by those having the
        relevant variable and those with NaNs at the var.
        Set the imputation model based on those w/o NaNs.
        Loop (rowindex-loop) over those with NaNs to write imputations to table of filled values
   
   */
static void impute_a_variable(const char *datatab, const char *underlying, impustruct *is, 
        const int min_group_size, gsl_rng *r, const int draw_count, apop_data *category_matrix, 
        const apop_data *fingerprint_vars, const char *id_col, char *filltab,
        char *previous_filltab){
    static int fail_id=0, model_id=-1;
    apop_data *nanvals = get_all_nanvals(*is, id_col, datatab);
    if (!nanvals) return;
    char *dt;
    char *dataxxx = (char*)datatab; //can't constify checkout, because of R

    //if there is a previous fill tab, then we need to do a re-estimation
    //of the model every time. If not, then we do one est & many draws.
    int outermax = previous_filltab ? draw_count : 1;
    int innermax = previous_filltab ? 1 : draw_count;

    apop_name *clean_names = NULL;
    if (outermax > 1) clean_names = apop_name_copy(nanvals->names);
    has_sqlite3_index(datatab, is->depvar, 'y');
    has_sqlite3_index(datatab, id_col, 'y');

    for (int outerdraw=0; outerdraw < outermax; outerdraw++){
        if (previous_filltab){
            asprintf(&dt, "%s_copy", datatab);
            check_out_impute(&dataxxx, &dt, &outerdraw, NULL, &previous_filltab);
        } else dt=strdup(datatab);
        begin_transaction();

        is->is_bounds_checkable = (ri_from_ext(is->depvar, "0") != -100); //-100=var not found.

        bool still_has_missings=true, hit_zero=false;
        do {
            for (int i=0; i < nanvals->names->rowct; i++){ //see notes above.
                Apop_row(nanvals, i, row_i);
                if (!still_is_nan(row_i)) continue;
                get_nans_and_notnans(is, nanvals->names->row[i] /*ego_id*/, 
                        dt, underlying, min_group_size, category_matrix, fingerprint_vars, id_col);

                if (!is->isnan) goto bail; //because that first guy should've been missing.
                if (!is->notnan || GSL_MAX((is->notnan)->textsize[0]
                            , (is->notnan)->matrix ? (is->notnan)->matrix->size1: 0) < min_group_size)
                    goto bail;
                is->is_hotdeck = (is->base_model->estimate == apop_multinomial->estimate 
                                        ||is->base_model->estimate ==apop_pmf->estimate);
                model_est(is, &model_id); //notnan may be pmf_compressed here.
                prep_for_draw(is->notnan, is);
                for (int innerdraw=0; innerdraw< innermax; innerdraw++)
                    make_a_draw(is, r, ++fail_id, model_id, 
                                    GSL_MAX(outerdraw, innerdraw), nanvals, filltab);
                apop_model_free(is->fitted_model); //if (is_hotdeck) apop_data_free(is->fitted_model->data);
                bail:
                apop_data_free(is->notnan);
                apop_data_free(is->isnan);
            }
            /*shrink the category matrix by one, then loop back and try again if need be. This could be more efficient, 
              but take recourse in knowing that the categories that need redoing are the ones with 
              few elements in them.*/
            if (category_matrix && *category_matrix->textsize>1){
                apop_text_alloc(category_matrix, category_matrix->textsize[0]-1, category_matrix->textsize[1]);
                index_cats(datatab, category_matrix);

            } else {
                hit_zero++;
                nans_no_constraints= is->isnan;
                notnans_no_constraints= is->notnan;
                last_no_constraint = strdup(is->selectclause);
            }
            still_has_missings = apop_map_sum(nanvals, .fn_r=still_is_nan);
        } while (still_has_missings && !hit_zero);
        if (previous_filltab) apop_table_exists(dt, 'd');
        commit_transaction();
        if (still_has_missings && hit_zero) printf("Even with no constraints, I still "
                             "couldn't find enough data to model the data set.");
        if (outermax > 1){
            apop_name_free(nanvals->names);
            nanvals->names = apop_name_copy(clean_names);
        }
    }
    apop_data_free(nanvals);
    apop_name_free(clean_names);
}

apop_model null_model = {.name="null model"};

/* TeaKEY(impute/method, <<<Specifies what model to use to impute output vars for a given impute key.>>>)
 */
apop_model *tea_get_model_by_name(char *name, impustruct *model){
    static get_am_from_registry_type *rapop_model_from_registry;
    static int is_inited=0;
    if (!is_inited && using_r)
        rapop_model_from_registry = (void*) R_GetCCallable("Rapophenia", "get_am_from_registry");

    apop_model *out= !strcmp(name, "normal")
          ||!strcmp(name, "gaussian")
				? apop_normal :
			!strcmp(name, "multivariate normal")
				? apop_multivariate_normal :
			!strcmp(name, "lognormal")
				? apop_lognormal :
			!strcmp(name, "rake") ||!strcmp(name, "raking")
				?  (model->is_em = true, &null_model) :
			!strcmp(name, "hotdeck")
		  ||!strcmp(name, "hot deck")
	      ||!strcmp(name, "multinomial")
		  ||!strcmp(name, "pmf")
				? (model->is_hotdeck=true, apop_pmf) :
			!strcmp(name, "poisson")
				? (model->is_regression=true, apop_poisson) :
			!strcmp(name, "ols")
				? (model->is_regression=true, apop_ols) :
			!strcmp(name, "logit")
				? (model->is_regression=true, apop_logit) :
			!strcmp(name, "probit")
				? (model->is_regression=true, apop_probit) :
			!strcmp(name, "rel")
				? relmodel :
			!strcmp(name, "kernel")
	      ||!strcmp(name, "kernel density")
				? apop_kernel_density 
				: &null_model;
        if (using_r && !strcmp(out->name, "Null model")) //probably an R model.
            out= rapop_model_from_registry(name);
        Apop_stopif(!strcmp(out->name, "Null model"), return &(apop_model){}, 0, "model selection fail.");
        Apop_model_add_group(out, apop_parts_wanted, .predicted='y'); //no cov
        if (!strcmp(out->name, "PDF or sparse matrix")) out->dsize=-2;
        return out;
}

void prep_imputations(char *configbase, char *id_col, gsl_rng **r){
    int seed = get_key_float(configbase, "seed");
    *r = apop_rng_alloc((!isnan(seed) && seed>=0) ? seed : 35);
    apop_table_exists("impute_log", 'd');
    apop_query("create table impute_log (%s, 'fail_id', 'model', 'draw', 'declared', 'status')", id_col);
    if (!apop_table_exists("model_log"))
        apop_query("create table model_log ('model_id', 'parameter', 'value')");
}

/* 
TeaKEY(impute/vars, <<<A comma-separated list of the variables to be put into the imputation model. 
For OLS-type models where there is a distinction between inputs and outputs, don't use this; use the "impute/input vars" and "impute/output vars" keys. Note that this is always the plural "vars", even if you are imputing only one field.>>>)
TeaKEY(impute/input vars, <<<A comma-separated list of the independent, right-hand side variables for imputation methods such as OLS that require them. These variables are taken as given and will not be imputed in this step, so you probably need to have a previous imputation step to ensure that they are complete.>>>)
TeaKEY(impute/output vars, <<<The variables that will be imputed. For OLS-type models, the left-hand, dependent variable (notice that we still use the plural "vars"). For models that have no distinction between inputs and outputs, this behaves identically to the "impute/vars" key (so only use one or the other).>>>)
 */
impustruct read_model_info(char const *configbase, char const *tag, char const *id_col){
	apop_data *varlist=NULL, *indepvarlist=NULL, *outputvarlist=NULL;
    apop_regex(get_key_word_tagged(configbase, "vars", tag),
                " *([^,]*[^ ]) *(,|$) *", &varlist); //split at the commas
    apop_regex(get_key_word_tagged(configbase, "output vars", tag),
                " *([^,]*[^ ]) *(,|$) *", &outputvarlist);
    apop_regex(get_key_word_tagged(configbase, "input vars", tag),
                " *([^,]*[^ ]) *(,|$) *", &indepvarlist);
    Apop_stopif(!varlist && !outputvarlist, return (impustruct){.error=1}, 0, "I couldn't find a 'vars' or 'output vars' line in the %s segment", configbase);

	impustruct model = (impustruct) {.position=-2, .vartypes=strdup("n")};
    model.depvar = strdup(outputvarlist ? **outputvarlist->text : **varlist->text);
    model.allvars_ct = (indepvarlist ? *indepvarlist->textsize : 0)
                       + (varlist ? *varlist->textsize : 0)
                       + (outputvarlist ? *outputvarlist->textsize : 0);

    //find the right model.
    char *model_name = get_key_word_tagged(configbase, "method", tag);
    model.base_model = tea_get_model_by_name(model_name, &model);
    Apop_stopif(!strlen(model.base_model->name), model.error=1; return model, 0, "model selection fail; you requested %s.", model_name);

/* In this section, we construct the select clause that will produce the data we need for estimation. apop_query_to_mixed_data also requires a list of type elements, so get that too.

Further, the text-to-factors function requires a spare column in the numeric part for each text element.

Hot deck: needs no transformations at all.
Raking: needs all the variables, in numeric format
*/

    char coltype = get_coltype(model.depvar); // \0==not found.
    if (coltype=='r' || coltype=='i') 
          asprintf(&model.vartypes, "%sm", model.vartypes);
    else  asprintf(&model.vartypes, "%st", model.vartypes), model.textdep=true;

    if (model.is_em){
        int i=0;
        model.allvars = malloc(sizeof(char*)*model.allvars_ct);
        if (varlist) for (; i< *varlist->textsize; i++) model.allvars[i]= strdup(*varlist->text[i]);
        if (outputvarlist) for (; i< *outputvarlist->textsize; i++) model.allvars[i]= strdup(*outputvarlist->text[i]);
        if (indepvarlist) for (int j=0; j< *indepvarlist->textsize; j++) model.allvars[i+j]= strdup(*indepvarlist->text[j]);

        /*TeaKEY(impute/near misses, <<<If this is set to any value, then the EM algorithm (the
          only consumer of this option) will weight nearby cells when selecting cells to draw
          from for partial imputations. Else, it will use only cells that match the nonmissing data.>>>)*/
        if (get_key_word_tagged(configbase, "near misses", tag)) model.allow_near_misses = true;
    }

    //if a text dependent var & a regression, set aside a column to be filled in with factors.
    if (model.is_regression)
         asprintf(&model.selectclause, "%s, %s%s", id_col, model.textdep ? "1, ": " ", model.depvar);	
    else asprintf(&model.selectclause, "%s, %s", id_col, model.depvar);	
    char *indep_vars = get_key_word_tagged(configbase, "input vars", tag); //as a comma-separated list, for SQL.
    if (indep_vars){
        asprintf(&model.selectclause, "%s%c %s", 
                XN(model.selectclause),model.selectclause ? ',' : ' ',
                                    process_string(indep_vars, &(model.vartypes)));
        asprintf(&model.vartypes, "all numeric"); //lost patience implementing text data for OLS.
    }
	apop_data_free(varlist); apop_data_free(outputvarlist); apop_data_free(indepvarlist);
    return model;
}

int impute_is_prepped; //restarts with new read_specs.
char *configbase = "impute";

/* TeaKEY(impute/input table, <<<The table holding the base data, with missing values. 
  Optional; if missing, then I rely on the sytem having an active table already recorded. So if you've already called {\tt doInput()} in R, for example, I can pick up that the output from that routine (which may be a view, not the table itself) is the input to this one.>>>)
  TeaKEY(impute/seed, <<<The RNG seed>>>)
  TeaKEY(impute/draw count, <<<How many multiple imputations should we do? Default: 1.>>>)
  TeaKEY(impute/output table, <<<Where the fill-ins will be written. You'll still need {\tt checkOutImpute} to produce a completed table. If you give me a value for {\tt impute/eariler output table}, that will be the default output table; if not, the default is named {\tt filled}.>>>)
  TeaKEY(impute/earlier output table, <<<If this imputation depends on a previous one, then give the fill-in table from the previous output here.>>>)
  TeaKEY(impute/margin table, <<<Raking only: if you need to fit the model's margins to out-of-sample data, specify that data set here.>>>)
 */
int do_impute(char **tag, char **idatatab){ 
    /* At the beginning of this function, we check the spec file to verify that the
     * user has specified all of the necessary keys for impute(...) to function correctly.
     * If they haven't we alert them to this and exit the function.
     */

    Apop_stopif(get_key_word("input", "output table") == NULL, , 0, "You didn't specify an output table in your input key so I'm going to use `filled' as a default. If you want another name then specify one in your spec file.");
    Apop_stopif(get_key_word("impute", "input table") == NULL, return -1, 0, "You need to specify an input table in your impute key.");
    Apop_stopif(get_key_word("impute", "method") == NULL, return -1, 0, "You need to specify the method by which you would like to impute your variables. Recall that method is a subkey of the impute key.");
    
    //This fn does nothing but read the config file and do appropriate setup.
    //See impute_a_variable for the real work.
    Apop_stopif(!*tag, return -1, 0, "All the impute segments really should be tagged.");
    Apop_stopif(!*idatatab, return -1, 0, "I need an input table, "
                        "via a '%s/input table' key. Or, search the documentation "
                        "for the active table (which is currently not set).", configbase);
    Apop_stopif(!apop_table_exists(*idatatab), return -1, 0, "'%s/input table' is %s, but I can't "
                     "find that table in the db.", configbase, *idatatab);

    char *underlying = get_key_word_tagged(configbase, "underlying table", *tag);



/* TeaKEY(impute/categories, <<<Denotes the categorized set of variables by which to impute your output vars.>>>)
 */
    apop_data *category_matrix = get_key_text_tagged(configbase, "categories", *tag);

/* TeaKEY(impute/min group size, <<<Specifies the minimum number of known inputs that must be present in order to perform an imputation on a set of data points.>>>)
 */
    float min_group_size = get_key_float_tagged(configbase, "min group size", *tag);
    if (isnan(min_group_size)) min_group_size = 1;

    float draw_count = get_key_float_tagged(configbase, "draw count", *tag);
    if (isnan(draw_count) || !draw_count) draw_count = 1;

    char *weight_col = get_key_word_tagged(configbase, "weights", *tag);
    char *out_tab = get_key_word_tagged(configbase, "output table", *tag);

    char *previous_fill_tab = get_key_word_tagged(configbase, "earlier output table", *tag);
    if (!out_tab && previous_fill_tab) out_tab = previous_fill_tab;
    Apop_stopif(!out_tab, out_tab = "filled",
        0, "You didn't specify an output table in your input key so I'm going to use `filled' as a default. If you want another name than specify one in your spec file.");

    char *id_col= get_key_word(NULL, "id");
    if (!id_col) {
        id_col=strdup("rowid");
        if (verbose) printf("I'm using the rowid as the unique identifier for the "
                    "index for the imputations. This is not ideal; you may want "
                    "to add an explicit Social Security number-type identifier.");
    }
    char *tmp_db_name_col = strdup(apop_opts.db_name_column);
    sprintf(apop_opts.db_name_column, "%s", id_col);

    index_cats(*idatatab, category_matrix);

    static gsl_rng *r;
    if (!impute_is_prepped++) prep_imputations(configbase, id_col, &r);
    //I depend on this column order in a few other places, like check_out_impute_base.
    if (!apop_table_exists(out_tab))
        apop_query("create table %s ('draw', 'value', '%s', 'field');"
                "create index idx_%s_%s   on %s (%s);"
                "create index idx_%s_field  on %s (field);"
                "create index idx_%s_draw on %s (draw);",
                    out_tab,id_col, id_col, out_tab, out_tab, id_col,
                    out_tab, out_tab, out_tab, out_tab);
    apop_data *fingerprint_vars = get_key_text("fingerprint", "key");

    impustruct model = read_model_info(configbase, *tag, id_col);
    Apop_stopif(model.error, return -1, 0, "Trouble reading in model info.");

    if (model.is_em) {
        apop_data *catlist=NULL;
        if (category_matrix){
            char *cats = apop_text_paste(category_matrix, .between=", ");
            catlist = apop_query_to_text("select distinct %s from %s", cats, *idatatab);
            Apop_stopif(!catlist || catlist->error, return -1, 0, 
                "Trouble querying for categories [select distinct %s from %s].", cats, *idatatab);
        }
        for (int i=0; i< (catlist ? *catlist->textsize: 1); i++){
            char *wherecat = NULL;
            if (catlist){
                char *and = " ";
                for (int j=0; j< catlist->textsize[1]; j++){
                    qxprintf(&wherecat, "%s %s %s='%s'", XN(wherecat), and, catlist->names->text[j],
                            catlist->text[i][j]);
                    and = " and ";
                }
            }
if(catlist) printf("%s\n", catlist->text[i][0]);
            char *margintab = get_key_word_tagged(configbase, "margin table", *tag);
            rake_to_completion(*idatatab, underlying, model, min_group_size, 
                        r, draw_count, wherecat, fingerprint_vars, id_col, 
                        weight_col, out_tab, margintab, previous_fill_tab);
        }
    }
    else impute_a_variable(*idatatab, underlying, &model, min_group_size, 
                r, draw_count, category_matrix, fingerprint_vars, id_col, out_tab,
                previous_fill_tab);
    apop_data_free(fingerprint_vars);
    apop_data_free(category_matrix);
    sprintf(apop_opts.db_name_column, "%s", tmp_db_name_col);
    return 0;
}



/* TeaKEY(impute, <<<The key where the user defines all of the subkeys related to the doMImpute() part of the imputation process. For details on these subkeys, see their descriptions elsewhere in the appendix.>>>)
 */
void impute(char **idatatab){ 
    /* At the beginning of this function, we check the spec file to verify that the
     * user has specified all of the necessary keys for impute(...) to function correctly.
     * If they haven't we alert them to this and exit the function.
     */
    
    //The actual function starts here:
    apop_data *tags = apop_query_to_text("%s", "select distinct tag from keys where key like 'impute/%'");
    for (int i=0; i< *tags->textsize; i++){
        char *out_tab = get_key_word_tagged(configbase, "output table", *tags->text[i]);
        if (!out_tab) out_tab = "filled";
        apop_table_exists(out_tab, 'd');
    }

    for (int i=0; i< *tags->textsize; i++)
        do_impute(tags->text[i], idatatab);
    apop_data_free(tags);
}

/* multiple_imputation_variance's default now.
static apop_data *colmeans(apop_data *in){
    apop_data *sums = apop_data_summarize(in);
    Apop_col_tv(sums, "mean", means);
    apop_data *out = apop_matrix_to_data(apop_vector_to_matrix(means, 'r'));
    apop_name_stack(out->names, in->names, 'c', 'c');
    apop_data *cov = apop_data_add_page(out, apop_data_covariance(in), "<Covariance>");
    gsl_matrix_scale(cov->matrix, sqrt(in->matrix->size1));
    return out;
}*/

void get_means(){
    char *configbase = "impute_by_groups";
	char *idatatab = get_key_word(configbase, "datatab");
    if (!idatatab) idatatab = datatab;
    apop_data * main_data = apop_query_to_data("select * from %s", idatatab);
    //apop_data * fill_ins = apop_query_to_data("select * from all_imputes");
    //apop_data_show(apop_multiple_imputation_variance(colmeans, main_data, fill_ins));
    apop_data_free(main_data);
}
