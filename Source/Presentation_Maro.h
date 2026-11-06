#pragma once

#include "Lan_Maro.h"

namespace maro {

class FramePresenter {
public:
    void push(const MatchFrame& frame);
    void advance(float dt);
    const MatchFrame& frame() const { return displayed_; }

private:
    MatchFrame source_;
    MatchFrame target_;
    MatchFrame displayed_;
    float duration_ = 0.0f;
    float progress_ = 0.0f;
};

}
