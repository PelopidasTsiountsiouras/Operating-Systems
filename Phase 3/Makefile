CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -O2
TARGET  = tinyshell
SRC     = tinyshell.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)
	