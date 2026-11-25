/*
 * filedb.h  -  Shared definitions for FileDB.
 *
 * Every .c file includes this header, so they all agree on
 * what a Table and a Record look like.
 */
#ifndef FILEDB_H
#define FILEDB_H

/* ---------- Limits (fixed sizes keep the code simple) ---------- */
#define MAX_COLS     10     /* max columns in one table               */
#define MAX_RECORDS  1000   /* max records (rows) in one table        */
#define MAX_LEN      32     /* max length of one value, incl. '\0'    */
#define MAX_TABLES   100    /* max tables in the list of tables       */
#define DATA_DIR     "data"             /* folder where all table files are kept */
#define LIST_FILE    "data/tables.lst"  /* text file with one table name per line */

/* ---------- Storage formats ---------- */
#define FORMAT_TEXT    0    /* saved as  data/<name>.txt  */
#define FORMAT_BINARY  1    /* saved as  data/<name>.bin  */

/* One record (one row). Every value is stored as a string. */
typedef struct {
    char values[MAX_COLS][MAX_LEN];
} Record;

/* One whole table, loaded into memory from its file. */
typedef struct {
    char   name[MAX_LEN];
    int    format;                  /* FORMAT_TEXT or FORMAT_BINARY */
    int    num_cols;
    char   cols[MAX_COLS][MAX_LEN]; /* column names                 */
    int    num_records;
    Record records[MAX_RECORDS];
} Table;

/* ---------- storage.c : everything that touches files ---------- */
void copy_value(char dest[], char src[]);
void remove_newline(char s[]);
void make_path(char path[], char name[], int format);
long file_size(char path[]);
int  find_table(char name[], int *format);
long binary_header_size(int num_cols);
long binary_record_size(int num_cols);
int  load_table(char name[], Table *t);
int  save_table_as(Table *t, char path[], int format);
int  save_table(Table *t);
int  append_record(Table *t, Record *r);
int  overwrite_record(Table *t, int index);
void add_to_list(char name[]);
void remove_from_list(char name[]);

/* ---------- commands.c : understanding and running commands ---------- */
int  execute(char line[]);   /* returns 1 when the user typed "exit" */

#endif
