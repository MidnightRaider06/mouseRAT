//#include "stdafx.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <iostream>

using namespace std;

int main() {
	cout << "Doing things in Visual Studio" << endl;

	const wchar_t* SERVER_IP = L"192.168.100.6";
	const int SERVER_PORT = 5555;

	// Initialize Winsock dll
	WSADATA wsaData;
	int wsaerr;
	WORD wVersionRequested = MAKEWORD(2, 2);
	wsaerr = WSAStartup(wVersionRequested, &wsaData);

	if (wsaerr != 0) {
		cout << "The Winsock dll not found!" << endl;
		return 0;
	}
	else {
		cout << "The winsock dll found!" << endl;
		cout << "The status : " << wsaData.szSystemStatus << endl;
	}

	// Create socket to connect to the server
	SOCKET clientSocket = INVALID_SOCKET;
	clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (clientSocket == INVALID_SOCKET) {
		cout << "Error at socket() : " << WSAGetLastError() << endl;
		WSACleanup();
		return 0;
	} else {
		cout << "socket() is OK!" << endl;
	}

	// Attempt to connect the socket to the server
	sockaddr_in clientService;
	clientService.sin_family = AF_INET;
	InetPton(AF_INET, SERVER_IP, &clientService.sin_addr.s_addr);
	clientService.sin_port = htons(SERVER_PORT);

	// Will wait 75 seconds for server to respond.
	if (connect(clientSocket, (SOCKADDR*)&clientService, sizeof(clientService)) == SOCKET_ERROR) {
		cout << "Client: connect() failed to connect." << endl;
		WSACleanup();
		return 0;
	}
	else {
		cout << "Client: connect() is OK." << endl;
		cout << "Client: Can start sending and receiving data�" << endl;
	}

	closesocket(clientSocket);
	WSACleanup();

	
}