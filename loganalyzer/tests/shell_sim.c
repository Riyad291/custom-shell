/*
 * shell_sim.c - stand-in for the Custom Shell, used by test 12.
 *
 * Calls cmd_loganalyzer() twice inside one process, exactly as the real
 * shell will. If getopt's optind or any other state is not reset between
 * calls, the second run produces different output and the test fails.
 *
 *   gcc -Wall -Wextra -O2 -o tests/shell_sim tests/shell_sim.c loganalyzer.c
 */

#include <stdio.h>
#include "../loganalyzer.h"

int main(void)
{
    char *av[] = { "loganalyzer", "-f", "tests/small.log", "-t", "3", NULL };
    int ac = 5;
    int a, b;

    printf("=== first call ===\n");
    a = cmd_loganalyzer(ac, av);

    printf("\n=== second call ===\n");
    b = cmd_loganalyzer(ac, av);

    printf("\nexit codes: %d %d\n", a, b);
    return (a == b) ? 0 : 1;
}
