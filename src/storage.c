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
 */
#include <stdio.h>
#include <string.h>
#include "filedb.h"

static const char MAGIC[4] = { 'F', 'D', 'B', '1' };   /* first 4 bytes of every binary file */

/* "students" + FORMAT_TEXT  ->  "data/students.txt" */
void make_path(char *path, const char *name, int format)
{
    sprintf(path, "%s/%s.%s", DATA_DIR, name, format == FORMAT_TEXT ? "txt" : "bin");
}

/* Size of a file in bytes, or -1 if the file does not exist. */
long file_size(const char *path)
{
    long size;
    FILE *f = fopen(path, "rb");
    if (f == NULL) return -1;
    fseek(f, 0, SEEK_END);      /* go to the end of the file */
    size = ftell(f);            /* our position = the size   */
    fclose(f);
    return size;
}

/* Does the table exist? If yes, return 1 and tell which format it uses. */
int find_table(const char *name, int *format)
{
    char path[128];

    make_path(path, name, FORMAT_TEXT);
    if (file_size(path) >= 0) { *format = FORMAT_TEXT; return 1; }

    make_path(path, name, FORMAT_BINARY);
    if (file_size(path) >= 0) { *format = FORMAT_BINARY; return 1; }

    return 0;
}

/* Binary layout: header = "FDB1" (4) + column count (4) + one 32-byte name per column. */
long binary_header_size(int num_cols) { return 4 + sizeof(int) + (long)num_cols * MAX_LEN; }
long binary_record_size(int num_cols) { return (long)num_cols * MAX_LEN; }

/* ===================== TEXT format ===================== */

/* Split "1,Ravi,85\n" into values. Returns how many values were found. */
static int split_line(char *line, char out[][MAX_LEN])
{
    int count = 0;
    char *part = strtok(line, ",\r\n");          /* cut at commas and the newline */
    while (part != NULL && count < MAX_COLS) {
        snprintf(out[count], MAX_LEN, "%s", part);   /* copy safely, at most 31 chars */
        count++;
        part = strtok(NULL, ",\r\n");
    }
    return count;
}

/* Write values as one line: "1,Ravi,85\n". Used for the header and for records. */
static void write_line(FILE *f, char values[][MAX_LEN], int count)
{
    int i;
    for (i = 0; i < count; i++)
        fprintf(f, "%s%s", values[i], i < count - 1 ? "," : "\n");
}

static int load_text(FILE *f, Table *t)
{
    char line[MAX_COLS * MAX_LEN + 16];

    if (fgets(line, sizeof line, f) == NULL) return 0;        /* empty file */
    t->num_cols = split_line(line, t->cols);                   /* line 1 = column names */

    while (t->num_records < MAX_RECORDS && fgets(line, sizeof line, f) != NULL) {
        if (split_line(line, t->records[t->num_records].values) > 0)   /* skip blank lines */
            t->num_records++;
    }
    return 1;
}

/* ===================== BINARY format ===================== */

static int load_binary(FILE *f, Table *t)
{
    char magic[4];
    size_t n;

    /* Header: check it is our file, then read the column count and names. */
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, MAGIC, 4) != 0) return 0;
    if (fread(&t->num_cols, sizeof(int), 1, f) != 1) return 0;
    if (t->num_cols < 1 || t->num_cols > MAX_COLS) return 0;
    n = (size_t)t->num_cols;
    if (fread(t->cols, MAX_LEN, n, f) != n) return 0;

    /* Records: keep reading fixed-size blocks until the file ends. */
    while (t->num_records < MAX_RECORDS &&
           fread(t->records[t->num_records].values, MAX_LEN, n, f) == n)
        t->num_records++;
    return 1;
}

/* ===================== Used by the commands ===================== */

/* Read a whole table from its file into memory. Returns 1 on success. */
int load_table(const char *name, Table *t)
{
    char path[128];
    FILE *f;
    int ok;

    memset(t, 0, sizeof(Table));            /* start from an all-zero table */
    if (!find_table(name, &t->format)) return 0;
    snprintf(t->name, MAX_LEN, "%s", name);

    make_path(path, name, t->format);
    f = fopen(path, t->format == FORMAT_TEXT ? "r" : "rb");
    if (f == NULL) return 0;

    ok = (t->format == FORMAT_TEXT) ? load_text(f, t) : load_binary(f, t);
    fclose(f);
    return ok;
}

/* Write the WHOLE table to a file ("w" erases the file first). */
int save_table_as(Table *t, const char *path, int format)
{
    int i;
    FILE *f = fopen(path, format == FORMAT_TEXT ? "w" : "wb");
    if (f == NULL) return 0;

    if (format == FORMAT_TEXT) {
        write_line(f, t->cols, t->num_cols);
        for (i = 0; i < t->num_records; i++)
            write_line(f, t->records[i].values, t->num_cols);
    } else {
        fwrite(MAGIC, 1, 4, f);
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
    f = fopen(path, t->format == FORMAT_TEXT ? "a" : "ab");
    if (f == NULL) return 0;

    if (t->format == FORMAT_TEXT) write_line(f, r->values, t->num_cols);
    else                          fwrite(r->values, MAX_LEN, t->num_cols, f);
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
    if (f == NULL) return 0;

    position = binary_header_size(t->num_cols) + index * binary_record_size(t->num_cols);
    fseek(f, position, SEEK_SET);
    fwrite(t->records[index].values, MAX_LEN, t->num_cols, f);
    fclose(f);
    return 1;
}
