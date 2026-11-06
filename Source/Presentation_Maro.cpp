#include "Presentation_Maro.h"

#include <algorithm>
#include <cmath>

namespace maro {
namespace {

Vec blend(Vec from, Vec to, float progress) {
    return {from.x + (to.x - from.x) * progress, from.y + (to.y - from.y) * progress};
}

}

void FramePresenter::push(const MatchFrame& frame) {
    const float interval = frame.elapsed - target_.elapsed;
    source_ = displayed_;
    target_ = frame;
    displayed_ = frame;
    progress_ = 0.0f;
    const bool paused = std::any_of(frame.choosing.begin(), frame.choosing.end(), [](bool pending) {
        return pending;
    });
    duration_ = source_.actors.size() == target_.actors.size()
        && interval > 0.0f && interval <= 0.25f && !paused && frame.result == MatchResult::Playing
        ? std::clamp(interval, 1.0f / 120.0f, 0.05f) : 0.0f;
    advance(0.0f);
}

void FramePresenter::advance(float dt) {
    if (duration_ <= 0.0f) {
        return;
    }
    progress_ = std::min(duration_, progress_ + std::max(dt, 0.0f));
    const float fraction = progress_ / duration_;
    for (std::size_t i = 0; i < displayed_.actors.size(); ++i) {
        auto& actor = displayed_.actors[i];
        const auto& from = source_.actors[i];
        const auto& to = target_.actors[i];
        if (actor.alive() && from.alive()) {
            actor.position = blend(from.position, to.position, fraction);
            const auto facing = blend(from.facing, to.facing, fraction);
            const float length = std::hypot(facing.x, facing.y);
            actor.facing = length > 0.001f ? Vec{facing.x / length, facing.y / length} : to.facing;
        }
    }
    std::size_t old = 0;
    for (std::size_t i = 0; i < displayed_.bullets.size(); ++i) {
        const auto& to = target_.bullets[i];
        while (old < source_.bullets.size() && source_.bullets[old].id < to.id) {
            ++old;
        }
        if (old < source_.bullets.size() && source_.bullets[old].id == to.id) {
            displayed_.bullets[i].position = blend(source_.bullets[old].position, to.position, fraction);
        }
    }
}

}
