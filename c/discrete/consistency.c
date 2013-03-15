#include "internal.h"
#include "tea.h"
#include <apop.h>
#include <setjmp.h>
#include <stdbool.h>

void init_edit_list();
void xprintf(char **q, char *format, ...); //in ../impute/parse_sql.c

/* A great deal of the pain that went into writing this file was from letting
   the user provide variables in any order, and only a few. We thus have one
   numbering system for the user's list of variables, and one for the edit
   matrix. Be on your toes: the indices are half one and half the other, and I
   (BK) put insufficient notice of which is which. I added two conversion
   matrices, em_to_user and user_to_em, late in the game; you can probably make
   more use of them than their current use.*/

int *em_to_user, *user_to_em;

/** DISCRETE, by Winkler et al, had the goal of finding the smallest change on the
  smallest subset of the records that would pass all edits. Our goal is to report to the
  caller all records that fail any edit, and all possible values that could be
  imputed---a sort of maximal edit set.  Thus the rewrite. 
*/

/* A field where all elements in em are T isn't really a part of the edit---it's just a
   placeholder. So, check whether a field enters an edit or is just along for the ride. */
static int entering (int const row, int const rec){
    for (int i = find_b[rec]-1; i< find_e[rec]; i++)
        if (!gsl_matrix_get(edit_grid->matrix, row, i))
            return 1;
    return 0;
}

int prune_edits(apop_data *d, int row, int col, void *us){ //callback for the fn below
    bool *usable_sql = us;
    return usable_sql[row];
}

void check_for_all_vars(bool *usable_sql, char * const restrict*record_name_in, int record_in_size){
    for (int r=0; r< edit_grid->vector->size; r++){
        if(apop_data_get(edit_grid, r, -1) != 2) {//won't matter in prune_edits above.
            usable_sql[r] = false;
            continue; 
        }
        usable_sql[r] = true;
        for (int j=0; j< record_in_size; j++){
            bool found=false;
            for (int i=0; i< edit_list[r].var_ct; i++)
                if (!strcmp(edit_list[r].vars_used[i].name, record_name_in[j]))
                        {found=true; break;}
            if (!found) {
                usable_sql[r] = false;
                break;
            }
        }
    }
}

/** Check each edit for a failure, meaning every T in the record matches a T in the edit (i.e.
  a row in the em matrix).

   Once we find a record T that matches an edit F (we verify that the record does 
not have the same value as the edit in that column), we know that the record 
passes the edit; move on to the next.

  If the input array failures is NULL, then we're in brief mode: first time we find a
  failed edit, then exit reporting non-passing.

  If failures is an array to be filled (of size nsize: the number of fields in
  em), then check all edits, and mark down every field involved in any failure.
  There is one slot per field (i.e., it is unrelated to any subset of records
  requested by the user).
  */
static int check_a_record(int const * restrict row,  int * failures, 
                   int rownumber, char *const *data_as_query, char * const restrict*record_name_in, int record_in_size){
    int rowfailures[nflds];
    int out = 0, has_c_edits=0;
    if (failures)
        memset(failures, 0, sizeof(int)*nflds);
    if (!edit_grid)
        return 0;
    for (int i=0; i < edit_grid->matrix->size1; i++){
        if (gsl_vector_get(edit_grid->vector, i) == 2) {//this row has real values in it; skip.
            //check that all of the variables used in the query are in the list
            //sent to consistency_check
            has_c_edits=1; 
            continue;
        }
        int rowfailed = 1; //each row is guilty until proven innocent.
        int has_actual_failed_fields=0;
        memset(rowfailures, 0, sizeof(int)*nflds);
        int rec = 0;
        for (int j =0; j < edit_grid->matrix->size2; j++){
            if (row[j] == -1){
                if (gsl_matrix_get(edit_grid->matrix, i, j)==-1)
                    j = find_e[rec]-1;  //this is a dummy record, so fast-forward to end of record.
                else { //then we can't test this edit: a needed field is missing from row[].
                    rowfailed = 0;
                    break;
                }
                //else, row[j]==-1 && edit grid has a -1
            } else if (row[j]){
                if (gsl_matrix_get(edit_grid->matrix, i, j)) {
                    if (entering(i, rec)){
                        rowfailures[rec]= j-find_b[rec]+2;//use the record value as a marker.
                        has_actual_failed_fields++;
                    }
                } else {
                    rowfailed = 0;
                    break; //out of the j-loop.
                }
				j = find_e[rec]-1;//only one true val per record, so fast-forward to end of record.
            }
            if (find_e[rec]-1==j) //at end of record?
                rec++;
        }
        if (rowfailed && has_actual_failed_fields){
            out = 1;
            if (failures){
                for(int k= 0; k < nflds; k++)
                    failures[k] += !!rowfailures[k]; //adds one or zero.
               /* for (int m=0; m< edit_grid->textsize[1]; m++)
                    if (strlen(edit_grid->text[i][m]))
                        apop_query("insert into editinfo values(%i, %i, 'f', \"%s\", '')",
													 rownumber, i, edit_grid->text[i][m]);
                for (int m=0; m< nflds; m++)
                    if (rowfailures[m])
                        apop_query("insert into editinfo values(%i, %i, 'r', \"%s\", '%s')",
													 rownumber, i, used_vars[m].name,
                                                     ext_from_ri(used_vars[m].name, rowfailures[m]));*/
                if (verbose){
                    printf("Failed edits:\n");
                    for (int m=0; m< edit_grid->textsize[1]; m++)
                        if (strlen(edit_grid->text[i][m]))
                            printf("\t%s\n", edit_grid->text[i][m]);
                    printf("Because:\n");
                    for (int m=0; m< nflds; m++)
                        if (rowfailures[m])
                            printf("\t%s = %s\n", used_vars[m].name,
                              ext_from_ri(used_vars[m].name, rowfailures[m]));
                    printf("\n");
                }
            }
        }
    }
    if (has_c_edits){
        bool usable_sql[edit_grid->vector->size];
        check_for_all_vars(usable_sql, record_name_in, record_in_size);
	Apop_stopif(*data_as_query==NULL, return, 0, "*data_as_query is null. Should be a string of data.");
        apop_query("%s", *data_as_query);
        char *q = apop_text_paste(edit_grid, .between=") or (", .after=")",
                  .before= "select count(*) from tea_test where (", .prune=prune_edits, .prune_parameter=usable_sql);
        if (strcmp(q, "select count(*) from tea_test where ()"))//any relevant sql?
            out = apop_query_to_float("%s", q); //matches = failure = return 1.
        free(q);
        apop_table_exists("tea_test", 'd');
    }
    return out;
}

/* Fill the table of all possible alternatives. This involves checking all combinations of
   an indeterminate number of variables, and that's the sort of thing most easily done via recursion. 

   When you get to the last field in the list, you will have a record that is ready to
   test via check_a_record. If that function returns zero, then write to \c fillme. To
   make life easier, <tt>fillme</tt>'s vector holds a single number: the length of the
   array so far.

   Now that you know what happens at the end, the recursion that leads up to it will, for each field, blank out the field, then for (i= each possible record value), set the record to i, and call the function to operate on the next field down.  This loop-and-recurse setup will guarantee that the last field in the list will run a test on all possible values.

   Notice that fields that aren't marked as failing are just skipped over.
*/
/*   \key timeout Once it has been established that a record has failed a consistency
   check, the search for alternatives begins. Say that variables one, two, and three each have 100
    options; then there are 1,000,000 options to check against possibly thousands
    of checks. If a timeout is present in the spec (outside of all groups), then the
    alternative search halts and returns what it has after the given number of seconds
    have passed.

   */
static void do_a_combo(int *record, char *const restrict  *record_name_in, int const *user_to_em, int record_in_size, 
         int const *failed_edits_in, int this_field, int field_count, apop_data *fillme,
		 jmp_buf jmpbuf, time_t const timeout){

    int option_ct, skipme=0;
	int this_field_index = em_to_user[this_field];
	if (this_field_index==-1 || !failed_edits_in[this_field]){//not a failed field; skip.
		skipme=1;
		option_ct=1;
	}
	else option_ct = find_e[this_field_index] - find_b[this_field_index] +1;
    if (!skipme)
        memset(record+find_b[this_field_index]-1, 0, sizeof(int)* option_ct);
	//else leave this field exactly as it was originally
    for (int i=0; i < option_ct; i++){
        if (!skipme){
            record[find_b[this_field_index]-1+i] = 1;
            if (i > 0) //clear the last value
                record[find_b[this_field_index]-1+i-1] = 0;
        }
        if (this_field +1 < nflds)
            do_a_combo(record, record_name_in, user_to_em, record_in_size,
							failed_edits_in, this_field+1, field_count, fillme, jmpbuf, timeout);
        else{
			if (timeout && time(NULL) > timeout) longjmp(jmpbuf, 1);
            if (!check_a_record(record, NULL, 0, NULL, record_name_in, record_in_size)){//OK record; write it.
                int this_row = apop_data_get(fillme, 0, -1);
                apop_vector_increment(fillme->vector, 0);
                for (int k=0; k< field_count; k++){
                    int this_field_indexx = user_to_em[k];
                    if (failed_edits_in[this_field_indexx]){
                        for (int j=find_b[this_field_indexx]-1; j< find_e[this_field_indexx]; j++)
                            if (record[j]){
								if (fillme->matrix->size1 <=this_row)
									fillme->matrix = apop_matrix_realloc(
												fillme->matrix,
												fillme->matrix->size1*2, fillme->matrix->size2);
								//int rec_by_user_ordering = edit_order_to_user_order(this_field_indexx, record_name_in, field_count);
								int rec_by_user_ordering = em_to_user[this_field_indexx];
                                apop_data_set(fillme, .colname=record_name_in[rec_by_user_ordering],
                                        .row=this_row, 
                                        .val=j-(find_b[this_field_indexx]-1));
								break;
							}
                    }
                }
            }
        }
    }
}

/* This function just gets the maximal dimensions of the alternatives table, allocates,
    and fills via the recursive \c do_a_combo function above. So this is really just the
    prep function for the recursion, where the real work happens.
*/
static apop_data * get_alternatives(int *restrict record, char *const  restrict  *record_name_in, 
							int const *restrict user_to_em, int const record_in_size, 
							int const *restrict  failing_records){
    int total_fails = 0;
    int rows = 1;
    for (int i = 0; i< record_in_size; i++){
	    int this_field = user_to_em[i];
        Apop_stopif(this_field < 0, apop_data*out=apop_data_alloc(); out->error='f'; return out,
               0,  "I couldn't find %s.", record_name_in[i]);
        total_fails += failing_records[this_field] ? 1 : 0;
        if (failing_records[this_field]){
            rows *= find_e[this_field] - find_b[this_field]+1;
        }
    }
	Apop_stopif(!total_fails,  apop_data*out=apop_data_alloc(); out->error='c'; return out,
                0, "Failed internal consistency check: I marked this "
				"record as failed but couldn't find which fields were causing failure.");
    apop_data *out = apop_data_alloc(1, rows, total_fails);
    out->vector->data[0] = 0;
    for (int i=0; i< nflds; i++)
        if (failing_records[i])
            apop_name_add(out->names, used_vars[i].name, 'c');

	//prepare the timeout; call the recursion
	jmp_buf jmpbuf;
	double user_timeout = get_key_float("timeout", NULL);
	time_t timeout = isnan(user_timeout) ? 0 : user_timeout+time(NULL);
	if (!setjmp(jmpbuf))
		do_a_combo(record, record_name_in, user_to_em, edit_grid->matrix->size2, failing_records, 0, record_in_size, out, jmpbuf, timeout);
	else
		fprintf(stderr,"Timed out on consistency query. Partial alternatives returned, but set may not be complete.\n");
    out->matrix = apop_matrix_realloc(out->matrix, out->vector->data[0], out->matrix->size2);
    return out;
}

//set up the cross-index conversions. 
static void setup_conversion_tables(char * const restrict* record_name_in, int record_in_size){
	em_to_user = realloc(em_to_user, sizeof(int)*total_var_ct);
	user_to_em = realloc(user_to_em, sizeof(int)*record_in_size);
	memset(em_to_user, -1, sizeof(int)*total_var_ct);
	memset(user_to_em, -1, sizeof(int)*record_in_size);
	for (int i=0 ; i< record_in_size; i++)
        for (int j=0 ; j< total_var_ct; j++)
            if (apop_strcmp((char*)used_vars[j].name, (char*) record_name_in[i])
                    && used_vars[j].type != 'r'){
                em_to_user[j] = i;
                user_to_em[i] = j;
                break;
            }
}

// Generate a record in DISCRETE's preferred format for the discrete-valued fields,
// and a query for the real-valued.
static void fill_a_record(int record[], int const record_width, char * const restrict *record_name_in, 
                        char * const restrict* ud_values, int record_in_size, 
                                int id, char **qstring){
    for (int i=0; i < record_width; i++)
        record[i]=-1;   //-1 == ignore-this-field marker
    for (int i=0; i < record_in_size; i++){
        int ri_position = ri_from_ext(record_name_in[i], ud_values[i]);
        if (ri_position == -100) continue;  //This variable wasn't declared ==> can't be in an edit.
	    //	printf("Using %s at var %d with value %s\n",
    	//		record_name_in[i],user_to_em[i],ud_values[i]);
        for(int  kk = find_b[user_to_em[i]]-1; kk< find_e[user_to_em[i]]; kk++)
          record[kk] = 0;
        Apop_stopif(ri_position == -1 , return, 0, "I couldn't find the value %s in your "
                "declarations for the variable %s. Please remove the error from the data or "
                "add that value to the declaration, then restart the program so I can rebuild "
                "some internal data structures.", ud_values[i], record_name_in[i]);
        int bit = find_b[user_to_em[i]]-1 + ri_position-1;
        Apop_stopif(bit >= record_width || bit < 0, return, 0,
                    "About to shift position %i in a record, but there "
                    "are only %i entries.", bit, record_width);
        record[bit] = 1;
    }
    if (verbose){
        printf("record %i:\n", id);
        for (int i=0; i< record_width; i++)
            printf("%i\t", record[i]);
        printf("\n");
    } 
    /*qstring will hold a query to generate a sample table, like "create table tea_test(a,
    b); insert into tea_test values('left', 'right');"*/
    char *insert, comma=' ';
    asprintf(qstring, "create table tea_test(");
    asprintf(&insert, "insert into tea_test values(");
    for (int i=0; i< record_in_size; i++){
        xprintf(qstring, "%s%c'%s' ", *qstring, comma, record_name_in[i]);
        xprintf(&insert, "%s%c'%s' ", insert, comma, ud_values[i]);
        comma=',';
    }
    xprintf(qstring, "%s); %s);", *qstring, insert);
    free(insert);
}

//A lengthy assertion checking that failed_fields and fails_edits are in sync.
static void do_fields_and_fails_agree(int *failed_fields, int fails_edits, int nflds){
    int total_fails = 0;
    for (int k=0; k < nflds; k++)
        total_fails += failed_fields[k];
    assert((total_fails !=0 && fails_edits) || (total_fails==0 && !fails_edits));
}

/* see tea.h for documentation.  */
apop_data * consistency_check(char * const *record_name_in, char * const *ud_values, 
			int const *record_in_size, char const *const *what_you_want, 
			int const *id, int *fails_edits, int *failed_fields){
	Apop_stopif(*record_in_size <= 0, return NULL, 1, "zero record size; returning NULL.");
    if (!edit_grid) init_edit_list();
    if (!edit_grid){ //then there are no edits.
		*fails_edits = 0;
        return NULL;
	}
    int width = edit_grid->matrix->size2;
	Apop_stopif(!width, return NULL, 1, "zero edit grid; returning NULL.");
    int record[width];
    char *qstring = NULL;
    if (record_name_in) setup_conversion_tables(record_name_in, *record_in_size);
	fill_a_record(record, width, record_name_in, ud_values, *record_in_size, *id, &qstring);
    if (!strcmp(what_you_want[0], "passfail")){
        *fails_edits = check_a_record(record, NULL, 0, &qstring, record_name_in, *record_in_size);
        free(qstring);
        return NULL;
    }
    *fails_edits = check_a_record(record, failed_fields, *id, &qstring, record_name_in, *record_in_size);
    free(qstring);
    do_fields_and_fails_agree(failed_fields, *fails_edits, nflds);

    if (!strcmp(what_you_want[0], "failed_fields"))
        return NULL;
    if (!strcmp(what_you_want[0], "find_alternatives") && *fails_edits)
		return get_alternatives(record, record_name_in, user_to_em, *record_in_size, failed_fields);
	return NULL;
}

apop_data *checkData(apop_data *data){

    //copy field names from the input data.
	int nvars = data->names->colct + data->names->textct;
	char *fields[nvars];
    memcpy(fields, data->names->column, sizeof(char*)*data->names->colct);
    memcpy(&fields[data->names->colct], data->names->text, sizeof(char*)*data->names->textct);

	//now that we have the variables, we can call check_a_record for each row
	int id=1;
	int nrow = data->matrix ? data->matrix->size1: *data->textsize;
	int fails_edits, failed_fields[nvars];
	char *vals[nvars];
	char const *what = "failed_fields";
	apop_data *failCount = apop_data_calloc(nrow,nvars);
    apop_name_stack(failCount->names, data->names, 'c', 'c');
    apop_name_stack(failCount->names, data->names, 'c', 't');

	for(int idx=0; idx<nrow; idx++){
		for(int jdx=0; jdx<nvars; jdx++){
			if(data->matrix && jdx < data->matrix->size2)
				asprintf(&vals[jdx], "%g", apop_data_get(data,idx,jdx));
			else vals[jdx] = data->text[idx][jdx];
		}
		consistency_check(fields,vals,&nvars,&what, &id,&fails_edits,failed_fields);
		//insert failure counts
		for(int jdx=0; jdx < nvars; jdx++){
			apop_data_set(failCount,.row=idx,.col=jdx,.val=failed_fields[jdx]);
		}
	}
	return failCount;
}
