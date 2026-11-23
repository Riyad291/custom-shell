CC      = gcc
CFLAGS  = -Wall -Wextra -O2

all: processgroup

processgroup: processgroup.c processgroup.h
	$(CC) $(CFLAGS) -o $@ processgroup.c

# Tiny stand-in for the Custom Shell: proves processgroup_main() links and
# runs when called from another program.
tests/shell_sim: tests/shell_sim.c processgroup.c processgroup.h
	$(CC) $(CFLAGS) -DPROCESSGROUP_NO_MAIN -I. -o $@ tests/shell_sim.c processgroup.c

test: processgroup tests/shell_sim
	./tests/run_tests.sh ./processgroup

man:
	man ./processgroup.1

clean:
	rm -f processgroup tests/shell_sim

.PHONY: all test man clean
