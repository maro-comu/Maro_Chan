#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mstcpip.h>

#include "Lan_Maro.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <numeric>

namespace maro {
namespace {

using Byte = std::uint8_t;
using Buffer = std::vector<Byte>;
using Packet = std::shared_ptr<const Buffer>;
using Clock = std::chrono::steady_clock;

constexpr std::size_t HeaderSize = 4;
constexpr std::size_t PacketLimit = 65536;
constexpr std::size_t SendLimit = 4 * PacketLimit;
constexpr int SlotLimit = 16;
constexpr auto Timeout = std::chrono::seconds(8);
constexpr std::uint32_t Protocol = 0x034F524D;

enum class Kind : Byte { Hello = 1, Lobby, Start, Input, Choice, Frame, Reject };
enum class Rejection : Byte { Full = 1, Started, Version };

class Winsock {
public:
    Winsock() {
        WSADATA data{};
        ready_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }

    ~Winsock() {
        if (ready_) {
            WSACleanup();
        }
    }

    bool ready() const { return ready_; }

private:
    bool ready_ = false;
};

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

bool configureSocket(SOCKET socket, bool listener = false) {
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0) {
        return false;
    }
    BOOL enabled = TRUE;
    if (listener) {
        return setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                          reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == 0;
    }
    tcp_keepalive keepalive{1, 8000, 1000};
    DWORD ignored = 0;
    WSAIoctl(socket, SIO_KEEPALIVE_VALS, &keepalive, sizeof(keepalive), nullptr, 0,
             &ignored, nullptr, nullptr);
    return setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
                      reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == 0;
}

std::uint32_t readWord(const Byte* bytes) {
    std::uint32_t result = 0;
    for (int i = 0; i < 4; ++i) {
        result |= static_cast<std::uint32_t>(bytes[i]) << (i * 8);
    }
    return result;
}

class Writer {
public:
    explicit Writer(Kind kind, std::size_t capacity = 64) : bytes_(HeaderSize, 0) {
        bytes_.reserve(capacity);
        byte(static_cast<Byte>(kind));
    }

    void byte(Byte value) { bytes_.push_back(value); }

    void word(std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            byte(static_cast<Byte>(value >> (i * 8)));
        }
    }

    void number(float value) {
        static_assert(sizeof(float) == sizeof(std::uint32_t));
        std::uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        word(bits);
    }

    void point(Vec value) {
        number(value.x);
        number(value.y);
    }

    Packet finish() {
        const auto size = bytes_.size() - HeaderSize;
        for (std::size_t i = 0; i < HeaderSize; ++i) {
            bytes_[i] = static_cast<Byte>(size >> (i * 8));
        }
        return std::make_shared<const Buffer>(std::move(bytes_));
    }

private:
    Buffer bytes_;
};

class Reader {
public:
    Reader(const Byte* begin, const Byte* end) : cursor_(begin), end_(end) {}

    Byte byte() {
        if (cursor_ == end_) {
            valid_ = false;
            return 0;
        }
        return *cursor_++;
    }

    std::uint32_t word() {
        if (end_ - cursor_ < 4) {
            valid_ = false;
            return 0;
        }
        const auto value = readWord(cursor_);
        cursor_ += 4;
        return value;
    }

    float number() {
        const auto bits = word();
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        if (!std::isfinite(value)) {
            valid_ = false;
            return 0.0f;
        }
        return value;
    }

    Vec point() { return {number(), number()}; }
    bool done() const { return valid_ && cursor_ == end_; }

private:
    const Byte* cursor_;
    const Byte* end_;
    bool valid_ = true;
};

struct Pending {
    Packet bytes;
    std::size_t offset = 0;
};

struct Peer {
    explicit Peer(Socket connection = Socket()) : socket(std::move(connection)) {}

    bool queue(const Packet& packet, bool replaceFrame = false) {
        if (replaceFrame && !outgoing.empty() && outgoing.back().offset == 0
            && (*outgoing.back().bytes)[HeaderSize] == static_cast<Byte>(Kind::Frame)) {
            queuedBytes -= outgoing.back().bytes->size();
            outgoing.pop_back();
        }
        if (queuedBytes + packet->size() > SendLimit) {
            socket.close();
            return false;
        }
        queuedBytes += packet->size();
        outgoing.push_back({packet});
        return true;
    }

    bool flush() {
        while (!outgoing.empty()) {
            auto& item = outgoing.front();
            const auto remaining = static_cast<int>(item.bytes->size() - item.offset);
            const auto* first = reinterpret_cast<const char*>(item.bytes->data() + item.offset);
            const int written = send(socket.get(), first, remaining, 0);
            if (written == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
                return true;
            }
            if (written <= 0) {
                return false;
            }
            item.offset += written;
            queuedBytes -= written;
            if (item.offset == item.bytes->size()) {
                outgoing.pop_front();
            }
        }
        return true;
    }

    template<class Handler>
    bool read(Handler&& handler) {
        std::array<Byte, 16384> buffer;
        for (int batch = 0; batch < 4; ++batch) {
            const int count = recv(socket.get(), reinterpret_cast<char*>(buffer.data()),
                                   static_cast<int>(buffer.size()), 0);
            if (count == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
                return true;
            }
            if (count <= 0) {
                return false;
            }
            incoming.insert(incoming.end(), buffer.begin(), buffer.begin() + count);
            std::size_t consumed = 0;
            while (incoming.size() - consumed >= HeaderSize) {
                const Byte* begin = incoming.data() + consumed;
                const std::size_t length = readWord(begin);
                if (length == 0 || length > PacketLimit) {
                    return false;
                }
                if (incoming.size() - consumed < HeaderSize + length) {
                    break;
                }
                Reader reader(begin + HeaderSize, begin + HeaderSize + length);
                const auto kind = static_cast<Kind>(reader.byte());
                if (!handler(kind, reader)) {
                    return false;
                }
                consumed += HeaderSize + length;
            }
            incoming.erase(incoming.begin(), incoming.begin() + consumed);
        }
        return true;
    }

    Socket socket;
    Clock::time_point joinedAt = Clock::now();
    int slot = -1;
    bool rejected = false;
    Buffer incoming;
    std::deque<Pending> outgoing;
    std::size_t queuedBytes = 0;
};

int playerCount(const MatchConfig& config) {
    return std::accumulate(config.teamSizes.begin(), config.teamSizes.begin() + config.teams, 0);
}

bool validPoint(Vec point, float bound) {
    return std::abs(point.x) <= bound && std::abs(point.y) <= bound;
}

Packet writeInput(const Input& input) {
    Writer writer(Kind::Input);
    writer.point(input.movement);
    writer.point(input.aim);
    writer.point(input.destination);
    writer.byte((input.attack ? 1 : 0) | (input.guard ? 2 : 0) | (input.parry ? 4 : 0)
                | (input.moveTo ? 8 : 0) | (input.attackPressed ? 16 : 0));
    return writer.finish();
}

bool readInput(Reader& reader, Input& input) {
    input.movement = reader.point();
    input.aim = reader.point();
    input.destination = reader.point();
    const Byte flags = reader.byte();
    input.attack = (flags & 1) != 0;
    input.guard = (flags & 2) != 0;
    input.parry = (flags & 4) != 0;
    input.moveTo = (flags & 8) != 0;
    input.attackPressed = (flags & 16) != 0;
    return reader.done() && flags < 32 && validPoint(input.movement, 1.0f)
        && validPoint(input.aim, 4096.0f) && validPoint(input.destination, 4096.0f);
}

Packet writeFrame(const MatchFrame& frame) {
    Writer writer(Kind::Frame, 64 + frame.actors.size() * 50
        + frame.bullets.size() * 26 + frame.blasts.size() * 17);
    writer.number(frame.elapsed);
    writer.byte(static_cast<Byte>(frame.result));
    writer.byte(static_cast<Byte>(frame.winner + 1));
    writer.byte(static_cast<Byte>(frame.actors.size()));
    for (std::size_t i = 0; i < frame.actors.size(); ++i) {
        const auto& actor = frame.actors[i];
        writer.point(actor.position);
        writer.point(actor.facing);
        writer.number(actor.hp);
        writer.number(actor.maxHp);
        writer.number(actor.radius);
        writer.number(actor.parryTime);
        writer.word(static_cast<std::uint32_t>(actor.kills));
        writer.byte(static_cast<Byte>(actor.team));
        writer.byte(static_cast<Byte>(actor.level));
        writer.byte((actor.guarding ? 1 : 0) | (frame.choosing[i] ? 2 : 0) | (actor.wideGuard ? 4 : 0));
        writer.number(frame.levelTime[i]);
        if (frame.choosing[i]) {
            for (const auto id : frame.augments[i]) {
                writer.byte(static_cast<Byte>(id));
            }
        }
    }
    const auto bulletCount = std::min(frame.bullets.size(), Arena::BulletLimit);
    writer.word(static_cast<std::uint32_t>(bulletCount));
    for (std::size_t i = 0; i < bulletCount; ++i) {
        const auto& bullet = frame.bullets[i];
        writer.point(bullet.position);
        writer.point(bullet.velocity);
        writer.number(bullet.radius);
        writer.byte(static_cast<Byte>(bullet.team));
        writer.byte(bullet.effects);
        writer.word(bullet.id);
    }
    const auto blastCount = std::min(frame.blasts.size(), Arena::BlastLimit);
    writer.byte(static_cast<Byte>(blastCount));
    for (std::size_t i = 0; i < blastCount; ++i) {
        const auto& blast = frame.blasts[i];
        writer.point(blast.position);
        writer.number(blast.radius);
        writer.number(blast.remaining);
        writer.byte(static_cast<Byte>(blast.team));
    }
    return writer.finish();
}

bool readFrame(Reader& reader, const MatchConfig& config, MatchFrame& frame) {
    frame.elapsed = reader.number();
    const auto result = static_cast<MatchResult>(reader.byte());
    frame.winner = static_cast<int>(reader.byte()) - 1;
    const auto actors = reader.byte();
    if (frame.elapsed < 0 || actors != playerCount(config)
        || (result != MatchResult::Playing && result != MatchResult::Victory && result != MatchResult::Draw)
        || (result == MatchResult::Victory ? frame.winner < 0 || frame.winner >= config.teams
                                           : frame.winner != -1)) {
        return false;
    }
    frame.result = result;
    frame.actors.resize(actors);
    int slot = 0;
    for (int team = 0; team < config.teams; ++team) {
        for (int member = 0; member < config.teamSizes[team]; ++member, ++slot) {
            auto& actor = frame.actors[slot];
            actor.position = reader.point();
            actor.facing = reader.point();
            actor.hp = reader.number();
            actor.maxHp = reader.number();
            actor.radius = reader.number();
            actor.parryTime = reader.number();
            const auto kills = reader.word();
            actor.team = reader.byte();
            actor.level = reader.byte();
            const Byte flags = reader.byte();
            actor.guarding = (flags & 1) != 0;
            frame.choosing[slot] = (flags & 2) != 0;
            actor.wideGuard = (flags & 4) != 0;
            frame.levelTime[slot] = reader.number();
            if (actor.team != team || actor.level < 1 || actor.level > Arena::MaxLevel
                || kills >= SlotLimit || flags > 7 || actor.hp < 0 || actor.hp > actor.maxHp
                || actor.maxHp <= 0 || actor.radius <= 0 || actor.parryTime < 0
                || frame.levelTime[slot] < 0 || !validPoint(actor.position, 4096.0f)
                || !validPoint(actor.facing, 1.01f)) {
                return false;
            }
            actor.kills = static_cast<int>(kills);
            if (frame.choosing[slot]) {
                for (auto& id : frame.augments[slot]) {
                    id = reader.byte();
                    if (id >= Arena::AugmentCount) {
                        return false;
                    }
                }
            }
        }
    }
    const auto bullets = reader.word();
    if (bullets > Arena::BulletLimit) {
        return false;
    }
    frame.bullets.resize(bullets);
    std::uint32_t previousBulletId = 0;
    for (auto& bullet : frame.bullets) {
        bullet.position = reader.point();
        bullet.velocity = reader.point();
        bullet.radius = reader.number();
        bullet.team = reader.byte();
        bullet.effects = reader.byte();
        bullet.id = reader.word();
        if (bullet.team >= config.teams || bullet.radius <= 0 || !validPoint(bullet.position, 4096.0f)
            || !validPoint(bullet.velocity, 4096.0f) || bullet.effects > 31 || bullet.id <= previousBulletId) {
            return false;
        }
        previousBulletId = bullet.id;
    }
    const auto blasts = reader.byte();
    if (blasts > Arena::BlastLimit) {
        return false;
    }
    frame.blasts.resize(blasts);
    for (auto& blast : frame.blasts) {
        blast.position = reader.point();
        blast.radius = reader.number();
        blast.remaining = reader.number();
        blast.team = reader.byte();
        if (blast.team >= config.teams || blast.radius <= 0.0f || blast.radius > 96.0f
            || blast.remaining <= 0.0f || blast.remaining > Arena::BlastDuration || !validPoint(blast.position, 4096.0f)) {
            return false;
        }
    }
    return reader.done();
}

sockaddr_in endpoint() {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(LanSession::Port);
    return address;
}

}

struct LanSession::Impl {
    explicit Impl(bool host) : hosting(host) {}

    Winsock winsock;
    Socket listener;
    Peer upstream;
    std::vector<Peer> clients;
    MatchConfig config;
    MatchFrame frame;
    MatchFrame pendingFrame;
    std::uint64_t revision = 0;
    std::vector<Input> inputs;
    std::vector<std::pair<int, int>> choices;
    std::vector<int> departures;
    std::array<int, SlotLimit> choiceLevels{};
    std::uint32_t occupied = 0;
    std::string error;
    int slot = -1;
    bool hosting;
    bool started = false;
    bool connecting = false;
    bool lobbyChanged = false;

    void pollHost();
    void pollGuest();
    void acceptClients();
    void finishConnect();
    void sendHello();
    void sendLobby();
    void disconnect(const std::string& reason);
    bool fromClient(Peer& peer, Kind kind, Reader& reader);
    bool fromHost(Kind kind, Reader& reader);
};

LanSession::LanSession(bool hosting) : impl_(std::make_unique<Impl>(hosting)) {}
LanSession::~LanSession() = default;

std::shared_ptr<LanSession> LanSession::host(const MatchConfig& config) {
    auto session = std::shared_ptr<LanSession>(new LanSession(true));
    auto& state = *session->impl_;
    state.config = config;
    state.config.teams = std::clamp(config.teams, 2, 4);
    for (auto& size : state.config.teamSizes) {
        size = std::clamp(size, 1, 4);
    }
    state.inputs.resize(playerCount(state.config));
    if (!state.winsock.ready()) {
        state.error = "Windows 네트워크를 초기화할 수 없습니다.";
        return session;
    }
    state.listener = Socket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    const auto address = endpoint();
    if (!state.listener.open() || !configureSocket(state.listener.get(), true)
        || bind(state.listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0
        || listen(state.listener.get(), SOMAXCONN) != 0) {
        state.listener.close();
        state.error = "48150 포트에서 방을 열 수 없습니다.";
        return session;
    }
    state.slot = 0;
    state.occupied = 1;
    return session;
}

std::shared_ptr<LanSession> LanSession::join(const std::string& ipv4) {
    auto session = std::shared_ptr<LanSession>(new LanSession(false));
    auto& state = *session->impl_;
    if (!state.winsock.ready()) {
        state.error = "Windows 네트워크를 초기화할 수 없습니다.";
        return session;
    }
    auto address = endpoint();
    if (InetPtonA(AF_INET, ipv4.c_str(), &address.sin_addr) != 1) {
        state.error = "올바른 방 코드를 입력하세요.";
        return session;
    }
    auto& socketRef = state.upstream.socket;
    socketRef = Socket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!socketRef.open() || !configureSocket(socketRef.get())) {
        state.disconnect("접속을 시작할 수 없습니다.");
        return session;
    }
    if (connect(socketRef.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
        state.sendHello();
    } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        state.connecting = true;
    } else {
        state.disconnect("방에 접속할 수 없습니다.");
    }
    return session;
}

std::vector<std::string> LanSession::localAddresses() {
    Winsock winsock;
    if (!winsock.ready()) {
        return {};
    }
    ULONG size = 15000;
    std::vector<Byte> bytes(size);
    constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(bytes.data());
    auto result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        bytes.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(bytes.data());
        result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    }
    std::vector<std::string> addresses;
    if (result != NO_ERROR) {
        return addresses;
    }
    for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (auto* entry = adapter->FirstUnicastAddress; entry; entry = entry->Next) {
            char text[INET_ADDRSTRLEN]{};
            const auto* address = reinterpret_cast<const sockaddr_in*>(entry->Address.lpSockaddr);
            if (address && address->sin_family == AF_INET
                && InetNtopA(AF_INET, &address->sin_addr, text, sizeof(text))
                && std::find(addresses.begin(), addresses.end(), text) == addresses.end()) {
                addresses.emplace_back(text);
            }
        }
    }
    return addresses;
}

void LanSession::Impl::acceptClients() {
    for (int accepted = 0; accepted < SlotLimit; ++accepted) {
        Socket socketRef(accept(listener.get(), nullptr, nullptr));
        if (!socketRef.open()) {
            return;
        }
        if (clients.size() < SlotLimit + 8 && configureSocket(socketRef.get())) {
            clients.emplace_back(std::move(socketRef));
        }
    }
}

void LanSession::Impl::sendLobby() {
    for (auto& peer : clients) {
        if (peer.slot < 0 || !peer.socket.open()) {
            continue;
        }
        Writer writer(Kind::Lobby);
        writer.byte(static_cast<Byte>(config.teams));
        for (int count : config.teamSizes) {
            writer.byte(static_cast<Byte>(count));
        }
        writer.byte(static_cast<Byte>(peer.slot));
        writer.word(occupied);
        peer.queue(writer.finish());
    }
    lobbyChanged = false;
}

bool LanSession::Impl::fromClient(Peer& peer, Kind kind, Reader& reader) {
    if (peer.rejected) {
        return true;
    }
    if (peer.slot < 0) {
        if (kind != Kind::Hello) {
            return false;
        }
        const auto version = reader.word();
        if (!reader.done()) {
            return false;
        }
        int freeSlot = 1;
        const int count = playerCount(config);
        while (freeSlot < count && (occupied & (1u << freeSlot)) != 0) {
            ++freeSlot;
        }
        if (version != Protocol || started || freeSlot == count) {
            Writer rejection(Kind::Reject);
            const auto reason = version != Protocol ? Rejection::Version
                : started ? Rejection::Started : Rejection::Full;
            rejection.byte(static_cast<Byte>(reason));
            peer.rejected = true;
            return peer.queue(rejection.finish());
        }
        peer.slot = freeSlot;
        occupied |= 1u << freeSlot;
        lobbyChanged = true;
        return true;
    }
    if (!started) {
        return false;
    }
    if (kind == Kind::Input) {
        Input input;
        if (!readInput(reader, input)) {
            return false;
        }
        auto& last = inputs[peer.slot];
        input.parry |= last.parry;
        input.attackPressed |= last.attackPressed;
        last = input;
        return true;
    }
    if (kind == Kind::Choice) {
        const int level = reader.byte();
        const int index = reader.byte();
        if (!reader.done() || level < 1 || level > Arena::MaxLevel || index >= 3) {
            return false;
        }
        const auto actorIndex = static_cast<std::size_t>(peer.slot);
        if (actorIndex < frame.actors.size() && frame.actors[actorIndex].level == level
            && frame.choosing[actorIndex] && choiceLevels[actorIndex] != level) {
            choices.emplace_back(peer.slot, index);
            choiceLevels[actorIndex] = level;
        }
        return true;
    }
    return false;
}

void LanSession::Impl::pollHost() {
    acceptClients();
    const auto now = Clock::now();
    for (auto& peer : clients) {
        if (peer.socket.open() && (peer.slot >= 0 || now - peer.joinedAt <= Timeout)) {
            if (!peer.read([&](Kind kind, Reader& reader) { return fromClient(peer, kind, reader); })
                || !peer.flush() || (peer.rejected && peer.outgoing.empty())) {
                peer.socket.close();
            }
        } else {
            peer.socket.close();
        }
        if (!peer.socket.open() && peer.slot >= 0) {
            occupied &= ~(1u << peer.slot);
            inputs[peer.slot] = {};
            if (started) {
                departures.push_back(peer.slot);
            } else {
                lobbyChanged = true;
            }
        }
    }
    clients.erase(std::remove_if(clients.begin(), clients.end(), [](const Peer& peer) {
        return !peer.socket.open();
    }), clients.end());
    if (lobbyChanged) {
        sendLobby();
    }
}

void LanSession::Impl::disconnect(const std::string& reason) {
    upstream.socket.close();
    error = reason;
}

void LanSession::Impl::sendHello() {
    Writer writer(Kind::Hello);
    writer.word(Protocol);
    if (!upstream.queue(writer.finish()) || !upstream.flush()) {
        disconnect("방에 접속할 수 없습니다.");
    }
}

void LanSession::Impl::finishConnect() {
    const auto socket = upstream.socket.get();
    fd_set writable;
    fd_set failed;
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(socket, &writable);
    FD_SET(socket, &failed);
    timeval wait{};
    const int ready = select(0, nullptr, &writable, &failed, &wait);
    if (ready == 0) {
        return;
    }
    int reason = 0;
    int length = sizeof(reason);
    if (ready == SOCKET_ERROR || FD_ISSET(socket, &failed)
        || getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&reason), &length) != 0
        || reason != 0) {
        disconnect("방에 접속할 수 없습니다.");
        return;
    }
    connecting = false;
    sendHello();
}

bool LanSession::Impl::fromHost(Kind kind, Reader& reader) {
    if (kind == Kind::Reject && slot < 0) {
        const auto reason = static_cast<Rejection>(reader.byte());
        if (reader.done()) {
            switch (reason) {
            case Rejection::Full: error = "방이 가득 찼습니다."; break;
            case Rejection::Started: error = "이미 시작된 경기입니다."; break;
            case Rejection::Version: error = "호스트와 게임 버전이 다릅니다."; break;
            }
        }
        return false;
    }
    if (kind == Kind::Lobby && !started) {
        MatchConfig received;
        received.teams = reader.byte();
        for (auto& size : received.teamSizes) {
            size = reader.byte();
        }
        const int assigned = reader.byte();
        const auto mask = reader.word();
        if (!reader.done() || received.teams < 2 || received.teams > 4
            || std::any_of(received.teamSizes.begin(), received.teamSizes.end(), [](int size) {
                return size < 1 || size > 4;
            })) {
            return false;
        }
        const int count = playerCount(received);
        if (assigned < 1 || assigned >= count || (slot >= 0 && assigned != slot)
            || mask >= (1u << count) || (mask & 1u) == 0 || (mask & (1u << assigned)) == 0) {
            return false;
        }
        config = received;
        slot = assigned;
        occupied = mask;
        return true;
    }
    if (kind == Kind::Start && slot > 0 && !started && reader.done()) {
        started = true;
        return true;
    }
    if (kind == Kind::Frame && started) {
        if (!readFrame(reader, config, pendingFrame)) {
            return false;
        }
        std::swap(frame, pendingFrame);
        ++revision;
        return true;
    }
    return false;
}

void LanSession::Impl::pollGuest() {
    if (slot < 0 && Clock::now() - upstream.joinedAt > Timeout) {
        disconnect("접속 시간이 초과됐습니다. 방 코드와 방 상태를 확인하세요.");
        return;
    }
    if (connecting) {
        finishConnect();
    }
    if (connecting || !upstream.socket.open()) {
        return;
    }
    if (!upstream.read([&](Kind kind, Reader& reader) { return fromHost(kind, reader); })
        || !upstream.flush()) {
        disconnect(error.empty() ? "호스트와 연결이 끊겼습니다." : error);
    }
}

void LanSession::poll() {
    if (impl_->hosting && impl_->listener.open()) {
        impl_->pollHost();
    } else if (!impl_->hosting && impl_->upstream.socket.open()) {
        impl_->pollGuest();
    }
}

bool LanSession::hosting() const { return impl_->hosting; }
bool LanSession::started() const { return impl_->started; }
bool LanSession::connected() const {
    return impl_->slot >= 0 && (hosting() ? impl_->listener.open() : impl_->upstream.socket.open());
}
const std::string& LanSession::error() const { return impl_->error; }
int LanSession::localSlot() const { return impl_->slot; }
const MatchConfig& LanSession::config() const { return impl_->config; }
const MatchFrame& LanSession::frame() const { return impl_->frame; }
std::uint64_t LanSession::frameRevision() const { return impl_->revision; }

std::array<bool, SlotLimit> LanSession::occupiedSlots() const {
    std::array<bool, SlotLimit> occupied{};
    for (int slot = 0; slot < SlotLimit; ++slot) {
        occupied[slot] = (impl_->occupied & (1u << slot)) != 0;
    }
    return occupied;
}

int LanSession::connectedPlayers() const {
    const auto occupied = occupiedSlots();
    return static_cast<int>(std::count(occupied.begin(), occupied.end(), true));
}

void LanSession::start() {
    auto& state = *impl_;
    if (!hosting() || !connected() || started() || connectedPlayers() != playerCount(state.config)) {
        return;
    }
    state.started = true;
    const auto packet = Writer(Kind::Start).finish();
    for (auto& peer : state.clients) {
        if (peer.slot >= 0 && peer.queue(packet) && !peer.flush()) {
            peer.socket.close();
        }
    }
}

void LanSession::sendInput(const Input& input) {
    if (!hosting() && started() && connected()) {
        auto& peer = impl_->upstream;
        if (!peer.queue(writeInput(input)) || !peer.flush()) {
            impl_->disconnect("호스트와 연결이 끊겼습니다.");
        }
    }
}

void LanSession::sendChoice(int index) {
    if (hosting() || !started() || !connected() || index < 0 || index >= 3) {
        return;
    }
    const auto slot = static_cast<std::size_t>(localSlot());
    if (slot >= frame().actors.size() || !frame().choosing[slot]) {
        return;
    }
    Writer writer(Kind::Choice);
    writer.byte(static_cast<Byte>(frame().actors[slot].level));
    writer.byte(static_cast<Byte>(index));
    auto& peer = impl_->upstream;
    if (!peer.queue(writer.finish()) || !peer.flush()) {
        impl_->disconnect("호스트와 연결이 끊겼습니다.");
    }
}

std::vector<Input> LanSession::takeInputs() {
    auto inputs = impl_->inputs;
    for (auto& input : impl_->inputs) {
        input.parry = input.attackPressed = false;
    }
    return inputs;
}

std::vector<std::pair<int, int>> LanSession::takeChoices() {
    return std::exchange(impl_->choices, {});
}

std::vector<int> LanSession::takeDepartures() {
    return std::exchange(impl_->departures, {});
}

void LanSession::publish(const MatchFrame& frame) {
    if (!hosting() || !started() || frame.actors.size() != impl_->inputs.size()) {
        return;
    }
    impl_->frame = frame;
    ++impl_->revision;
    const auto packet = writeFrame(frame);
    for (auto& peer : impl_->clients) {
        if (peer.slot >= 0 && peer.socket.open() && peer.queue(packet, true) && !peer.flush()) {
            peer.socket.close();
        }
    }
}

}
