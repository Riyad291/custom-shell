/*
 * loganalyzer.h - public interface for the loganalyzer command
 *
 * Operating System I, Group Project (Custom Shell), Group 6
 *
 * The shell calls cmd_loganalyzer() directly, in its own process.
 * The command therefore never calls exit() and never relies on
 * global state surviving (or not surviving) between calls.
 *
 * Returns: 0 on success
 *          1 on a usage error or a file/system error
 *        130 if the scan was interrupted by SIGINT
 */

#ifndef LOGANALYZER_H
#define LOGANALYZER_H

int cmd_loganalyzer(int argc, char **argv);

#endif /* LOGANALYZER_H */
