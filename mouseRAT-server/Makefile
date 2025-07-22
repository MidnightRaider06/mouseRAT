SERVER_FILES = main.cpp TCPServer.cpp

CC = g++
CFLAGS = -Wall -Wextra -g

OUTPUT = server.exe

default: build
	./$(OUTPUT)

build: $(SERVER_FILES)
	$(CC) $(CFLAGS) $(SERVER_FILES) -o $(OUTPUT)

clean:
	rm -f $(OUTPUT)