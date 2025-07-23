#include <iostream>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <cstring>

using namespace std;

#pragma once
class CommandExecutor {

private:
	HANDLE hStdinRead;
	HANDLE hStdinWrite;
	HANDLE hStdoutRead;
	HANDLE hStdoutWrite;

public:
	CommandExecutor();
	~CommandExecutor();
	int initPipes();
	int closePipes();
	int startTerminal();
	string executeCommand(const string& command);
};

