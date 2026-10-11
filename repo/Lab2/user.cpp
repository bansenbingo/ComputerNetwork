// Lab 2 client: interactive TCP client with a receive thread and message queue.
// Build with MinGW: g++ -std=c++17 -Wall -Wextra user.cpp -o user.exe -lws2_32

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <conio.h>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <chrono>

#ifdef _MSC_VER
#pragma comment(lib, "Ws2_32.lib")
#endif

namespace {
constexpr std::uint32_t kMaxBody = 64 * 1024;
constexpr int kServerPort = 4669; // server port from the student's ID
constexpr std::uint32_t kRequestTime = 1;
constexpr std::uint32_t kRequestName = 2;
constexpr std::uint32_t kRequestList = 3;
constexpr std::uint32_t kRequestMessage = 4;
constexpr std::uint32_t kResponseTime = 101;
constexpr std::uint32_t kResponseName = 102;
constexpr std::uint32_t kResponseList = 103;
constexpr std::uint32_t kForwardMessage = 104;
constexpr std::uint32_t kResponseMessageAck = 105;
constexpr std::uint32_t kGreeting = 106;
constexpr std::uint32_t kResponseError = 199;
constexpr int kHeaderSize = 12; // type, request id, body length

struct Packet {
    std::uint32_t type = 0;
    std::uint32_t requestId = 0;
    std::string body;
};

bool sendAll(SOCKET socket, const char* data, int length) {
    int sent = 0;
    while (sent < length) {
        const int count = send(socket, data + sent, length - sent, 0);
        if (count == SOCKET_ERROR || count == 0) return false;
        sent += count;
    }
    return true;
}

bool recvExact(SOCKET socket, char* data, int length) {
    int received = 0;
    while (received < length) {
        const int count = recv(socket, data + received, length - received, 0);
        if (count == SOCKET_ERROR || count == 0) return false;
        received += count;
    }
    return true;
}

bool sendPacket(SOCKET socket, const Packet& packet) {
    if (packet.body.size() > kMaxBody) return false;
    std::uint32_t header[kHeaderSize / sizeof(std::uint32_t)] = {
        htonl(packet.type), htonl(packet.requestId),
        htonl(static_cast<std::uint32_t>(packet.body.size()))};
    if (!sendAll(socket, reinterpret_cast<const char*>(header), kHeaderSize)) return false;
    return packet.body.empty() || sendAll(socket, packet.body.data(), static_cast<int>(packet.body.size()));
}

bool recvPacket(SOCKET socket, Packet& packet) {
    std::uint32_t header[kHeaderSize / sizeof(std::uint32_t)]{};
    if (!recvExact(socket, reinterpret_cast<char*>(header), kHeaderSize)) return false;
    if (ntohl(header[2]) > kMaxBody) return false;
    packet.type = ntohl(header[0]);
    packet.requestId = ntohl(header[1]);
    const auto bodyLength = ntohl(header[2]);
    packet.body.assign(bodyLength, '\0');
    return bodyLength == 0 || recvExact(socket, packet.body.data(), static_cast<int>(bodyLength));
}

struct QueuedMessage {
    Packet packet;
};

std::mutex queueMutex;
std::queue<QueuedMessage> messages;
std::atomic<bool> stopReceiver{false};
std::atomic<bool> connected{false};
std::atomic<bool> disconnectedEvent{false};
std::atomic<bool> clientListFetched{false};
std::atomic<std::uint32_t> waitingRequestId{0};
std::atomic<bool> waitingRequestDone{false};
SOCKET clientSocket = INVALID_SOCKET;
std::thread receiverThread;
std::uint32_t nextRequestId = 1;

void enqueue(Packet packet) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        messages.push({std::move(packet)});
    }
}

void receiverLoop() {
    Packet packet;
    while (!stopReceiver.load() && recvPacket(clientSocket, packet)) {
        enqueue(std::move(packet));
        packet = Packet{};
    }
    if (!stopReceiver.load()) {
        disconnectedEvent = true;
        enqueue({0, 0, "server connection closed"});
    }
}

void drainMessages() {
    std::queue<QueuedMessage> local;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        std::swap(local, messages);
    }
    while (!local.empty()) {
        const Packet& packet = local.front().packet;
        if (packet.requestId != 0 && packet.requestId == waitingRequestId.load()) {
            waitingRequestDone = true;
        }
        switch (packet.type) {
        case kResponseTime:
            std::cout << "[server time] " << packet.body << "\n";
            break;
        case kResponseName:
            std::cout << "[server name] " << packet.body << "\n";
            break;
        case kResponseList:
            std::cout << "[active clients]\n" << packet.body;
            clientListFetched = true;
            break;
        case kForwardMessage:
            std::cout << "[forwarded message]\n" << packet.body << "\n";
            break;
        case kResponseMessageAck:
            std::cout << "[server] " << packet.body << "\n";
            break;
        case kResponseError:
            std::cout << "[server error] " << packet.body << "\n";
            break;
        case kGreeting:
            std::cout << "[server] " << packet.body << "\n";
            break;
        case 0:
            std::cout << "[connection] " << packet.body << "\n";
            break;
        default:
            std::cout << "[server packet type=" << packet.type << "] " << packet.body << "\n";
            break;
        }
        local.pop();
    }
    if (disconnectedEvent.exchange(false)) {
        connected = false;
        clientListFetched = false;
    }
}

// Read a console line without blocking the main event loop. While the user is
// typing, the main thread continues draining messages from the receive thread.
std::string readLineWithEvents(const std::string& prompt) {
    std::cout << prompt << std::flush;
    std::string line;
    while (true) {
        drainMessages();
        if (_kbhit()) {
            const int first = _getch();
            if (first == '\r' || first == '\n') {
                std::cout << '\n';
                return line;
            }
            if (first == '\b') {
                if (!line.empty()) {
                    line.pop_back();
                    std::cout << "\b \b" << std::flush;
                }
                continue;
            }
            if (first == 0 || first == 224) {
                // Consume the second byte of an extended key (arrows, function keys, ...).
                (void)_getch();
                continue;
            }
            line.push_back(static_cast<char>(first));
            std::cout << static_cast<char>(first) << std::flush;
        } else {
            Sleep(30);
        }
    }
}

std::uint32_t sendRequest(std::uint32_t type, const std::string& body = std::string()) {
    if (!connected.load()) {
        std::cout << "Connect first.\n";
        return 0;
    }
    const std::uint32_t requestId = nextRequestId++;
    waitingRequestId = requestId;
    waitingRequestDone = false;
    const Packet request{type, requestId, body};
    if (!sendPacket(clientSocket, request)) {
        std::cerr << "send failed: " << WSAGetLastError() << "\n";
        waitingRequestId = 0;
        return 0;
    }
    return requestId;
}

void waitForResponse(std::uint32_t requestId) {
    if (requestId == 0) return;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (connected.load() && !waitingRequestDone.load() && std::chrono::steady_clock::now() < deadline) {
        drainMessages();
        Sleep(20);
    }
    drainMessages();
    if (!waitingRequestDone.load() && connected.load()) {
        std::cout << "Timed out waiting for the server response.\n";
    }
    waitingRequestId = 0;
    waitingRequestDone = false;
}

void stopConnection() {
    if (!connected.load() && !receiverThread.joinable()) return;
    stopReceiver = true;
    if (clientSocket != INVALID_SOCKET) {
        shutdown(clientSocket, SD_BOTH);
    }
    if (receiverThread.joinable()) receiverThread.join();
    if (clientSocket != INVALID_SOCKET) {
        closesocket(clientSocket);
        clientSocket = INVALID_SOCKET;
    }
    connected = false;
    clientListFetched = false;
    stopReceiver = false;
    disconnectedEvent = false;
    waitingRequestId = 0;
    waitingRequestDone = false;
    std::cout << "Disconnected.\n";
}

bool connectToServer() {
    if (connected.load()) {
        std::cout << "Already connected.\n";
        return true;
    }
    const std::string host = readLineWithEvents("Server IP or host: ");
    const int port = kServerPort;
    std::cout << "Using server port " << port << " (student ID last four digits).\n";
    clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (clientSocket == INVALID_SOCKET) {
        std::cerr << "socket failed: " << WSAGetLastError() << "\n";
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(port));
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
            std::cerr << "Could not resolve server address.\n";
            closesocket(clientSocket);
            clientSocket = INVALID_SOCKET;
            return false;
        }
        address.sin_addr = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
        freeaddrinfo(result);
    }
    if (connect(clientSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        std::cerr << "connect failed: " << WSAGetLastError() << "\n";
        closesocket(clientSocket);
        clientSocket = INVALID_SOCKET;
        return false;
    }
    connected = true;
    clientListFetched = false;
    stopReceiver = false;
    disconnectedEvent = false;
    waitingRequestId = 0;
    waitingRequestDone = false;
    receiverThread = std::thread(receiverLoop);
    std::cout << "Connected to " << host << ':' << port << ".\n";
    return true;
}

void printMenu() {
    std::cout << "\n========== Lab2 TCP client ==========" << '\n'
              << "1. Connect\n"
              << "2. Disconnect\n"
              << "3. Get server time\n"
              << "4. Get server name\n"
              << "5. List active clients\n"
              << "6. Send message\n"
              << "7. Exit\n";
}

} // namespace

int main() {
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed.\n";
        return 1;
    }
    std::cout << "Lab2 TCP client. The receive thread runs after connection.\n";
    bool quitting = false;
    while (!quitting) {
        drainMessages();
        printMenu();
        const std::string choiceText = readLineWithEvents("Select: ");
        int choice = 0;
        try { choice = std::stoi(choiceText); } catch (...) { choice = 0; }
        if (!connected.load() && choice != 1 && choice != 7) {
            std::cout << "Not connected: only Connect and Exit are available.\n";
            continue;
        }
        switch (choice) {
        case 1:
            connectToServer();
            break;
        case 2:
            stopConnection();
            break;
        case 3:
            waitForResponse(sendRequest(kRequestTime));
            break;
        case 4:
            waitForResponse(sendRequest(kRequestName));
            break;
        case 5:
            waitForResponse(sendRequest(kRequestList));
            break;
        case 6: {
            if (!clientListFetched.load()) {
                std::cout << "Retrieve the active client list with option 5 before sending a message.\n";
                break;
            }
            const std::string target = readLineWithEvents("Target client number (use option 5 first): ");
            int targetId = 0;
            try { targetId = std::stoi(target); } catch (...) { targetId = 0; }
            const std::string message = readLineWithEvents("Message: ");
            waitForResponse(sendRequest(kRequestMessage, std::to_string(targetId) + "\n" + message));
            break;
        }
        case 7:
            stopConnection();
            quitting = true;
            break;
        default:
            std::cout << "Unknown menu choice.\n";
            break;
        }
        // Allow queued responses and unsolicited forwarded messages to be shown promptly.
        for (int i = 0; i < 5; ++i) {
            Sleep(20);
            drainMessages();
        }
    }
    stopConnection();
    WSACleanup();
    return 0;
}
