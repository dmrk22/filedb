/*
 * commands.c  -  Understands what the user typed and runs it.
 *
 * Every command follows the same 3 steps:
 *   1. TOKENIZE : split the line into words ("tokens")
 *   2. LOAD     : read the table's file into memory
 *   3. ACT      : search / change the records, then save back to the file
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include "filedb.h"

/* The table we are working on. It is 'static' (global) because it is
 * about 320 KB - too big to safely put on the function call stack.     */
static Table table;

/* The words of the current command, e.g. {"find","students","marks",">","80"} */
static char tok[MAX_TOKENS][64];
static int  ntok;

/* A condition such as:  marks > 80  */
typedef struct {
    int  col;              /* which column to look at  */
    char op[16];           /* =  !=  >  <  >=  <=  contains */
    char value[MAX_LEN];   /* the value to compare with */
} Condition;

/* =====================================================================
 *  STEP 1 : TOKENIZER
 *  Splits a line into words. Text in "double quotes" stays one word,
 *  so  insert students 1 "Ravi Kumar" 85  gives 5 words.
 * ===================================================================== */
static void tokenize(const char *line)
{
    const char *p = line;
    ntok = 0;

    while (*p != '\0' && ntok < MAX_TOKENS) {
        int len = 0;

        while (isspace((unsigned char)*p)) p++;        /* skip spaces */
        if (*p == '\0') break;

        if (*p == '"') {                               /* quoted word */
            p++;
            while (*p != '\0' && *p != '"') {
                if (len < 63) tok[ntok][len++] = *p;
                p++;
            }
            if (*p == '"') p++;
        } else {                                       /* normal word */
            while (*p != '\0' && !isspace((unsigned char)*p)) {
                if (len < 63) tok[ntok][len++] = *p;
                p++;
            }
        }
        tok[ntok][len] = '\0';
        ntok++;
    }
}

/* =====================================================================
 *  SMALL HELPERS
 * ===================================================================== */

/* Table/column names: start with a letter, then letters, digits and _ only
 * (so nobody can type "../" to reach files outside the data folder).     */
static int valid_name(const char *s)
{
    int i;
    if (!isalpha((unsigned char)s[0]) || strlen(s) >= MAX_LEN) return 0;
    for (i = 0; s[i] != '\0'; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '_') return 0;
    return 1;
}

/* Values must fit in MAX_LEN and must not contain a comma
 * (the comma separates values in the text file).            */
static int valid_value(const char *s)
{
    if (strlen(s) >= MAX_LEN) {
        printf("Error: value \"%s\" is too long (max %d characters).\n", s, MAX_LEN - 1);
        return 0;
    }
    if (strchr(s, ',') != NULL) {
        printf("Error: values cannot contain a comma.\n");
        return 0;
    }
    return 1;
}

/* Returns the position of a column by its name, or -1 if not found. */
static int column_index(Table *t, const char *name)
{
    int i;
    for (i = 0; i < t->num_cols; i++)
        if (strcmp(t->cols[i], name) == 0) return i;
    return -1;
}

/* Returns 1 if the string is a number like 42, -7 or 3.5 */
static int is_number(const char *s)
{
    char *end;
    if (s[0] == '\0') return 0;
    strtod(s, &end);
    return *end == '\0';
}

/* Compares two values. Numbers are compared as numbers (so 9 < 10),
 * everything else alphabetically. Returns <0, 0 or >0 like strcmp(). */
static int compare_values(const char *a, const char *b)
{
    if (is_number(a) && is_number(b)) {
        double x = atof(a), y = atof(b);
        return (x > y) - (x < y);
    }
    return strcmp(a, b);
}

/* Does 'value' satisfy the condition? */
static int matches(const char *value, Condition *c)
{
    int r;
    if (strcmp(c->op, "contains") == 0) return strstr(value, c->value) != NULL;

    r = compare_values(value, c->value);
    if (strcmp(c->op, "=")  == 0) return r == 0;
    if (strcmp(c->op, "!=") == 0) return r != 0;
    if (strcmp(c->op, ">")  == 0) return r >  0;
    if (strcmp(c->op, "<")  == 0) return r <  0;
    if (strcmp(c->op, ">=") == 0) return r >= 0;
    if (strcmp(c->op, "<=") == 0) return r <= 0;
    return 0;
}

/* Reads "<column> <op> <value>" from the tokens starting at 'start'. */
static int parse_condition(int start, Condition *c)
{
    const char *ops[] = { "=", "!=", ">", "<", ">=", "<=", "contains" };
    int i, ok = 0;

    c->col = column_index(&table, tok[start]);
    if (c->col < 0) {
        printf("Error: no column named '%s' in table '%s'.\n", tok[start], table.name);
        return 0;
    }
    for (i = 0; i < 7; i++)
        if (strcmp(tok[start + 1], ops[i]) == 0) ok = 1;
    if (!ok) {
        printf("Error: unknown operator '%s'. Use = != > < >= <= contains\n", tok[start + 1]);
        return 0;
    }
    strcpy(c->op, tok[start + 1]);
    snprintf(c->value, MAX_LEN, "%s", tok[start + 2]);
    return 1;
}

/* Loads a table into the global 'table'. Prints an error if missing. */
static int open_table(const char *name)
{
    if (!valid_name(name) || !load_table(name, &table)) {
        printf("Error: table '%s' does not exist. Type 'tables' to see all tables.\n", name);
        return 0;
    }
    return 1;
}

static const char *format_name(int format)
{
    return format == FORMAT_TEXT ? "text" : "binary";
}

/* Prints a line like  +------+--------+  */
static void print_border(int widths[], int n)
{
    int c, i;
    for (c = 0; c < n; c++) {
        printf("+");
        for (i = 0; i < widths[c] + 2; i++) printf("-");
    }
    printf("+\n");
}

/* Prints the records whose show[i] is 1, as a neat box table. */
static void print_records(int show[])
{
    int widths[MAX_COLS];
    int c, i, shown = 0;

    /* Each column is as wide as its longest value. */
    for (c = 0; c < table.num_cols; c++) {
        widths[c] = (int)strlen(table.cols[c]);
        for (i = 0; i < table.num_records; i++)
            if (show[i] && (int)strlen(table.records[i].values[c]) > widths[c])
                widths[c] = (int)strlen(table.records[i].values[c]);
    }

    print_border(widths, table.num_cols);
    for (c = 0; c < table.num_cols; c++) printf("| %-*s ", widths[c], table.cols[c]);
    printf("|\n");
    print_border(widths, table.num_cols);

    for (i = 0; i < table.num_records; i++) {
        if (!show[i]) continue;
        for (c = 0; c < table.num_cols; c++)
            printf("| %-*s ", widths[c], table.records[i].values[c]);
        printf("|\n");
        shown++;
    }
    if (shown > 0) print_border(widths, table.num_cols);
    printf("%d record(s) found.\n", shown);
}

/* =====================================================================
 *  THE COMMANDS
 * ===================================================================== */

static void cmd_help(void)
{
    printf("FileDB commands:\n"
           "  create  <table> <text|binary> <col1> <col2> ...   make a new table\n"
           "  insert  <table> <value1> <value2> ...             add a record\n"
           "  show    <table>                                   show all records\n"
           "  find    <table> <col> <op> <value>                search records\n"
           "  sort    <table> <col> [asc|desc]                  show records sorted\n"
           "  count   <table> [<col> <op> <value>]              count records\n"
           "  update  <table> set <col> <value> where <col> <op> <value>\n"
           "  delete  <table> where <col> <op> <value>          remove records\n"
           "  tables                                            list all tables\n"
           "  describe <table>                                  columns, format, size\n"
           "  compare <table>                                   text vs binary file size\n"
           "  convert <table> <text|binary>                     change storage format\n"
           "  drop    <table>                                   delete a table\n"
           "  help | exit\n"
           "Operators: =  !=  >  <  >=  <=  contains\n"
           "Tip: use \"double quotes\" for values with spaces.\n");
}

static void cmd_create(void)
{
    int format, i;

    if (ntok < 4) { printf("Usage: create <table> <text|binary> <col1> <col2> ...\n"); return; }
    if (!valid_name(tok[1])) {
        printf("Error: a table name must start with a letter and use only letters, digits and _\n");
        return;
    }
    if (find_table(tok[1], &format)) { printf("Error: table '%s' already exists.\n", tok[1]); return; }

    if      (strcmp(tok[2], "text")   == 0) format = FORMAT_TEXT;
    else if (strcmp(tok[2], "binary") == 0) format = FORMAT_BINARY;
    else { printf("Error: format must be 'text' or 'binary'.\n"); return; }

    if (ntok - 3 > MAX_COLS) { printf("Error: at most %d columns.\n", MAX_COLS); return; }

    memset(&table, 0, sizeof(Table));
    strcpy(table.name, tok[1]);
    table.format = format;
    table.num_cols = ntok - 3;
    for (i = 0; i < table.num_cols; i++) {
        if (!valid_name(tok[i + 3])) { printf("Error: bad column name '%s'.\n", tok[i + 3]); return; }
        strcpy(table.cols[i], tok[i + 3]);
    }

    if (!save_table(&table)) { printf("Error: could not create the file.\n"); return; }
    printf("OK: table '%s' created with %d column(s), stored as %s.\n",
           table.name, table.num_cols, format_name(format));
}

static void cmd_insert(void)
{
    Record r;
    int c;

    if (ntok < 2) { printf("Usage: insert <table> <value1> <value2> ...\n"); return; }
    if (!open_table(tok[1])) return;

    if (ntok - 2 != table.num_cols) {
        printf("Error: table '%s' has %d column(s) but you gave %d value(s).\n",
               table.name, table.num_cols, ntok - 2);
        return;
    }
    if (table.num_records >= MAX_RECORDS) { printf("Error: table is full (%d records).\n", MAX_RECORDS); return; }

    memset(&r, 0, sizeof r);    /* zero-fill, so no garbage bytes reach the file */
    for (c = 0; c < table.num_cols; c++) {
        if (!valid_value(tok[c + 2])) return;
        strcpy(r.values[c], tok[c + 2]);
    }

    if (!append_record(&table, &r)) { printf("Error: could not write to the file.\n"); return; }
    printf("OK: 1 record inserted into '%s' (appended to the end of the file).\n", table.name);
}

static void cmd_show(void)
{
    int show[MAX_RECORDS], i;
    if (ntok != 2) { printf("Usage: show <table>\n"); return; }
    if (!open_table(tok[1])) return;
    for (i = 0; i < table.num_records; i++) show[i] = 1;
    print_records(show);
}

static void cmd_find(void)
{
    int show[MAX_RECORDS], i;
    Condition cond;

    if (ntok != 5) { printf("Usage: find <table> <col> <op> <value>\n"); return; }
    if (!open_table(tok[1]) || !parse_condition(2, &cond)) return;

    /* Linear search: check every record one by one. */
    for (i = 0; i < table.num_records; i++)
        show[i] = matches(table.records[i].values[cond.col], &cond);
    print_records(show);
}

static void cmd_sort(void)
{
    int show[MAX_RECORDS], col, desc = 0, i, j;
    Record temp;

    if (ntok < 3 || ntok > 4) { printf("Usage: sort <table> <col> [asc|desc]\n"); return; }
    if (!open_table(tok[1])) return;
    col = column_index(&table, tok[2]);
    if (col < 0) { printf("Error: no column named '%s'.\n", tok[2]); return; }
    if (ntok == 4) desc = (strcmp(tok[3], "desc") == 0);

    /* Bubble sort: keep swapping neighbours that are in the wrong order.
     * Only the copy in memory is sorted - the file is not changed.     */
    for (i = 0; i < table.num_records - 1; i++) {
        for (j = 0; j < table.num_records - 1 - i; j++) {
            int r = compare_values(table.records[j].values[col], table.records[j + 1].values[col]);
            if ((!desc && r > 0) || (desc && r < 0)) {
                temp = table.records[j];
                table.records[j] = table.records[j + 1];
                table.records[j + 1] = temp;
            }
        }
    }
    for (i = 0; i < table.num_records; i++) show[i] = 1;
    print_records(show);
}

static void cmd_count(void)
{
    Condition cond;
    int i, n = 0;

    if (ntok != 2 && ntok != 5) { printf("Usage: count <table> [<col> <op> <value>]\n"); return; }
    if (!open_table(tok[1])) return;

    if (ntok == 2) {
        n = table.num_records;
    } else {
        if (!parse_condition(2, &cond)) return;
        for (i = 0; i < table.num_records; i++)
            if (matches(table.records[i].values[cond.col], &cond)) n++;
    }
    printf("Count: %d\n", n);
}

static void cmd_update(void)
{
    Condition cond;
    int set_col, i, changed = 0;

    if (ntok != 9 || strcmp(tok[2], "set") != 0 || strcmp(tok[5], "where") != 0) {
        printf("Usage: update <table> set <col> <value> where <col> <op> <value>\n");
        return;
    }
    if (!open_table(tok[1])) return;
    set_col = column_index(&table, tok[3]);
    if (set_col < 0) { printf("Error: no column named '%s'.\n", tok[3]); return; }
    if (!valid_value(tok[4]) || !parse_condition(6, &cond)) return;

    for (i = 0; i < table.num_records; i++) {
        if (matches(table.records[i].values[cond.col], &cond)) {
            memset(table.records[i].values[set_col], 0, MAX_LEN);  /* clear old bytes */
            strcpy(table.records[i].values[set_col], tok[4]);
            changed++;
            /* Binary: fix just this record inside the file (fseek). */
            if (table.format == FORMAT_BINARY) overwrite_record(&table, i);
        }
    }

    /* Text: lines have different lengths, so we rewrite the whole file. */
    if (table.format == FORMAT_TEXT && changed > 0) save_table(&table);

    printf("OK: %d record(s) updated", changed);
    if (changed > 0)
        printf(table.format == FORMAT_BINARY ? " in place using fseek().\n"
                                             : " (whole text file rewritten).\n");
    else
        printf(".\n");
}

static void cmd_delete(void)
{
    Condition cond;
    int i, kept = 0, removed;

    if (ntok != 6 || strcmp(tok[2], "where") != 0) {
        printf("Usage: delete <table> where <col> <op> <value>\n");
        return;
    }
    if (!open_table(tok[1]) || !parse_condition(3, &cond)) return;

    /* Keep only the records that do NOT match, sliding them forward. */
    for (i = 0; i < table.num_records; i++)
        if (!matches(table.records[i].values[cond.col], &cond))
            table.records[kept++] = table.records[i];

    removed = table.num_records - kept;
    table.num_records = kept;
    if (removed > 0) save_table(&table);
    printf("OK: %d record(s) deleted.\n", removed);
}

static void cmd_tables(void)
{
    DIR *dir = opendir(DATA_DIR);
    struct dirent *entry;
    int found = 0;

    if (dir == NULL) { printf("No tables yet.\n"); return; }

    while ((entry = readdir(dir)) != NULL) {
        char name[MAX_LEN], path[128];
        char *dot = strrchr(entry->d_name, '.');
        int len;

        if (dot == NULL || (strcmp(dot, ".txt") != 0 && strcmp(dot, ".bin") != 0)) continue;
        len = (int)(dot - entry->d_name);
        if (len <= 0 || len >= MAX_LEN) continue;
        strncpy(name, entry->d_name, len);
        name[len] = '\0';
        if (!load_table(name, &table)) continue;

        make_path(path, name, table.format);
        printf("  %-20s %-7s %4d record(s)  %6ld bytes\n",
               name, format_name(table.format), table.num_records, file_size(path));
        found++;
    }
    closedir(dir);
    if (found == 0) printf("No tables yet. Try: create students text id name marks\n");
}

static void cmd_describe(void)
{
    char path[128];
    int c;

    if (ntok != 2) { printf("Usage: describe <table>\n"); return; }
    if (!open_table(tok[1])) return;
    make_path(path, table.name, table.format);

    printf("Table   : %s\n", table.name);
    printf("File    : %s\n", path);
    printf("Format  : %s\n", format_name(table.format));
    printf("Records : %d\n", table.num_records);
    printf("Size    : %ld bytes\n", file_size(path));
    printf("Columns :");
    for (c = 0; c < table.num_cols; c++) printf(" %s", table.cols[c]);
    printf("\n");
    if (table.format == FORMAT_BINARY)
        printf("Layout  : header %ld bytes + %ld bytes per record\n",
               binary_header_size(table.num_cols), binary_record_size(table.num_cols));
}

static void cmd_compare(void)
{
    char text_path[128], bin_path[128];
    long text_size, bin_size;

    if (ntok != 2) { printf("Usage: compare <table>\n"); return; }
    if (!open_table(tok[1])) return;

    /* Save the same data in BOTH formats to temporary files and measure them. */
    sprintf(text_path, "%s/_compare.txt", DATA_DIR);
    sprintf(bin_path,  "%s/_compare.bin", DATA_DIR);
    save_table_as(&table, text_path, FORMAT_TEXT);
    save_table_as(&table, bin_path,  FORMAT_BINARY);
    text_size = file_size(text_path);
    bin_size  = file_size(bin_path);
    remove(text_path);
    remove(bin_path);

    printf("Same %d record(s) stored two ways:\n", table.num_records);
    printf("  text   : %6ld bytes  (only the characters needed; easy to read)\n", text_size);
    printf("  binary : %6ld bytes  (every record is %ld bytes; fast to jump to)\n",
           bin_size, binary_record_size(table.num_cols));
}

static void cmd_convert(void)
{
    char old_path[128];
    int new_format;

    if (ntok != 3) { printf("Usage: convert <table> <text|binary>\n"); return; }
    if (!open_table(tok[1])) return;

    if      (strcmp(tok[2], "text")   == 0) new_format = FORMAT_TEXT;
    else if (strcmp(tok[2], "binary") == 0) new_format = FORMAT_BINARY;
    else { printf("Error: format must be 'text' or 'binary'.\n"); return; }

    if (new_format == table.format) { printf("Table is already %s.\n", tok[2]); return; }

    make_path(old_path, table.name, table.format);
    table.format = new_format;
    if (!save_table(&table)) { printf("Error: could not write the new file.\n"); return; }
    remove(old_path);
    printf("OK: '%s' is now stored as %s.\n", table.name, tok[2]);
}

static void cmd_drop(void)
{
    char path[128];
    int format;

    if (ntok != 2) { printf("Usage: drop <table>\n"); return; }
    if (!valid_name(tok[1]) || !find_table(tok[1], &format)) {
        printf("Error: table '%s' does not exist.\n", tok[1]);
        return;
    }
    make_path(path, tok[1], format);
    remove(path);
    printf("OK: table '%s' deleted.\n", tok[1]);
}

/* =====================================================================
 *  execute() - the single entry point. main.c (terminal) and the
 *  website both send every line the user types here.
 * ===================================================================== */
int execute(const char *line)
{
    tokenize(line);
    if (ntok == 0) return 0;

    if      (strcmp(tok[0], "exit")     == 0) return 1;
    else if (strcmp(tok[0], "help")     == 0) cmd_help();
    else if (strcmp(tok[0], "create")   == 0) cmd_create();
    else if (strcmp(tok[0], "insert")   == 0) cmd_insert();
    else if (strcmp(tok[0], "show")     == 0) cmd_show();
    else if (strcmp(tok[0], "find")     == 0) cmd_find();
    else if (strcmp(tok[0], "sort")     == 0) cmd_sort();
    else if (strcmp(tok[0], "count")    == 0) cmd_count();
    else if (strcmp(tok[0], "update")   == 0) cmd_update();
    else if (strcmp(tok[0], "delete")   == 0) cmd_delete();
    else if (strcmp(tok[0], "tables")   == 0) cmd_tables();
    else if (strcmp(tok[0], "describe") == 0) cmd_describe();
    else if (strcmp(tok[0], "compare")  == 0) cmd_compare();
    else if (strcmp(tok[0], "convert")  == 0) cmd_convert();
    else if (strcmp(tok[0], "drop")     == 0) cmd_drop();
    else printf("Error: unknown command '%s'. Type 'help' to see all commands.\n", tok[0]);

    fflush(stdout);
    return 0;
}
