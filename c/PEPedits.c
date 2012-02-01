#include <R.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>
#include "tea.h"

char **(*TEA_R_STRSXP_TO_C)(SEXP s);
SEXP (*TEA_R_get_apop_data_matrix)(const apop_data *D);

void R_init_tea(DllInfo *info){
	TEA_R_STRSXP_TO_C = (char **(*)(SEXP)) R_GetCCallable("Rapophenia","R_STRSXP_TO_C");
	TEA_R_get_apop_data_matrix = (SEXP(*)(const apop_data*)) R_GetCCallable("Rapophenia","R_get_apop_data_matrix");
}

SEXP RCheckConsistency(SEXP Rrecord_name_in, SEXP Rud_values, SEXP Rrecord_in_size,
        SEXP Rwhat_you_want, SEXP Rid, SEXP Rfails_edits, SEXP Rrecord_fails){
	
	apop_data *alternatives;

//Rrecord_name_in is a STRSXP if it's coming from as.character...
//this means it's a pointer to a list of CHARSXPs...

	char **record_name_in = TEA_R_STRSXP_TO_C(Rrecord_name_in);
	char **ud_values = TEA_R_STRSXP_TO_C(Rud_values);
	int *record_in_size = INTEGER(Rrecord_in_size);
	const char **what_you_want = TEA_R_STRSXP_TO_C(Rwhat_you_want);
	int *id = INTEGER(Rid);
	int *fails_edits = INTEGER(Rfails_edits);
	int *record_fails = INTEGER(Rrecord_fails);

	alternatives = consistency_check((char const **)record_name_in, 
						(char const **)ud_values, record_in_size,
		what_you_want, (const int *) id, fails_edits, record_fails);
	//apop_data_show(alternatives);
	//alternatives are right, so losing stuff somwhere below
	if(alternatives){
		SEXP Rmat = TEA_R_get_apop_data_matrix(alternatives);
		apop_data_free(alternatives);
		return(Rmat);
	}else{
		return(R_NilValue);
	}
}

