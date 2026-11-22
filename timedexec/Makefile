
CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=gnu11

all: timedexec

timedexec: timedexec.c timedexec.h
	$(CC) $(CFLAGS) -DSTANDALONE -o timedexec timedexec.c

timedexec.o: timedexec.c timedexec.h
	$(CC) $(CFLAGS) -c -o timedexec.o timedexec.c

test: timedexec timedexec.o
	bash tests/run_tests.sh
	$(CC) $(CFLAGS) -o tests/integration_test tests/integration_test.c timedexec.o
	./tests/integration_test

man:
	man ./timedexec.1

clean:
	rm -f timedexec timedexec.o tests/integration_test

.PHONY: all test man clean
