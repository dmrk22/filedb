/*
 * storage.c  -  Reading and writing table files.
 *
 * Each table is one file in the data/ folder, in one of two formats:
 *
 *   TEXT   data/<name>.txt     first line = column names, then one line per record
 *          id,name,marks
 *          1,Ravi,85
 *
 *   BINARY data/<name>.bin     "FDB1" | number of columns | column names | records...
 *          Every value takes exactly MAX_LEN (32) bytes, so every record has the
 *          same size. That lets us jump straight to record N with fseek().
 *
 * data/tables.lst is a small text file with the name of every table, one per line.
 */
#include <stdio.h>
#include <string.h>
#include "filedb.h"

/* ===================== Small helpers ===================== */

/* Copy a value into a 32-byte slot. All 32 bytes are cleared first,
   so no old letters are left behind in the slot.                      */
void copy_value(char dest[], char src[])
{
    int i;
    for (i = 0; i < MAX_LEN; i++)
        dest[i] = '\0';
    for (i = 0; src[i] != '\0' && i < MAX_LEN - 1; i++)
        dest[i] = src[i];
}

/* fgets() keeps the Enter key at the end of a line: cut it off. */
void remove_newline(char s[])
{
    int i;
    for (i = 0; s[i] != '\0'; i++)
        if (s[i] == '\n' || s[i] == '\r')
            s[i] = '\0';
}

/* "students" + FORMAT_TEXT  ->  "data/students.txt" */
void make_path(char path[], char name[], int format)
{
    if (format == FORMAT_TEXT)
        sprintf(path, "%s/%s.txt", DATA_DIR, name);
    else
        sprintf(path, "%s/%s.bin", DATA_DIR, name);
}

/* Size of a file in bytes, or -1 if the file does not exist. */
long file_size(char path[])
{
    long size;
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    fseek(f, 0, SEEK_END);      /* go to the end of the file */
    size = ftell(f);            /* our position = the size   */
    fclose(f);
    return size;
}

/* Does the table exist? If yes, return 1 and store its format in *format. */
int find_table(char name[], int *format)
{
    char path[128];

    make_path(path, name, FORMAT_TEXT);
    if (file_size(path) >= 0) {
        *format = FORMAT_TEXT;
        return 1;
    }
    make_path(path, name, FORMAT_BINARY);
    if (file_size(path) >= 0) {
        *format = FORMAT_BINARY;
        return 1;
    }
    return 0;
}

/* Binary layout: header = "FDB1" (4 bytes) + column count (an int) + one 32-byte name per column. */
long binary_header_size(int num_cols)
{
    return 4 + sizeof(int) + num_cols * MAX_LEN;
}

long binary_record_size(int num_cols)
{
    return num_cols * MAX_LEN;
}

/* ===================== TEXT format ===================== */

/* Split "1,Ravi,85" into values. Returns how many values were found. */
int split_line(char line[], char out[][MAX_LEN])
{
    char word[MAX_LEN];
    int count = 0, len = 0, i = 0;

    remove_newline(line);
    if (line[0] == '\0')
        return 0;                                   /* blank line */

    while (1) {
        if (line[i] == ',' || line[i] == '\0') {    /* one value ends here */
            word[len] = '\0';
            if (count < MAX_COLS) {
                copy_value(out[count], word);
                count++;
            }
            len = 0;
            if (line[i] == '\0')
                break;                              /* end of the line */
        } else if (len < MAX_LEN - 1) {
            word[len] = line[i];
            len++;
        }
        i++;
    }
    return count;
}

/* Write values as one line: "1,Ravi,85". Used for the header and for records. */
void write_line(FILE *f, char values[][MAX_LEN], int count)
{
    int i;
    for (i = 0; i < count; i++) {
        fprintf(f, "%s", values[i]);
        if (i < count - 1)
            fprintf(f, ",");
    }
    fprintf(f, "\n");
}

int load_text(FILE *f, Table *t)
{
    char line[MAX_COLS * MAX_LEN + 16];
    int c, n;

    if (fgets(line, sizeof(line), f) == NULL)
        return 0;                                        /* empty file */
    t->num_cols = split_line(line, t->cols);             /* line 1 = column names */

    while (t->num_records < MAX_RECORDS && fgets(line, sizeof(line), f) != NULL) {
        n = split_line(line, t->records[t->num_records].values);
        if (n == 0)
            continue;                                    /* skip blank lines */
        for (c = n; c < t->num_cols; c++)                /* missing values become empty */
            copy_value(t->records[t->num_records].values[c], "");
        t->num_records++;
    }
    return 1;
}

/* ===================== BINARY format ===================== */

int load_binary(FILE *f, Table *t)
{
    char magic[5];
    int n;

    /* Header: check it is our file, then read the column count and names. */
    if (fread(magic, 1, 4, f) != 4)
        return 0;
    magic[4] = '\0';
    if (strcmp(magic, "FDB1") != 0)
        return 0;
    if (fread(&t->num_cols, sizeof(int), 1, f) != 1)
        return 0;
    if (t->num_cols < 1 || t->num_cols > MAX_COLS)
        return 0;
    n = fread(t->cols, MAX_LEN, t->num_cols, f);
    if (n != t->num_cols)
        return 0;

    /* Records: keep reading fixed-size blocks until the file ends. */
    while (t->num_records < MAX_RECORDS) {
        n = fread(t->records[t->num_records].values, MAX_LEN, t->num_cols, f);
        if (n != t->num_cols)
            break;
        t->num_records++;
    }
    return 1;
}

/* ===================== Used by the commands ===================== */

/* Read a whole table from its file into memory. Returns 1 on success. */
int load_table(char name[], Table *t)
{
    char path[128];
    FILE *f;
    int ok;

    t->num_cols = 0;
    t->num_records = 0;
    if (!find_table(name, &t->format))
        return 0;
    copy_value(t->name, name);

    make_path(path, name, t->format);
    if (t->format == FORMAT_TEXT) {
        f = fopen(path, "r");
        if (f == NULL)
            return 0;
        ok = load_text(f, t);
    } else {
        f = fopen(path, "rb");
        if (f == NULL)
            return 0;
        ok = load_binary(f, t);
    }
    fclose(f);
    return ok;
}

/* Write the WHOLE table to a file ("w" erases the file first). */
int save_table_as(Table *t, char path[], int format)
{
    int i;
    FILE *f;

    if (format == FORMAT_TEXT) {
        f = fopen(path, "w");
        if (f == NULL)
            return 0;
        write_line(f, t->cols, t->num_cols);
        for (i = 0; i < t->num_records; i++)
            write_line(f, t->records[i].values, t->num_cols);
    } else {
        f = fopen(path, "wb");
        if (f == NULL)
            return 0;
        fwrite("FDB1", 1, 4, f);                    /* magic number */
        fwrite(&t->num_cols, sizeof(int), 1, f);
        fwrite(t->cols, MAX_LEN, t->num_cols, f);
        for (i = 0; i < t->num_records; i++)
            fwrite(t->records[i].values, MAX_LEN, t->num_cols, f);
    }
    fclose(f);
    return 1;
}

/* Write the table to its own file, in its own format. */
int save_table(Table *t)
{
    char path[128];
    make_path(path, t->name, t->format);
    return save_table_as(t, path, t->format);
}

/* Add ONE record at the end of the file. Append mode never touches old data. */
int append_record(Table *t, Record *r)
{
    char path[128];
    FILE *f;

    make_path(path, t->name, t->format);
    if (t->format == FORMAT_TEXT) {
        f = fopen(path, "a");
        if (f == NULL)
            return 0;
        write_line(f, r->values, t->num_cols);
    } else {
        f = fopen(path, "ab");
        if (f == NULL)
            return 0;
        fwrite(r->values, MAX_LEN, t->num_cols, f);
    }
    fclose(f);
    return 1;
}

/*
 * BINARY only: replace record number `index` inside the file.
 * All records are the same size, so we can work out where it starts:
 *     position = header size + index * record size
 * then jump there with fseek() and overwrite just that record.
 */
int overwrite_record(Table *t, int index)
{
    char path[128];
    long position;
    FILE *f;

    make_path(path, t->name, FORMAT_BINARY);
    f = fopen(path, "r+b");                 /* read + write, does NOT erase the file */
    if (f == NULL)
        return 0;

    position = binary_header_size(t->num_cols) + index * binary_record_size(t->num_cols);
    fseek(f, position, SEEK_SET);
    fwrite(t->records[index].values, MAX_LEN, t->num_cols, f);
    fclose(f);
    return 1;
}

/* ===================== The list of tables (data/tables.lst) ===================== */

/* A new table: add its name at the end of the list. */
void add_to_list(char name[])
{
    FILE *f = fopen(LIST_FILE, "a");
    if (f == NULL)
        return;
    fprintf(f, "%s\n", name);
    fclose(f);
}

/* A dropped table: read all names, then write them back without this one. */
void remove_from_list(char name[])
{
    char names[MAX_TABLES][MAX_LEN];
    char line[64];
    int count = 0, i;
    FILE *f = fopen(LIST_FILE, "r");

    if (f == NULL)
        return;
    while (count < MAX_TABLES && fgets(line, sizeof(line), f) != NULL) {
        remove_newline(line);
        if (line[0] != '\0' && strcmp(line, name) != 0) {
            copy_value(names[count], line);
            count++;
        }
    }
    fclose(f);

    f = fopen(LIST_FILE, "w");
    if (f == NULL)
        return;
    for (i = 0; i < count; i++)
        fprintf(f, "%s\n", names[i]);
    fclose(f);
}
