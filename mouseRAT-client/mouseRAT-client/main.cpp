//#include "stdafx.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <iostream>
#include <chrono>
#include <stdexcept>
#include <system_error>
#include <string>
#include <limits>
#include "FrameAssembler.h"
#include "Handshake.h"

using namespace std;

void receiveRequests(SOCKET socket) {
    using Clock = std::chrono::steady_clock;
    protocol::FrameAssembler decoder;
    protocol::OutboundQueue outbound;
    std::optional<Clock::time_point> frameStarted;
    std::optional<Clock::time_point> pongDeadline;
    auto lastFrame = Clock::now();
    Clock::time_point requestStarted{};
    std::size_t terminalRemaining = 0, pongRemaining = 0;
    std::uint64_t pingSequence = 0;
    std::array<char, 8> pingNonce{};
    std::uint32_t lastRequest = 0;
    std::array<char, 4096> input{};

    const auto sendPending = [&] {
        const auto sent = outbound.sendTo(socket);
        terminalRemaining -= (std::min)(terminalRemaining, sent);
        pongRemaining -= (std::min)(pongRemaining, sent);
    };

    while (true) {
        const auto checkDeadlines = [&] {
            const auto now = Clock::now();
            if (frameStarted && now >= *frameStarted + std::chrono::seconds(30)) {
                throw std::runtime_error("Frame timeout");
            }

            if (terminalRemaining != 0 && now >= requestStarted + std::chrono::minutes(5)) {
                throw std::runtime_error("Request response timeout");
            }

            if (pongDeadline && now >= *pongDeadline) {
                throw std::runtime_error("Heartbeat timeout");
            }

            if (!pongDeadline && now >= lastFrame + std::chrono::seconds(10)) {
                if (pingSequence == (std::numeric_limits<std::uint64_t>::max)()) {
                    throw std::runtime_error("Heartbeat nonce space exhausted");
                }

                ++pingSequence;
                for (std::size_t index = 0; index < pingNonce.size(); ++index) {
                    pingNonce[index] = static_cast<char>(pingSequence >> (56 - 8 * index));
                }

                if (!outbound.enqueue({ 0x30, 8, 0 }, { pingNonce.data(), pingNonce.size() })) {
                    throw std::runtime_error("Heartbeat queue is full");
                }

                pongDeadline = Clock::now() + std::chrono::seconds(30);
            }
        };
        checkDeadlines();
        fd_set readable{}, writable{};
        FD_SET(socket, &readable);
        if (!outbound.empty()) {
            FD_SET(socket, &writable);
        }

        timeval timeout{ 0, 100000 };
        if (select(0, &readable, &writable, nullptr, &timeout) == SOCKET_ERROR) {
            const auto error = WSAGetLastError();
            throw std::system_error(error, std::system_category(), "select");
        }

        checkDeadlines();
        if (!FD_ISSET(socket, &readable)) {
            if (FD_ISSET(socket, &writable)) {
                sendPending();
            }

            continue;
        }

        const auto received = recv(socket, input.data(), static_cast<int>(input.size()), 0);
        if (received == SOCKET_ERROR) {
            const auto error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) {
                continue;
            }

            throw std::system_error(error, std::system_category(), "recv");
        }

        if (received == 0) {
            if (decoder.hasPartialFrame() || !outbound.empty()) {
                throw std::runtime_error("Disconnected during an incomplete exchange");
            }

            return;
        }

        const auto arrived = Clock::now();
        std::string_view bytes(input.data(), static_cast<std::size_t>(received));
        while (!bytes.empty()) {
            if (!decoder.hasPartialFrame()) {
                frameStarted = arrived;
            }

            std::size_t consumed;
            const auto frame = decoder.consume(bytes, consumed);
            bytes.remove_prefix(consumed);
            checkDeadlines();
            if (!frame) {
                continue;
            }

            frameStarted.reset();
            lastFrame = Clock::now();
            if (frame->header.type == 0x30) {
                if (pongRemaining != 0) {
                    throw std::invalid_argument("Overlapping PING messages");
                }

                if (!outbound.enqueue({ 0x31, 8, 0 }, frame->payload)) {
                    throw std::runtime_error("Heartbeat queue is full");
                }

                pongRemaining = outbound.size();
                continue;
            }

            if (frame->header.type == 0x31) {
                if (!pongDeadline || frame->payload != std::string_view(pingNonce.data(), pingNonce.size())) {
                    throw std::invalid_argument("Unsolicited or mismatched PONG");
                }

                pongDeadline.reset();
                continue;
            }

            if (frame->header.type != 0x10 && frame->header.type != 0x20) {
                throw std::invalid_argument("Unexpected session message");
            }

            if (terminalRemaining != 0 || frame->header.requestId != lastRequest + 1) {
                throw std::invalid_argument("Overlapping request or invalid request ID sequence");
            }

            lastRequest = frame->header.requestId;
            const std::string error = std::string(3, '\0') + "\1Request is not supported";
            if (!outbound.enqueue({ 0x7f, static_cast<std::uint32_t>(error.size()), lastRequest }, error)) {
                throw std::runtime_error("Outbound queue is full");
            }

            terminalRemaining = outbound.size();
            requestStarted = Clock::now();
        }

        if (FD_ISSET(socket, &writable)) {
            sendPending();
        }
    }
}

int main(int argc, char* argv[]) {

    const char* SERVER_IP = argc > 1 ? argv[1] : "192.168.100.6";
	const int SERVER_PORT = 5555;

	// Initialize Winsock dll
	WSADATA wsaData;
	int wsaerr;
	WORD wVersionRequested = MAKEWORD(2, 2);
	wsaerr = WSAStartup(wVersionRequested, &wsaData);

	if (wsaerr != 0) {
		cout << "The Winsock dll not found!" << endl;
        return 1;
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
        return 1;
	} else {
		cout << "socket() is OK!" << endl;
	}

    int result = 1;
    try {
        sockaddr_in server{};
        server.sin_family = AF_INET;
        server.sin_port = htons(SERVER_PORT);
        if (InetPtonA(AF_INET, SERVER_IP, &server.sin_addr) != 1) {
            throw std::invalid_argument("Server address must be an IPv4 address");
        }

        if (connect(clientSocket, reinterpret_cast<const sockaddr*>(&server), sizeof(server)) == SOCKET_ERROR) {
            const auto error = WSAGetLastError();
            throw std::system_error(error, std::system_category(), "connect");
        }

        const auto connectedAt = std::chrono::steady_clock::now();
        u_long nonblocking = 1;
        if (ioctlsocket(clientSocket, FIONBIO, &nonblocking) != 0) {
            const auto error = WSAGetLastError();
            throw std::system_error(error, std::system_category(), "ioctlsocket");
        }

        protocol::performHandshake(clientSocket, connectedAt);
        cout << "Handshake complete\n";
        receiveRequests(clientSocket);
        result = 0;
    } catch (const std::exception& error) {
        cerr << error.what() << '\n';
    }

    closesocket(clientSocket);
    WSACleanup();
    return result;
}
