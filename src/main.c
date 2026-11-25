/*
 * main.c  -  The terminal version of FileDB.
 *
 * It is a simple loop: read a line, run it, print the result, repeat.
 * (The website does not use this file; it calls execute() directly.)
 */
#include <stdio.h>
#include "filedb.h"

int main(void)
{
    char line[1024];
    FILE *f;

    /* Opening the list of tables also checks that the data/ folder exists. */
    f = fopen(LIST_FILE, "a");
    if (f == NULL) {
        printf("Error: there is no 'data' folder here. Create it first with:  mkdir data\n");
        return 1;
    }
    fclose(f);

    printf("FileDB - a simple file-based data engine. Type 'help' to start.\n");

    while (1) {
        printf("filedb> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL)
            break;                         /* end of input */
        if (execute(line) == 1)
            break;                         /* user typed exit */
    }

    printf("Bye!\n");
    return 0;
}
