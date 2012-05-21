#include "discrete.h"
#include "tea.h"

void generate_indices(char const *tablename); //in text_in.c
extern int file_read;

#define Qcheck(...) if (__VA_ARGS__) abort();

/* recode_list is the recode spec for one variable. Could be 
   A | age >3
   B |
or it could be
    age/2. + 7
*/
char *one_recode_to_string(apop_data const *recode_list, int *is_formula, int *has_else, int doedits){
    char *clauses=NULL;
    for (int j=0; j < *recode_list->textsize; j++){
        apop_data *one_rc = NULL;
        char const *thisrc = *recode_list->text[0];
        apop_regex(thisrc, "[ \t]*(([^|]|\\|\\|)+)[ \t]*(\\||$)", &one_rc);//break into delimited parts
        //first column is the data, second is garbage (an artifact of regex parens), third may be a pipe
        //If there are multiple pipes, then there are multiple rows like that.
        //E.g., "A | age > 3" becomes a 2x3 table:
        // A    , x , |  ,
        // age>3, x ,    ,
        if (one_rc->textsize[1] < 3 || !strchr(one_rc->text[0][2], '|')){ 
            //Then it's not of the form "something | [...]", so treat as a formula.
            (*is_formula)++;
            return strdup(thisrc);
        } //else:
        if (*one_rc->textsize==2){
            //the "category | condition" version
            xprintf(&clauses, "%s when %s then '%s'\n", XN(clauses), *one_rc->text[1], strip(*one_rc->text[0]));
        } else if (*one_rc->textsize==1){ 
            //the default category.
            apop_assert(!*has_else, "You have a recode with two default categories.");
            xprintf(&clauses, "%s else '%s'\n", 
                clauses ? clauses : "when 0 then 0", strip(*one_rc->text[0]) );
            (*has_else)++;
        } else
            Apop_assert(0, "This line doesn't parse right as a recode: %s", thisrc);
        if (doedits) add_to_num_list(strip(one_rc->text[0][0]));
        apop_data_free(one_rc);
    }
    return clauses;
}

void recodes(char **key, char** tag, char **outstring, char **intab){
// All this function does is produce a query string. text_in/recodes then runs the query.
// **key may be "recodes" or "group recodes".
    apop_data *all_recodes;
    *outstring=NULL;
    if(tag && strlen(*tag)>0)
        all_recodes = apop_query_to_text("select distinct key from keys where "
                "(key like '%s%%' and key not like '%s%%no checks' and key not like 'group recodes/group id')"
                " and tag ='%s'",*key,*key, XN(tag[0]));
    else
        all_recodes = apop_query_to_text("select distinct key from keys where "
                "(key like '%s%%' and key not like '%s%%no checks' and key not like 'group recodes/group id')"
                " order by tag", *key, *key);
    if (!all_recodes || !all_recodes->textsize[0]) return;
    for(int i=0; i < all_recodes->textsize[0]; i++) // "recodes/var1" ==> "var1"
        all_recodes->text[i][0] += strlen(*key)+1;
    if (verbose) apop_data_show(all_recodes);

    int new_vars = apop_query_to_float("select count(*) from (select distinct key from keys where "
                                "(key like '%s%%' and key not like '%s%%no checks' and key not like 'group recodes/group id') %s%s%s "
                                , *key, *key, tag ? "and tag='":"", tag ? *tag:"", tag ? "')": ")");
    apop_data *editcheck = apop_query_to_text("select value from keys where "
                "key like '%s%%no checks' %s%s%s"
                , *key, tag ? "and tag='":"", tag ? *tag:"", tag ? "'": " ");

    if (!new_vars) return; //No recodes for the given **key.
    for (int i=0; i < new_vars; i++){
        char *varname = all_recodes->text[i][0]; //just an alias
        char comma = ' ';
        apop_data *recode_list = get_key_text(*key, varname);
        Apop_assert(recode_list && recode_list->textsize[0],
                        "%s looks like a recode field, but I can't parse "
                        "its recodes. Please check on this.", varname);
        
        int doedits = 0;
        if (editcheck)
            for (int e=0; doedits && e<editcheck->textsize[0]; e++)
                if (apop_regex(editcheck->text[e][0], varname))
                    doedits= 1;

        int has_else = 0, is_formula=0;
        if (doedits) add_var(varname, 1, 'n');
        if (verbose) printf("%s recode list size: %i\n",varname,recode_list->textsize[0]);
        char *clauses = one_recode_to_string(recode_list, &is_formula, &has_else, doedits);

        //clauses is now the core of a variable definition. Wrap it and add it to the list.
        if (!is_formula && !has_else) asprintf(&clauses, "%s else null\n", clauses);
        comma = ',';		
        if (is_formula)
            xprintf(outstring, "%s%c %s as %s\n", XN(*outstring), comma, strip(clauses), varname);
        else
            xprintf(outstring, "%s%c case %s end as %s\n", XN(*outstring), comma, strip(clauses), varname);
    }
    if (verbose) printf("recode string: %s\n", *outstring);
    apop_data_free(editcheck);
}

void get_in_out_tabs(char const *first_or_last, char **intab, char **out_name){
    //Names are a pain: the first input should be the intab,
    //the last output should be view[intab]
    //but in the middle, we may chain an arbitrary number of views together,
    //so we'll need to chain this in = last out and make up intermediate names.
    if (apop_strcmp(first_or_last, "first") 
           || apop_strcmp(first_or_last, "both"))
        *intab = get_key_word("input", "output table");
    else {//chaining from before
        free(*intab);
        *intab = strdup(*out_name);
    }
    if (apop_strcmp(first_or_last, "last") 
           || apop_strcmp(first_or_last, "both"))
         asprintf(out_name, "view%s", get_key_word("input", "output table"));
    else {
        free(*out_name);
        asprintf(out_name, "mid%s", *intab);
    }
}
    
int set_up_triggers(char const * intab){
    apop_data *imputables= get_variables_to_impute(NULL);
    if (!imputables) return 0;

    intab = get_key_word("input", "output table");
    char *idcol= get_key_word(NULL, "id");
    apop_query("create index %sidcol on %s(%s)", intab, intab, idcol);
    apop_data *o = apop_query_to_data("select * from %s limit 1", intab);
    for (int i=0; i< o->names->colct; i++){
        int is_imputable=0; //only variables in the impute list need triggers.
        char *col= o->names->column[i];
        for (int j=0; !is_imputable && j< imputables->textsize[0]; j++)
            if (apop_strcmp(*imputables->text[j], col))
                is_imputable++;
        if (is_imputable)
            Qcheck(apop_query("drop trigger if exists trig%s%s; "
                "create trigger trig%s%s instead of update of %s on view%s  "
                "begin update %s set %s = new.%s where %s=old.%s; end",
                 intab, col, intab, col, col, intab, intab, col, col, idcol, idcol));
    }
    apop_data_free(o);
    apop_data_free(imputables);
    return 0;
//	generate_indices(out_name); //exactly the same indices as the main tab.
}

/* Generate the recode view using the recodes segment.

    Called from \c read_spec (in peptalk.y).

We're also going to create triggers for those variables which have an imputation
section. So far, imputation is the only way in which variables can change, so if the
variable isn't mentioned in the imputes, it doesn't need a trigger.

\li If the data text isn't read in yet (and there is a text-reading section), then I will do
the read-in now.

\key {group recodes/group id} The column with a unique ID for each group (e.g., household number).
\key {group recodes/recodes} A set of recodes like the main set, but each calculation of the recode will be grouped by the group id, so you can use things like {\tt max(age)} or {\tt sum(income)}.

Returns 0 on OK, 1 on error.
*/
int make_recode_view(char **tag, char **first_or_last){
    //first_or_last may be "first", "last", "both", or "middle"
    char *q = strdup("select distinct key from keys where "
            " (key like 'recodes%' or key like 'group recodes%')");
    if (tag && *tag) asprintf(&q, "%s and tag like '%%%s%%'", q, *tag);
	apop_data *test_for_recodes = apop_query_to_text("%s", q);
    free(q);
	if (!test_for_recodes) return 0;
	apop_data_free(test_for_recodes);

    if (!file_read && get_key_word("input", "input file")) text_in();

    static char *intab =NULL;
    static char *out_name =NULL;
    get_in_out_tabs(*first_or_last, &intab, &out_name);

    char *recodestr=NULL, *group_recodestr=NULL;
    char *overwrite = get_key_word("input", "overwrite");
    if (!overwrite || !strcasecmp(overwrite,"n") 
                || !strcasecmp(overwrite,"no") 
                || !strcasecmp(overwrite,"0") )
        {free(overwrite), overwrite = NULL;}
    /*Apop_assert_c (!(!overwrite && apop_table_exists(out_name)), , 0,
                    "Recode view %s exists and input/overwrite tells me to not recreate it.", out_name);
*/
    apop_table_exists(out_name, 'd');
    char *rgroup="recodes", *grgroup="group recodes";
    recodes(&rgroup, tag, &recodestr, &intab);      //will query there for whether any recodes exist.
    recodes(&grgroup, tag, &group_recodestr, &intab);
    if (group_recodestr){
        char *group_id= get_key_word("group recodes", "group id");
        Apop_assert(group_id, "There's a group recodes section, but no \"group id\" tag.");
        apop_table_exists("tea_group_stats", 'd');
        apop_table_exists("tea_record_recodes", 'd');
        Qcheck(
        apop_query("create view tea_group_stats as "
                   "select %s %s from %s group by %s; ",
                     group_id, group_recodestr, intab, group_id)
        || apop_query("create view tea_record_recodes as select * %s from %s", XN(recodestr), intab)
        || apop_query("create view %s as select * from tea_record_recodes r, tea_group_stats g "
                   "where r.%s=g.%s", out_name, group_id, group_id)
            )
    }
    else 
        Qcheck(apop_query("create view %s as select * %s from %s", out_name, XN(recodestr), intab));
    return set_up_triggers(intab);
}
