CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=gnu11
TARGET  = command

all: $(TARGET)

$(TARGET): command.c command.h
	$(CC) $(CFLAGS) -DSTANDALONE -o $(TARGET) command.c

command.o: command.c command.h
	$(CC) $(CFLAGS) -c -o command.o command.c

clean:
	rm -f $(TARGET) command.o

.PHONY: all clean
