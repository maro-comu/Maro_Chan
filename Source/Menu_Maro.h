#pragma once

#include "cocos2d.h"

#include <functional>

namespace maro {

cocos2d::Scene* createMenuScene();
cocos2d::Node* createMenuLayer(std::function<void()> onContinue, std::function<void()> onLeaveMatch);

}
