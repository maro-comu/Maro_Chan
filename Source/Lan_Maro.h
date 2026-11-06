#pragma once

#include "Arena_Maro.h"

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace maro {

struct MatchFrame {
    float elapsed = 0.0f;
    std::vector<Actor> actors;
    std::vector<Bullet> bullets;
    std::vector<Blast> blasts;
    std::array<bool, 16> choosing{};
    std::array<std::array<int, 3>, 16> augments{};
    std::array<float, 16> levelTime{};
    MatchResult result = MatchResult::Playing;
    int winner = -1;
};

class LanSession {
public:
    static constexpr unsigned short Port = 48150;

    static std::shared_ptr<LanSession> host(const MatchConfig& config);
    static std::shared_ptr<LanSession> join(const std::string& ipv4);
    static std::vector<std::string> localAddresses();

    ~LanSession();
    void poll();
    bool hosting() const;
    bool started() const;
    bool connected() const;
    const std::string& error() const;
    int localSlot() const;
    int connectedPlayers() const;
    std::array<bool, 16> occupiedSlots() const;
    const MatchConfig& config() const;
    void start();
    void sendInput(const Input& input);
    void sendChoice(int index);
    std::vector<Input> takeInputs();
    std::vector<std::pair<int, int>> takeChoices();
    std::vector<int> takeDepartures();
    void publish(const MatchFrame& frame);
    const MatchFrame& frame() const;

private:
    struct Impl;
    explicit LanSession(bool hosting);
    std::unique_ptr<Impl> impl_;
};

}
