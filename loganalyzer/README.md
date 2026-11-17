# loganalyzer

Operating System I group project (Custom Shell), Group 6.
Written by Yeasin Arafat Riyad.

Parses big log files with `mmap(2)` and prints statistics about them.

## Build

    make                # standalone binary, for testing it on its own
    make loganalyzer.o  # object file for the shell, no main in it

## Run

    ./loganalyzer -f FILE [-p PATTERN] [-l LEVEL] [-t N] [-k ip|path] [-c] [-v] [-h]

## Test

    ./tests/gen_log.sh 5000000 tests/big.log   # about 370 MB, takes ~25 s
    ./tests/run_tests.sh                       # 22 checks
    man ./loganalyzer.1

## For the leader

Link `loganalyzer.o` and call:

    #include "loganalyzer.h"
    int cmd_loganalyzer(int argc, char **argv);

There is no `main()` in it (that is behind `#ifdef STANDALONE`), it never
calls `exit()`, it sets `optind = 1` when it starts, and it puts the old
SIGINT handler back before it returns. Returns 0 if it worked, 1 for a usage
or file error, 130 if someone hits Ctrl+C.
