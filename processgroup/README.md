# processgroup

Launch, monitor and control a batch of processes as one process group.
Part of the CSC 332 Custom Shell group project.

## Build and test

    make            # builds ./processgroup
    make test       # runs 60+ automated checks (tests/run_tests.sh)
    make man        # shows the man page

## Usage

    processgroup start [-q] [-d] [-n N] [--] COMMAND [ARG...]
    processgroup start [-q] [-d] -f FILE
    processgroup status PGID
    processgroup monitor [-i SECS] [-c COUNT] PGID
    processgroup signal PGID SIGNAL
    processgroup terminate [-w SECS] PGID

Quick demo:

    ./processgroup start -n 5 -- sleep 300     # prints the group id
    ./processgroup status <PGID>
    ./processgroup signal <PGID> STOP
    ./processgroup signal <PGID> CONT
    ./processgroup monitor -c 5 <PGID>
    ./processgroup terminate <PGID>

Use `-qd` with `start` when capturing the id: `PGID=$(./processgroup start -qd ...)`.

## Files

| File | Purpose |
|------|---------|
| processgroup.c | implementation (`processgroup_main` + stand-alone `main`) |
| processgroup.h | interface used by the Custom Shell |
| processgroup.1 | man page |
| Makefile | build, test, man, clean |
| tests/run_tests.sh | automated test cases |
| tests/shell_sim.c | tiny shell that calls `processgroup_main()` to test integration |

## Merging into the Custom Shell

Include `processgroup.h`, compile `processgroup.c` with `-DPROCESSGROUP_NO_MAIN`,
and call `processgroup_main(argc, argv)` with a NULL-terminated `argv` whose
first word is `processgroup`. Return values: 0 ok, 1 runtime error, 2 usage error.

Linux only (reads `/proc`).
