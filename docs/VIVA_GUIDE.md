# FileDB: viva guide

This guide explains the whole project in simple words, so every team member can answer questions about any part of it. Read it from top to bottom once, then use the question list at the end to practise.

Live website: **https://filedb-kappa.vercel.app** (for a quick demo, open https://filedb-kappa.vercel.app/?demo=binary)

---

## 1. The project in 30 seconds

> "FileDB is a small database engine written in C. You can create tables, insert records, search them, update them and delete them. Each table is saved as one file. When you create a table you choose the format: a **text file**, which people can read, or a **binary file**, where every record has the same size so the program can jump straight to any record with `fseek()` and change it in place. We also compiled the same C code to WebAssembly, so it runs on our website, which shows the file's bytes changing after every command."

---

## 2. Words you need to know

| Word | Simple meaning |
|---|---|
| **Record** | One row of data, like one student: `1, Ravi, CSE, 85`. |
| **Field / value** | One piece of a record, like `Ravi`. |
| **Column** | The name of a field, like `name`. |
| **Table** | A list of records with the same columns, like `students`. In FileDB, one table = one file. |
| **Text file** | A file of readable characters. Our text tables look like CSV: `1,Ravi,CSE,85`. |
| **Binary file** | A file of raw bytes, written exactly as they are in memory. Not meant for people to read. |
| **`struct`** | A C type that groups several variables into one. We use `Record` and `Table`. |
| **Tokenize** | Split a line into words ("tokens"). `find students marks > 80` becomes 5 tokens. |
| **Linear search** | Check every record one by one. Simple, and fine for small tables. |
| **Bubble sort** | A sorting method that keeps swapping neighbours that are in the wrong order. |
| **`fopen` mode** | How a file is opened: `"r"` read, `"w"` write (erases first), `"a"` append (add at the end). Add `b` for binary, and `+` for read and write. |
| **`fseek()`** | Moves the "cursor" inside a file to a given byte, so the next read or write happens there. |
| **`ftell()`** | Tells you where the cursor is (we use it to get the file size). |
| **Append** | Add to the end of a file without touching what is already there. |
| **Magic number** | A few fixed bytes at the start of a file that identify its type. Ours is `FDB1`. |
| **Header** | Information at the start of the file that describes the rest (number of columns, column names). |
| **Fixed-size record** | Every record takes exactly the same number of bytes. That makes positions easy to calculate. |
| **WebAssembly (wasm)** | A format that lets compiled programs (like C) run inside a web browser. |
| **Emscripten (`emcc`)** | The tool that compiles C to WebAssembly. |
| **IndexedDB** | Storage inside the browser. The website keeps your table files there. |
| **Makefile** | A file of build commands. `make` builds the program, `make test` runs the tests. |

---

## 3. The four source files

| File | Job | Lines |
|---|---|---|
| `src/filedb.h` | Shared definitions: limits, `Record` and `Table` structs, list of functions | ~50 |
| `src/storage.c` | Everything that reads or writes files | ~240 |
| `src/commands.c` | Understands what the user typed and runs the command | ~560 |
| `src/main.c` | The terminal loop: prompt, read a line, run it, repeat | ~30 |

**Why split it like this?** Each file has one job. If the file format changes, only `storage.c` changes. If we add a command, only `commands.c` changes. This is called *separation of concerns*.

---

## 4. `filedb.h`: the shared definitions

```c
#define MAX_COLS     10     /* max columns in one table          */
#define MAX_RECORDS  1000   /* max records in one table          */
#define MAX_LEN      32     /* max length of one value, incl. \0 */

typedef struct {
    char values[MAX_COLS][MAX_LEN];   /* 10 values, 32 characters each */
} Record;

typedef struct {
    char   name[MAX_LEN];
    int    format;                    /* FORMAT_TEXT or FORMAT_BINARY  */
    int    num_cols;
    char   cols[MAX_COLS][MAX_LEN];   /* column names                  */
    int    num_records;
    Record records[MAX_RECORDS];
} Table;
```

- **Every value is stored as a string** (`char[32]`). This keeps the code simple: no separate types for numbers and text. When we compare, we check whether both values look like numbers (see `compare_values`).
- A `Table` is about **320 KB** (1000 records × 10 values × 32 bytes).
- `#ifndef FILEDB_H ... #endif` is an **include guard**. It stops the header from being included twice.

---

## 5. `storage.c`: reading and writing files

### The two formats

**Text** (`data/students.txt`):

```text
id,name,dept,marks
1,Ravi,CSE,85
2,Anita Sharma,ECE,92
```

**Binary** (`data/books.bin`):

```text
"FDB1" (4 bytes) | num_cols (4-byte int) | column names (num_cols x 32 bytes) | record 0 | record 1 | ...
```

Each record in the binary file is `num_cols × 32` bytes.

### Functions, one line each

| Function | What it does |
|---|---|
| `make_path()` | Builds the file name, e.g. `"data/students.txt"`, using `sprintf`. |
| `find_table()` | Looks for `name.txt`, then `name.bin` (a file exists if `file_size()` is not -1). Tells you which format the table uses. |
| `file_size()` | `fseek()` to the end of the file, then `ftell()` gives the size in bytes. |
| `binary_header_size()` | `4 + sizeof(int) + num_cols × 32` |
| `binary_record_size()` | `num_cols × 32` |
| `split_line()` | Splits `"1,Ravi,CSE,85"` at the commas into separate values, using `strtok()`. |
| `write_line()` | Writes values as one comma-separated line. Used for the header line and for every record. |
| `load_text()` | Reads line 1 as column names, then one record per line with `fgets()`. |
| `load_binary()` | Checks the magic bytes `FDB1`, reads the column count and names, then reads fixed-size records with `fread()` until the file ends. |
| `load_table()` | Finds the table's file and calls `load_text()` or `load_binary()`. |
| `save_table_as()` | Writes the **whole** table to a file in either format (`"w"` / `"wb"` erases the file first). |
| `save_table()` | Calls `save_table_as()` with the table's own file and format. |
| `append_record()` | Opens the file in append mode (`"a"` / `"ab"`) and writes one record at the end. |
| `overwrite_record()` | **Binary only.** Opens with `"r+b"`, jumps to the record with `fseek()`, overwrites just that record. |

### The most important idea: `overwrite_record()`

```c
position = binary_header_size(t->num_cols) + index * binary_record_size(t->num_cols);
fseek(f, position, SEEK_SET);
fwrite(t->records[index].values, MAX_LEN, t->num_cols, f);
```

Example with 4 columns: header = 4 + 4 + 4 × 32 = 136 bytes, each record = 4 × 32 = 128 bytes.
Record 2 starts at 136 + 2 × 128 = **392**. We jump there and write 128 bytes. Nothing else in the file changes.

**A text file cannot do this**, because its lines have different lengths. Changing `78` to `100` makes the line one character longer, so everything after it has to move. That is why text updates rewrite the whole file.

### How we know where the binary records end

`fread()` returns how many items it actually read. When it returns fewer than we asked for, we have reached the end of the file:

```c
if (fread(r->values, MAX_LEN, t->num_cols, f) != (size_t)t->num_cols) break;
```

So we never store the record count in the file. The file size tells us.

---

## 6. `commands.c`: understanding and running commands

Every command follows the same three steps:

1. **Tokenize**: split the line into words.
2. **Load**: read the table's file into memory (`open_table()` → `load_table()`).
3. **Act**: do the work, then save if something changed.

### Global variables

```c
static Table table;               /* the table we are working on (~320 KB) */
static char  tok[MAX_TOKENS][64]; /* the words of the current command      */
static int   ntok;                /* how many words                        */
static int   selected[MAX_RECORDS]; /* selected[i] = 1 if record i matches */
```

**Why is `table` global instead of a local variable?** Local variables live on the *stack*, which is small: in WebAssembly it is only 64 KB by default, and our table is 320 KB. Global variables live in a separate memory area that exists for the whole program. `static` here means "only visible inside this file".

### Helper functions

| Function | What it does |
|---|---|
| `tokenize()` | Walks through the line character by character. Spaces end a word. A `"` starts a quoted word that can contain spaces. |
| `valid_name()` | Names must start with a letter and use only letters, digits and `_`. This also blocks tricks like `drop ../secret`. |
| `valid_value()` | A value must be 1 to 31 characters and must not contain a comma. (Empty values are not allowed, because `strtok` would skip them when reading the file back.) |
| `column_index()` | Finds a column's position by its name, or returns -1. |
| `is_number()` | Uses `strtod()`. If the whole string was used up, it is a number. |
| `compare_values()` | Compares as numbers if both are numbers (so 9 < 78), otherwise alphabetically with `strcmp()`. |
| `parse_format()` | Turns `"text"` / `"binary"` into `FORMAT_TEXT` / `FORMAT_BINARY`. Used by `create` and `convert`. |
| `matches()` | `matches("85", ">", "80")` returns 1. Handles `=`, `!=`, `>`, `<`, `>=`, `<=`, `contains`. |
| `select_where()` | Checks the column and operator, then marks every matching record in `selected[]` (linear search). Returns how many matched. |
| `print_records()` | Works out how wide each column must be, then prints the selected records as a box table. |

### The commands

| Function | How it works |
|---|---|
| `cmd_create()` | Checks the name, format and columns, fills an empty `Table`, and saves it (this creates the file). |
| `cmd_insert()` | Loads the table, checks the number of values, zero-fills a `Record` with `memset`, copies the values, and calls `append_record()`. |
| `cmd_show()` | Marks every record to be shown and prints them. |
| `cmd_find()` | Calls `select_where()`, then prints the matching records. |
| `cmd_sort()` | Bubble sort on the copy in memory, then prints. **The file is not changed.** |
| `cmd_count()` | Counts all records, or only the matching ones. |
| `cmd_update()` | Changes the value in every matching record. Binary: `overwrite_record()` for each one. Text: `save_table()` once at the end. |
| `cmd_delete()` | Keeps only the records that do *not* match, sliding them forward to close the gaps, then saves the whole table. |
| `cmd_tables()` | Uses `opendir()` / `readdir()` to list the `.txt` and `.bin` files in `data/`. |
| `cmd_describe()` | Prints format, file, size and, for binary, the header and record sizes. |
| `cmd_compare()` | Saves the same data to two temporary files (text and binary), measures both, then deletes them. |
| `cmd_convert()` | Saves the table in the other format, then deletes the old file. |
| `cmd_drop()` | Deletes the table's file with `remove()`. |
| `execute()` | Tokenizes the line and calls the right `cmd_` function. Returns 1 for `exit`. |

### Why `memset` before writing?

```c
memset(&r, 0, sizeof r);   /* zero-fill, so no garbage bytes reach the file */
```

A new variable in C contains whatever was left in that memory before ("garbage"). In a binary file we write all 32 bytes of each slot, so without `memset` those leftover bytes would end up in the file. We also clear the slot before an update. Otherwise changing `"Let Us C"` to `"C"` would leave `"t Us C"` hidden after the new text. There is a test for exactly this.

---

## 7. `main.c`: the terminal loop

```c
mkdir(DATA_DIR, 0755);                   /* make sure data/ exists */
while (1) {
    printf("filedb> ");
    if (fgets(line, sizeof line, stdin) == NULL) break;   /* end of input */
    if (execute(line) == 1) break;                       /* user typed exit */
}
```

This is called a **REPL**: Read, Evaluate, Print, Loop.

---

## 8. The website (how C runs in a browser)

1. `make web` runs **Emscripten** (`emcc`), which compiles `storage.c` and `commands.c` into `filedb.wasm` (WebAssembly) plus `filedb.js` (the code that loads it).
2. `main.c` is left out. Instead, `web/app.js` calls the C function directly:

   ```js
   M.ccall('execute', 'number', ['string'], [cmd]);   // runs our C execute()
   ```

3. Whatever the C code prints with `printf` is captured and shown in the console panel.
4. Emscripten gives the C code a pretend file system, so `fopen("data/students.txt")` works. We connect `data/` to the browser's **IndexedDB**, so tables survive a page refresh.
5. JavaScript only **draws** things: the table grid and the bytes of the file. All the database work is done by the C code.
6. Before each command, `app.js` copies every file's bytes. Afterwards it compares them byte by byte and highlights what changed. This makes the difference between the formats visible:
   - `insert` → only new bytes at the **end** light up (append).
   - binary `update` → only the changed bytes inside **one 32-byte slot** light up (`fseek`).
   - text `update` that changes a value's length → **everything after it** changes (rewrite).

---

## 9. Text vs binary: the trade-offs

| | Text | Binary |
|---|---|---|
| Readable by people | Yes | No |
| Size | Smaller (only the characters typed) | Bigger (fixed 32-byte slots, filled with zeros) |
| Change one record | Rewrite the whole file | `fseek()` + overwrite in place |
| Jump to record N | Must read every line before it | Calculate the position directly |
| Portable between computers | Yes | Depends on `int` size and byte order |

**Why was binary *bigger* in our demo?** Because we give every value a fixed 32-byte slot, even `"1"`. Binary is not always smaller. It is smaller when you store numbers as real `int`s (4 bytes for 2147483647 instead of 10 characters), but we store everything as text slots to keep the code simple. Its real advantage here is **fixed-size records → random access**.

---

## 10. How long does each operation take? (time complexity)

*n* = number of records.

| Operation | Time | Why |
|---|---|---|
| Load a table | O(n) | Reads every record into memory |
| `insert` | O(n) | Loads the table to check it, then one append write |
| `find`, `count` | O(n) | Linear search |
| `sort` | O(n²) | Bubble sort. Fine for 1000 records; `qsort()` would be O(n log n) |
| `update` (binary) | O(n) to search, but only the matching records are written | `fseek` + `fwrite` per match |
| `update` (text), `delete` | O(n) | The whole file is rewritten |

---

## 11. Limitations, and how we would improve them

| Limitation | Improvement |
|---|---|
| Only one condition per query | Add `and` / `or` |
| All values are strings | Store real types (`int`, `float`) in binary files |
| Linear search | Add an index (for example a B-tree or a hash table) on a column |
| Max 1000 records, 10 columns, 31 characters | Use `malloc()` to grow memory as needed |
| Whole table loaded into memory | Read records one at a time from the file |
| If the program crashes while rewriting, the file can be half-written | Write to a temporary file, then `rename()` it over the old one (rename is atomic) |
| Only one user at a time | File locking |
| No real SQL | Write a small SQL parser |

---

## 12. Likely viva questions, with short answers

**What does your project do?**
It stores records in files and lets you create, insert, search, update, sort, count and delete them using simple commands. Each table can be a text file or a binary file.

**Why did you write it in C?**
C gives direct control over files and memory (`fopen`, `fread`, `fwrite`, `fseek`, structs, byte layout), which is exactly what this topic is about.

**What is the difference between a text file and a binary file?**
A text file stores readable characters, line by line. A binary file stores raw bytes exactly as they are in memory. Our binary records all have the same size, so we can jump to any record.

**Why can you update a binary record in place but not a text one?**
Binary records have a fixed size, so the position of record N is `header + N × record_size`, and a new value fits in the same 32-byte slot. Text lines have different lengths, so a longer or shorter value would overlap the next line. We have to rewrite the file.

**What does `fseek()` do?**
It moves the file position to a byte offset. `fseek(f, position, SEEK_SET)` moves to `position` bytes from the start of the file. `SEEK_END` means from the end (we use it to find the file size).

**Which `fopen` modes did you use?**
`"r"` / `"rb"` to read, `"w"` / `"wb"` to write a fresh file (erases it first), `"a"` / `"ab"` to append, and `"r+b"` to read and write without erasing (for in-place updates).

**What is `FDB1` at the start of the binary file?**
A magic number. When loading, we check those 4 bytes. If they don't match, it's not our file and we refuse to load it.

**How do you know how many records are in a binary file?**
We read fixed-size records until `fread()` returns fewer items than requested, which means the end of the file. Equivalently: (file size − header size) ÷ record size.

**Why do you store numbers as text even in the binary file?**
To keep the code simple: every value is a `char[32]`. We compare numerically when both values look like numbers.

**How do you compare `9` and `78` correctly?**
`compare_values()` uses `is_number()` (which uses `strtod`). If both are numbers, it compares them as `double`s. Otherwise it uses `strcmp()`.

**What is tokenizing? How do you handle values with spaces?**
Splitting the input into words. Normally a space ends a word, but if a word starts with `"`, we keep reading until the closing `"`.

**Which search algorithm do you use?**
Linear search: we check every record. It's O(n), and simple and fast enough for 1000 records.

**Which sorting algorithm do you use? Does it change the file?**
Bubble sort, O(n²). It sorts only the copy in memory, so the file is not changed.

**How does delete work?**
We loop over the records and copy every record that does not match to the front of the array (`kept++`). Then we set the new count and rewrite the file.

**Why is the `table` variable global?**
It is about 320 KB. Local variables go on the stack, which is small (only 64 KB in WebAssembly). Global and static variables are stored in a separate area.

**What does `static` mean on a function?**
The function can only be used inside that `.c` file. It's private.

**Why do you use `memset` before writing a record?**
To fill the record with zeros, so no leftover "garbage" bytes from memory get written into the binary file.

**How do you prevent buffer overflows?**
We check lengths before copying: values must be under 32 characters (`valid_value`), tokens are cut at 63 characters, and values read back from a file are copied with `snprintf`, which never writes past the buffer.

**What stops a user from deleting other files, like `drop ../important`?**
`valid_name()` allows only letters, digits and `_`, starting with a letter. So a name can never contain `/` or `..`.

**What is `sizeof(int)` here? Is the binary file portable?**
4 bytes on our computers and in WebAssembly. The `int` is written in the computer's byte order (little-endian on Intel, Apple and WebAssembly). A computer with a different `int` size or byte order could misread the column count, so the binary format is not fully portable. The text format is.

**How does the website run C code?**
We compile the same C files to WebAssembly with Emscripten. JavaScript calls our `execute()` function for every command and shows what it printed. Emscripten provides a virtual file system, which we save into the browser's IndexedDB.

**What happens if the power goes off while saving?**
`"w"` erases the file before writing, so a crash could leave it half-written. To fix this, write to a temporary file first and then `rename()` it. We listed this as a future improvement.

**How did you test the project?**
`tests/run_tests.sh` runs 35 automatic tests: every command, both formats, data surviving a restart, the file layout, and every error message. We also checked that one test really fails when its bug is put back.

**What would you add next?**
`and`/`or` conditions, real number types, an index for faster search, `malloc` for bigger tables, and safe saving with `rename()`.

---

## 13. A 2-minute demo script

1. Open the website. Say: *"This is our C engine, compiled to WebAssembly and running in the browser."*
2. Click **Load sample data**. Point at the `students` grid and its text file on the right: *"One line per record, separated by commas."*
3. Type `find students marks > 80`: *"Linear search; numbers are compared as numbers."*
4. Type `update students set marks 100 where id = 3` and point at the file: *"The text file had to be rewritten. Everything after the change moved."*
5. Click **books** and switch to the binary file: *"Header with FDB1, then fixed 32-byte slots."* Point at a byte to show its explanation.
6. Type `update books set price 350 where id = 2`: *"Only one byte changed. `fseek()` jumped straight to that record."*
7. Type `compare students`: *"Text is smaller; binary gives fixed-size records and in-place updates."*
8. Finish in the terminal: `make test` → *"35 automatic tests pass."*
