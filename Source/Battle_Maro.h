#pragma once

#include <memory>

namespace cocos2d {
class Scene;
}

namespace maro {

class LanSession;
enum class ControlMode;
cocos2d::Scene* createBattleScene(std::shared_ptr<LanSession> session, ControlMode controls);

}
