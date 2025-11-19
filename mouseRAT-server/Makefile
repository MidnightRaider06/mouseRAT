SERVER_FILES = $(addprefix src/, main.cpp TCPServer.cpp ServerLoop.cpp Protocol.cpp FrameDecoder.cpp OutboundQueue.cpp RequestTracker.cpp DiskWorker.cpp ScreenshotStore.cpp ScreenshotTransfers.cpp)
HEADERS = $(wildcard include/*.h)

CC = g++
CPPFLAGS = -Iinclude
CFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -g -pthread

OUTPUT = build/server

.PHONY: default build run clean

default: build

run: build
	./$(OUTPUT)

build: $(OUTPUT)

$(OUTPUT): $(SERVER_FILES) $(HEADERS) Makefile
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SERVER_FILES) -o $(OUTPUT)

clean:
	rm -f $(OUTPUT)
