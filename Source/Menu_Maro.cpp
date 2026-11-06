#include "Menu_Maro.h"

#include "Arena_Maro.h"
#include "Battle_Maro.h"
#include "Lan_Maro.h"
#include "RoomCode_Maro.h"
#include "Settings_Maro.h"
#include "Ui_Maro.h"
#include "ui/UIEditBox/UIEditBox.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <utility>

namespace maro {

namespace {

using namespace cocos2d;

const Vec2 leftAnchor{0.0f, 0.5f};
const Vec2 rightAnchor{1.0f, 0.5f};
constexpr const char* teamCountSetting = "maro.teamCount";
constexpr const char* roomCodeSetting = "maro.roomCode";
constexpr std::array<const char*, 4> teamSizeSettings{{
    "maro.teamSize1", "maro.teamSize2", "maro.teamSize3", "maro.teamSize4"
}};

void setText(Label* label, const std::string& text) {
    if (label->getString() != text) {
        label->setString(text);
    }
}

std::string relayEndpoint() {
    auto endpoint = FileUtils::getInstance()->getStringFromFile("Relay_Maro.cfg");
    const auto end = endpoint.find_last_not_of(" \r\n\t");
    return end == std::string::npos ? std::string{} : endpoint.substr(0, end + 1);
}

class SettingsView : public Node {
protected:
    bool init() override {
        if (!Node::init()) {
            return false;
        }
        auto* listener = EventListenerKeyboard::create();
        listener->onKeyPressed = [this](EventKeyboard::KeyCode key, Event* event) {
            if (key == EventKeyboard::KeyCode::KEY_ESCAPE) {
                event->stopPropagation();
                toggleSettings();
            }
        };
        getEventDispatcher()->addEventListenerWithSceneGraphPriority(listener, this);
        return true;
    }

    void toggleSettings() {
        if (settingsOverlay_) {
            settingsOverlay_->removeFromParent();
            settingsOverlay_ = nullptr;
            return;
        }
        settingsOverlay_ = createSettingsOverlay(loadSettings(),
            [](const Settings& settings) { applyDisplaySettings(settings); },
            [this] { toggleSettings(); });
        if (settingsOverlay_) {
            addChild(settingsOverlay_, 100);
        }
    }

private:
    Node* settingsOverlay_ = nullptr;
};

class RoomPanel final : public Node {
public:
    static RoomPanel* create(std::shared_ptr<LanSession> session, std::function<void()> onLeave) {
        auto* panel = new RoomPanel(std::move(session));
        if (!panel->initRoom(std::move(onLeave))) {
            delete panel;
            return nullptr;
        }
        panel->autorelease();
        return panel;
    }

private:
    explicit RoomPanel(std::shared_ptr<LanSession> session) : session_(std::move(session)) {}

    bool initRoom(std::function<void()> onLeave) {
        if (!Node::init()) {
            return false;
        }
        connection_ = ui::label(this, "서버 연결 중", 19, {250, 647}, ui::Ink, leftAnchor);
        connection_->setWidth(720);
        buildTeamCards();

        ui::panel(this, {48, 46, 1184, 88}, Color4F(ui::Surface), Color4F(ui::Border));
        count_ = ui::label(this, "연결 확인 중", 25, {76, 104}, ui::Ink, leftAnchor);
        status_ = ui::label(this, "", 16, {76, 72}, ui::Muted, leftAnchor);
        auto* buttons = ui::menu(this);
        copy_ = ui::button(buttons, "코드 복사", {1128, 647}, {208, 38}, [this] {
            copied_ = copyRoomCode(session_->roomCode());
            copyUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            refresh();
        }, false, 16);
        ui::button(buttons, "방 나가기", {session_->hosting() ? 905.0f : 1096.0f, 90},
                   {136, 50}, std::move(onLeave));
        if (session_->hosting()) {
            start_ = ui::button(buttons, "경기 시작", {1096, 90}, {216, 50}, [this] {
                session_->start();
            }, false, 22);
        }
        refresh();
        scheduleUpdate();
        return true;
    }

    void update(float) override {
        session_->poll();
        if (session_->started() && session_->connected() && session_->error().empty()) {
            auto* battle = createBattleScene(session_, loadSettings().controls);
            if (battle) {
                unscheduleUpdate();
                Director::getInstance()->replaceScene(battle);
            }
            return;
        }
        refresh();
    }

    void buildTeamCards() {
        for (int team = 0; team < 4; ++team) {
            const float x = 48.0f + 600.0f * (team % 2);
            const float y = 388.0f - 228.0f * (team / 2);
            auto* card = Node::create();
            card->setPosition({x, y});
            addChild(card);
            cards_[team] = card;
            const auto color = ui::teamColor(team);
            ui::panel(card, {0, 0, 584, 212}, Color4F(ui::Surface), Color4F(ui::Border));
            ui::panel(card, {0, 0, 4, 212}, Color4F(color));
            ui::label(card, std::to_string(team + 1) + "팀", 24, {27, 178}, color, leftAnchor);
            for (int seat = 0; seat < 4; ++seat) {
                seats_[team][seat] = ui::label(card, "", 18, {30, 140.0f - seat * 34}, ui::Muted, leftAnchor);
            }
        }
    }

    int refreshTeamCards(bool connected) {
        const auto& config = session_->config();
        const auto slots = session_->occupiedSlots();
        const auto local = session_->localSlot();
        int player = 0;
        for (int team = 0; team < 4; ++team) {
            const bool visible = connected && team < config.teams;
            cards_[team]->setVisible(visible);
            if (!visible) {
                continue;
            }
            for (int seat = 0; seat < 4; ++seat) {
                auto* label = seats_[team][seat];
                const bool enabled = seat < config.teamSizes[team];
                label->setVisible(enabled);
                if (enabled) {
                    const char* name = !slots[player] ? "참가 대기" : player == local ? "나" : "연결됨";
                    setText(label, std::to_string(seat + 1) + "번  /  " + name);
                    label->setTextColor(Color4B(slots[player] ? ui::Ink : ui::Muted));
                    ++player;
                }
            }
        }
        return player;
    }

    void refresh() {
        const bool connected = session_->connected();
        const auto code = session_->roomCode();
        setText(connection_, !session_->error().empty() ? "방 연결 종료"
            : code.empty() ? "서버 연결 중" : "방 코드   " + code);
        copy_->setEnabled(connected && !code.empty());
        ui::setButtonActive(copy_, connected && !code.empty());
        const bool feedback = std::chrono::steady_clock::now() < copyUntil_;
        setText(static_cast<Label*>(copy_->getLabel()), feedback ? (copied_ ? "복사 완료" : "복사 재시도") : "코드 복사");
        const int total = refreshTeamCards(connected);
        const int count = session_->connectedPlayers();
        const bool ready = connected && session_->error().empty() && count == total;
        if (start_) {
            ui::setButtonActive(start_, ready);
            start_->setEnabled(ready);
        }
        if (!session_->error().empty()) {
            setText(count_, "연결 오류");
            setText(status_, session_->error());
            status_->setTextColor(Color4B(ui::teamColor(1)));
            return;
        }
        setText(count_, connected ? "연결된 인원 " + std::to_string(count) + " / " + std::to_string(total)
                                  : "서버 연결 중");
        if (session_->hosting()) {
            setText(status_, !connected ? "인터넷 방을 준비하고 있습니다. 잠시 기다려 주세요."
                : ready ? "모든 자리가 찼습니다." : "모든 참가자가 접속하면 경기를 시작할 수 있습니다.");
        } else {
            setText(status_, connected ? "방장의 경기 시작을 기다리는 중입니다." : "방장에게 연결하는 중입니다.");
        }
    }

    std::shared_ptr<LanSession> session_;
    std::array<Node*, 4> cards_{};
    std::array<std::array<Label*, 4>, 4> seats_{};
    Label* count_ = nullptr;
    Label* status_ = nullptr;
    Label* connection_ = nullptr;
    MenuItemLabel* copy_ = nullptr;
    std::chrono::steady_clock::time_point copyUntil_{};
    bool copied_ = false;
    MenuItemLabel* start_ = nullptr;
};

class SetupLayer final : public SettingsView {
public:
    bool initMenu(std::function<void()> onContinue, std::function<void()> onLeaveMatch) {
        if (!SettingsView::init()) {
            return false;
        }
        canResume_ = static_cast<bool>(onContinue);
        if (canResume_) {
            auto* barrier = EventListenerTouchOneByOne::create();
            barrier->setSwallowTouches(true);
            barrier->onTouchBegan = [](Touch*, Event*) { return true; };
            getEventDispatcher()->addEventListenerWithSceneGraphPriority(barrier, this);
        }
        restoreSettings();

        ui::panel(this, {0, 0, 1280, 800}, Color4F(ui::Background));
        ui::label(this, "멀티 사격 게임", 58, {47, 692}, ui::Ink, leftAnchor);
        ui::label(this, "경기 준비", 18, {48, 647}, ui::Muted, leftAnchor);
        auto* header = ui::menu(this);
        ui::button(header, "나가기", {981, 692}, {146, 42}, [] {
            Director::getInstance()->end();
        }, false, 16);
        ui::button(header, "설정  /  ESC", {1156, 692}, {152, 42}, [this] {
            toggleSettings();
        }, false, 16);

        setupContent_ = Node::create();
        addChild(setupContent_);
        buildMatchSetup();
        buildRoomPanel();
        buildSummary(std::move(onContinue), std::move(onLeaveMatch));
        refresh();
        return true;
    }

private:
    struct TeamRow {
        Node* node = nullptr;
        std::array<MenuItemLabel*, 4> sizeButtons{};
    };

    void buildMatchSetup() {
        ui::panel(setupContent_, {48, 160, 696, 472}, Color4F(ui::Surface), Color4F(ui::Border));
        ui::label(setupContent_, "팀 구성", 24, {76, 595}, ui::Ink, leftAnchor);
        ui::label(setupContent_, "기본 구성", 15, {76, 549}, ui::Muted, leftAnchor);
        ui::label(setupContent_, "팀 수", 15, {76, 501}, ui::Muted, leftAnchor);

        auto* setupButtons = ui::menu(setupContent_);
        for (int i = 0; i < 4; ++i) {
            const int size = i + 1;
            const auto value = std::to_string(size);
            presets[i] = ui::button(setupButtons, value + "v" + value, {242.0f + 126.0f * i, 549}, {110, 36}, [this, size] {
                config.teams = 2;
                config.teamSizes[0] = size;
                config.teamSizes[1] = size;
                refresh();
            });
        }
        for (int i = 0; i < 3; ++i) {
            const int teams = i + 2;
            teamCountButtons[i] = ui::button(setupButtons, std::to_string(teams), {220.0f + 76.0f * i, 501}, {66, 36}, [this, teams] {
                config.teams = teams;
                refresh();
            });
        }

        ui::panel(setupContent_, {76, 472, 640, 1}, Color4F(ui::Border));
        ui::label(setupContent_, "팀", 14, {76, 448}, ui::Muted, leftAnchor);
        ui::label(setupContent_, "팀별 참가 인원", 14, {716, 448}, ui::Muted, rightAnchor);

        for (int team = 0; team < 4; ++team) {
            const float y = 414.0f - 58.0f * team;
            auto& row = teamRows[team];
            row.node = Node::create();
            setupContent_->addChild(row.node);
            ui::panel(row.node, {76, y - 24, 640, 48}, Color4F(ui::Background));
            ui::panel(row.node, {76, y - 24, 3, 48}, Color4F(ui::teamColor(team)));
            ui::label(row.node, std::to_string(team + 1) + "팀", 18, {96, y}, ui::Ink, leftAnchor);
            if (team == 0) {
                ui::label(row.node, "방장 포함", 14, {211, y}, ui::Muted, leftAnchor);
            }

            auto* rowButtons = ui::menu(row.node);
            for (int i = 0; i < 4; ++i) {
                const int size = i + 1;
                row.sizeButtons[i] = ui::button(rowButtons, std::to_string(size), {472.0f + 70.0f * i, y}, {54, 34}, [this, team, size] {
                    config.teamSizes[team] = size;
                    refresh();
                });
            }
        }
    }

    void buildRoomPanel() {
        ui::panel(setupContent_, {768, 160, 464, 472}, Color4F(ui::Surface), Color4F(ui::Border));
        auto* buttons = ui::menu(setupContent_);
        auto* create = ui::button(buttons, "방 만들기", {1000, 512}, {408, 56}, [this] { host(); }, !canResume_, 22);
        create->setEnabled(!canResume_);
        ui::panel(setupContent_, {796, 437, 408, 1}, Color4F(ui::Border));
        ui::panel(setupContent_, {796, 340, 408, 50}, Color4F(ui::Background), Color4F(ui::Border));
        address = cocos2d::ui::EditBox::create({294, 46}, "");
        address->setPosition({951, 365});
        address->setInputMode(cocos2d::ui::EditBox::InputMode::SINGLE_LINE);
        address->setFont("Arial", 19);
        address->setFontColor(ui::Ink);
        address->setPlaceholderFont("Malgun Gothic", 18);
        address->setPlaceholderFontColor(ui::Muted);
        address->setPlaceHolder("방 코드");
        address->setMaxLength(32);
        const auto lastCode = normalizeRoomCode(UserDefault::getInstance()->getStringForKey(roomCodeSetting));
        if (lastCode.size() == 8) {
            address->setText(lastCode.c_str());
        }
        address->setEnabled(!canResume_);
        setupContent_->addChild(address);
        auto* paste = ui::button(buttons, "붙여넣기", {1150, 365}, {96, 36}, [this] {
            const auto code = clipboardRoomCode();
            if (code.size() == 8) {
                address->setText(code.c_str());
                notice->setString("");
            } else {
                notice->setString("복사된 방 코드 8자리를 확인해 주세요.");
            }
        }, false, 14);
        paste->setEnabled(!canResume_);
        auto* joinRoom = ui::button(buttons, "방 참여", {1000, 285}, {408, 50}, [this] { join(); }, false, 22);
        joinRoom->setEnabled(!canResume_);
    }

    void buildSummary(std::function<void()> onContinue, std::function<void()> onLeaveMatch) {
        ui::panel(setupContent_, {48, 46, 1184, 88}, Color4F(ui::Surface), Color4F(ui::Border));
        ui::label(setupContent_, "방을 만들 때 적용할 구성", 14, {76, 107}, ui::Muted, leftAnchor);
        matchFormat = ui::label(setupContent_, " ", 26, {76, 77}, ui::Ink, leftAnchor);
        if (canResume_) {
            ui::label(setupContent_, "진행 중인 경기", 17, {678, 90}, ui::Muted, rightAnchor);
            auto* buttons = ui::menu(setupContent_);
            if (onLeaveMatch) {
                ui::button(buttons, "경기 나가기", {805, 90}, {188, 50}, std::move(onLeaveMatch), false, 20);
            }
            ui::button(buttons, "계속하기", {1064, 90}, {280, 50}, std::move(onContinue), true, 23);
        } else {
            notice = ui::label(setupContent_, "", 16, {1204, 90}, ui::teamColor(1), rightAnchor);
        }
    }

    void restoreSettings() {
        auto* defaults = UserDefault::getInstance();
        config.teams = std::clamp(defaults->getIntegerForKey(teamCountSetting, config.teams), 2, 4);
        for (std::size_t i = 0; i < teamSizeSettings.size(); ++i) {
            config.teamSizes[i] = std::clamp(defaults->getIntegerForKey(teamSizeSettings[i], config.teamSizes[i]), 1, 4);
        }
    }

    void host() {
        auto* defaults = UserDefault::getInstance();
        defaults->setIntegerForKey(teamCountSetting, config.teams);
        for (std::size_t i = 0; i < teamSizeSettings.size(); ++i) {
            defaults->setIntegerForKey(teamSizeSettings[i], config.teamSizes[i]);
        }
        defaults->flush();
        config.controls = loadSettings().controls;
        openRoom(LanSession::hostOnline(config, relayEndpoint()));
    }

    void join() {
        const auto code = normalizeRoomCode(address->getText());
        if (code.size() != 8) {
            notice->setString("방 코드 8자리를 확인해 주세요.");
            return;
        }
        address->setText(code.c_str());
        auto* defaults = UserDefault::getInstance();
        defaults->setStringForKey(roomCodeSetting, code);
        defaults->flush();
        openRoom(LanSession::joinOnline(code, relayEndpoint()));
    }

    void openRoom(std::shared_ptr<LanSession> session) {
        if (!session) {
            notice->setString("연결을 열 수 없습니다.");
        } else if (!session->error().empty()) {
            notice->setString(session->error());
        } else if (auto* panel = RoomPanel::create(std::move(session), [this] { closeRoom(); })) {
            notice->setString("");
            address->setVisible(false);
            setupContent_->setVisible(false);
            room_ = panel;
            addChild(room_);
        }
    }

    void closeRoom() {
        room_->removeFromParent();
        room_ = nullptr;
        setupContent_->setVisible(true);
        address->setVisible(true);
    }

    void refresh() {
        for (int i = 0; i < 4; ++i) {
            const int size = i + 1;
            ui::setButtonActive(presets[i], config.teams == 2 && config.teamSizes[0] == size && config.teamSizes[1] == size);
        }
        for (int i = 0; i < 3; ++i) {
            ui::setButtonActive(teamCountButtons[i], config.teams == i + 2);
        }

        std::string format;
        int fighters = 0;
        for (int team = 0; team < 4; ++team) {
            const bool visible = team < config.teams;
            auto& row = teamRows[team];
            row.node->setVisible(visible);
            if (!visible) {
                continue;
            }
            const int size = config.teamSizes[team];
            fighters += size;
            if (!format.empty()) {
                format += 'v';
            }
            format += std::to_string(size);
            for (int i = 0; i < 4; ++i) {
                ui::setButtonActive(row.sizeButtons[i], size == i + 1);
            }
        }
        matchFormat->setString(format + "   /   " + std::to_string(fighters) + "명");
    }

    MatchConfig config;
    std::array<MenuItemLabel*, 4> presets{};
    std::array<MenuItemLabel*, 3> teamCountButtons{};
    std::array<TeamRow, 4> teamRows{};
    Label* matchFormat = nullptr;
    Label* notice = nullptr;
    cocos2d::ui::EditBox* address = nullptr;
    Node* setupContent_ = nullptr;
    RoomPanel* room_ = nullptr;
    bool canResume_ = false;
};

}

cocos2d::Node* createMenuLayer(std::function<void()> onContinue, std::function<void()> onLeaveMatch) {
    auto* layer = new SetupLayer;
    if (!layer->initMenu(std::move(onContinue), std::move(onLeaveMatch))) {
        delete layer;
        return nullptr;
    }
    layer->autorelease();
    return layer;
}

cocos2d::Scene* createMenuScene() {
    auto* scene = Scene::create();
    if (scene) {
        if (auto* layer = createMenuLayer({}, {})) {
            scene->addChild(layer);
        }
    }
    return scene;
}

}
