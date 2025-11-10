SERVER_FILES = main.cpp TCPServer.cpp ServerLoop.cpp Protocol.cpp FrameDecoder.cpp OutboundQueue.cpp RequestTracker.cpp DiskWorker.cpp

CC = g++
CFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -g -pthread

OUTPUT = server.exe

.PHONY: default build run clean

default: build

run: build
	./$(OUTPUT)

build: $(OUTPUT)

$(OUTPUT): $(SERVER_FILES) TCPServer.h UniqueFd.h Connection.h ServerLoop.h Protocol.h FrameDecoder.h OutboundQueue.h RequestTracker.h DiskWorker.h
	$(CC) $(CFLAGS) $(SERVER_FILES) -o $(OUTPUT)

clean:
	rm -f $(OUTPUT)
