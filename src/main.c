/*
 * main.c  -  The terminal version of FileDB.
 *
 * It is a simple REPL: Read a line, Evaluate it, Print the result, Loop.
 * (The website does not use this file; it calls execute() directly.)
 */
#include <stdio.h>
#include <sys/stat.h>
#include "filedb.h"

int main(void)
{
    char line[1024];

    mkdir(DATA_DIR, 0755);   /* make sure data/ exists (ignored if it already does) */

    printf("FileDB - a simple file-based data engine. Type 'help' to start.\n");

    while (1) {
        printf("filedb> ");
        fflush(stdout);

        if (fgets(line, sizeof line, stdin) == NULL) break;  /* end of input */
        if (execute(line) == 1) break;                       /* user typed exit */
    }

    printf("Bye!\n");
    return 0;
}
