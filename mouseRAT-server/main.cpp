#include <iostream>
#include "TCPServer.h"

using namespace std;

int main() {

    TCPServer *server = new TCPServer(5555);

    if (!server->bindSocket()) return -1;
    if (!server->listenSocket()) return -1;

    int clientSocket = server->acceptConnection();

    char sendBuffer[200] = "\n";
    char receiveBuffer[4096] = "\n";
    int byteCount = 0;

    if (clientSocket >= 0) {

        // Write/read loop to clientSocket
        while (strcmp(sendBuffer, "quit") != 0) {

            //SEND
            cout << "\033[31m" << "Enter your command: " << "\033[0m";
            cin.getline(sendBuffer, 200);
            byteCount = send(clientSocket, sendBuffer, strlen(sendBuffer), 0);
            if (byteCount < 0) {
                cout << "Server: error sending data" << endl;
                return -1;
            } else {
                cout << "Server: sent " << byteCount << " bytes" << endl;
            }

            //RECEIVE
            byteCount = recv(clientSocket, receiveBuffer, 4096, 0);
            if (byteCount <= 0) {
                cout << "Server: error receiving data or connection closed" << endl;
                return -1;
            } else {
                receiveBuffer[byteCount] = '\0';

                cout << "Received data from "
          << server->getClientIP() << ":"
          << server->getClientPort()
          << " : " << "\033[32m" << receiveBuffer << "\033[0m" << endl;
            }

        }
        
        shutdown(clientSocket, SHUT_RDWR);
        close(clientSocket);
    }

    // serverSocket will be closed in the destructor
    return 0;

}