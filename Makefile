# FileDB build file.
#   make        -> builds the terminal program  ./filedb
#   make web    -> builds the website engine    web/filedb.js + web/filedb.wasm
#   make test   -> runs the automatic tests
#   make clean  -> removes built files

CC     = gcc
CFLAGS = -Wall -Wextra -std=c99 -pedantic
SRC    = src/storage.c src/commands.c

filedb: $(SRC) src/main.c src/filedb.h
	$(CC) $(CFLAGS) -o filedb $(SRC) src/main.c
	mkdir -p data

# main.c is left out: the web page calls execute() directly.
# IDBFS = the browser's storage, so tables survive a page refresh.
web: $(SRC) src/filedb.h
	emcc -O2 $(SRC) -o web/filedb.js \
	  -s EXPORTED_FUNCTIONS=_execute \
	  -s EXPORTED_RUNTIME_METHODS=ccall,FS,IDBFS \
	  -s MODULARIZE=1 -s EXPORT_NAME=createFileDB \
	  -s ALLOW_MEMORY_GROWTH=1 -lidbfs.js --no-entry

test: filedb
	sh tests/run_tests.sh

clean:
	rm -f filedb web/filedb.js web/filedb.wasm
	rm -rf data

.PHONY: web test clean
