#pragma once

#include "Arena_Maro.h"

#include <functional>
#include <string>

namespace cocos2d {
class Node;
}

namespace maro {

struct Settings {
    ControlMode controls = ControlMode::Wasd;
    float masterVolume = 0.8f;
    float effectsVolume = 1.0f;
    bool screenShake = true;
    bool showFps = false;
    bool fullscreen = false;
    int windowSize = 3;
    int frameLimit = 60;
    bool vSync = false;
};

Settings loadSettings();
void saveSettings(const Settings& settings);
void setGameFullscreen(bool fullscreen);
void applyDisplaySettings(const Settings& settings);
cocos2d::Node* createSettingsOverlay(const Settings& settings,
    std::function<void(const Settings&)> onChange, std::function<void()> onClose,
    std::function<void()> onLeave = {}, const std::string& leaveLabel = {},
    std::function<void()> onMain = {});

}
