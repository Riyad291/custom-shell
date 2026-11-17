/*
 * command.c - template for a Custom Shell command
 *
 * Follows the contract in ../docs/integration.md:
 *   no main() outside STANDALONE, no exit(), optind reset, no live globals.
 *
 * Build standalone:  gcc -DSTANDALONE -Wall -Wextra -O2 -o command command.c
 * Build for shell:   gcc -Wall -Wextra -O2 -c command.c
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <getopt.h>

#include "command.h"

#define PROG "command"          /* <-- rename */

/* A signal handler can only touch a file-scope object. Reset it on entry. */
static volatile sig_atomic_t stop_flag = 0;

static void on_sigint(int sig)
{
    (void)sig;
    stop_flag = 1;
}

static void usage(FILE *out)
{
    fprintf(out,
        "Usage: " PROG " -f FILE [-v] [-h]\n"
        "\n"
        "  -f FILE   input file (required)\n"
        "  -v        verbose\n"
        "  -h        show this help\n");
}

int cmd_command(int argc, char **argv)
{
    struct sigaction sa, old_sa;
    const char *path = NULL;
    int verbose = 0, c, rc = 0, have_old = 0;

    /* Rule 3 and rule 4: both of these survive between calls. */
    optind = 1;
    stop_flag = 0;

    while ((c = getopt(argc, argv, "f:vh")) != -1) {
        switch (c) {
        case 'f': path = optarg; break;
        case 'v': verbose = 1;   break;
        case 'h': usage(stdout); return 0;
        default:  usage(stderr); return 1;       /* rule 2: return, not exit */
        }
    }

    if (path == NULL) {
        fprintf(stderr, PROG ": no input file, use -f FILE\n");
        usage(stderr);
        return 1;
    }

    /* Catch Ctrl+C, and hand the terminal back on the way out. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, &old_sa) == 0)
        have_old = 1;

    /* ------------------------------------------------------------------
     * Your work goes here. Check every system call:
     *
     *     if (something() == -1) {
     *         fprintf(stderr, PROG ": %s: %s\n", path, strerror(errno));
     *         rc = 1;
     *         goto done;
     *     }
     *
     * In any long loop, check the stop flag every so often:
     *
     *     if (stop_flag) { rc = 130; goto done; }
     * ------------------------------------------------------------------ */

    if (verbose)
        printf(PROG ": would process %s\n", path);

    if (have_old)
        sigaction(SIGINT, &old_sa, NULL);

    return rc;
}

#ifdef STANDALONE
int main(int argc, char **argv)
{
    return cmd_command(argc, argv);
}
#endif
