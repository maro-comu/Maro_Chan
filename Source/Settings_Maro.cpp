#include "Settings_Maro.h"

#include "Ui_Maro.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <new>
#include <utility>

namespace maro {
namespace {

using namespace cocos2d;

constexpr auto ControlKey = "maro.controlMode";
constexpr auto MasterVolumeKey = "maro.masterVolume";
constexpr auto EffectsVolumeKey = "maro.effectsVolume";
constexpr auto ScreenShakeKey = "maro.screenShake";
constexpr auto ShowFpsKey = "maro.showFps";
constexpr auto FullscreenKey = "maro.fullscreen";
constexpr auto WindowSizeKey = "maro.windowSize";
constexpr auto FrameLimitKey = "maro.frameLimit";
constexpr auto VSyncKey = "maro.vSync";
constexpr std::array<const char*, 5> WindowSizes{"960 × 600", "1280 × 800", "1440 × 900", "1600 × 1000", "1920 × 1200"};
constexpr std::array<int, 3> FrameLimits{60, 120, 144};
const Vec2 LeftAnchor{0.0f, 0.5f};
const Vec2 RightAnchor{1.0f, 0.5f};

float volume(float value, float fallback) {
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : fallback;
}

void buttonText(MenuItemLabel* button, const std::string& text) {
    static_cast<Label*>(button->getLabel())->setString(text);
}

class SettingsOverlay final : public Node {
public:
    bool initOverlay(const Settings& settings, std::function<void(const Settings&)> onChange,
                     std::function<void()> onClose, std::function<void()> onLeave,
                     const std::string& leaveLabel, std::function<void()> onMain) {
        if (!Node::init()) {
            return false;
        }
        settings_ = settings;
        onChange_ = std::move(onChange);

        ui::panel(this, {0, 0, 1280, 800}, {0.025f, 0.035f, 0.052f, 0.88f});
        ui::panel(this, {104, 64, 1072, 672}, Color4F(ui::Background), Color4F(ui::Border));
        ui::label(this, "설정", 35, {138, 685}, ui::Ink, LeftAnchor);
        ui::label(this, "변경 사항 자동 저장", 14, {1142, 681}, ui::Muted, RightAnchor);
        ui::panel(this, {136, 178, 310, 448}, Color4F(ui::Surface), Color4F(ui::Border));
        ui::panel(this, {462, 178, 294, 448}, Color4F(ui::Surface), Color4F(ui::Border));
        ui::panel(this, {772, 178, 372, 448}, Color4F(ui::Surface), Color4F(ui::Border));

        auto* buttons = ui::menu(this);
        buttons->setLocalZOrder(1);
        buildControls(buttons);
        buildPreferences(buttons);
        ui::button(buttons, "기본값 복원", {221, 120}, {170, 42}, [this] {
            settings_ = Settings{};
            changed();
        }, false, 16);
        const bool bothActions = onLeave && onMain;
        if (onLeave) {
            ui::button(buttons, leaveLabel.empty() ? "나가기" : leaveLabel, {bothActions ? 740.0f : 638.0f, 120}, {192, 42},
                std::move(onLeave), false, 16);
        }
        if (onMain) {
            ui::button(buttons, "메인으로", {bothActions ? 536.0f : 638.0f, 120}, {142, 42},
                std::move(onMain), false, 16);
        }
        if (onClose) {
            ui::button(buttons, "계속하기  /  ESC", {1040, 120}, {208, 42}, std::move(onClose), true, 16);
        }

        auto* barrier = EventListenerTouchOneByOne::create();
        barrier->setSwallowTouches(true);
        barrier->onTouchBegan = [](Touch*, Event*) { return true; };
        getEventDispatcher()->addEventListenerWithSceneGraphPriority(barrier, this);
        refresh();
        return true;
    }

private:
    void buildControls(Menu* buttons) {
        ui::label(this, "조작", 22, {158, 584}, ui::Ink, LeftAnchor);
        wasd_ = ui::button(buttons, "WASD", {220, 534}, {124, 40}, [this] {
            selectControls(ControlMode::Wasd);
        }, false, 17);
        moba_ = ui::button(buttons, "MOBA", {362, 534}, {124, 40}, [this] {
            selectControls(ControlMode::Moba);
        }, false, 17);

        ui::panel(this, {158, 494, 266, 1}, Color4F(ui::Border));
        constexpr std::array<const char*, 5> actions{"이동", "조준", "사격", "방어", "패링"};
        for (std::size_t row = 0; row < actions.size(); ++row) {
            const auto y = 455.0f - static_cast<float>(row) * 42.0f;
            ui::label(this, actions[row], 16, {158, y}, ui::Muted, LeftAnchor);
            bindings_[row] = ui::label(this, " ", 16, {424, y}, ui::Ink, RightAnchor);
        }
    }

    void buildPreferences(Menu* buttons) {
        ui::label(this, "소리", 22, {484, 584}, ui::Ink, LeftAnchor);
        volumeRow(buttons, "전체 음량", 518, &Settings::masterVolume, masterValue_);
        volumeRow(buttons, "효과음 음량", 462, &Settings::effectsVolume, effectsValue_);
        ui::label(this, "화면", 22, {794, 584}, ui::Ink, LeftAnchor);
        fullscreen_ = toggleRow(buttons, "화면 모드", 534, &Settings::fullscreen);
        windowSize_ = choiceRow(buttons, "창 크기", 484, [this](int delta) {
            const int next = std::clamp(settings_.windowSize + delta, 0, 4);
            if (next != settings_.windowSize) {
                settings_.windowSize = next;
                changed();
            }
        });
        frameLimit_ = choiceRow(buttons, "FPS 제한", 434, [this](int delta) {
            const auto current = std::find(FrameLimits.begin(), FrameLimits.end(), settings_.frameLimit);
            const int index = current == FrameLimits.end() ? 0 : static_cast<int>(current - FrameLimits.begin());
            const int next = FrameLimits[std::clamp(index + delta, 0, 2)];
            if (next != settings_.frameLimit) {
                settings_.frameLimit = next;
                changed();
            }
        });
        vSync_ = toggleRow(buttons, "수직 동기화", 384, &Settings::vSync);
        shake_ = toggleRow(buttons, "화면 흔들림", 334, &Settings::screenShake);
        fps_ = toggleRow(buttons, "FPS 표시", 284, &Settings::showFps);
    }

    void volumeRow(Menu* buttons, const char* title, float y, float Settings::*setting, Label*& value) {
        ui::label(this, title, 16, {484, y}, ui::Ink, LeftAnchor);
        value = ui::label(this, " ", 17, {665, y}, ui::Ink);
        ui::button(buttons, "−", {614, y}, {36, 32}, [this, setting] { adjustVolume(setting, -0.1f); }, false, 19);
        ui::button(buttons, "+", {716, y}, {36, 32}, [this, setting] { adjustVolume(setting, 0.1f); }, false, 19);
    }

    MenuItemLabel* toggleRow(Menu* buttons, const char* title, float y, bool Settings::*setting) {
        ui::label(this, title, 16, {794, y}, ui::Ink, LeftAnchor);
        return ui::button(buttons, " ", {1064, y}, {116, 34}, [this, setting] {
            settings_.*setting = !(settings_.*setting);
            changed();
        }, false, 15);
    }

    Label* choiceRow(Menu* buttons, const char* title, float y, std::function<void(int)> choose) {
        ui::label(this, title, 16, {794, y}, ui::Ink, LeftAnchor);
        auto* value = ui::label(this, " ", 15, {1018, y}, ui::Ink);
        ui::button(buttons, "‹", {930, y}, {32, 34}, [choose] { choose(-1); }, false, 21);
        ui::button(buttons, "›", {1106, y}, {32, 34}, [choose] { choose(1); }, false, 21);
        return value;
    }

    void selectControls(ControlMode mode) {
        if (settings_.controls != mode) {
            settings_.controls = mode;
            changed();
        }
    }

    void adjustVolume(float Settings::*setting, float delta) {
        auto& value = settings_.*setting;
        const auto next = std::clamp(std::round((value + delta) * 10.0f) / 10.0f, 0.0f, 1.0f);
        if (next != value) {
            value = next;
            changed();
        }
    }

    void changed() {
        saveSettings(settings_);
        Director::getInstance()->setDisplayStats(settings_.showFps);
        refresh();
        if (onChange_) {
            const auto callback = onChange_;
            const auto settings = settings_;
            callback(settings);
        }
    }

    void refresh() {
        const bool moba = settings_.controls == ControlMode::Moba;
        constexpr std::array<const char*, 5> WasdBindings{"W · A · S · D", "마우스", "좌클릭", "우클릭", "우클릭 + 좌클릭"};
        constexpr std::array<const char*, 5> MobaBindings{"좌클릭", "마우스", "Q", "E", "Shift"};
        const auto& bindings = moba ? MobaBindings : WasdBindings;
        for (std::size_t row = 0; row < bindings_.size(); ++row) {
            bindings_[row]->setString(bindings[row]);
        }
        ui::setButtonActive(wasd_, !moba);
        ui::setButtonActive(moba_, moba);
        masterValue_->setString(std::to_string(static_cast<int>(std::lround(settings_.masterVolume * 100.0f))) + "%");
        effectsValue_->setString(std::to_string(static_cast<int>(std::lround(settings_.effectsVolume * 100.0f))) + "%");
        updateToggle(shake_, settings_.screenShake, "켜기", "끄기");
        updateToggle(fps_, settings_.showFps, "켜기", "끄기");
        updateToggle(fullscreen_, settings_.fullscreen, "전체 화면", "창 모드");
        updateToggle(vSync_, settings_.vSync, "켜기", "끄기");
        windowSize_->setString(WindowSizes[settings_.windowSize]);
        frameLimit_->setString(std::to_string(settings_.frameLimit) + " FPS");
    }

    static void updateToggle(MenuItemLabel* button, bool enabled, const char* on, const char* off) {
        buttonText(button, enabled ? on : off);
        ui::setButtonActive(button, enabled);
    }

    Settings settings_;
    std::function<void(const Settings&)> onChange_;
    MenuItemLabel* wasd_ = nullptr;
    MenuItemLabel* moba_ = nullptr;
    MenuItemLabel* shake_ = nullptr;
    MenuItemLabel* fps_ = nullptr;
    MenuItemLabel* fullscreen_ = nullptr;
    MenuItemLabel* vSync_ = nullptr;
    Label* masterValue_ = nullptr;
    Label* effectsValue_ = nullptr;
    Label* windowSize_ = nullptr;
    Label* frameLimit_ = nullptr;
    std::array<Label*, 5> bindings_{};
};

}

Settings loadSettings() {
    auto* defaults = cocos2d::UserDefault::getInstance();
    Settings settings;
    settings.controls = defaults->getBoolForKey(ControlKey) ? ControlMode::Moba : ControlMode::Wasd;
    settings.masterVolume = volume(defaults->getFloatForKey(MasterVolumeKey, settings.masterVolume), settings.masterVolume);
    settings.effectsVolume = volume(defaults->getFloatForKey(EffectsVolumeKey, settings.effectsVolume), settings.effectsVolume);
    settings.screenShake = defaults->getBoolForKey(ScreenShakeKey, settings.screenShake);
    settings.showFps = defaults->getBoolForKey(ShowFpsKey, settings.showFps);
    settings.fullscreen = defaults->getBoolForKey(FullscreenKey, settings.fullscreen);
    settings.windowSize = std::clamp(defaults->getIntegerForKey(WindowSizeKey, settings.windowSize), 0, 4);
    settings.frameLimit = defaults->getIntegerForKey(FrameLimitKey, settings.frameLimit);
    if (std::find(FrameLimits.begin(), FrameLimits.end(), settings.frameLimit) == FrameLimits.end()) {
        settings.frameLimit = 60;
    }
    settings.vSync = defaults->getBoolForKey(VSyncKey, settings.vSync);
    return settings;
}

void saveSettings(const Settings& settings) {
    auto* defaults = cocos2d::UserDefault::getInstance();
    const Settings fallback;
    defaults->setBoolForKey(ControlKey, settings.controls == ControlMode::Moba);
    defaults->setFloatForKey(MasterVolumeKey, volume(settings.masterVolume, fallback.masterVolume));
    defaults->setFloatForKey(EffectsVolumeKey, volume(settings.effectsVolume, fallback.effectsVolume));
    defaults->setBoolForKey(ScreenShakeKey, settings.screenShake);
    defaults->setBoolForKey(ShowFpsKey, settings.showFps);
    defaults->setBoolForKey(FullscreenKey, settings.fullscreen);
    defaults->setIntegerForKey(WindowSizeKey, std::clamp(settings.windowSize, 0, 4));
    defaults->setIntegerForKey(FrameLimitKey,
        std::find(FrameLimits.begin(), FrameLimits.end(), settings.frameLimit) == FrameLimits.end() ? 60 : settings.frameLimit);
    defaults->setBoolForKey(VSyncKey, settings.vSync);
    defaults->flush();
}

cocos2d::Node* createSettingsOverlay(const Settings& settings,
    std::function<void(const Settings&)> onChange, std::function<void()> onClose,
    std::function<void()> onLeave, const std::string& leaveLabel, std::function<void()> onMain) {
    auto* overlay = new (std::nothrow) SettingsOverlay;
    if (overlay && overlay->initOverlay(settings, std::move(onChange), std::move(onClose),
        std::move(onLeave), leaveLabel, std::move(onMain))) {
        overlay->autorelease();
        return overlay;
    }
    delete overlay;
    return nullptr;
}

}
