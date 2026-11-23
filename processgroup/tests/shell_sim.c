/*
 * shell_sim.c - minimal stand-in for the Custom Shell.
 * Reads one command per line from stdin; a line starting with "processgroup"
 * is handed to processgroup_main(), exactly as the real shell would do.
 */
#include <stdio.h>
#include <string.h>
#include "processgroup.h"

#define MAX_WORDS 64

int main(void) {
    char line[1024];
    while (printf("shell> "), fflush(stdout), fgets(line, sizeof line, stdin) != NULL) {
        char *argv[MAX_WORDS + 1];
        int argc = 0;
        for (char *w = strtok(line, " \t\n"); w != NULL && argc < MAX_WORDS; w = strtok(NULL, " \t\n")) {
            argv[argc++] = w;
        }
        argv[argc] = NULL;
        if (argc == 0) {
            continue;
        }
        if (strcmp(argv[0], "exit") == 0) {
            break;
        }
        if (strcmp(argv[0], "processgroup") == 0) {
            int rc = processgroup_main(argc, argv);
            printf("[exit status %d]\n", rc);
        } else {
            printf("shell_sim: unknown command '%s'\n", argv[0]);
        }
    }
    return 0;
}
