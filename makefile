CC = gcc
CFLAGS = -Wall -pthread -g
TARGET = procx

all: $(TARGET)

$(TARGET): procx.c
	$(CC) $(CFLAGS) -o $(TARGET) procx.c

clean:
	rm -f $(TARGET)
