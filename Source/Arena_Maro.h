#pragma once

#include <array>
#include <cstdint>
#include <random>
#include <vector>

namespace maro {

struct Vec {
    float x = 0.0f;
    float y = 0.0f;
};

enum class ControlMode { Wasd, Moba };

struct MatchConfig {
    int teams = 2;
    std::array<int, 4> teamSizes{1, 1, 1, 1};
    ControlMode controls = ControlMode::Wasd;
};

struct Input {
    Vec movement;
    Vec aim;
    bool attack = false;
    bool guard = false;
    bool parry = false;
    bool moveTo = false;
    Vec destination;
    bool attackPressed = false;
};

struct Actor {
    Vec position;
    Vec facing;
    int team = 0;
    int level = 1;
    int kills = 0;
    float hp = 100.0f;
    float maxHp = 100.0f;
    float radius = 16.0f;
    float parryTime = 0.0f;
    bool guarding = false;
    bool wideGuard = false;

    bool alive() const { return hp > 0.0f; }
};

struct Bullet {
    enum Effect : std::uint8_t { Explosive = 1, Ricochet = 2, Piercing = 4, Charged = 8, Slow = 16 };

    Vec position;
    Vec velocity;
    int team = 0;
    float radius = 4.0f;
    std::uint8_t effects = 0;

private:
    friend class Arena;
    int owner = 0;
    float damage = 0.0f;
    float lifetime = 0.0f;
    std::uint16_t hitActors = 0;
    bool fragment = false;
};

struct Blast {
    Vec position;
    float radius = 0.0f;
    float remaining = 0.0f;
    int team = 0;
};

struct Obstacle {
    float left;
    float bottom;
    float right;
    float top;
};

struct Augment {
    const char* name;
    const char* description;
};

enum class MatchResult { Playing, Victory, Defeat, Draw };

class Arena {
public:
    static constexpr float Width = 1080.0f;
    static constexpr float Height = 560.0f;
    static constexpr int MaxLevel = 6;
    static constexpr float LevelInterval = 15.0f;
    static constexpr int AugmentCount = 32;
    static constexpr std::size_t BulletLimit = 512;
    static constexpr std::size_t BlastLimit = 64;
    static constexpr float BlastDuration = 0.32f;

    explicit Arena(MatchConfig config = {}, unsigned seed = std::random_device{}());

    void update(float dt, const std::vector<Input>& inputs);
    Vec directionTo(int index, Vec destination) const;
    bool chooseAugment(int index, int option);
    std::array<Augment, 3> choices(int index) const;
    std::array<int, 3> choiceIds(int index) const;
    float timeToNextLevel(int index) const;
    void removePlayer(int index);

    const std::vector<Actor>& actors() const { return actors_; }
    const std::vector<Bullet>& bullets() const { return bullets_; }
    const std::vector<Blast>& blasts() const { return blasts_; }
    static const std::array<Obstacle, 2>& obstacles();
    static const Augment& augment(int id);
    bool choosingAugment(int index) const;
    bool hasPendingAugments() const { return result_ == MatchResult::Playing && pendingAugments_ > 0; }
    MatchResult result() const { return result_; }
    float elapsed() const { return elapsed_; }
    int winner() const { return winner_; }

private:
    struct Fighter {
        float speed = 210.0f;
        float damage = 21.0f;
        float bulletSpeed = 600.0f;
        float fireInterval = 0.32f;
        float guardFactor = 0.40f;
        float parryInterval = 0.90f;
        float recovery = 0.0f;
        float killHeal = 0.0f;
        float shotCooldown = 0.0f;
        float parryCooldown = 0.0f;
        float lastHit = -10.0f;
        float nextLevelAt = LevelInterval;
        float slowUntil = 0.0f;
        float dashUntil = 0.0f;
        float phoenixUntil = 0.0f;
        float revengeAt = 0.0f;
        float guardChargeUntil = 0.0f;
        float parryHealAt = 0.0f;
        float lastShot = -10.0f;
        std::uint32_t perks = 0;
        int earnedLevels = 0;
        int charge = 0;
        std::array<int, 3> choices{};
        bool choosing = false;
        bool attackPressed = false;
        bool parryPressed = false;
        bool phoenixSpent = false;
        bool pendingFragments = false;
    };

    void step(float dt, const std::vector<Input>& inputs);
    void moveActor(Actor& actor, Vec displacement) const;
    void separateActors();
    void shoot(int index, bool stationary);
    void emitFragments(int index);
    void advanceBullets(float dt);
    void explode(const Bullet& bullet, int directTarget);
    void applyHit(const Bullet& bullet, int target);
    void settleMatch();
    void awardLevel(int index);
    void applyAugment(int index, int augment);
    void offerAugments(int index);
    bool validActor(int index) const;
    bool clearPath(Vec start, Vec end, float clearance) const;
    Vec navigate(Vec start, Vec goal, float clearance) const;

    std::vector<Actor> actors_;
    std::vector<Fighter> fighters_;
    std::vector<Bullet> bullets_;
    std::vector<Blast> blasts_;
    std::mt19937 random_;
    MatchResult result_ = MatchResult::Playing;
    int winner_ = -1;
    float elapsed_ = 0.0f;
    float accumulator_ = 0.0f;
    int pendingAugments_ = 0;
};

}
