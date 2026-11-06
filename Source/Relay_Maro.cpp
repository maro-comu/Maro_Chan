#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include "Relay_Maro.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace maro {
namespace {

using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
constexpr std::size_t MessageLimit = 65536;
constexpr std::size_t ChunkSize = 16384;
constexpr std::size_t QueueLimit = 2 * 1024 * 1024;
constexpr auto OperationTimeout = std::chrono::seconds(8);
constexpr auto ConnectionTimeout = std::chrono::seconds(90);

struct AsyncState {
    std::mutex mutex;
    std::condition_variable changed;
    bool sentRequest = false;
    bool headers = false;
    bool read = false;
    bool written = false;
    bool closeComplete = false;
    bool failed = false;
    bool closed = false;
    bool tracked = false;
    DWORD bytes = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE;
};

void CALLBACK httpStatus(HINTERNET, DWORD_PTR context, DWORD status, void* information, DWORD length) {
    auto* state = reinterpret_cast<AsyncState*>(context);
    if (!state) {
        return;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        state->sentRequest = true;
        break;
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
        state->headers = true;
        break;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
        if (information && length >= sizeof(WINHTTP_WEB_SOCKET_STATUS)) {
            const auto* result = static_cast<const WINHTTP_WEB_SOCKET_STATUS*>(information);
            state->bytes = result->dwBytesTransferred;
            state->type = result->eBufferType;
            state->read = true;
        } else {
            state->failed = true;
        }
        break;
    case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:
        state->written = true;
        break;
    case WINHTTP_CALLBACK_STATUS_CLOSE_COMPLETE:
        state->closeComplete = true;
        break;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
        state->failed = true;
        break;
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
        state->closed = true;
        break;
    }
    state->changed.notify_all();
}

bool waitFor(AsyncState& state, bool AsyncState::*completed,
             std::chrono::milliseconds timeout = OperationTimeout) {
    std::unique_lock<std::mutex> lock(state.mutex);
    const auto finished = [&] {
        return state.*completed || state.failed || state.closed;
    };
    if (timeout.count() == 0) {
        state.changed.wait(lock, finished);
    } else if (!state.changed.wait_for(lock, timeout, finished)) {
        return false;
    }
    return state.*completed && !state.failed && !state.closed;
}

bool configureAsync(HINTERNET handle, AsyncState& state) {
    constexpr DWORD notifications = WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE
        | WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE | WINHTTP_CALLBACK_FLAG_READ_COMPLETE
        | WINHTTP_CALLBACK_FLAG_WRITE_COMPLETE | WINHTTP_CALLBACK_FLAG_REQUEST_ERROR
        | WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING | WINHTTP_CALLBACK_STATUS_CLOSE_COMPLETE;
    const DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&state);
    const bool configured = WinHttpSetOption(handle, WINHTTP_OPTION_CONTEXT_VALUE,
        const_cast<DWORD_PTR*>(&context), sizeof(context))
        && WinHttpSetStatusCallback(handle, httpStatus, notifications, 0) != WINHTTP_INVALID_STATUS_CALLBACK;
    std::lock_guard<std::mutex> lock(state.mutex);
    state.tracked = configured;
    return configured;
}

class Socket {
public:
    explicit Socket(SOCKET handle = INVALID_SOCKET) : handle_(handle) {}
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : handle_(std::exchange(other.handle_, INVALID_SOCKET)) {}
    Socket& operator=(Socket&& other) noexcept {
        close();
        handle_ = std::exchange(other.handle_, INVALID_SOCKET);
        return *this;
    }
    ~Socket() { close(); }
    SOCKET get() const { return handle_; }
    bool open() const { return handle_ != INVALID_SOCKET; }
    void close() {
        if (open()) {
            closesocket(handle_);
            handle_ = INVALID_SOCKET;
        }
    }
private:
    SOCKET handle_;
};

bool nonblocking(SOCKET socket) {
    u_long enabled = 1;
    BOOL noDelay = TRUE;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0
        && setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
            reinterpret_cast<const char*>(&noDelay), sizeof(noDelay)) == 0;
}

sockaddr_in loopback(unsigned short port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    return address;
}

std::uint32_t readId(const Bytes& message) {
    return static_cast<std::uint32_t>(message[1])
        | (static_cast<std::uint32_t>(message[2]) << 8)
        | (static_cast<std::uint32_t>(message[3]) << 16)
        | (static_cast<std::uint32_t>(message[4]) << 24);
}

Bytes packet(std::uint8_t kind, std::uint32_t id) {
    return {kind, static_cast<std::uint8_t>(id), static_cast<std::uint8_t>(id >> 8),
        static_cast<std::uint8_t>(id >> 16), static_cast<std::uint8_t>(id >> 24)};
}

bool validCode(const std::string& code) {
    return code.size() == 8 && std::all_of(code.begin(), code.end(), [](unsigned char value) {
        return (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9');
    });
}

struct Peer {
    Socket socket;
    bool connecting = false;
    std::deque<Bytes> outgoing;
    std::size_t offset = 0;
    std::size_t queued = 0;
    Clock::time_point joined = Clock::now();
};

}

struct RelayTunnel::Impl {
    Impl(bool isHost, std::string endpoint, std::string code, unsigned short port)
        : hosting(isHost), endpoint(std::move(endpoint)), code(std::move(code)), port(port) {}

    ~Impl() {
        leaving = true;
        stopping = true;
        inboxChanged.notify_one();
        cancel(request, requestState);
        cancel(websocket, socketState);
        if (worker.joinable()) {
            worker.join();
        }
    }

    bool hosting;
    std::string endpoint;
    mutable std::mutex stateMutex;
    std::string code;
    std::string failure;
    bool available = false;
    std::atomic<unsigned short> port;
    std::atomic<bool> stopping{false};
    std::atomic<bool> leaving{false};
    std::atomic<bool> remoteEnded{false};
    std::thread worker;
    std::mutex handlesMutex;
    HINTERNET request = nullptr;
    HINTERNET websocket = nullptr;
    AsyncState requestState;
    AsyncState socketState;
    std::mutex inboxMutex;
    std::condition_variable inboxChanged;
    std::deque<Bytes> inbox;
    std::size_t inboxBytes = 0;
    std::array<std::uint8_t, ChunkSize> receiveBuffer;
    std::array<std::uint8_t, ChunkSize + 5> sendBuffer;
    Socket listener;
    std::map<std::uint32_t, Peer> peers;
    std::atomic<std::uint32_t> guestId{0};
    bool guestClosed = false;

    void run();
    bool connectRelay(HINTERNET session);
    void receive();
    void forward();
    bool send(const std::uint8_t* data, std::size_t length);
    bool send(const Bytes& message) { return send(message.data(), message.size()); }
    bool process(Bytes message);
    bool readLocal(std::uint32_t id, Peer& peer);
    bool writeLocal(Peer& peer);
    bool listenLocal();
    bool connectLocal(Peer& peer);
    void fail(std::string message);
    void cancel(HINTERNET& handle, AsyncState& state);
};

void RelayTunnel::Impl::cancel(HINTERNET& handle, AsyncState& state) {
    HINTERNET closing = nullptr;
    {
        std::lock_guard<std::mutex> lock(handlesMutex);
        closing = std::exchange(handle, nullptr);
    }
    if (closing) {
        Bytes goodbye;
        if (&state == &socketState && leaving) {
            bool closeFrame = true;
            const auto id = guestId.load();
            if (hosting || id != 0) {
                goodbye = packet(4, id);
                {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    state.written = false;
                }
                const DWORD sent = WinHttpWebSocketSend(closing, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                    goodbye.data(), static_cast<DWORD>(goodbye.size()));
                if (sent == NO_ERROR || sent == ERROR_IO_PENDING) {
                    closeFrame = waitFor(state, &AsyncState::written, std::chrono::milliseconds(200));
                }
            }
            if (closeFrame) {
                const DWORD result = WinHttpWebSocketClose(closing, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
                if (result == NO_ERROR || result == ERROR_IO_PENDING) {
                    waitFor(state, &AsyncState::closeComplete, std::chrono::milliseconds(150));
                }
            }
        }
        WinHttpCloseHandle(closing);
        std::unique_lock<std::mutex> lock(state.mutex);
        if (state.tracked) {
            state.changed.wait(lock, [&] { return state.closed; });
        }
    }
}

void RelayTunnel::Impl::fail(std::string message) {
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (failure.empty() && !stopping) {
            failure = std::move(message);
        }
        available = false;
    }
    stopping = true;
}

bool RelayTunnel::Impl::listenLocal() {
    listener = Socket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    BOOL exclusive = TRUE;
    auto address = loopback(0);
    if (!listener.open() || !nonblocking(listener.get())
        || setsockopt(listener.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0
        || bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0
        || listen(listener.get(), 1) != 0) {
        return false;
    }
    int length = sizeof(address);
    if (getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return false;
    }
    port = ntohs(address.sin_port);
    return true;
}

bool RelayTunnel::Impl::connectLocal(Peer& peer) {
    peer.socket = Socket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    const auto address = loopback(port.load());
    if (!peer.socket.open() || !nonblocking(peer.socket.get())) {
        return false;
    }
    if (connect(peer.socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
        return true;
    }
    peer.connecting = WSAGetLastError() == WSAEWOULDBLOCK;
    return peer.connecting;
}

bool RelayTunnel::Impl::connectRelay(HINTERNET session) {
    if (endpoint.size() > 2048 || (!hosting && !validCode(code))) {
        fail("인터넷 방 주소와 방 코드를 확인해 주세요.");
        return false;
    }
    const std::wstring url(endpoint.begin(), endpoint.end());
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = components.dwHostNameLength = components.dwUrlPathLength = DWORD(-1);
    components.dwExtraInfoLength = components.dwUserNameLength = components.dwPasswordLength = DWORD(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components)
        || components.dwHostNameLength == 0 || components.dwUserNameLength || components.dwPasswordLength
        || components.dwExtraInfoLength) {
        fail("인터넷 방 서버 주소가 올바르지 않습니다.");
        return false;
    }
    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    if (!secure && (components.nScheme != INTERNET_SCHEME_HTTP
        || (host != L"127.0.0.1" && host != L"localhost"))) {
        fail("인터넷 방 서버는 HTTPS 주소를 사용해야 합니다.");
        return false;
    }
    std::wstring path(components.lpszUrlPath ? components.lpszUrlPath : L"", components.dwUrlPathLength);
    while (!path.empty() && path.back() == L'/') {
        path.pop_back();
    }
    path += hosting ? L"/host" : L"/join/" + std::wstring(code.begin(), code.end());
    HINTERNET connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
    if (!connection) {
        fail("인터넷 방 서버에 연결할 수 없습니다.");
        return false;
    }
    bool upgraded = false;
    {
        std::lock_guard<std::mutex> lock(handlesMutex);
        if (!stopping) {
            request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
            if (request && configureAsync(request, requestState)) {
                upgraded = WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)
                    && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                        WINHTTP_NO_REQUEST_DATA, 0, 0, reinterpret_cast<DWORD_PTR>(&requestState));
            }
        }
    }
    if (!upgraded || !waitFor(requestState, &AsyncState::sentRequest, ConnectionTimeout)) {
        cancel(request, requestState);
        WinHttpCloseHandle(connection);
        fail("인터넷 방 서버에 연결할 수 없습니다.");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(handlesMutex);
        upgraded = !stopping && request && WinHttpReceiveResponse(request, nullptr);
    }
    if (!upgraded || !waitFor(requestState, &AsyncState::headers, ConnectionTimeout)) {
        cancel(request, requestState);
        WinHttpCloseHandle(connection);
        fail("인터넷 방 서버의 응답을 받지 못했습니다.");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(handlesMutex);
        DWORD status = 0;
        DWORD length = sizeof(status);
        upgraded = !stopping && request
            && WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX) && status == 101;
        if (upgraded) {
            websocket = WinHttpWebSocketCompleteUpgrade(request, reinterpret_cast<DWORD_PTR>(&socketState));
            upgraded = websocket && configureAsync(websocket, socketState);
        }
    }
    cancel(request, requestState);
    WinHttpCloseHandle(connection);
    if (!upgraded) {
        fail("방에 연결할 수 없습니다. 서버 상태와 방 코드를 확인해 주세요.");
    }
    return upgraded;
}

bool RelayTunnel::Impl::send(const std::uint8_t* data, std::size_t length) {
    if (data != sendBuffer.data()) {
        std::copy_n(data, length, sendBuffer.data());
    }
    {
        std::lock_guard<std::mutex> lock(socketState.mutex);
        socketState.written = false;
    }
    DWORD status = ERROR_OPERATION_ABORTED;
    {
        std::lock_guard<std::mutex> lock(handlesMutex);
        if (!stopping && websocket) {
            status = WinHttpWebSocketSend(websocket, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                sendBuffer.data(), static_cast<DWORD>(length));
        }
    }
    if ((status == NO_ERROR || status == ERROR_IO_PENDING) && waitFor(socketState, &AsyncState::written)) {
        return true;
    }
    fail("인터넷 방 연결이 끊겼습니다.");
    cancel(websocket, socketState);
    return false;
}

void RelayTunnel::Impl::receive() {
    auto& buffer = receiveBuffer;
    Bytes message;
    while (!stopping) {
        {
            std::lock_guard<std::mutex> lock(socketState.mutex);
            socketState.read = false;
        }
        DWORD status = ERROR_OPERATION_ABORTED;
        {
            std::lock_guard<std::mutex> lock(handlesMutex);
            if (!stopping && websocket) {
                status = WinHttpWebSocketReceive(websocket, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, nullptr);
            }
        }
        if ((status != NO_ERROR && status != ERROR_IO_PENDING)
            || !waitFor(socketState, &AsyncState::read, std::chrono::milliseconds(0))) {
            break;
        }
        DWORD count = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        {
            std::lock_guard<std::mutex> lock(socketState.mutex);
            count = socketState.bytes;
            type = socketState.type;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            break;
        }
        if ((type != WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE
            && type != WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) || count > buffer.size()
            || message.size() + count > MessageLimit) {
            break;
        }
        message.insert(message.end(), buffer.begin(), buffer.begin() + count);
        if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            if (message.size() > 1 && message.size() <= 1024 && message[0] == 5) {
                fail(std::string(message.begin() + 1, message.end()));
                break;
            }
            std::lock_guard<std::mutex> lock(inboxMutex);
            if (message.empty() || inboxBytes + message.size() > QueueLimit) {
                break;
            }
            inboxBytes += message.size();
            inbox.push_back(std::move(message));
            message.clear();
            inboxChanged.notify_one();
        }
    }
    remoteEnded = true;
    inboxChanged.notify_one();
    cancel(websocket, socketState);
}

bool RelayTunnel::Impl::process(Bytes message) {
    if (message[0] == 1 && hosting && message.size() == 9) {
        const std::string room(message.begin() + 1, message.end());
        if (!validCode(room)) {
            return false;
        }
        std::lock_guard<std::mutex> lock(stateMutex);
        code = room;
        available = true;
        return true;
    }
    if (message.size() < 5) {
        return false;
    }
    const auto id = readId(message);
    if (id == 0) {
        return false;
    }
    if (message[0] == 2 && message.size() == 5 && peers.count(id) == 0 && peers.size() < 15) {
        auto& peer = peers[id];
        if (hosting) {
            if (!connectLocal(peer)) {
                peers.erase(id);
                return send(packet(4, id));
            }
        } else {
            if (guestId != 0) {
                return false;
            }
            guestId = id;
            std::lock_guard<std::mutex> lock(stateMutex);
            available = true;
        }
        return true;
    }
    const auto found = peers.find(id);
    if (found == peers.end()) {
        return message[0] == 3 || (message[0] == 4 && message.size() == 5);
    }
    if (message[0] == 4 && message.size() == 5) {
        if (!hosting) {
            guestClosed = true;
        } else {
            peers.erase(found);
        }
        return true;
    }
    if (message[0] != 3 || message.size() == 5) {
        return false;
    }
    auto& peer = found->second;
    const auto length = message.size() - 5;
    if (peer.queued + length > QueueLimit) {
        return false;
    }
    peer.queued += length;
    peer.outgoing.emplace_back(message.begin() + 5, message.end());
    return true;
}

bool RelayTunnel::Impl::readLocal(std::uint32_t id, Peer& peer) {
    auto& buffer = sendBuffer;
    const int count = recv(peer.socket.get(), reinterpret_cast<char*>(buffer.data() + 5), ChunkSize, 0);
    if (count == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
        return true;
    }
    if (count <= 0) {
        return false;
    }
    buffer[0] = 3;
    for (std::size_t byte = 0; byte < sizeof(id); ++byte) {
        buffer[byte + 1] = static_cast<std::uint8_t>(id >> (byte * 8));
    }
    return send(buffer.data(), 5 + count);
}

bool RelayTunnel::Impl::writeLocal(Peer& peer) {
    while (!peer.outgoing.empty()) {
        auto& message = peer.outgoing.front();
        const int count = ::send(peer.socket.get(), reinterpret_cast<const char*>(message.data() + peer.offset),
            static_cast<int>(message.size() - peer.offset), 0);
        if (count == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            return true;
        }
        if (count <= 0) {
            return false;
        }
        peer.queued -= count;
        peer.offset += count;
        if (peer.offset == message.size()) {
            peer.offset = 0;
            peer.outgoing.pop_front();
        }
    }
    return true;
}

void RelayTunnel::Impl::forward() {
    const auto waitingSince = Clock::now();
    Clock::time_point drainUntil{};
    while (!stopping) {
        const bool ended = remoteEnded.load();
        std::deque<Bytes> messages;
        {
            std::lock_guard<std::mutex> lock(inboxMutex);
            messages.swap(inbox);
            inboxBytes = 0;
        }
        for (auto& message : messages) {
            if (!process(std::move(message))) {
                fail("인터넷 방에서 올바르지 않은 응답을 받았습니다.");
                break;
            }
        }
        if (stopping) {
            break;
        }
        if (ended && hosting) {
            fail("인터넷 방 연결이 끊겼습니다.");
            break;
        }
        if (!hosting && guestId && listener.open() && !peers[guestId].socket.open()) {
            Socket accepted(accept(listener.get(), nullptr, nullptr));
            if (accepted.open() && nonblocking(accepted.get())) {
                peers[guestId].socket = std::move(accepted);
                listener.close();
            }
        }
        if (ended || guestClosed) {
            if (drainUntil == Clock::time_point{}) {
                drainUntil = Clock::now() + std::chrono::milliseconds(250);
            }
            const auto peer = peers.find(guestId);
            if (peer == peers.end() || !peer->second.socket.open() || !writeLocal(peer->second)
                || peer->second.outgoing.empty() || Clock::now() >= drainUntil) {
                fail(guestClosed ? "방장이 방 연결을 종료했습니다." : "인터넷 방 연결이 끊겼습니다.");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        for (auto peer = peers.begin(); peer != peers.end() && !stopping;) {
            auto& connection = peer->second;
            bool healthy = true;
            if (connection.connecting) {
                fd_set writable;
                fd_set failed;
                FD_ZERO(&writable);
                FD_ZERO(&failed);
                FD_SET(connection.socket.get(), &writable);
                FD_SET(connection.socket.get(), &failed);
                timeval immediate{};
                const int ready = select(0, nullptr, &writable, &failed, &immediate);
                if (ready > 0) {
                    int error = 0;
                    int length = sizeof(error);
                    healthy = getsockopt(connection.socket.get(), SOL_SOCKET, SO_ERROR,
                        reinterpret_cast<char*>(&error), &length) == 0 && error == 0;
                    connection.connecting = false;
                } else if (ready == SOCKET_ERROR || Clock::now() - connection.joined > OperationTimeout) {
                    healthy = false;
                }
            }
            if (healthy && connection.socket.open() && !connection.connecting) {
                healthy = writeLocal(connection) && readLocal(peer->first, connection);
            }
            if (!healthy) {
                const auto id = peer->first;
                peer = peers.erase(peer);
                send(packet(4, id));
                if (!hosting) {
                    fail("게임 연결이 종료되었습니다.");
                }
            } else {
                ++peer;
            }
        }
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (!available && Clock::now() - waitingSince > ConnectionTimeout) {
                if (failure.empty()) {
                    failure = "방에 연결할 시간이 초과됐습니다. 방 코드를 확인해 주세요.";
                }
                stopping = true;
            }
        }
        std::unique_lock<std::mutex> lock(inboxMutex);
        inboxChanged.wait_for(lock, std::chrono::milliseconds(1), [&] {
            return !inbox.empty() || remoteEnded || stopping;
        });
    }
}

void RelayTunnel::Impl::run() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        fail("Windows 네트워크를 초기화할 수 없습니다.");
        return;
    }
    if ((!hosting && !listenLocal()) || (hosting && port.load() == 0)) {
        fail("게임 연결을 준비할 수 없습니다.");
        WSACleanup();
        return;
    }
    HINTERNET session = WinHttpOpen(L"MaroArena/0.0.7", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (!session) {
        fail("인터넷 연결을 준비할 수 없습니다.");
        WSACleanup();
        return;
    }
    WinHttpSetTimeouts(session, 15000, 15000, 8000, 90000);
    if (connectRelay(session) && !stopping) {
        std::thread receiving([this] { receive(); });
        forward();
        cancel(websocket, socketState);
        receiving.join();
    }
    cancel(request, requestState);
    cancel(websocket, socketState);
    WinHttpCloseHandle(session);
    peers.clear();
    listener.close();
    WSACleanup();
}

RelayTunnel::RelayTunnel(bool hosting, std::string endpoint, std::string code, unsigned short port)
    : impl_(std::make_unique<Impl>(hosting, std::move(endpoint), std::move(code), port)) {
    impl_->worker = std::thread([state = impl_.get()] { state->run(); });
}

RelayTunnel::~RelayTunnel() = default;

std::shared_ptr<RelayTunnel> RelayTunnel::host(std::string endpoint, unsigned short localPort) {
    return std::shared_ptr<RelayTunnel>(new RelayTunnel(true, std::move(endpoint), {}, localPort));
}

std::shared_ptr<RelayTunnel> RelayTunnel::join(std::string endpoint, std::string roomCode) {
    return std::shared_ptr<RelayTunnel>(new RelayTunnel(false, std::move(endpoint), std::move(roomCode), 0));
}

bool RelayTunnel::ready() const {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    return impl_->available && !impl_->stopping;
}

std::string RelayTunnel::roomCode() const {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    return impl_->code;
}

std::string RelayTunnel::error() const {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    return impl_->failure;
}

unsigned short RelayTunnel::localPort() const {
    return impl_->port.load();
}

}
