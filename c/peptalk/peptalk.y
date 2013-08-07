%{

/* \file 
This is the main grammar for parsing the SQL-style input files to the edit
system. The grammar is at the top of the file, and you can see that
certain patterns trigger certain C functions, which appear after the
grammar.

I will assume you have had a look at the user-side documentation,
included in the R manual pages. 

There are about four types of operations:

--setting a key/value pair, which is put into the keys table. There are two forms:
    key:value
    #and
    key{
        value1
        value2
    }

--Declaring a new variable, via the "fields" group.
    For each new variable, I create a new table with a single column,
    where both the table and the column have the variable name. I fill
    the table with the valid values specified on the command line.

--Adding a consistency check
    The hard part here is that I need to work out what is a variable,
    and call the vars using the table.var form (where the table and var
    name are identical, as above.

--A pre-edit, of the form A => B (a subset of consistency checks. This implies two steps:
	* Rewrite this as update table set B were A.
	* Adding A as a standard old edit.
	The only annoying part here is storing A for later use.

The big magic trick to the setup is that each declaration produces one table
for one variable. Let the declarations say 
x: 1-2
y: 1-5 
Then we'll produce two new tables: one named x, whose sole column will be named x, and which will have two rows; one table named y, with a column named y, with values 1 to five.

A query over a set of variables will output the cross product of all combinations---that's just how SQL works, and we can use that to produce a comprehensive list of variables that pass/fail an edit.

Say the user adds the constraint
x >= y.

We'll convert that to 
select *
from x, y
where x.x >= y.y

which will produce the output

x y
---
1 1
2 1
2 2

and that's (almost) exactly the form we need to feed into the edit system. 

We keep a list of variables that have been used so far. That list includes a marker for the last query that used the variable, because we need to add a variable to the list of tables in the from clause exactly once for each query.


But wait---let's make this more complicated: recodes are artificial variables generated by
the user, which will exist in a view but not the original table or declarations. This gets
done after the parsing is done, and besides, we don't want to impose an order on the
segments of the config. 

So: the file is parsed in two passes.
Pass zero:   key table filled
            variables declared

then, recodes generate more variables, and the view table is set

Pass one:   generate edits

The functions here are all run by the yyparse() function. To see the context in which that lives, see bridge.c

*/

//int yydebug=1;

#include <stdlib.h>
#include <stdbool.h>
#include "internal.h"
#include <string.h>
#define YYSTYPE char*
#define YYMAXDEPTH 100000
    YYSTYPE last_value = 0;
int yylex(void);
int yyerror(const char *s) ;
int add_keyval(char *, char*);
int genbnds_();                                /* ********* SPEER **********/
int speer_();                                  /* ********* SPEER **********/
void add_to_num_list_seq(char *min, char*max);
void extend_key(char *in);
void reduce_key();
void store_right(char*); 
void add_check(char *);
void moreblob(char **, char *, char *);
char add_var_no_edit(char const *var, int is_recode, char type);
int lineno;        /* current line number  */

used_var_t *used_vars;
edit_t *edit_list;
char *var_list, *current_key, parsed_type;
int val_count, preed2;
apop_data *pre_edits;
int pass, has_edits, file_read;
char  *nan_marker;

%}
//tokens delivered from lex
%token SPACE NUMBER DTEXT THEN EOL TEXT ',' ':' OPENKEY CLOSEKEY '$' '-' '*' TYPE
%%
list: list item 
    | item
    ;

item: keyval
	| keygroup    
    | EOL
    | SPACE
    | declaration     /*flex eats the { }s for the declaration group*/
    | error
    ;

keyval: blob ':' blob  {extend_key($1); if(add_keyval(current_key, $3) == -1){YYABORT;}
            else { reduce_key($1); } } /* add_keyval(...) returns -1 on error and calls YYABORT
                                        which causes yyparse() to immediately return
                                        with 1. This is used in read_spec() to detect
                                        whether an error occurred in filling the keys
                                        table (designed specifically to prevent problems
                                        with there being no database key).
                                        */
      ;

keygroup: blob OPENKEY {extend_key($1);} keylist CLOSEKEY {reduce_key($1);} 
      ;

keylist: keylist keylist_item 
       | keylist_item
       ;

keylist_item: preedit 
           | keyval
           | keygroup
           | EOL
           | error   {warning("Trouble parsing an item [%s] in the list of fields[%d]", $1, lineno);}
	   | blob { if (apop_strcmp(current_key, "checks")) {add_check($1);}
                   else if(add_keyval(current_key, $1) != -1){} else YYABORT;}
           ;            // See reason above in keyval statement declaration 
                        // for logic of if statement with add_keyval.

preedit  : blob {add_check($1); preed2=1;} THEN blob {store_right($4);preed2=0;} EOL ;

declaration: DTEXT optionalcolon TYPE {add_var($2,0, parsed_type);} numlist
										{add_to_num_list(nan_marker ? strdup(nan_marker): NULL);}  EOL
           | DTEXT optionalcolon TYPE {add_var($2,0, parsed_type);}  EOL
           | DTEXT optionalcolon {parsed_type='c'; add_var($2,0, 'c');} numlist
										{add_to_num_list(nan_marker ? strdup(nan_marker): NULL);} EOL
           ;

optionalcolon: ':'
             | optionalcolon SPACE
             |
             ;

numlist : num_item
        | numlist ',' num_item
        | numlist SPACE
        |
        ;

num_item : NUMBER '-' NUMBER  {add_to_num_list_seq($1, $3);}
         | NUMBER             {add_to_num_list($1);}
         | DTEXT               {add_to_num_list($1);}
         | '*'               {add_to_num_list("*");}
         |'$' NUMBER           {used_vars[total_var_ct-1].weight = atof($2);}
         | error   			{Rf_warning("Error in list around [%s] line [%d].", $1,lineno);}
         ;

blob : blob blob_elmt {moreblob(&$$,$1,$2);}
     | blob_elmt {moreblob(&$$,"",$1);}
     | error
     ;

blob_elmt : TEXT 
         | NUMBER
         | ','   {$$=",";}
         | '*'   {$$="*";}
         | '-'   {$$="-";}
         | '$'   {$$="$";}
         | SPACE {$$=" ";}
         ;
%%

#include <apop.h>
#include <string.h>
void xprintf(char **q, char *format, ...); //impute/parse_sql.c
#define XN(in) ((in) ? (in) : "")          //same.



/************************************************************************/ 
// SPEER bounds generating variables
/************************************************************************/ 
#include "f2c.h"
#include "stdio.h"

struct {
    real lower[81]	/* was [9][9] */, upper[81]	/* was [9][9] */;
} comgen_;
#define comgen_1 comgen_
/* Table of constant values */
static integer c__1 = 1;
/************************************************************************/ 


void extend_q(char **, char*, char*);

int query_ct;
char *current_var, *datatab, *database, *nan_marker=NULL, *preed, *last_preed;
char *tag = NULL;

//Declarations. Create a new table for each variable,
//then just read in values as given.

char * strip(const char *in){
    static char stripregex[1000] = "";
    char w[] = "[:space:]\n";
    apop_data *d;
  	if (!strlen(stripregex)){
      sprintf(stripregex, "[%s\"]*([^%s]|[^%s]+.*[^%s]+)[%s\"]*", w,w,w,w,w);
    }
	if (!apop_regex(in, stripregex, &d)) return NULL;
	char* out= strdup(d->text[0][0]);
    apop_data_free(d);
    return out;
}

int transacting; //global var in bridge.c
static void set_database(char *dbname){  //only on pass 0.
    if (!strcmp(dbname, "mem")) dbname=":memory:";
    if (verbose) printf("opening database %s.\n", dbname);
    transacting=0; //reset the begin/commit counter.
    Apop_stopif(apop_db_open(dbname), return, 0, "failed to open database %s", dbname);
    database= strdup(dbname);
    apop_table_exists("keys", 'd');
    apop_table_exists("variables", 'd');
    apop_query("create table keys (key, tag, count, value); "
               "create table variables (name);");
}

int add_keyval(char *key, char *val){ 
    if (pass) return 0;
	char *skey = strip(key);
	char *sval = strip(val);
    char *rest = strrchr(skey,'/');
    if (!sval) return 0;
	if (apop_strcmp(skey, "database"))
		set_database(sval);
	Apop_stopif(!database, return -1, 0, "The first item in the config file (.spec) needs to be \"database:db_file_name.db\".");
        if (strcmp(rest ? (rest + 1) : skey,"paste in")) { //disagree i.e. not a paste in
  	  apop_query("insert into keys values (\"%s\", \"%s\", %i, \"%s\")", skey, XN(tag), ++val_count, sval);
        // paste in macro code by selecting lines from the database
        } else {
	  // select from keys what was stored in the macro where tag = skey
            apop_data *pastein = apop_query_to_text("select * from keys where key like '%s%%'",sval);
            Apop_stopif(!pastein, return -1, -1,"paste in macro %s not found in keys table.",sval);
            Apop_stopif(pastein->error,return -1, -1,"SQL: query for %s failed.",sval);
            for (int j = 0;j < pastein->textsize[0];j++) {
              char *nkey, *vkey;
              nkey = malloc(100);
              nkey = strtok(pastein->text[j][0],"/ ");
              nkey = strtok(NULL,"/");
              vkey = malloc(100);
              strcpy(vkey,pastein->text[j][3]);
	  // insert full code into keys
              int ii = rest-skey;
              char *query; 

              asprintf(&query,"insert into keys values " 
                          "(\"%%%d.%ds/%%s\", \"%%s\", %%i, \"%%s\")",ii,ii);
  	      apop_query (query, skey,
                          nkey,
                          XN(tag), ++val_count, vkey) ;
            }
        }
	free(skey); free(sval);
    return 0;
}

char add_var_no_edit(char const *var, int is_recode, char type){
    if (!var) return 's'; //should never happen.
    if (pass !=0 && !is_recode) return 's';
	/* set current_var
		prep the optionct list
		add it all to used_vars
	*/
    if (is_recode) //recodes may have already been declared.
        for (int i=0; i< total_var_ct; i++)
            if (apop_strcmp(var, used_vars[i].name)) return 's';
    free(current_var);
	if (verbose)
	    printf("A var: %s (type %s)\n", var, type=='n' ? "no type" : type=='i' ? "integer" : "unknown");
    current_var = strdup(var);
    //we used to have costs, but I removed them after revison b8a03fe3740eb6c07c .
	total_var_ct++;
	used_vars = realloc(used_vars, sizeof(used_var_t)*(total_var_ct+1));
	used_vars[total_var_ct-1] = (used_var_t) {.name=strdup(var), .weight=1, .last_query=-1, .type=type};
	used_vars[total_var_ct] = (used_var_t) {};//null sentinel.
    return 'c';
}


void add_var(char const *var, int is_recode, char type){
	/* set call the to-do list in add_var_no_edit
		create the table in the db
	*/
    char stop_or_continue = add_var_no_edit(var, is_recode, type);
    if (stop_or_continue =='s') return;
    optionct = realloc(optionct, sizeof(int)*(total_var_ct));
    optionct[total_var_ct-1] = 0;
    if (type!='r'){
        apop_table_exists(current_var, 'd');
		/*ahh here it is, table was always text*/
		if(type=='i')
	        apop_query("create table %s (%s integer); create index indx%svar on %s(%s)",
                                    var, var,var,var,var);
		else
	        apop_query("create table %s (%s text); create index indx%svar on %s(%s)",
                                    var, var,var,var,var);
        apop_query("insert into variables values ('%s')", var);
    }
}


/* 
one variable, multiple values
		case var
		when a1 then val
		when a2 then val2
		else valn
		end

general version
		case 
		when var=a1 then val
		when var2=a2 then val2
		else valn
		end

*/

char *make_case_list(char *in){
    //take in 2, 3,4-8 ,10, 12 and spit out where 2 or 3 or 4 or 5 or 6 ... then val
	apop_data *d;
	apop_regex(in, "([^,]+)(,|$)", &d);
	char *out = NULL;
	for (int j=0; j < d->textsize[0]; j++) {
		apop_data *splitme=NULL;
		if (apop_regex(d->text[j][0], "^[ \t]*([0-9]+)[ \t]*-[ \t]*([0-9]+)[ \t]*$", &splitme))
			for (int i=atoi(splitme->text[0][0]); i<=atoi(splitme->text[0][1]); i++)
				xprintf(&out, "%s%s %i", XN(out), out ? " or ":"", i);
		else 
			xprintf(&out, "%s%s %s", XN(out), out ? " or ":"", d->text[j][0]);
	} 
	apop_data_free(d);
	return out;
}

void add_to_num_list(char *v){
    if (pass !=0) return;
    char *vs = strip(v);
    Apop_assert_c(!(parsed_type=='r' && (v && strlen(vs)>0)), , 1,
         "I ignore ranges for real variables. Please add limits in the check{} section.");
    if (parsed_type=='r') return; 
/*	if (apop_strcmp(vs, "*")){
		text_in(); //See, I need to prep the elements before I do text_in,
                   //so this * feature creates a dependency loop. Resolving
                   //this is now low on the to-do list.
		apop_data *invalues = apop_query_to_text("select distinct %s from %s", current_var, datatab);
		for (int i=0; i< invalues->textsize[0]; i++)
			add_to_num_list(invalues->text[i][0]);
		apop_data_free(invalues);
	}*/
	int already_have = 
			(vs && apop_query_to_float("select count(*) from %s where %s='%s'", 
												current_var, current_var, vs))
			||
			(!vs && apop_query_to_float("select count(*) from %s where %s is null", 
													current_var, current_var));
    //if not already an option, add it in:
    if(!already_have){
		if (!vs) apop_query("insert into %s values (NULL);", current_var);
		else {
			char *tail;
			if (parsed_type == 'i') {
                int val = strtod(vs, &tail); val=0||val; //I don't use val. just check that it parses OK.
            }
			if (parsed_type != 'i' || *tail != '\0'){ //then this can't be parsed cleanly as a number
				apop_query("insert into %s values ('%s');", current_var, vs);
                if (parsed_type == 'i' && *tail != '\0')
                    Apop_notify(0, "The variable %s is set to integer type, but I can't "
                                "cleanly parse %s as a number. Please fix the value or add"
                                "a type specifier of 'cat'.", current_var, vs);
            } else
				apop_query("insert into %s values (%s);", current_var, vs);
		}
        optionct[total_var_ct-1]++;
    }
	free(v);
	free(vs);
}

// Specifies range for integer variable.  For example 0-16.  Ranges do not apply for real variables.
//
// The edited table is defined for each element of the range in the database.
//
// A check is preformed to make sure the range is not backwards like 16-0 or not a range like 4-4.  Variables should be variable.

void add_to_num_list_seq(char *min, char*max){
    if (pass !=0) return;
    Apop_assert_c(parsed_type!='r', , 1,
         "TEA ignores ranges for real variables. Pleas	e add limits in the check{} section.");
    Apop_stopif(atoi(min)>=atoi(max),return,0,"Maximum value %s does not exceed Minimum values %s",max,min);
    for (int i = atoi(min); i<=atoi(max);i++){
        apop_query("insert into %s values (%i);", current_var, i);
		optionct[total_var_ct-1]++;
    }
	free(min);
	free(max);
}

//Queries.
     

void moreblob(char **out, char* so_far, char *more){
    //if (pass==0 && !apop_strcmp(current_key, "checks")) {
    if (!apop_strcmp(current_key, "checks") || preed2) {
		asprintf(out, "%s%s", XN(so_far), more);
        return;
    }
     
    
/************************************************************************/ 
// SPEER bounds generating routine
    if( pass == 1 ) {
        genbnds_();
        speer_();
      }
    
  
    //more = strip(more);  //leak?
    if(pass==1 && apop_strcmp(current_key, "checks")){
        /*If you're here, you're in query mode and extending a query.
        Queries are mostly just read in verbatim, but if we find a
        variable X, then we have to do three things: 
        add the table X to the from clause of the query, 
        check that the X table exists, and 
        rewrite X as X.X in the where clause that the user gave us.  */
        int is_used=0;
        int is_in_list=0;

        //First, handle the edit_list, which stores the edit w/o modification.
        if (!edit_list) {
            edit_list = malloc(sizeof(edit_t));
            edit_list[0] = (edit_t){};
        }	
        if (!edit_list[query_ct].clause){ //always allocate one unit ahead, so this test will work.
            edit_list = realloc(edit_list, sizeof(edit_t)* (query_ct+2));
            edit_list[query_ct+1] = (edit_t){};
        }
        edit_t *el = &edit_list[query_ct]; //Just an alias.
        xprintf(&el->clause, "%s%s", XN(el->clause), more);

        //OK, now take care of the variable lists.
        for (int i=0; i< total_var_ct; i++)
            if (used_vars && !strcasecmp(more, used_vars[i].name)){
                is_in_list++;
                if (used_vars[i].last_query == query_ct)
                    is_used++;
                used_vars[i].last_query = query_ct;

                //if not in the vars_used list, add it on
                int already_used =0;
                for (int j=0; j< el->var_ct; j++)
                    if (apop_strcmp(more, el->vars_used[j].name)){
                        already_used++;
                        break;
                    }
                if (!already_used){
                    el->vars_used = realloc(el->vars_used, sizeof(used_var_t)*++el->var_ct);
                    el->vars_used[el->var_ct-1] = used_vars[i];
                }
            }

        //Finally, extend the query
        xprintf(&preed, "%s%s", XN(preed), more); //needed for pre-edits; harmless elsewhere
        if (!is_in_list){ //then it's not a variable.
            asprintf(out, "%s%s", XN(so_far), more);
            return;
        }
        bool isreal = false;
        for (int i=0, isfound=0; i< total_var_ct && !isfound && !isreal; i++) 
            if((isfound=!strcmp(more, used_vars[i].name)))
                isreal= (used_vars[i].type=='r');
        if (!is_used){ //it's a var, but I haven't added it to the list for this q.
            if (!var_list) var_list = strdup(more);
            else           asprintf(&var_list, "%s, %s", more, var_list);
            if (!isreal){
                if (!datatab) datatab = get_key_word("input","output table");
                Apop_stopif(!datatab, return, 0, "I need the name of the data table so I can set up the recodes."
                                     " Put an 'output table' key in the input segment of the spec.");
                if (!apop_table_exists(more))
                    apop_query( "create table %s as "
                                "select distinct %s.rowid as %s from %s;"
                                , more, more, more, datatab);
            }
        }
        asprintf(out, " %s %s.%s ", XN(so_far), more, more);
    }
}

void extend_key(char *key_in){
    static size_t default_tag;
    char *inkey = strip(key_in);
    char* open_brace_posn = strchr(inkey, '[');
    char* end_brace_posn = strchr(inkey, ']');
    if (open_brace_posn && end_brace_posn){
        *end_brace_posn = '\0';  //cut off the inkey at the end brace.
        tag= strip(open_brace_posn+1);//copy everything after the open-brace (minus surrounding whitespace)
        *open_brace_posn = '\0'; //cut off the inkey at the open brace.
    } else if (!current_key) //top level; make a fake tag
        asprintf(&tag, "%zu tag", default_tag++);

    //Push a key segment on the stack
    if (!current_key)
        current_key = strip(inkey);
    else 
        xprintf(&current_key, "%s/%s", current_key, strip(inkey));
}

void reduce_key(char *in){
    //Pop a key segment off the stack
    Apop_stopif(!current_key, return, 0, "The {curly braces} don't seem to match. Please "
            "check your last few changes.");
    int i;
    for (i=strlen(current_key); i > -1; i--)
        if (current_key[i] == '/') break;
    if (i == -1){
        free(current_key);
        current_key = NULL;
        tag[0] = '\0';      //the tags are currently top-level only
    }
    else current_key[i]= '\0'; //crop the string to that point.
}


/* This function is for post-parsing. it actually executes the list of pre-edits generated
by store_right below.  */

void do_preedits(char **datatab){
    //takes in pointer-to-string for compatibility with R.
	if (pre_edits)
		for (int i=0; i < pre_edits->textsize[0]; i++)
			apop_query("update %s %s", *datatab, pre_edits->text[i][0]);
}

/* I store a list of pre-edits in preedits, visible anywhere internal.h is present.
   They're read here, and then executed using do_preedits.  */
void store_right(char *fix){ 
    if (pass !=1) return;
    int ts = pre_edits ? pre_edits->textsize[0] : 0;
    pre_edits = apop_text_alloc(pre_edits, ts+1, 1);
    apop_text_add(pre_edits,ts,0, " set %s where %s;", fix, last_preed);
    free(last_preed);
}

apop_data *ud_queries = NULL; //ud=user-defined.
extern int explicit_ct;

/* All the extend_q work above produces a list of variables (and thus
a list of tables, since we're naming tables and variables identically),
and a where clause. Here, we join those into a proper query, and run.
*/
void add_check(char *this_query){
    if (pass !=1 || !this_query || !strip(this_query)) 
        return;  //only after declaration pass; no blanks
    has_edits=1;
	Apop_stopif(!var_list, return, 0, 
				"The query \n%s\ndoesn't use any declared variables, which is a problem. "
                "Please check your typing/syntax, and make sure you've declared the variables you'd "
                "like me to check.\n", this_query);
    char *select = NULL;

	//select part1.rowid as part1, var2.rowid as var2 ... 
	char comma = ' ';
    for (int i=0; i< edit_list[query_ct].var_ct; i++){
		xprintf(&select, "%s%c %s.rowid as %s", XN(select), comma,
				edit_list[query_ct].vars_used[i].name, edit_list[query_ct].vars_used[i].name);
		comma =',';
	}

	ud_queries = apop_text_alloc(ud_queries, (ud_queries ?  ud_queries->textsize[0] : 0) +1, 1);
	apop_text_add(ud_queries, ud_queries->textsize[0]-1, 0, "select distinct %s from %s where %s", 
							select, var_list, this_query);
    if (preed) {
        last_preed = strdup(preed);
        preed[0]='\0';
    }
    free(var_list); var_list=NULL;
    query_ct++;
    explicit_ct++;
}
