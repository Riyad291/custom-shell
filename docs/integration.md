# Integration contract

All the commands in this repo get merged into one program, `customshell`.
The shell runs each command as a function inside its own process, not as a
separate program. Everything below follows from that. Please read it before
you start writing, because fixing these things afterwards is a lot more
annoying than doing them now.

## The interface

Each command `foo` needs at least two files:

```
foo/foo.h     ->  int cmd_foo(int argc, char **argv);
foo/foo.c     ->  the code
```

The header has nothing in it except that one line and an include guard.

## The four rules

**1. No `main()` in the code you hand in.**

You still want to run your command by itself while you are working on it, so
put a `main` behind a macro at the bottom of your `.c` file:

```c
#ifdef STANDALONE
int main(int argc, char **argv) { return cmd_foo(argc, argv); }
#endif
```

Compile with `-DSTANDALONE` while you test. The leader compiles the same
file without it and gets an object file with no `main` in it.

**2. Never call `exit()`.**

`exit()` shuts down the whole shell, not just your command. Return a number
instead:

| Code | Meaning |
| --- | --- |
| 0 | it worked |
| 1 | usage error, or a file or system error |
| 2 | something specific to your command, write it in your man page |
| 130 | Ctrl+C |

**3. Set `optind = 1` at the top of your function.**

```c
int cmd_foo(int argc, char **argv)
{
    optind = 1;             /* getopt remembers where it stopped last time */
    ...
}
```

If you skip this, your command works the first time someone types it and
then parses nothing the second time. Everyone hits this one. It also stays
hidden until the commands are merged, which is usually the week nobody has
time for it.

**4. Nothing at file scope that holds a value between calls.**

The shell keeps running after your command returns, so any global or
`static` variable still has whatever you left in it last time. Keep your
state in local variables, or in one struct you pass around. The only
exception is a `volatile sig_atomic_t` flag for a signal handler, because
that has to be at file scope. If you have one, set it back to 0 at the top
of your function next to `optind`.

## Signals

If you install a handler, save the old one and put it back before you
return, so the shell gets its own behaviour back:

```c
struct sigaction sa, old;
/* ... fill in sa ... */
sigaction(SIGINT, &sa, &old);
/* ... do the work ... */
sigaction(SIGINT, &old, NULL);
```

## Error messages

Put your command name in front of every message, and send it to stderr:

```c
fprintf(stderr, "foo: %s: %s\n", path, strerror(errno));
```

Once five commands share one shell, a bare "No such file or directory" does
not tell anyone which command printed it.

## What you have to hand in

The assignment asks for source code, test cases and man pages, so your
folder should end up looking like this:

```
foo/
├── foo.c
├── foo.h
├── foo.1            man page, check it with: man ./foo.1
├── Makefile
└── tests/
    └── run_tests.sh
```

## A test that catches the merge bug

Write a small driver that calls your function twice in the same process and
check that both runs print the same thing. If they do not, something is
breaking rule 3 or rule 4. There is a working one in
`loganalyzer/tests/shell_sim.c` you can copy.
