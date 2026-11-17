# Custom Shell

Operating System I group project, Group 6.

Custom Linux commands for resource management, IPC and performance, merged
into one program called `customshell`.

## Who is building what

| Command | System calls | Owner | Status |
| --- | --- | --- | --- |
| loganalyzer | `mmap`, `madvise`, `memchr` | Yeasin | done, tested |
| | | | |
| | | | |
| | | | |
| customshell (merge) | `fork`, `exec` | | |

Put your name in the table and push it, so two people do not start the same
command.

## Layout

```
custom-shell/
├── docs/integration.md   read this before you write any code
├── _template/            copy this to start your command
├── shell/                where the merge happens
└── <command>/            one folder per command
    ├── <command>.c/.h
    ├── <command>.1       man page
    ├── Makefile
    └── tests/
```

## Build

```sh
make          # build every command
make test     # run every command's tests
make clean
```

Each folder also builds on its own if you run `make` inside it.

## Before you start writing

Read `docs/integration.md`. The short version: the shell calls your command
as a function, so no `main()`, no `exit()`, set `optind = 1` when your
function starts, and do not leave anything in globals. It takes a minute to
do up front and a long evening to fix later.

## What we have to hand in

Source code, test cases and man pages. Whoever owns a command does all three
for it, and demos it live during the presentation.

## Do not commit

Build output and test files are already in `.gitignore`. The test logs run
into hundreds of megabytes and GitHub rejects anything over 100 MB, so do
not push a `*.log`. Generate them on your own machine instead:

```sh
cd loganalyzer && ./tests/gen_log.sh 5000000 tests/big.log
```
