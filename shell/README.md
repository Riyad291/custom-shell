# shell

The leader's folder. `shell.c` goes here: the loop that prints a prompt,
reads a line, splits it into `argv` and calls the right `cmd_*` function.

Nothing here yet.

## Rough idea of the dispatch table

```c
#include "../loganalyzer/loganalyzer.h"
/* ... one include per command ... */

struct builtin {
    const char *name;
    int (*fn)(int argc, char **argv);
};

static const struct builtin builtins[] = {
    { "loganalyzer", cmd_loganalyzer },
    /* ... */
    { NULL, NULL }
};
```

Every command follows `../docs/integration.md`, so they are all safe to call
more than once in the same process.
