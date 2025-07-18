#include <iostream>
#include <cstring>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h> 

using namespace std;

int main() {
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        cout << "Unable to open socket" << endl;
        return(EXIT_FAILURE);
    } else {
        cout << "socket " << serverSocket << " opened!" << endl;
    }

    sockaddr_in service;
    memset(&service, 0, sizeof(service));
    service.sin_family = AF_INET;
    int port = 5555;
    service.sin_port = htons(port);
    service.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverSocket, (struct sockaddr*)&service, sizeof(service)) < 0) {
        cout << "bind() failed" << endl;
        shutdown(serverSocket, SHUT_RDWR);
        return 0;
    } else {
        cout << "bind() is OK!" << endl;
    }

    if (listen(serverSocket, 1) < 0) {
        cout << "listen() failed" << endl;
        shutdown(serverSocket, SHUT_RDWR);
        return 0;
    } else {
        cout << "listen() is OK, I'm waiting for connections…" << endl;
    }

    int acceptSocket = accept(serverSocket, NULL, NULL);
    if (acceptSocket < 0) {
        cout << "accept() failed" << endl;
        shutdown(serverSocket, SHUT_RDWR);
        return -1;
    } else {
        cout << "accept() is OK!" << endl;
    }

    // Close sockets after use
    shutdown(acceptSocket, SHUT_RDWR);
    shutdown(serverSocket, SHUT_RDWR);

    return 0;
}