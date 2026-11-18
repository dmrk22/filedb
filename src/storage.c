/*
 * storage.c  -  Everything that reads or writes table files.
 *
 * A table lives in ONE file inside the data/ folder:
 *
 *   TEXT format   (data/<name>.txt)  - human readable, like a CSV file
 *       line 1      : column names      id,name,marks
 *       other lines : one record each   1,Ravi,85
 *
 *   BINARY format (data/<name>.bin)  - raw bytes, fixed-size records
 *       [ "FDB1" | num_cols | column names ] [ record ] [ record ] ...
 *       Every record has the SAME size, so record number i starts at
 *       header_size + i * record_size. We can jump straight to it with fseek().
 */
#include <stdio.h>
#include <string.h>
#include "filedb.h"

static const char MAGIC[4] = { 'F', 'D', 'B', '1' };  /* marks a FileDB binary file */

/* Builds the file path for a table, e.g. "data/students.txt". */
void make_path(char *path, const char *name, int format)
{
    sprintf(path, "%s/%s.%s", DATA_DIR, name, format == FORMAT_TEXT ? "txt" : "bin");
}

/* Returns 1 if the file can be opened for reading, else 0. */
static int file_exists(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) return 0;
    fclose(f);
    return 1;
}

/* Looks for a table's file. Returns 1 and sets *format if found, else 0. */
int find_table(const char *name, int *format)
{
    char path[128];

    make_path(path, name, FORMAT_TEXT);
    if (file_exists(path)) { *format = FORMAT_TEXT; return 1; }

    make_path(path, name, FORMAT_BINARY);
    if (file_exists(path)) { *format = FORMAT_BINARY; return 1; }

    return 0;
}

/* Returns the size of a file in bytes, or -1 if it cannot be opened. */
long file_size(const char *path)
{
    long size;
    FILE *f = fopen(path, "rb");
    if (f == NULL) return -1;
    fseek(f, 0, SEEK_END);   /* jump to the end ...           */
    size = ftell(f);         /* ... and ask where we are      */
    fclose(f);
    return size;
}

/* Binary layout sizes (in bytes). */
long binary_header_size(int num_cols) { return 4 + sizeof(int) + (long)num_cols * MAX_LEN; }
long binary_record_size(int num_cols) { return (long)num_cols * MAX_LEN; }

/*
 * Splits "a,b,c" into separate values. Unlike strtok(), it keeps empty
 * values ("a,,c" gives 3 values). Returns how many values were found.
 */
static int split_line(char *line, char out[][MAX_LEN], int max)
{
    int count = 0, len = 0;
    char *p;

    for (p = line; ; p++) {
        if (*p == ',' || *p == '\0') {
            if (count < max) out[count][len] = '\0';
            count++;
            len = 0;
            if (*p == '\0') break;
        } else if (count < max && len < MAX_LEN - 1) {
            out[count][len++] = *p;
        }
    }
    return count;
}

/* Removes the newline that fgets() keeps at the end of a line. */
static void strip_newline(char *s)
{
    s[strcspn(s, "\r\n")] = '\0';
}

/* ---------- Loading ---------- */

static int load_text(FILE *f, Table *t)
{
    char line[MAX_COLS * MAX_LEN + 16];

    if (fgets(line, sizeof line, f) == NULL) return 0;   /* empty file */
    strip_newline(line);
    t->num_cols = split_line(line, t->cols, MAX_COLS);

    while (t->num_records < MAX_RECORDS && fgets(line, sizeof line, f) != NULL) {
        Record *r = &t->records[t->num_records];
        strip_newline(line);
        if (line[0] == '\0') continue;                   /* skip blank lines */
        memset(r, 0, sizeof(Record));
        split_line(line, r->values, t->num_cols);
        t->num_records++;
    }
    return 1;
}

static int load_binary(FILE *f, Table *t)
{
    char magic[4];

    /* 1. Header: check the magic bytes, then read the column info. */
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, MAGIC, 4) != 0) return 0;
    if (fread(&t->num_cols, sizeof(int), 1, f) != 1) return 0;
    if (t->num_cols < 1 || t->num_cols > MAX_COLS) return 0;
    if (fread(t->cols, MAX_LEN, t->num_cols, f) != (size_t)t->num_cols) return 0;

    /* 2. Records: read fixed-size blocks until the end of the file. */
    while (t->num_records < MAX_RECORDS) {
        Record *r = &t->records[t->num_records];
        memset(r, 0, sizeof(Record));
        if (fread(r->values, MAX_LEN, t->num_cols, f) != (size_t)t->num_cols) break;
        t->num_records++;
    }
    return 1;
}

/* Loads a whole table from its file into memory. Returns 1 on success. */
int load_table(const char *name, Table *t)
{
    char path[128];
    FILE *f;
    int ok;

    memset(t, 0, sizeof(Table));
    if (!find_table(name, &t->format)) return 0;
    strcpy(t->name, name);

    make_path(path, name, t->format);
    f = fopen(path, t->format == FORMAT_TEXT ? "r" : "rb");
    if (f == NULL) return 0;

    ok = (t->format == FORMAT_TEXT) ? load_text(f, t) : load_binary(f, t);
    fclose(f);
    return ok;
}

/* ---------- Saving ---------- */

static void write_text_record(FILE *f, Table *t, Record *r)
{
    int c;
    for (c = 0; c < t->num_cols; c++)
        fprintf(f, "%s%s", r->values[c], c < t->num_cols - 1 ? "," : "\n");
}

/* Writes the WHOLE table to 'path' in the given format. Returns 1 on success. */
int save_table_as(Table *t, const char *path, int format)
{
    int i;
    FILE *f = fopen(path, format == FORMAT_TEXT ? "w" : "wb");
    if (f == NULL) return 0;

    if (format == FORMAT_TEXT) {
        for (i = 0; i < t->num_cols; i++)
            fprintf(f, "%s%s", t->cols[i], i < t->num_cols - 1 ? "," : "\n");
        for (i = 0; i < t->num_records; i++)
            write_text_record(f, t, &t->records[i]);
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

/* Saves the table to its own file, in its own format. */
int save_table(Table *t)
{
    char path[128];
    make_path(path, t->name, t->format);
    return save_table_as(t, path, t->format);
}

/*
 * Adds ONE record to the end of the file, without rewriting the rest.
 * Opening with "a" (append) puts the write position at the end.
 */
int append_record(Table *t, Record *r)
{
    char path[128];
    FILE *f;

    make_path(path, t->name, t->format);
    f = fopen(path, t->format == FORMAT_TEXT ? "a" : "ab");
    if (f == NULL) return 0;

    if (t->format == FORMAT_TEXT) write_text_record(f, t, r);
    else                          fwrite(r->values, MAX_LEN, t->num_cols, f);

    fclose(f);
    return 1;
}

/*
 * BINARY ONLY: overwrites record number 'index' directly inside the file.
 * Because all records have the same size, we can calculate exactly where
 * it starts and jump there with fseek(). No other bytes are touched.
 * (A text file cannot do this: lines have different lengths.)
 */
int overwrite_record(Table *t, int index)
{
    char path[128];
    long position;
    FILE *f;

    make_path(path, t->name, FORMAT_BINARY);
    f = fopen(path, "r+b");                     /* read + write, no truncation */
    if (f == NULL) return 0;

    position = binary_header_size(t->num_cols) + index * binary_record_size(t->num_cols);
    fseek(f, position, SEEK_SET);
    fwrite(t->records[index].values, MAX_LEN, t->num_cols, f);

    fclose(f);
    return 1;
}
