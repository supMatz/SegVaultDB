// db_api_test.c — stub temporaneo per testare solo la GUI
#include "db_api.h"
#include <stdlib.h>
#include <string.h>

bool         db_init(const char* d)          { (void)d; return true; }
QueryResult* db_execute(const char* s)       { (void)s; return NULL; }
void         db_result_free(QueryResult* r)  { free(r); }
bool         db_begin(void)                  { return true; }
bool         db_commit(void)                 { return true; }
bool         db_rollback(void)               { return true; }
bool         db_savepoint(const char* n)     { (void)n; return true; }
bool         db_rollback_to(const char* n)   { (void)n; return true; }
bool         db_use(const char* n)           { (void)n; return true; }
const char*  db_current_database(void)       { return "test_db"; }
void         db_shutdown(void)               {}
NameList*    db_list_databases(void)         { return NULL; }
NameList*    db_list_tables(const char* d)   { (void)d; return NULL; }
NameList*    db_list_views(const char* d)    { (void)d; return NULL; }
NameList*    db_list_procedures(const char* d){ (void)d; return NULL; }
NameList*    db_list_functions(const char* d) { (void)d; return NULL; }
NameList*    db_list_triggers(const char* d, const char* t) { (void)d;(void)t; return NULL; }
ColumnInfo*  db_list_columns(const char* d, const char* t, int* n) { (void)d;(void)t; *n=0; return NULL; }
void         db_columns_free(ColumnInfo* c, int n) { (void)n; free(c); }
void         db_namelist_free(NameList* nl)  { free(nl); }
void         db_free_string(char* s)         { free(s); }
SessionInfo* db_session_info(void)           { return NULL; }
void         db_session_free(SessionInfo* s) { free(s); }
QueryResult** db_execute_multi(const char* s, int* n) { (void)s; *n=0; return NULL; }
void         db_result_free_multi(QueryResult** r, int n) { (void)r;(void)n; }