# loganalyzer - Operating System I group project (Custom Shell), Group 6

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=gnu11
TARGET  = loganalyzer

all: $(TARGET)

# standalone binary, for developing and testing on its own
$(TARGET): loganalyzer.c loganalyzer.h
	$(CC) $(CFLAGS) -DSTANDALONE -o $(TARGET) loganalyzer.c

# object file for the team leader to link into the Custom Shell (no main)
loganalyzer.o: loganalyzer.c loganalyzer.h
	$(CC) $(CFLAGS) -c -o loganalyzer.o loganalyzer.c

test: $(TARGET)
	./tests/run_tests.sh

man:
	man ./loganalyzer.1

clean:
	rm -f $(TARGET) loganalyzer.o tests/*.log

.PHONY: all test man clean
