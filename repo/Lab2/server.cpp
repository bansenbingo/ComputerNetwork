// Lab 2 server: TCP socket API, one worker thread per connected client.
// Build with MinGW: g++ -std=c++17 -Wall -Wextra server.cpp -o server.exe -lws2_32
// Protocol: 12-byte network-order header (type, request_id, body_length), then UTF-8 text.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "Ws2_32.lib")
#endif

namespace {
constexpr std::uint32_t kMaxBody = 64 * 1024;
constexpr std::uint32_t kMaxMessage = kMaxBody - 256; // reserve space for sender metadata
constexpr int kServerPort = 4669; // student ID's last four digits
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

struct Client {
    int id = 0;
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in address{};
    std::mutex sendMutex;
    std::atomic<bool> connected{true};
};

std::mutex clientsMutex;
std::unordered_map<int, std::shared_ptr<Client>> clients;
int nextClientId = 1;
std::atomic<bool> serverRunning{true};
std::atomic<SOCKET> listenSocket{INVALID_SOCKET};
std::mutex logMutex;

bool parsePositiveInt(const std::string& text, int& value, int maximum) {
    if (text.empty()) return false;
    int number = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9') return false;
        const int next = digit - '0';
        if (number > (maximum - next) / 10) return false;
        number = number * 10 + next;
    }
    value = number;
    return number > 0 && number <= maximum;
}

void log(const std::string& text) {
    std::lock_guard<std::mutex> lock(logMutex);
    std::cout << text << std::endl;
}

bool sendToClient(const std::shared_ptr<Client>& client, const Packet& packet) {
    if (!client || packet.body.size() > kMaxBody) return false;
    std::lock_guard<std::mutex> lock(client->sendMutex);
    // Recheck after acquiring the lock: the worker may have closed this socket.
    if (!client->connected.load() || client->socket == INVALID_SOCKET) return false;
    if (sendPacket(client->socket, packet)) return true;
    client->connected = false;
    shutdown(client->socket, SD_BOTH); // wake its owning worker, which closes the socket
    return false;
}

std::string addressText(const sockaddr_in& address) {
    char ip[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip));
    std::ostringstream result;
    result << ip << ':' << ntohs(address.sin_port);
    return result.str();
}

std::string makeClientList() {
    std::vector<std::shared_ptr<Client>> snapshot;
    {
        std::lock_guard<std::mutex> lock(clientsMutex);
        for (const auto& [id, client] : clients) {
            if (client->connected.load()) snapshot.push_back(client);
        }
    }
    std::sort(snapshot.begin(), snapshot.end(), [](const auto& a, const auto& b) { return a->id < b->id; });
    std::ostringstream result;
    result << snapshot.size() << '\n';
    for (const auto& client : snapshot) {
        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &client->address.sin_addr, ip, sizeof(ip));
        result << client->id << ' ' << ip << ' ' << ntohs(client->address.sin_port) << '\n';
    }
    return result.str();
}

void removeClient(const std::shared_ptr<Client>& client) {
    if (!client) return;
    client->connected = false;
    {
        std::lock_guard<std::mutex> sendLock(client->sendMutex);
        shutdown(client->socket, SD_BOTH);
        closesocket(client->socket);
        client->socket = INVALID_SOCKET;
    }
    {
        std::lock_guard<std::mutex> lock(clientsMutex);
        clients.erase(client->id);
    }
    log("[server] client #" + std::to_string(client->id) + " disconnected (" + addressText(client->address) + ")");
}

void clientWorker(const std::shared_ptr<Client>& client) {
    // A greeting is optional in the report; it also confirms that the worker owns this socket.
    sendToClient(client, {kGreeting, 0, "Connected to Lab2 server"});

    Packet request;
    while (client->connected.load() && recvPacket(client->socket, request)) {
        if (request.requestId == 0 ||
            (request.type != kRequestMessage && !request.body.empty())) {
            sendToClient(client, {kResponseError, request.requestId, "invalid request id or body"});
            continue;
        }
        switch (request.type) {
        case kRequestTime: {
            log("[server] client #" + std::to_string(client->id) + " requested server time");
            const std::time_t now = std::time(nullptr);
            std::tm localTime{};
            localtime_s(&localTime, &now);
            std::ostringstream timeText;
            timeText << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
            sendToClient(client, {kResponseTime, request.requestId, timeText.str()});
            break;
        }
        case kRequestName: {
            log("[server] client #" + std::to_string(client->id) + " requested server name");
            wchar_t name[MAX_COMPUTERNAME_LENGTH + 1]{};
            DWORD length = MAX_COMPUTERNAME_LENGTH + 1;
            if (!GetComputerNameW(name, &length)) {
                sendToClient(client, {kResponseError, request.requestId, "GetComputerName failed"});
                break;
            }
            const int size = WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
            std::string value(size, '\0');
            WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(length), value.data(), size, nullptr, nullptr);
            sendToClient(client, {kResponseName, request.requestId, value});
            break;
        }
        case kRequestList:
            log("[server] client #" + std::to_string(client->id) + " requested active client list");
            sendToClient(client, {kResponseList, request.requestId, makeClientList()});
            break;
        case kRequestMessage: {
            const auto separator = request.body.find('\n');
            if (separator == std::string::npos) {
                sendToClient(client, {kResponseError, request.requestId, "message request must be: target_id\\nmessage"});
                break;
            }
            int targetId = 0;
            if (!parsePositiveInt(request.body.substr(0, separator), targetId, 2147483647)) {
                sendToClient(client, {kResponseError, request.requestId, "invalid target client number"});
                break;
            }
            const std::string message = request.body.substr(separator + 1);
            if (message.empty() || message.size() > kMaxMessage) {
                sendToClient(client, {kResponseError, request.requestId, "message must contain 1 to 65280 UTF-8 bytes"});
                break;
            }
            std::shared_ptr<Client> target;
            {
                std::lock_guard<std::mutex> lock(clientsMutex);
                const auto it = clients.find(targetId);
                if (it != clients.end()) target = it->second;
            }
            if (!target || !target->connected.load()) {
                sendToClient(client, {kResponseError, request.requestId, "target client is not connected"});
                break;
            }
            if (target->id == client->id) {
                sendToClient(client, {kResponseError, request.requestId, "choose another connected client"});
                break;
            }
            std::ostringstream forwarded;
            forwarded << "from #" << client->id << ' ' << addressText(client->address) << "\n" << message;
            if (sendToClient(target, {kForwardMessage, 0, forwarded.str()})) {
                sendToClient(client, {kResponseMessageAck, request.requestId, "message forwarded"});
                log("[server] forwarded #" + std::to_string(client->id) + " -> #" + std::to_string(targetId));
            } else {
                sendToClient(client, {kResponseError, request.requestId, "failed to send to target client"});
            }
            break;
        }
        default:
            sendToClient(client, {kResponseError, request.requestId, "unknown request type"});
            break;
        }
    }
    removeClient(client);
}

BOOL WINAPI consoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT || signal == CTRL_BREAK_EVENT) {
        serverRunning = false;
        const SOCKET socket = listenSocket.exchange(INVALID_SOCKET);
        if (socket != INVALID_SOCKET) {
            shutdown(socket, SD_BOTH);
            closesocket(socket);
        }
        return TRUE;
    }
    return FALSE;
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(consoleHandler, TRUE);
    std::cout << "Lab2 TCP server (Winsock, one thread per client)\n";
    const int port = kServerPort;
    std::cout << "Using student ID last four digits as the fixed listening port: " << port << "\n";

    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed.\n";
        return 1;
    }
    const SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    listenSocket = serverSocket;
    if (serverSocket == INVALID_SOCKET) {
        std::cerr << "socket failed: " << WSAGetLastError() << "\n";
        WSACleanup();
        return 1;
    }
    BOOL exclusive = TRUE;
    setsockopt(serverSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    serverAddress.sin_port = htons(static_cast<u_short>(port));
    if (bind(serverSocket, reinterpret_cast<sockaddr*>(&serverAddress), sizeof(serverAddress)) == SOCKET_ERROR ||
        listen(serverSocket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "bind/listen failed: " << WSAGetLastError() << "\n";
        listenSocket = INVALID_SOCKET;
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }
    std::cout << "Listening on TCP port " << port << ". Press Ctrl+C to stop.\n";

    std::vector<std::thread> workers;
    while (serverRunning.load()) {
        sockaddr_in clientAddress{};
        int addressLength = sizeof(clientAddress);
        const SOCKET currentListenSocket = listenSocket.load();
        if (currentListenSocket == INVALID_SOCKET) break;
        SOCKET clientSocket = accept(currentListenSocket, reinterpret_cast<sockaddr*>(&clientAddress), &addressLength);
        if (clientSocket == INVALID_SOCKET) {
            if (serverRunning.load()) std::cerr << "accept failed: " << WSAGetLastError() << "\n";
            break;
        }
        if (!serverRunning.load()) {
            closesocket(clientSocket);
            break;
        }
        // Bound a blocked send so shutdown cannot wait indefinitely behind a sender.
        DWORD sendTimeout = 5000;
        setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&sendTimeout), sizeof(sendTimeout));
        auto client = std::make_shared<Client>();
        client->socket = clientSocket;
        client->address = clientAddress;
        {
            std::lock_guard<std::mutex> lock(clientsMutex);
            client->id = nextClientId++;
            clients.emplace(client->id, client);
        }
        log("[server] client #" + std::to_string(client->id) + " connected from " + addressText(clientAddress));
        try {
            workers.emplace_back(clientWorker, client);
        } catch (const std::exception& error) {
            log(std::string("[server] could not start worker: ") + error.what());
            removeClient(client);
        }
    }

    std::vector<std::shared_ptr<Client>> remainingClients;
    {
        std::lock_guard<std::mutex> lock(clientsMutex);
        for (const auto& [id, client] : clients) remainingClients.push_back(client);
    }
    for (const auto& client : remainingClients) {
        client->connected = false;
        std::lock_guard<std::mutex> sendLock(client->sendMutex);
        if (client->socket != INVALID_SOCKET) shutdown(client->socket, SD_BOTH);
    }
    for (auto& worker : workers) if (worker.joinable()) worker.join();
    const SOCKET remainingListenSocket = listenSocket.exchange(INVALID_SOCKET);
    if (remainingListenSocket != INVALID_SOCKET) closesocket(remainingListenSocket);
    WSACleanup();
    return 0;
}
