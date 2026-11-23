/*
 * processgroup.h - interface of the processgroup command
 *
 * The Custom Shell calls processgroup_main() exactly as it would call
 * main(): argv[0] is "processgroup", argv[1] the subcommand, and argv must
 * be NULL-terminated. Returns 0 on success, 1 on a runtime error and 2 on a
 * usage error. Compile processgroup.c with -DPROCESSGROUP_NO_MAIN when
 * linking it into the shell.
 */
#ifndef PROCESSGROUP_H
#define PROCESSGROUP_H

int processgroup_main(int argc, char *argv[]);

#endif /* PROCESSGROUP_H */
