# timedexec

Execute a command with wall-clock, CPU-time and memory limits. Part of the CSC 332 Custom Shell group project.

## Build and test

```bash
make            # builds ./timedexec
make test       # runs 16 automated checks and an integration test
make man        # shows the man page
```

## Usage

```bash
timedexec [-t SECS] [-c SECS] [-m MB] [-v] [-s] -- COMMAND [ARG...]
timedexec -h
```

At least one resource limit is required.

| Option | Purpose |
| --- | --- |
| `-t SECS` | Wall-clock timeout |
| `-c SECS` | CPU-time limit |
| `-m MB` | Memory address-space limit |
| `-v` | Show PID, limits and elapsed time |
| `-s` | Show CPU usage and peak resident memory (KiB) |
| `-h` | Display help |

Quick demo:

```bash
./timedexec -t 5 -- sleep 1                         # normal execution
./timedexec -t 2 -- sleep 10                        # wall-clock timeout
./timedexec -c 1 -- sh -c 'while :; do :; done'       # CPU limit
./timedexec -m 50 -- python3 -c 'a = bytearray(100 * 1024 * 1024)' # memory limit
./timedexec -v -s -t 5 -c 2 -m 100 -- sleep 1        # combined limits and statistics
```

CPU and memory limits apply per process. The memory limit controls virtual address space, not resident memory.

## Files

| **File** | **Purpose** |
| --- | --- |
| timedexec.c | implementation (`cmd_timedexec` + stand-alone `main`) |
| timedexec.h | interface used by the Custom Shell |
| timedexec.1 | man page |
| Makefile | build, test, man, clean |
| tests/run_tests.sh | automated test cases |
| tests/integration_test.c | tests repeated calls to `cmd_timedexec()` |

## Merging into the Custom Shell

Include `timedexec.h`, compile `timedexec.c` without `-DSTANDALONE` (or run `make timedexec.o`), and link it with the shell. Call `cmd_timedexec(argc, argv)` with a NULL-terminated `argv` whose first word is `timedexec`.

Return values: 0 success, 1 invalid arguments or system error, 124 wall-clock timeout, 125 child setup error, 127 execution failure, 128+N termination by signal N. Otherwise, the child's exit status is returned.

Linux only. Uses `fork()`, `execvp()`, `setrlimit()`, `wait4()`, `sigaction()` and `clock_gettime()`. Automated tests require Bash and Python 3.
