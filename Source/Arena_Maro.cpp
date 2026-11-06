#include "Arena_Maro.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace maro {
namespace {

constexpr float FixedStep = 1.0f / 120.0f;
constexpr float MoveStopDistance = 4.0f;
constexpr float DefenseArc = 0.30f;
constexpr int NoActor = -1;
constexpr float NoHit = std::numeric_limits<float>::infinity();

enum class Upgrade {
    Vitality, Quickdraw, HeavyRounds, Sprinter, Bulwark, Riposte, Velocity, Recovery, Siphon,
    Shotgun, Explosive, Ricochet, Piercing, Slow, Lifesteal, EmergencySprint, ParryRush, Phoenix,
    Overcharge, SteadyAim, Revenge, ChainParry, BiggerBlast, SplitParry, GuardCharge, FrostShatter,
    Firework, FinalMagazine, HotBounce, WideGuard, OpeningShot, ParryMedic
};

constexpr auto upgradeBit(Upgrade upgrade) { return std::uint32_t{1} << static_cast<int>(upgrade); }
constexpr int FirstPerk = static_cast<int>(Upgrade::Shotgun);
constexpr int LastWeapon = static_cast<int>(Upgrade::Piercing);

constexpr std::array<Augment, Arena::AugmentCount> Upgrades{{
    {"활력", "최대 체력이 25 늘고 체력 25를 회복합니다."},
    {"속사", "사격 간격이 14% 짧아집니다."},
    {"강화탄", "탄 한 발의 피해가 5 늘어납니다."},
    {"질주", "이동 속도가 26 늘어납니다."},
    {"철벽", "방어로 받는 피해가 22% 줄어듭니다."},
    {"반격", "패링 재사용 대기시간이 20% 줄어듭니다."},
    {"고속탄", "탄속이 초당 120만큼 빨라집니다."},
    {"재생", "4초간 피해를 피하면 초당 체력 3을 회복합니다."},
    {"흡수", "적을 처치하면 체력 20을 회복합니다."},
    {"산탄총", "탄 5발을 부채꼴로 쏩니다. 탄마다 피해 36%, 사격 간격은 40% 늘어납니다."},
    {"폭발탄", "탄이 충돌해 멈추면 70 범위의 다른 적에게 45% 피해를 줍니다. 엄폐물이 폭발을 막습니다."},
    {"도탄", "탄이 벽이나 엄폐물에 한 번 튕깁니다."},
    {"관통탄", "한 발로 적 둘을 맞힐 수 있습니다. 두 번째 피해는 65%입니다."},
    {"냉각탄", "맞은 적의 이동 속도가 0.85초간 26% 줄어듭니다."},
    {"흡혈탄", "입힌 실제 피해의 14%만큼 체력을 회복합니다."},
    {"위기 질주", "체력이 35% 미만이면 이동 속도가 30% 빨라집니다."},
    {"패링 돌진", "패링을 쓰면 0.30초간 이동 속도가 65% 빨라집니다."},
    {"불사조", "경기 중 한 번 치명타를 버티고 체력 26과 0.6초 보호를 얻습니다."},
    {"과충전", "매 3번째 사격의 피해가 55%, 탄속이 20% 늘어납니다."},
    {"조준 안정", "거의 움직이지 않을 때 사격 간격이 20% 짧아집니다."},
    {"복수의 파편", "피해를 버티면 약한 파편 8발을 흩뿌립니다. 재사용 대기시간 2.5초."},
    {"연속 패링", "탄을 반사할 때마다 패링을 즉시 다시 쓸 수 있습니다."},
    {"폭발 증폭", "폭발 범위가 96으로, 주변 피해가 60%로 늘어납니다."},
    {"패링 분열", "탄을 반사하면 30% 피해의 파편 두 발이 옆으로 퍼집니다."},
    {"방패 반격", "방어로 피해를 줄이면 5초 안의 다음 사격 피해가 35% 늘어납니다."},
    {"빙결 파쇄", "이미 느려진 적에게 30% 추가 피해를 줍니다."},
    {"처치 폭죽", "처치한 적의 자리에서 폭발해 주변 적에게 기본 사격의 45% 피해를 줍니다."},
    {"마지막 탄창", "체력이 35% 미만이면 사격 간격이 27% 짧아집니다."},
    {"도탄 과열", "벽에 튕긴 탄의 피해가 45% 늘어납니다."},
    {"강철 방패", "정면 방어 범위가 양옆까지 넓어집니다."},
    {"첫발 강화", "1.35초 이상 사격하지 않으면 다음 사격 피해가 40% 늘어납니다."},
    {"전투 의무병", "탄을 반사하면 체력을 7 회복합니다. 회복 간격은 최소 0.45초입니다."}
}};

constexpr std::array<Obstacle, 2> Cover{{
    {348.0f, 248.0f, 418.0f, 424.0f}, {662.0f, 136.0f, 732.0f, 312.0f}
}};

Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y}; }
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y}; }
Vec operator*(Vec v, float scalar) { return {v.x * scalar, v.y * scalar}; }
float dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y; }
float lengthSquared(Vec v) { return dot(v, v); }
Vec perpendicular(Vec v) { return {-v.y, v.x}; }
bool finite(Vec v) { return std::isfinite(v.x) && std::isfinite(v.y); }

Vec unit(Vec v) {
    const auto squared = lengthSquared(v);
    return squared > 0.000001f ? v * (1.0f / std::sqrt(squared)) : Vec{};
}

Vec bounded(Vec point, float margin) {
    return {std::clamp(point.x, margin, Arena::Width - margin),
            std::clamp(point.y, margin, Arena::Height - margin)};
}

bool inside(Vec point, const Obstacle& wall, float clearance) {
    return point.x >= wall.left - clearance && point.x <= wall.right + clearance
        && point.y >= wall.bottom - clearance && point.y <= wall.top + clearance;
}

float wallHit(Vec start, Vec delta, const Obstacle& wall, float clearance, Vec* normal = nullptr) {
    const std::array<float, 2> origin{start.x, start.y};
    const std::array<float, 2> movement{delta.x, delta.y};
    const std::array<float, 2> low{wall.left - clearance, wall.bottom - clearance};
    const std::array<float, 2> high{wall.right + clearance, wall.top + clearance};
    float first = 0.0f;
    float last = 1.0f;
    for (int axis = 0; axis < 2; ++axis) {
        if (std::abs(movement[axis]) < 0.000001f) {
            if (origin[axis] < low[axis] || origin[axis] > high[axis]) {
                return NoHit;
            }
            continue;
        }
        const auto entry = (low[axis] - origin[axis]) / movement[axis];
        const auto exit = (high[axis] - origin[axis]) / movement[axis];
        const auto near = std::min(entry, exit);
        if (near >= first) {
            first = near;
            if (normal) {
                const auto sign = movement[axis] > 0.0f ? -1.0f : 1.0f;
                *normal = axis == 0 ? Vec{sign, 0.0f} : Vec{0.0f, sign};
            }
        }
        last = std::min(last, std::max(entry, exit));
        if (first > last) {
            return NoHit;
        }
    }
    return first;
}

float boundaryHit(Vec start, Vec delta, float radius, Vec& normal) {
    const auto end = start + delta;
    float x = NoHit;
    float y = NoHit;
    if (delta.x < 0.0f && end.x < radius) {
        x = (radius - start.x) / delta.x;
    } else if (delta.x > 0.0f && end.x > Arena::Width - radius) {
        x = (Arena::Width - radius - start.x) / delta.x;
    }
    if (delta.y < 0.0f && end.y < radius) {
        y = (radius - start.y) / delta.y;
    } else if (delta.y > 0.0f && end.y > Arena::Height - radius) {
        y = (Arena::Height - radius - start.y) / delta.y;
    }
    normal = x <= y ? Vec{delta.x > 0.0f ? -1.0f : 1.0f, 0.0f}
                    : Vec{0.0f, delta.y > 0.0f ? -1.0f : 1.0f};
    return std::max(0.0f, std::min(x, y));
}

float actorHit(Vec start, Vec delta, const Actor& actor, float bulletRadius) {
    const auto offset = start - actor.position;
    const auto radius = actor.radius + bulletRadius;
    const auto outside = lengthSquared(offset) - radius * radius;
    if (outside <= 0.0f) {
        return 0.0f;
    }
    const auto travel = lengthSquared(delta);
    const auto approach = dot(offset, delta);
    const auto discriminant = approach * approach - travel * outside;
    if (travel <= 0.0f || approach >= 0.0f || discriminant < 0.0f) {
        return NoHit;
    }
    const auto first = (-approach - std::sqrt(discriminant)) / travel;
    return first <= 1.0f ? first : NoHit;
}

}

Arena::Arena(MatchConfig config, unsigned seed)
    : random_(seed) {
    config.teams = std::clamp(config.teams, 2, 4);
    std::array<Vec, 4> spawns{{{106.0f, 280.0f}, {974.0f, 280.0f},
                               {540.0f, 84.0f}, {540.0f, 476.0f}}};
    if (config.teams == 3) {
        spawns[1] = {930.0f, 102.0f};
        spawns[2] = {930.0f, 458.0f};
    }

    actors_.reserve(16);
    fighters_.reserve(16);
    bullets_.reserve(BulletLimit);
    blasts_.reserve(BlastLimit);
    const Vec center{Width * 0.5f, Height * 0.5f};
    for (int team = 0; team < config.teams; ++team) {
        const auto count = std::clamp(config.teamSizes[team], 1, 4);
        const auto facing = unit(center - spawns[team]);
        const auto row = perpendicular(facing);
        for (int slot = 0; slot < count; ++slot) {
            Actor actor;
            actor.position = spawns[team] + row * ((slot - (count - 1) * 0.5f) * 44.0f);
            actor.facing = facing;
            actor.team = team;
            actors_.push_back(actor);
            fighters_.emplace_back();
            offerAugments(static_cast<int>(actors_.size()) - 1);
        }
    }
}

void Arena::update(float dt, const std::vector<Input>& inputs) {
    if (hasPendingAugments() || result_ != MatchResult::Playing || !(dt > 0.0f)) {
        return;
    }
    for (std::size_t index = 0; index < std::min(inputs.size(), fighters_.size()); ++index) {
        auto& fighter = fighters_[index];
        fighter.parryPressed |= inputs[index].parry;
        fighter.attackPressed |= inputs[index].attackPressed;
    }
    accumulator_ += std::min(dt, 0.25f);
    while (accumulator_ >= FixedStep) {
        accumulator_ -= FixedStep;
        step(FixedStep, inputs);
        if (hasPendingAugments() || result_ != MatchResult::Playing) {
            accumulator_ = 0.0f;
            break;
        }
    }
}

void Arena::step(float dt, const std::vector<Input>& inputs) {
    elapsed_ += dt;
    for (auto& blast : blasts_) {
        blast.remaining -= dt;
    }
    blasts_.erase(std::remove_if(blasts_.begin(), blasts_.end(), [](const Blast& blast) {
        return blast.remaining <= 0.0f;
    }), blasts_.end());
    for (int index = 0; index < static_cast<int>(actors_.size()); ++index) {
        auto& actor = actors_[index];
        if (!actor.alive()) {
            continue;
        }
        auto& fighter = fighters_[index];
        Input controls;
        controls.aim = actor.position + actor.facing;
        if (static_cast<std::size_t>(index) < inputs.size()) {
            controls = inputs[index];
        }
        if (finite(controls.aim)) {
            const auto facing = unit(bounded(controls.aim, 0.0f) - actor.position);
            if (lengthSquared(facing) > 0.0f) {
                actor.facing = facing;
            }
        }

        actor.parryTime = std::max(0.0f, actor.parryTime - dt);
        fighter.parryCooldown = std::max(0.0f, fighter.parryCooldown - dt);
        fighter.shotCooldown = std::max(0.0f, fighter.shotCooldown - dt);
        if (std::exchange(fighter.parryPressed, false) && fighter.parryCooldown == 0.0f) {
            actor.parryTime = 0.22f;
            fighter.parryCooldown = fighter.parryInterval;
            if (fighter.perks & upgradeBit(Upgrade::ParryRush)) {
                fighter.dashUntil = elapsed_ + 0.30f;
            }
        }
        actor.guarding = controls.guard;
        const bool defending = actor.guarding || actor.parryTime > 0.0f;
        const auto manual = finite(controls.movement)
            ? Vec{std::clamp(controls.movement.x, -1.0f, 1.0f), std::clamp(controls.movement.y, -1.0f, 1.0f)} : Vec{};
        const auto movement = controls.moveTo ? directionTo(index, controls.destination)
            : lengthSquared(manual) > 1.0f ? unit(manual) : manual;
        const auto dash = fighter.dashUntil > elapsed_;
        auto speed = fighter.speed * (dash ? 1.65f : defending ? 0.62f : 1.0f);
        if ((fighter.perks & upgradeBit(Upgrade::EmergencySprint)) && actor.hp < actor.maxHp * 0.35f) {
            speed *= 1.30f;
        }
        if (fighter.slowUntil > elapsed_) {
            speed *= 0.74f;
        }
        moveActor(actor, movement * (speed * dt));
        const auto attack = std::exchange(fighter.attackPressed, false) || controls.attack;
        if (attack && !defending && fighter.shotCooldown == 0.0f) {
            shoot(index, lengthSquared(movement) < 0.01f);
        }
        if (fighter.recovery > 0.0f && actor.hp < actor.maxHp && elapsed_ - fighter.lastHit >= 4.0f) {
            actor.hp = std::min(actor.maxHp, actor.hp + fighter.recovery * dt);
        }
    }

    separateActors();
    advanceBullets(dt);
    settleMatch();
    if (result_ == MatchResult::Playing) {
        for (int index = 0; index < static_cast<int>(actors_.size()); ++index) {
            awardLevel(index);
        }
    }
}

void Arena::moveActor(Actor& actor, Vec displacement) const {
    const auto destination = bounded(actor.position + displacement, actor.radius);
    const auto open = [&](Vec point) {
        return std::none_of(Cover.begin(), Cover.end(), [&](const Obstacle& wall) {
            return inside(point, wall, actor.radius);
        });
    };

    if (open({destination.x, actor.position.y})) {
        actor.position.x = destination.x;
    }
    if (open({actor.position.x, destination.y})) {
        actor.position.y = destination.y;
    }
}

void Arena::separateActors() {
    for (std::size_t i = 0; i < actors_.size(); ++i) {
        auto& first = actors_[i];
        if (!first.alive()) {
            continue;
        }
        for (std::size_t j = i + 1; j < actors_.size(); ++j) {
            auto& second = actors_[j];
            const auto gap = second.position - first.position;
            const auto distanceSquared = lengthSquared(gap);
            const auto minimum = first.radius + second.radius;
            if (!second.alive() || distanceSquared >= minimum * minimum) {
                continue;
            }
            const auto distance = std::sqrt(distanceSquared);
            const auto away = distance > 0.001f ? gap * (1.0f / distance) : Vec{1.0f, 0.0f};
            const auto correction = away * ((minimum - distance) * 0.5f);
            moveActor(first, correction * -1.0f);
            moveActor(second, correction);
        }
    }
}

void Arena::shoot(int index, bool stationary) {
    const auto& actor = actors_[index];
    auto& fighter = fighters_[index];
    const bool shotgun = (fighter.perks & upgradeBit(Upgrade::Shotgun)) != 0;
    bool charged = false;
    if (fighter.perks & upgradeBit(Upgrade::Overcharge)) {
        fighter.charge = (fighter.charge + 1) % 3;
        charged = fighter.charge == 0;
    }
    const bool guardCharged = fighter.guardChargeUntil > elapsed_;
    const bool opening = (fighter.perks & upgradeBit(Upgrade::OpeningShot)) && elapsed_ - fighter.lastShot >= 1.35f;
    fighter.guardChargeUntil = 0.0f;
    fighter.lastShot = elapsed_;
    Bullet bullet;
    bullet.position = actor.position;
    bullet.team = actor.team;
    bullet.owner = index;
    bullet.damage = fighter.damage * (charged ? 1.55f : 1.0f) * (shotgun ? 0.36f : 1.0f)
        * (guardCharged ? 1.35f : 1.0f) * (opening ? 1.40f : 1.0f);
    bullet.lifetime = shotgun ? 0.95f : 1.6f;
    bullet.radius = shotgun ? 3.0f : 4.0f;
    if (fighter.perks & upgradeBit(Upgrade::Explosive)) { bullet.effects |= Bullet::Explosive; }
    if (fighter.perks & upgradeBit(Upgrade::Ricochet)) { bullet.effects |= Bullet::Ricochet; }
    if (fighter.perks & upgradeBit(Upgrade::Piercing)) { bullet.effects |= Bullet::Piercing; }
    if (fighter.perks & upgradeBit(Upgrade::Slow)) { bullet.effects |= Bullet::Slow; }
    if (charged || guardCharged || opening) { bullet.effects |= Bullet::Charged; }

    const auto speed = fighter.bulletSpeed * (charged ? 1.20f : 1.0f);
    const auto spread = shotgun ? 2 : 0;
    const auto side = perpendicular(actor.facing);
    for (int pellet = -spread; pellet <= spread && bullets_.size() < BulletLimit; ++pellet) {
        const auto angle = static_cast<float>(pellet) * 0.09f;
        bullet.velocity = (actor.facing * std::cos(angle) + side * std::sin(angle)) * speed;
        bullets_.push_back(bullet);
    }
    const bool steady = stationary && (fighter.perks & upgradeBit(Upgrade::SteadyAim));
    const bool finalMagazine = (fighter.perks & upgradeBit(Upgrade::FinalMagazine)) && actor.hp < actor.maxHp * 0.35f;
    fighter.shotCooldown = fighter.fireInterval * (shotgun ? 1.40f : 1.0f) * (steady ? 0.80f : 1.0f)
        * (finalMagazine ? 0.73f : 1.0f);
}

void Arena::emitFragments(int index) {
    const auto& actor = actors_[index];
    Bullet fragment;
    fragment.position = actor.position;
    fragment.team = actor.team;
    fragment.owner = index;
    fragment.radius = 2.5f;
    fragment.damage = 7.0f;
    fragment.lifetime = 0.35f;
    fragment.fragment = true;
    for (int ray = 0; ray < 8 && bullets_.size() < BulletLimit; ++ray) {
        const auto angle = static_cast<float>(ray) * 0.785398163f;
        fragment.velocity = Vec{std::cos(angle), std::sin(angle)} * 490.0f;
        bullets_.push_back(fragment);
    }
}

void Arena::advanceBullets(float dt) {
    std::size_t surviving = 0;
    std::vector<Bullet> parryFragments;
    for (auto bullet : bullets_) {
        const auto delta = bullet.velocity * std::min(dt, bullet.lifetime);
        bullet.lifetime -= dt;
        Vec normal;
        auto first = boundaryHit(bullet.position, delta, bullet.radius, normal);
        for (const auto& wall : Cover) {
            Vec surface;
            const auto hit = wallHit(bullet.position, delta, wall, bullet.radius, &surface);
            if (hit < first) {
                first = hit;
                normal = surface;
            }
        }

        auto target = NoActor;
        for (int index = 0; index < static_cast<int>(actors_.size()); ++index) {
            const auto& actor = actors_[index];
            if (!actor.alive() || actor.team == bullet.team || (bullet.hitActors & (1u << index))) {
                continue;
            }
            const auto hit = actorHit(bullet.position, delta, actor, bullet.radius);
            if (hit < first) {
                first = hit;
                target = index;
            }
        }

        bullet.position = bullet.position + delta * std::min(first, 1.0f);
        if (target != NoActor) {
            auto& actor = actors_[target];
            const auto reflected = unit(bullet.velocity) * -1.0f;
            if (actor.parryTime > 0.0f && dot(actor.facing, reflected) > DefenseArc) {
                auto& fighter = fighters_[target];
                if (fighter.perks & upgradeBit(Upgrade::ChainParry)) {
                    fighter.parryCooldown = 0.0f;
                }
                if ((fighter.perks & upgradeBit(Upgrade::ParryMedic)) && fighter.parryHealAt <= elapsed_) {
                    actor.hp = std::min(actor.maxHp, actor.hp + 7.0f);
                    fighter.parryHealAt = elapsed_ + 0.45f;
                }
                bullet.position = bullet.position + reflected * 0.5f;
                bullet.velocity = bullet.velocity * -1.0f;
                bullet.team = actor.team;
                bullet.owner = target;
                bullet.damage = std::min(bullet.damage * 1.25f, 150.0f);
                bullet.hitActors = 0;
                if (!bullet.fragment && bullet.lifetime > 0.0f && (fighter.perks & upgradeBit(Upgrade::SplitParry))) {
                    auto fragment = bullet;
                    fragment.radius = 2.5f;
                    fragment.damage *= 0.30f;
                    fragment.effects &= Bullet::Slow;
                    fragment.fragment = true;
                    const auto side = perpendicular(bullet.velocity) * 0.159318f;
                    const auto forward = bullet.velocity * 0.987227f;
                    for (const float sign : {-1.0f, 1.0f}) {
                        if (parryFragments.size() < BulletLimit) {
                            fragment.velocity = forward + side * sign;
                            parryFragments.push_back(fragment);
                        }
                    }
                }
            } else {
                applyHit(bullet, target);
                if (!(bullet.effects & Bullet::Piercing)) {
                    if (bullet.effects & Bullet::Explosive) { explode(bullet, target); }
                    continue;
                }
                bullet.effects &= ~Bullet::Piercing;
                bullet.hitActors |= static_cast<std::uint16_t>(1u << target);
                bullet.damage *= 0.65f;
            }
        } else if (first != NoHit) {
            if (!(bullet.effects & Bullet::Ricochet) || lengthSquared(normal) == 0.0f) {
                if (bullet.effects & Bullet::Explosive) { explode(bullet, NoActor); }
                continue;
            }
            bullet.position = bullet.position + normal * 0.5f;
            bullet.velocity = bullet.velocity - normal * (2.0f * dot(bullet.velocity, normal));
            bullet.effects &= ~Bullet::Ricochet;
            if (fighters_[bullet.owner].perks & upgradeBit(Upgrade::HotBounce)) {
                bullet.damage *= 1.45f;
                bullet.effects |= Bullet::Charged;
            }
        }

        if (bullet.lifetime > 0.0f && bullet.position.x >= -bullet.radius
            && bullet.position.x <= Width + bullet.radius && bullet.position.y >= -bullet.radius
            && bullet.position.y <= Height + bullet.radius) {
            bullets_[surviving++] = bullet;
        }
    }
    bullets_.resize(surviving);
    const auto fragments = std::min(parryFragments.size(), BulletLimit - bullets_.size());
    bullets_.insert(bullets_.end(), parryFragments.begin(), parryFragments.begin() + fragments);
    for (int index = 0; index < static_cast<int>(fighters_.size()); ++index) {
        if (std::exchange(fighters_[index].pendingFragments, false) && actors_[index].alive()) {
            emitFragments(index);
        }
    }
}

void Arena::explode(const Bullet& bullet, int directTarget) {
    const bool boosted = (fighters_[bullet.owner].perks & upgradeBit(Upgrade::BiggerBlast)) != 0;
    const auto radius = boosted ? 96.0f : 70.0f;
    if (blasts_.size() >= BlastLimit) {
        blasts_.erase(blasts_.begin());
    }
    blasts_.push_back({bullet.position, radius, BlastDuration, bullet.team});
    auto splash = bullet;
    splash.damage *= boosted ? 0.60f : 0.45f;
    for (int index = 0; index < static_cast<int>(actors_.size()); ++index) {
        const auto& actor = actors_[index];
        const auto offset = actor.position - bullet.position;
        if (index != directTarget && !(bullet.hitActors & (1u << index)) && actor.alive()
            && actor.team != bullet.team && lengthSquared(offset) <= radius * radius
            && clearPath(bullet.position, actor.position, 0.0f)) {
            splash.velocity = offset;
            applyHit(splash, index);
        }
    }
}

void Arena::applyHit(const Bullet& bullet, int target) {
    auto& actor = actors_[target];
    auto& defender = fighters_[target];
    if (defender.phoenixUntil > elapsed_) {
        return;
    }
    const auto guardArc = actor.wideGuard ? -0.12f : DefenseArc;
    const auto guarded = actor.guarding && dot(actor.facing, unit(bullet.velocity) * -1.0f) > guardArc;
    auto& killer = actors_[bullet.owner];
    auto& fighter = fighters_[bullet.owner];
    const bool shatter = defender.slowUntil > elapsed_ && (fighter.perks & upgradeBit(Upgrade::FrostShatter));
    const auto damage = std::min(actor.hp, bullet.damage * (guarded ? defender.guardFactor : 1.0f)
        * (shatter ? 1.30f : 1.0f));
    actor.hp -= damage;
    defender.lastHit = elapsed_;
    if (guarded && (defender.perks & upgradeBit(Upgrade::GuardCharge))) {
        defender.guardChargeUntil = elapsed_ + 5.0f;
    }
    if (bullet.effects & Bullet::Slow) {
        defender.slowUntil = elapsed_ + 0.85f;
    }
    if (!actor.alive() && !defender.phoenixSpent && (defender.perks & upgradeBit(Upgrade::Phoenix))) {
        defender.phoenixSpent = true;
        defender.phoenixUntil = elapsed_ + 0.60f;
        actor.hp = 26.0f;
    }
    if (killer.alive() && !bullet.fragment && (fighter.perks & upgradeBit(Upgrade::Lifesteal))) {
        killer.hp = std::min(killer.maxHp, killer.hp + damage * 0.14f);
    }
    if (actor.alive()) {
        if (damage > 0.0f && !bullet.fragment && defender.revengeAt <= elapsed_
            && (defender.perks & upgradeBit(Upgrade::Revenge))) {
            defender.revengeAt = elapsed_ + 2.5f;
            defender.pendingFragments = true;
        }
        return;
    }

    actor.guarding = false;
    actor.parryTime = 0.0f;
    ++killer.kills;
    if (killer.alive()) {
        fighter.earnedLevels = std::min(fighter.earnedLevels + 1, MaxLevel - killer.level);
        killer.hp = std::min(killer.maxHp, killer.hp + fighter.killHeal);
    }
    if (fighter.perks & upgradeBit(Upgrade::Firework)) {
        Bullet firework;
        firework.position = actor.position;
        firework.team = bullet.team;
        firework.owner = bullet.owner;
        firework.damage = fighter.damage;
        explode(firework, target);
    }
}

void Arena::settleMatch() {
    std::array<bool, 4> teams{};
    for (const auto& actor : actors_) {
        teams[actor.team] = teams[actor.team] || actor.alive();
    }
    const auto remaining = std::count(teams.begin(), teams.end(), true);
    if (remaining == 0) {
        result_ = MatchResult::Draw;
    } else if (remaining == 1) {
        winner_ = static_cast<int>(std::find(teams.begin(), teams.end(), true) - teams.begin());
        result_ = MatchResult::Victory;
    }
}

void Arena::awardLevel(int index) {
    auto& actor = actors_[index];
    auto& fighter = fighters_[index];
    if (!actor.alive() || actor.level >= MaxLevel || fighter.choosing) {
        return;
    }
    if (fighter.earnedLevels == 0 && elapsed_ < fighter.nextLevelAt) {
        return;
    }
    fighter.earnedLevels = std::max(0, fighter.earnedLevels - 1);
    ++actor.level;
    fighter.nextLevelAt = elapsed_ + LevelInterval;
    offerAugments(index);
}

void Arena::applyAugment(int index, int augment) {
    auto& actor = actors_[index];
    auto& fighter = fighters_[index];
    const auto upgrade = static_cast<Upgrade>(augment);
    if (augment >= FirstPerk) {
        fighter.perks |= upgradeBit(upgrade);
        if (upgrade == Upgrade::WideGuard) {
            actor.wideGuard = true;
        }
        return;
    }
    switch (upgrade) {
    case Upgrade::Vitality:
        actor.maxHp += 25.0f;
        actor.hp += 25.0f;
        break;
    case Upgrade::Quickdraw:
        fighter.fireInterval *= 0.86f;
        break;
    case Upgrade::HeavyRounds:
        fighter.damage += 5.0f;
        break;
    case Upgrade::Sprinter:
        fighter.speed += 26.0f;
        break;
    case Upgrade::Bulwark:
        fighter.guardFactor *= 0.78f;
        break;
    case Upgrade::Riposte:
        fighter.parryInterval *= 0.80f;
        break;
    case Upgrade::Velocity:
        fighter.bulletSpeed += 120.0f;
        break;
    case Upgrade::Recovery:
        fighter.recovery += 3.0f;
        break;
    case Upgrade::Siphon:
        fighter.killHeal += 20.0f;
        break;
    default:
        break;
    }
}

void Arena::offerAugments(int index) {
    auto& fighter = fighters_[index];
    std::array<int, Upgrades.size()> pool{};
    int count = 0;
    for (int id = 0; id < static_cast<int>(pool.size()); ++id) {
        const auto upgrade = static_cast<Upgrade>(id);
        const bool available = (upgrade != Upgrade::BiggerBlast || (fighter.perks & (upgradeBit(Upgrade::Explosive) | upgradeBit(Upgrade::Firework))))
            && (upgrade != Upgrade::FrostShatter || (fighter.perks & upgradeBit(Upgrade::Slow)))
            && (upgrade != Upgrade::HotBounce || (fighter.perks & upgradeBit(Upgrade::Ricochet)));
        if (available && !(fighter.perks & upgradeBit(upgrade))) {
            pool[count++] = id;
        }
    }
    int option = 0;
    if (actors_[index].level == 1) {
        std::uniform_int_distribution<int> weapon(FirstPerk, LastWeapon);
        std::swap(pool[0], pool[weapon(random_)]);
        fighter.choices[option++] = pool[0];
    }
    for (; option < static_cast<int>(fighter.choices.size()); ++option) {
        std::uniform_int_distribution<int> remaining(option, count - 1);
        std::swap(pool[option], pool[remaining(random_)]);
        fighter.choices[option] = pool[option];
    }
    std::shuffle(fighter.choices.begin(), fighter.choices.end(), random_);
    fighter.choosing = true;
    ++pendingAugments_;
}

bool Arena::chooseAugment(int index, int option) {
    if (!choosingAugment(index) || option < 0 || option >= static_cast<int>(fighters_[index].choices.size())) {
        return false;
    }
    auto& fighter = fighters_[index];
    applyAugment(index, fighter.choices[option]);
    fighter.choosing = false;
    --pendingAugments_;
    awardLevel(index);
    return true;
}

bool Arena::validActor(int index) const {
    return index >= 0 && static_cast<std::size_t>(index) < actors_.size();
}

bool Arena::choosingAugment(int index) const {
    return hasPendingAugments() && validActor(index) && fighters_[index].choosing;
}

const Augment& Arena::augment(int id) {
    static constexpr Augment empty{"", ""};
    return id >= 0 && id < AugmentCount ? Upgrades[id] : empty;
}

std::array<int, 3> Arena::choiceIds(int index) const {
    return choosingAugment(index) ? fighters_[index].choices : std::array<int, 3>{-1, -1, -1};
}

std::array<Augment, 3> Arena::choices(int index) const {
    const auto ids = choiceIds(index);
    return {augment(ids[0]), augment(ids[1]), augment(ids[2])};
}

float Arena::timeToNextLevel(int index) const {
    if (!validActor(index)) {
        return 0.0f;
    }
    const auto& player = actors_[index];
    if (!player.alive() || player.level >= MaxLevel || result_ != MatchResult::Playing) {
        return 0.0f;
    }
    return std::max(0.0f, fighters_[index].nextLevelAt - elapsed_);
}

void Arena::removePlayer(int index) {
    if (!validActor(index) || result_ != MatchResult::Playing) {
        return;
    }
    auto& actor = actors_[index];
    actor.hp = 0.0f;
    actor.guarding = false;
    actor.parryTime = 0.0f;
    auto& fighter = fighters_[index];
    if (std::exchange(fighter.choosing, false)) {
        --pendingAugments_;
    }
    fighter.attackPressed = false;
    fighter.parryPressed = false;
    bullets_.erase(std::remove_if(bullets_.begin(), bullets_.end(), [index](const Bullet& bullet) {
        return bullet.owner == index;
    }), bullets_.end());
    settleMatch();
}

const std::array<Obstacle, 2>& Arena::obstacles() {
    return Cover;
}

bool Arena::clearPath(Vec start, Vec end, float clearance) const {
    const auto delta = end - start;
    return std::none_of(Cover.begin(), Cover.end(), [&](const Obstacle& wall) {
        return wallHit(start, delta, wall, clearance) != NoHit;
    });
}

Vec Arena::directionTo(int index, Vec destination) const {
    if (!validActor(index) || !finite(destination)) {
        return {};
    }
    const auto& player = actors_[index];
    return navigate(player.position, destination, player.radius);
}

Vec Arena::navigate(Vec start, Vec goal, float clearance) const {
    goal = bounded(goal, clearance);
    for (const auto& wall : Cover) {
        if (inside(goal, wall, clearance)) {
            const std::array<Vec, 4> edges{{{wall.left - clearance - 1.0f, goal.y},
                                            {wall.right + clearance + 1.0f, goal.y},
                                            {goal.x, wall.bottom - clearance - 1.0f},
                                            {goal.x, wall.top + clearance + 1.0f}}};
            goal = *std::min_element(edges.begin(), edges.end(), [goal](Vec a, Vec b) {
                return lengthSquared(goal - a) < lengthSquared(goal - b);
            });
        }
    }

    if (lengthSquared(goal - start) <= MoveStopDistance * MoveStopDistance) {
        return {};
    }
    if (clearPath(start, goal, clearance)) {
        return unit(goal - start);
    }

    std::array<Vec, 10> nodes{start, goal};
    const auto margin = clearance + 5.0f;
    std::size_t count = 2;
    for (const auto& wall : Cover) {
        nodes[count++] = {wall.left - margin, wall.bottom - margin};
        nodes[count++] = {wall.right + margin, wall.bottom - margin};
        nodes[count++] = {wall.right + margin, wall.top + margin};
        nodes[count++] = {wall.left - margin, wall.top + margin};
    }
    std::array<float, 10> distance;
    distance.fill(NoHit);
    distance[0] = 0.0f;
    std::array<std::size_t, 10> previous{};
    std::array<bool, 10> visited{};
    for (std::size_t step = 0; step < count; ++step) {
        auto next = count;
        auto nearest = NoHit;
        for (std::size_t index = 0; index < count; ++index) {
            if (!visited[index] && distance[index] < nearest) {
                nearest = distance[index];
                next = index;
            }
        }
        if (next == count) {
            return {};
        }
        if (next == 1) {
            auto waypoint = next;
            while (previous[waypoint] != 0) {
                waypoint = previous[waypoint];
            }
            return unit(nodes[waypoint] - start);
        }
        visited[next] = true;
        for (std::size_t index = 1; index < count; ++index) {
            if (!visited[index] && clearPath(nodes[next], nodes[index], clearance)) {
                const auto candidate = nearest + std::sqrt(lengthSquared(nodes[next] - nodes[index]));
                if (candidate < distance[index]) {
                    distance[index] = candidate;
                    previous[index] = next;
                }
            }
        }
    }
    return {};
}

}
