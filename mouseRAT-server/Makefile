SERVER_FILES = main.cpp TCPServer.cpp

CC = g++
CFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -g

OUTPUT = server.exe

default: build
	./$(OUTPUT)

build: $(SERVER_FILES) TCPServer.h UniqueFd.h
	$(CC) $(CFLAGS) $(SERVER_FILES) -o $(OUTPUT)

clean:
	rm -f $(OUTPUT)