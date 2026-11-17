# _template

A starting point for a command. Copy the folder, rename it, rename
`cmd_command` to `cmd_<yourname>`, change `PROG`, and fill in the part that
is marked.

    cp -r _template timedexec
    cd timedexec
    # rename the files and the function, then:
    make && ./timedexec -h

The top-level `make` skips this folder.

Read `../docs/integration.md` before you start. The template already follows
all four rules, so if you build on it you do not have to think about them.
