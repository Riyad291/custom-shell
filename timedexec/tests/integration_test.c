
#include <stdio.h>
#include "../timedexec.h"

int main(void)
{
    char *args1[] = {
        "timedexec", "-t", "3", "--", "echo", "First test", NULL
    };

    char *args2[] = {
        "timedexec", "-t", "3", "--", "echo", "Second test", NULL
    };

    int result1 = cmd_timedexec(6, args1);
    int result2 = cmd_timedexec(6, args2);

    if (result1 == 0 && result2 == 0) {
        printf("Integration test PASSED\n");
        return 0;
    }

    printf("Integration test FAILED\n");
    return 1;
}
