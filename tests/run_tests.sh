#!/bin/sh
# run_tests.sh - Automatic tests for FileDB.
#
# How it works: we send commands to ./filedb (like typing them) and check
# that the output contains the text we expect. Tests run inside a fresh
# temporary folder, so your own data/ folder is never touched.
#
# Run with:  make test

ROOT=$(cd "$(dirname "$0")/.." && pwd)
FILEDB="$ROOT/filedb"
WORK=$(mktemp -d)
cd "$WORK" || exit 1

pass=0
fail=0

ok()  { pass=$((pass + 1)); echo "  PASS  $1"; }
bad() { fail=$((fail + 1)); echo "  FAIL  $1"; echo "        $2"; }

# check "<test name>" "<commands>" "<text the output must contain>"
check() {
    out=$(printf '%s\nexit\n' "$2" | "$FILEDB")
    if printf '%s' "$out" | grep -qF -- "$3"; then ok "$1"
    else bad "$1" "expected to see: $3"; fi
}

# check_order "<test name>" "<commands>" "<first text>" "<second text>"
# Passes if <first text> appears on an earlier line than <second text>.
check_order() {
    out=$(printf '%s\nexit\n' "$2" | "$FILEDB")
    a=$(printf '%s\n' "$out" | grep -nF -- "$3" | head -1 | cut -d: -f1)
    b=$(printf '%s\n' "$out" | grep -nF -- "$4" | head -1 | cut -d: -f1)
    if [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ]; then ok "$1"
    else bad "$1" "expected '$3' before '$4'"; fi
}

echo "Running FileDB tests..."

# ---- Store ----
check "create a text table"     "create students text id name dept marks" "created with 4 column(s), stored as text"
check "create a binary table"   "create books binary id title price"      "stored as binary"
check "insert into text table"  "insert students 1 Ravi CSE 85"           "1 record inserted"
check "insert quoted value"     'insert students 2 "Anita Sharma" ECE 92' "1 record inserted"
check "insert into binary table" 'insert books 1 "Let Us C" 300'          "1 record inserted"
printf 'insert students 3 Kiran CSE 78\ninsert students 4 Meena MECH 9\nexit\n' | "$FILEDB" > /dev/null
check "data is saved to disk (survives restart)" "show students" "Anita Sharma"

# ---- Query ----
check "find with >"             "find students marks > 80"         "2 record(s) found."
check "find with contains"      "find students name contains ee"   "Meena"
check "count all"               "count students"                   "Count: 4"
check "count with condition"    "count students dept = CSE"        "Count: 2"
check_order "sort compares numbers as numbers (9 before 78)" "sort students marks" "| 9 " "| 78 "
check_order "sort descending"   "sort students marks desc"         "| 92 " "| 85 "

# ---- Update ----
check "update text table"       "update students set marks 95 where id = 3" "whole text file rewritten"
check "updated value is saved"  "find students id = 3"                      "95"
check "update binary in place"  'update books set title "C" where id = 1'   "in place using fseek()"
# "Let Us C" became "C": the leftover letters "Us C" must be gone from the file.
# (tr removes the zero bytes first, because grep stops reading at them.)
if tr -d '\000' < data/books.bin | grep -q "Us C"; then
    bad "update clears old bytes in binary file" "old text 'Us C' still inside books.bin"
else
    ok "update clears old bytes in binary file"
fi
check "delete records"          "delete students where dept = MECH" "1 record(s) deleted."

# ---- Files ----
if [ "$(head -c 4 data/books.bin)" = "FDB1" ]; then ok "binary file starts with magic bytes FDB1"
else bad "binary file starts with magic bytes FDB1" "header is wrong"; fi
if [ "$(head -n 1 data/students.txt)" = "id,name,dept,marks" ]; then ok "text file's first line holds column names"
else bad "text file's first line holds column names" "got: $(head -n 1 data/students.txt)"; fi
check "compare sizes"           "compare students"                 "binary :"
check "convert text -> binary"  "convert students binary"          "now stored as binary"
if [ -f data/students.bin ] && [ ! -f data/students.txt ]; then ok "convert replaced the old file"
else bad "convert replaced the old file" "files in data/: $(ls data)"; fi
check "data intact after convert" "show students"                  "3 record(s) found."
check "list tables"             "tables"                           "books"
check "drop a table"            "drop books"                       "deleted"

# ---- Errors are handled politely ----
check "wrong number of values"  "insert students 5 Raj"            "has 4 column(s) but you gave 2"
check "missing table"           "show nothing"                     "does not exist"
check "unknown column"          "find students age > 3"            "no column named 'age'"
check "unknown operator"        "find students marks => 3"         "unknown operator"
check "comma not allowed"       "insert students 5 a,b CSE 1"      "cannot contain a comma"
check "duplicate table"         "create students text a"           "already exists"
check "value too long"          "insert students 5 abcdefghijklmnopqrstuvwxyz0123456789 CSE 1" "too long"
check "no ../ path tricks"      "drop ../secret"                   "does not exist"
check "unknown command"         "fly away"                         "unknown command"

cd "$ROOT" && rm -rf "$WORK"
echo ""
echo "Result: $pass passed, $fail failed."
[ "$fail" -eq 0 ]
