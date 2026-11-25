/*
 * commands.c  -  Understands what the user typed and runs it.
 *
 * Every command works the same way:
 *   1. tokenize()     split the line into words
 *   2. open_table()   read the table's file into memory
 *   3. cmd_xxx()      do the work, then save the file if something changed
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "filedb.h"

#define MAX_TOKENS 40   /* max words in one command */
#define WORD_LEN   64   /* longer than MAX_LEN, so a too-long value is noticed, not cut */

/* Global variables, shared by all commands. A Table is about 320 KB:
   too big for a function's local variables, so it lives here instead. */
Table table;                       /* the table we are working on         */
char  tok[MAX_TOKENS][WORD_LEN];   /* the words of the command            */
int   ntok;                        /* how many words                      */
int   selected[MAX_RECORDS];       /* selected[i] = 1 if record i matches */

/* =====================================================================
 *  Character checks
 * ===================================================================== */
int is_letter(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); }
int is_digit(char ch)  { return ch >= '0' && ch <= '9'; }
int is_space(char ch)  { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'; }

/* =====================================================================
 *  Step 1: split the line into words
 *  insert students 1 "Ravi Kumar" 85   ->   insert | students | 1 | Ravi Kumar | 85
 * ===================================================================== */
void tokenize(char line[])
{
    int i = 0, len;
    ntok = 0;

    while (ntok < MAX_TOKENS) {
        while (is_space(line[i]))                  /* skip spaces */
            i++;
        if (line[i] == '\0')                       /* end of line */
            break;

        len = 0;
        if (line[i] == '"') {                      /* "quoted words" stay together */
            i++;
            while (line[i] != '\0' && line[i] != '"') {
                if (len < WORD_LEN - 1) {
                    tok[ntok][len] = line[i];
                    len++;
                }
                i++;
            }
            if (line[i] == '"')
                i++;                               /* skip the closing quote */
        } else {                                   /* a normal word ends at a space */
            while (line[i] != '\0' && !is_space(line[i])) {
                if (len < WORD_LEN - 1) {
                    tok[ntok][len] = line[i];
                    len++;
                }
                i++;
            }
        }
        tok[ntok][len] = '\0';
        ntok++;
    }
}

/* =====================================================================
 *  Small helpers
 * ===================================================================== */

/* Names: start with a letter, then letters, digits or _ (so "../" is impossible). */
int valid_name(char s[])
{
    int i;
    if (!is_letter(s[0]) || strlen(s) >= MAX_LEN)
        return 0;
    for (i = 0; s[i] != '\0'; i++)
        if (!is_letter(s[i]) && !is_digit(s[i]) && s[i] != '_')
            return 0;
    return 1;
}

/* Values: 1 to 31 characters, no comma (the comma separates values in text files). */
int valid_value(char s[])
{
    int i;
    if (s[0] == '\0') {
        printf("Error: values cannot be empty.\n");
        return 0;
    }
    if (strlen(s) >= MAX_LEN) {
        printf("Error: value \"%s\" is too long (max %d characters).\n", s, MAX_LEN - 1);
        return 0;
    }
    for (i = 0; s[i] != '\0'; i++) {
        if (s[i] == ',') {
            printf("Error: values cannot contain a comma.\n");
            return 0;
        }
    }
    return 1;
}

/* "text" -> FORMAT_TEXT, "binary" -> FORMAT_BINARY, anything else -> -1 */
int parse_format(char s[])
{
    if (strcmp(s, "text") == 0)
        return FORMAT_TEXT;
    if (strcmp(s, "binary") == 0)
        return FORMAT_BINARY;
    printf("Error: format must be 'text' or 'binary'.\n");
    return -1;
}

char *format_name(int format)
{
    if (format == FORMAT_TEXT)
        return "text";
    return "binary";
}

/* Position of a column ("marks" -> 3), or -1 if there is no such column. */
int column_index(char name[])
{
    int c;
    for (c = 0; c < table.num_cols; c++)
        if (strcmp(table.cols[c], name) == 0)
            return c;
    return -1;
}

/* Read a table into `table`. Prints an error if it does not exist. */
int open_table(char name[])
{
    if (valid_name(name) && load_table(name, &table))
        return 1;
    printf("Error: table '%s' does not exist. Type 'tables' to see all tables.\n", name);
    return 0;
}

/* =====================================================================
 *  Conditions:  <column> <op> <value>     e.g.   marks > 80
 * ===================================================================== */

/* Is this text a number, like 42, -7 or 3.5? */
int is_number(char s[])
{
    int i = 0, digits = 0, dots = 0;

    if (s[0] == '-')
        i = 1;
    while (s[i] != '\0') {
        if (is_digit(s[i]))
            digits++;
        else if (s[i] == '.')
            dots++;
        else
            return 0;
        i++;
    }
    return digits > 0 && dots <= 1;
}

/* Compare two values: as numbers if both are numbers (so 9 < 78), otherwise as text.
   Returns a negative number, 0, or a positive number, like strcmp().                 */
int compare_values(char a[], char b[])
{
    double x, y;

    if (is_number(a) && is_number(b)) {
        x = atof(a);
        y = atof(b);
        if (x < y)
            return -1;
        if (x > y)
            return 1;
        return 0;
    }
    return strcmp(a, b);
}

int valid_operator(char op[])
{
    char ops[7][10] = { "=", "!=", ">", "<", ">=", "<=", "contains" };
    int i;
    for (i = 0; i < 7; i++)
        if (strcmp(op, ops[i]) == 0)
            return 1;
    return 0;
}

/* Is "value op target" true?   e.g.  matches("85", ">", "80")  ->  1 */
int matches(char value[], char op[], char target[])
{
    int r;

    if (strcmp(op, "contains") == 0)
        return strstr(value, target) != NULL;      /* is target inside value? */

    r = compare_values(value, target);
    if (strcmp(op, "=") == 0)  return r == 0;
    if (strcmp(op, "!=") == 0) return r != 0;
    if (strcmp(op, ">") == 0)  return r > 0;
    if (strcmp(op, "<") == 0)  return r < 0;
    if (strcmp(op, ">=") == 0) return r >= 0;
    return r <= 0;                                 /* "<=" */
}

/*
 * Mark the records that match the condition written in words tok[w], tok[w+1], tok[w+2].
 * Linear search: every record is checked one by one.
 * Returns how many matched, or -1 if the column or operator is wrong.
 */
int select_where(int w)
{
    int col = column_index(tok[w]);
    int i, count = 0;

    if (col < 0) {
        printf("Error: no column named '%s' in table '%s'.\n", tok[w], table.name);
        return -1;
    }
    if (!valid_operator(tok[w + 1])) {
        printf("Error: unknown operator '%s'. Use = != > < >= <= contains\n", tok[w + 1]);
        return -1;
    }
    for (i = 0; i < table.num_records; i++) {
        selected[i] = matches(table.records[i].values[col], tok[w + 1], tok[w + 2]);
        count = count + selected[i];
    }
    return count;
}

void select_all(void)
{
    int i;
    for (i = 0; i < table.num_records; i++)
        selected[i] = 1;
}

/* =====================================================================
 *  Printing the selected records as a box
 * ===================================================================== */
void print_border(int width[])
{
    int c, i;
    for (c = 0; c < table.num_cols; c++) {
        printf("+");
        for (i = 0; i < width[c] + 2; i++)
            printf("-");
    }
    printf("+\n");
}

/* Print "| value" and then spaces, so every cell in a column has the same width. */
void print_cell(char s[], int width)
{
    int i;
    printf("| %s", s);
    for (i = strlen(s); i < width + 1; i++)
        printf(" ");
}

void print_records(void)
{
    int width[MAX_COLS];
    int c, i, len, shown = 0;

    /* each column is as wide as its longest value */
    for (c = 0; c < table.num_cols; c++) {
        width[c] = strlen(table.cols[c]);
        for (i = 0; i < table.num_records; i++) {
            len = strlen(table.records[i].values[c]);
            if (selected[i] && len > width[c])
                width[c] = len;
        }
    }

    print_border(width);
    for (c = 0; c < table.num_cols; c++)
        print_cell(table.cols[c], width[c]);
    printf("|\n");
    print_border(width);

    for (i = 0; i < table.num_records; i++) {
        if (!selected[i])
            continue;
        for (c = 0; c < table.num_cols; c++)
            print_cell(table.records[i].values[c], width[c]);
        printf("|\n");
        shown++;
    }
    if (shown > 0)
        print_border(width);
    printf("%d record(s) found.\n", shown);
}

/* =====================================================================
 *  The commands
 * ===================================================================== */

void cmd_help(void)
{
    printf("FileDB commands:\n");
    printf("  create  <table> <text|binary> <col1> <col2> ...   make a new table\n");
    printf("  insert  <table> <value1> <value2> ...             add a record\n");
    printf("  show    <table>                                   show all records\n");
    printf("  find    <table> <col> <op> <value>                search records\n");
    printf("  sort    <table> <col> [asc|desc]                  show records sorted\n");
    printf("  count   <table> [<col> <op> <value>]              count records\n");
    printf("  update  <table> set <col> <value> where <col> <op> <value>\n");
    printf("  delete  <table> where <col> <op> <value>          remove records\n");
    printf("  tables                                            list all tables\n");
    printf("  describe <table>                                  columns, format, size\n");
    printf("  compare <table>                                   text vs binary file size\n");
    printf("  convert <table> <text|binary>                     change storage format\n");
    printf("  drop    <table>                                   delete a table\n");
    printf("  help | exit\n");
    printf("Operators: =  !=  >  <  >=  <=  contains\n");
    printf("Tip: use \"double quotes\" for values with spaces.\n");
}

/* create students text id name marks */
void cmd_create(void)
{
    int format, c;

    if (ntok < 4) {
        printf("Usage: create <table> <text|binary> <col1> <col2> ...\n");
        return;
    }
    if (!valid_name(tok[1])) {
        printf("Error: a table name must start with a letter and use only letters, digits and _\n");
        return;
    }
    if (find_table(tok[1], &format)) {
        printf("Error: table '%s' already exists.\n", tok[1]);
        return;
    }
    format = parse_format(tok[2]);
    if (format < 0)
        return;
    if (ntok - 3 > MAX_COLS) {
        printf("Error: at most %d columns.\n", MAX_COLS);
        return;
    }

    copy_value(table.name, tok[1]);
    table.format = format;
    table.num_cols = ntok - 3;
    table.num_records = 0;
    for (c = 0; c < table.num_cols; c++) {
        if (!valid_name(tok[c + 3])) {
            printf("Error: bad column name '%s'.\n", tok[c + 3]);
            return;
        }
        copy_value(table.cols[c], tok[c + 3]);
    }

    if (!save_table(&table)) {
        printf("Error: could not create the file.\n");
        return;
    }
    add_to_list(table.name);
    printf("OK: table '%s' created with %d column(s), stored as %s.\n",
           table.name, table.num_cols, format_name(format));
}

/* insert students 1 Ravi 85 */
void cmd_insert(void)
{
    Record r;
    int c;

    if (ntok < 2) {
        printf("Usage: insert <table> <value1> <value2> ...\n");
        return;
    }
    if (!open_table(tok[1]))
        return;
    if (ntok - 2 != table.num_cols) {
        printf("Error: table '%s' has %d column(s) but you gave %d value(s).\n",
               table.name, table.num_cols, ntok - 2);
        return;
    }
    if (table.num_records >= MAX_RECORDS) {
        printf("Error: table is full (%d records).\n", MAX_RECORDS);
        return;
    }

    for (c = 0; c < table.num_cols; c++) {
        if (!valid_value(tok[c + 2]))
            return;
        copy_value(r.values[c], tok[c + 2]);
    }

    if (!append_record(&table, &r)) {
        printf("Error: could not write to the file.\n");
        return;
    }
    printf("OK: 1 record inserted into '%s' (appended to the end of the file).\n", table.name);
}

/* show students */
void cmd_show(void)
{
    if (ntok != 2) {
        printf("Usage: show <table>\n");
        return;
    }
    if (!open_table(tok[1]))
        return;
    select_all();
    print_records();
}

/* find students marks > 80 */
void cmd_find(void)
{
    if (ntok != 5) {
        printf("Usage: find <table> <col> <op> <value>\n");
        return;
    }
    if (!open_table(tok[1]))
        return;
    if (select_where(2) < 0)
        return;
    print_records();
}

/* sort students marks desc   (sorts the copy in memory, the file is not changed) */
void cmd_sort(void)
{
    int col, desc, i, j, r;
    Record temp;

    if (ntok != 3 && ntok != 4) {
        printf("Usage: sort <table> <col> [asc|desc]\n");
        return;
    }
    if (!open_table(tok[1]))
        return;
    col = column_index(tok[2]);
    if (col < 0) {
        printf("Error: no column named '%s'.\n", tok[2]);
        return;
    }
    desc = 0;
    if (ntok == 4 && strcmp(tok[3], "desc") == 0)
        desc = 1;

    /* Bubble sort: swap neighbours that are in the wrong order. */
    for (i = 0; i < table.num_records - 1; i++) {
        for (j = 0; j < table.num_records - 1 - i; j++) {
            r = compare_values(table.records[j].values[col], table.records[j + 1].values[col]);
            if (desc)
                r = -r;
            if (r > 0) {
                temp = table.records[j];
                table.records[j] = table.records[j + 1];
                table.records[j + 1] = temp;
            }
        }
    }
    select_all();
    print_records();
}

/* count students   or   count students dept = CSE */
void cmd_count(void)
{
    int n;

    if (ntok != 2 && ntok != 5) {
        printf("Usage: count <table> [<col> <op> <value>]\n");
        return;
    }
    if (!open_table(tok[1]))
        return;

    if (ntok == 2)
        n = table.num_records;
    else
        n = select_where(2);
    if (n >= 0)
        printf("Count: %d\n", n);
}

/* update students set marks 95 where id = 3 */
void cmd_update(void)
{
    int col, i, n;

    if (ntok != 9 || strcmp(tok[2], "set") != 0 || strcmp(tok[5], "where") != 0) {
        printf("Usage: update <table> set <col> <value> where <col> <op> <value>\n");
        return;
    }
    if (!open_table(tok[1]))
        return;
    col = column_index(tok[3]);
    if (col < 0) {
        printf("Error: no column named '%s'.\n", tok[3]);
        return;
    }
    if (!valid_value(tok[4]))
        return;
    n = select_where(6);
    if (n < 0)
        return;

    for (i = 0; i < table.num_records; i++) {
        if (!selected[i])
            continue;
        copy_value(table.records[i].values[col], tok[4]);   /* clears the old value too */
        if (table.format == FORMAT_BINARY)
            overwrite_record(&table, i);                     /* in place */
    }
    if (table.format == FORMAT_TEXT && n > 0)
        save_table(&table);                                  /* rewrite all */

    if (n == 0)
        printf("OK: 0 record(s) updated.\n");
    else if (table.format == FORMAT_BINARY)
        printf("OK: %d record(s) updated in place using fseek().\n", n);
    else
        printf("OK: %d record(s) updated (whole text file rewritten).\n", n);
}

/* delete students where dept = MECH */
void cmd_delete(void)
{
    int i, kept = 0, n;

    if (ntok != 6 || strcmp(tok[2], "where") != 0) {
        printf("Usage: delete <table> where <col> <op> <value>\n");
        return;
    }
    if (!open_table(tok[1]))
        return;
    n = select_where(3);
    if (n < 0)
        return;

    /* keep the records that did NOT match, moving them forward to close the gaps */
    for (i = 0; i < table.num_records; i++) {
        if (!selected[i]) {
            table.records[kept] = table.records[i];
            kept++;
        }
    }
    table.num_records = kept;

    if (n > 0)
        save_table(&table);
    printf("OK: %d record(s) deleted.\n", n);
}

/* tables   (reads the names from data/tables.lst) */
void cmd_tables(void)
{
    char name[64], path[128];
    int found = 0;
    FILE *f = fopen(LIST_FILE, "r");

    if (f != NULL) {
        while (fgets(name, sizeof(name), f) != NULL) {
            remove_newline(name);
            if (!load_table(name, &table))
                continue;
            make_path(path, name, table.format);
            printf("  %-20s %-7s %4d record(s)  %6ld bytes\n",
                   name, format_name(table.format), table.num_records, file_size(path));
            found++;
        }
        fclose(f);
    }
    if (found == 0)
        printf("No tables yet. Try: create students text id name marks\n");
}

/* describe books */
void cmd_describe(void)
{
    char path[128];
    int c;

    if (ntok != 2) {
        printf("Usage: describe <table>\n");
        return;
    }
    if (!open_table(tok[1]))
        return;
    make_path(path, table.name, table.format);

    printf("Table   : %s\n", table.name);
    printf("File    : %s\n", path);
    printf("Format  : %s\n", format_name(table.format));
    printf("Records : %d\n", table.num_records);
    printf("Size    : %ld bytes\n", file_size(path));
    printf("Columns :");
    for (c = 0; c < table.num_cols; c++)
        printf(" %s", table.cols[c]);
    printf("\n");
    if (table.format == FORMAT_BINARY)
        printf("Layout  : header %ld bytes + %ld bytes per record\n",
               binary_header_size(table.num_cols), binary_record_size(table.num_cols));
}

/* compare students   (saves the same data both ways and measures both files) */
void cmd_compare(void)
{
    char text_path[128], bin_path[128];
    long text_size, bin_size;

    if (ntok != 2) {
        printf("Usage: compare <table>\n");
        return;
    }
    if (!open_table(tok[1]))
        return;

    sprintf(text_path, "%s/_compare.txt", DATA_DIR);
    sprintf(bin_path, "%s/_compare.bin", DATA_DIR);
    save_table_as(&table, text_path, FORMAT_TEXT);
    save_table_as(&table, bin_path, FORMAT_BINARY);
    text_size = file_size(text_path);
    bin_size = file_size(bin_path);
    remove(text_path);
    remove(bin_path);

    printf("Same %d record(s) stored two ways:\n", table.num_records);
    printf("  text   : %6ld bytes  (only the characters needed; easy to read)\n", text_size);
    printf("  binary : %6ld bytes  (every record is %ld bytes; fast to jump to)\n",
           bin_size, binary_record_size(table.num_cols));
}

/* convert students binary   (write the new file, then delete the old one) */
void cmd_convert(void)
{
    char old_path[128];
    int format;

    if (ntok != 3) {
        printf("Usage: convert <table> <text|binary>\n");
        return;
    }
    if (!open_table(tok[1]))
        return;
    format = parse_format(tok[2]);
    if (format < 0)
        return;
    if (format == table.format) {
        printf("Table is already %s.\n", tok[2]);
        return;
    }

    make_path(old_path, table.name, table.format);
    table.format = format;
    if (!save_table(&table)) {
        printf("Error: could not write the new file.\n");
        return;
    }
    remove(old_path);
    printf("OK: '%s' is now stored as %s.\n", table.name, tok[2]);
}

/* drop students   (deletes the table's file and its name in the list) */
void cmd_drop(void)
{
    char path[128];
    int format;

    if (ntok != 2) {
        printf("Usage: drop <table>\n");
        return;
    }
    if (!valid_name(tok[1]) || !find_table(tok[1], &format)) {
        printf("Error: table '%s' does not exist.\n", tok[1]);
        return;
    }
    make_path(path, tok[1], format);
    remove(path);
    remove_from_list(tok[1]);
    printf("OK: table '%s' deleted.\n", tok[1]);
}

/* =====================================================================
 *  execute(): runs one line. Used by main.c and by the website.
 *  Returns 1 when the user typed "exit".
 * ===================================================================== */
int execute(char line[])
{
    tokenize(line);
    if (ntok == 0)
        return 0;

    if (strcmp(tok[0], "exit") == 0)
        return 1;
    else if (strcmp(tok[0], "help") == 0)     cmd_help();
    else if (strcmp(tok[0], "create") == 0)   cmd_create();
    else if (strcmp(tok[0], "insert") == 0)   cmd_insert();
    else if (strcmp(tok[0], "show") == 0)     cmd_show();
    else if (strcmp(tok[0], "find") == 0)     cmd_find();
    else if (strcmp(tok[0], "sort") == 0)     cmd_sort();
    else if (strcmp(tok[0], "count") == 0)    cmd_count();
    else if (strcmp(tok[0], "update") == 0)   cmd_update();
    else if (strcmp(tok[0], "delete") == 0)   cmd_delete();
    else if (strcmp(tok[0], "tables") == 0)   cmd_tables();
    else if (strcmp(tok[0], "describe") == 0) cmd_describe();
    else if (strcmp(tok[0], "compare") == 0)  cmd_compare();
    else if (strcmp(tok[0], "convert") == 0)  cmd_convert();
    else if (strcmp(tok[0], "drop") == 0)     cmd_drop();
    else
        printf("Error: unknown command '%s'. Type 'help' to see all commands.\n", tok[0]);

    fflush(stdout);
    return 0;
}
