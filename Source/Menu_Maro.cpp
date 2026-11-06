#include "Menu_Maro.h"

#include "Arena_Maro.h"
#include "Battle_Maro.h"
#include "Lan_Maro.h"
#include "Settings_Maro.h"
#include "Ui_Maro.h"
#include "ui/UIEditBox/UIEditBox.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace maro {

namespace {

using namespace cocos2d;

const Vec2 leftAnchor{0.0f, 0.5f};
const Vec2 rightAnchor{1.0f, 0.5f};
constexpr const char* teamCountSetting = "maro.teamCount";
constexpr const char* roomCodeSetting = "maro.roomCode";
constexpr std::string_view roomAlphabet = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
constexpr std::array<const char*, 4> teamSizeSettings{{
    "maro.teamSize1", "maro.teamSize2", "maro.teamSize3", "maro.teamSize4"
}};

void setText(Label* label, const std::string& text) {
    if (label->getString() != text) {
        label->setString(text);
    }
}

std::string encodeRoomCode(const std::string& address) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf_s(address.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4
        || std::max({a, b, c, d}) > 255) {
        return {};
    }
    auto value = (std::uint32_t{a} << 24) | (std::uint32_t{b} << 16) | (std::uint32_t{c} << 8) | d;
    std::string code(7, roomAlphabet.front());
    for (auto digit = code.rbegin(); digit != code.rend(); ++digit) {
        *digit = roomAlphabet[value & 31];
        value >>= 5;
    }
    return code;
}

std::string decodeRoomCode(const std::string& code) {
    if (code.size() != 7) {
        return {};
    }
    std::uint64_t value = 0;
    for (const unsigned char digit : code) {
        const auto index = roomAlphabet.find(static_cast<char>(std::toupper(digit)));
        if (index == std::string_view::npos) {
            return {};
        }
        value = (value << 5) | index;
    }
    if (value > UINT32_MAX) {
        return {};
    }
    return std::to_string(value >> 24) + "." + std::to_string((value >> 16) & 255)
        + "." + std::to_string((value >> 8) & 255) + "." + std::to_string(value & 255);
}

void buildHeader(Scene* scene, const char* title, const char* caption) {
    ui::panel(scene, {0, 0, 1280, 800}, Color4F(ui::Background));
    ui::label(scene, title, 58, {47, 692}, ui::Ink, leftAnchor);
    if (caption) {
        ui::label(scene, caption, 17, {1232, 750}, ui::Muted, rightAnchor);
    }
}

class SettingsScene : public Scene {
protected:
    bool init() override {
        if (!Scene::init()) {
            return false;
        }
        auto* listener = EventListenerKeyboard::create();
        listener->onKeyPressed = [this](EventKeyboard::KeyCode key, Event*) {
            if (key == EventKeyboard::KeyCode::KEY_ESCAPE) {
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
            [](const Settings& settings) { setGameFullscreen(settings.fullscreen); },
            [this] { toggleSettings(); });
        if (settingsOverlay_) {
            addChild(settingsOverlay_, 100);
        }
    }

private:
    Node* settingsOverlay_ = nullptr;
};

class StartScene final : public SettingsScene {
public:
    CREATE_FUNC(StartScene);

    bool init() override {
        if (!SettingsScene::init()) {
            return false;
        }
        ui::panel(this, {0, 0, 1280, 800}, Color4F(ui::Background));
        ui::panel(this, {292, 148, 696, 504}, Color4F(ui::Surface), Color4F(ui::Border));
        ui::panel(this, {584, 613, 112, 3}, Color4F(ui::Accent));
        ui::label(this, "멀티 사격 게임", 63, {640, 540}, ui::Ink);
        ui::label(this, "팀을 구성하고 친구와 사격 대결을 펼치세요.", 19, {640, 463}, ui::Muted);
        auto* buttons = ui::menu(this);
        ui::button(buttons, "계속하기", {640, 363}, {310, 58}, [] {
            Director::getInstance()->replaceScene(createMenuScene());
        }, true, 24);
        ui::button(buttons, "나가기", {640, 286}, {310, 50}, [] {
            Director::getInstance()->end();
        }, false, 21);
        ui::button(buttons, "설정  /  ESC", {640, 200}, {158, 40}, [this] {
            toggleSettings();
        }, false, 16);
        return true;
    }
};

class LobbyScene final : public SettingsScene {
public:
    static LobbyScene* create(std::shared_ptr<LanSession> session) {
        auto* lobby = new LobbyScene(std::move(session));
        if (!lobby->init()) {
            delete lobby;
            return nullptr;
        }
        lobby->autorelease();
        return lobby;
    }

    bool init() override {
        if (!SettingsScene::init()) {
            return false;
        }
        buildHeader(this, "경기 대기실", session_->hosting() ? "LAN  /  방장" : "LAN  /  참가자");
        auto* connection = ui::label(this, connectionInfo(), 17, {48, 641}, ui::Muted, leftAnchor);
        connection->setWidth(1184);
        buildTeamCards();

        ui::panel(this, {48, 46, 1184, 88}, Color4F(ui::Surface), Color4F(ui::Border));
        count_ = ui::label(this, "연결 확인 중", 25, {76, 104}, ui::Ink, leftAnchor);
        status_ = ui::label(this, "", 16, {76, 72}, ui::Muted, leftAnchor);
        auto* buttons = ui::menu(this);
        ui::button(buttons, "설정  /  ESC", {1156, 694}, {152, 42}, [this] {
            toggleSettings();
        }, false, 16);
        ui::button(buttons, "나가기", {session_->hosting() ? 905.0f : 1096.0f, 90}, {136, 50}, [] {
            Director::getInstance()->replaceScene(createMenuScene());
        });
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

private:
    explicit LobbyScene(std::shared_ptr<LanSession> session) : session_(std::move(session)) {}

    std::string connectionInfo() const {
        if (!session_->hosting()) {
            return "방장이 경기를 시작하면 자동으로 입장합니다.";
        }
        const auto addresses = LanSession::localAddresses();
        std::string list;
        for (const auto& address : addresses) {
            if (!list.empty()) {
                list += "  /  ";
            }
            list += encodeRoomCode(address);
        }
        return "방 코드   " + (list.empty() ? encodeRoomCode("127.0.0.1") : list);
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
                                  : "연결 확인 중");
        if (session_->hosting()) {
            setText(status_, ready ? "모든 자리가 찼습니다." : "모든 참가자가 접속하면 경기를 시작할 수 있습니다.");
        } else {
            setText(status_, connected ? "방장의 경기 시작을 기다리는 중입니다." : "방장에게 연결하는 중입니다.");
        }
    }

    std::shared_ptr<LanSession> session_;
    std::array<Node*, 4> cards_{};
    std::array<std::array<Label*, 4>, 4> seats_{};
    Label* count_ = nullptr;
    Label* status_ = nullptr;
    MenuItemLabel* start_ = nullptr;
};

class SetupScene final : public SettingsScene {
public:
    CREATE_FUNC(SetupScene);

    bool init() override {
        if (!SettingsScene::init()) {
            return false;
        }
        restoreSettings();

        buildHeader(this, "멀티 사격 게임", nullptr);
        auto* header = ui::menu(this);
        ui::button(header, "처음으로", {981, 692}, {146, 42}, [] {
            Director::getInstance()->replaceScene(createStartScene());
        }, false, 16);
        ui::button(header, "설정  /  ESC", {1156, 692}, {152, 42}, [this] {
            toggleSettings();
        }, false, 16);

        buildMatchSetup();
        buildRoomPanel();
        buildSummary();
        refresh();
        return true;
    }

private:
    struct TeamRow {
        Node* node = nullptr;
        std::array<MenuItemLabel*, 4> sizeButtons{};
    };

    void buildMatchSetup() {
        ui::panel(this, {48, 160, 696, 472}, Color4F(ui::Surface), Color4F(ui::Border));
        ui::label(this, "경기 설정", 24, {76, 595}, ui::Ink, leftAnchor);
        ui::label(this, "기본 구성", 15, {76, 549}, ui::Muted, leftAnchor);
        ui::label(this, "팀 수", 15, {76, 501}, ui::Muted, leftAnchor);

        auto* setupButtons = ui::menu(this);
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

        ui::panel(this, {76, 472, 640, 1}, Color4F(ui::Border));
        ui::label(this, "팀", 14, {76, 448}, ui::Muted, leftAnchor);
        ui::label(this, "팀별 참가 인원", 14, {716, 448}, ui::Muted, rightAnchor);

        for (int team = 0; team < 4; ++team) {
            const float y = 414.0f - 58.0f * team;
            auto& row = teamRows[team];
            row.node = Node::create();
            addChild(row.node);
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
        ui::panel(this, {768, 160, 464, 472}, Color4F(ui::Surface), Color4F(ui::Border));
        ui::label(this, "친구와 경기", 24, {796, 595}, ui::Ink, leftAnchor);
        ui::label(this, "왼쪽에서 팀을 구성한 뒤 방을 만드세요.", 17, {796, 548}, ui::Muted, leftAnchor);
        auto* buttons = ui::menu(this);
        ui::button(buttons, "방 만들기", {1000, 486}, {408, 52}, [this] { host(); }, true, 22);
        ui::panel(this, {796, 423, 408, 1}, Color4F(ui::Border));
        ui::label(this, "같은 네트워크의 친구 방에 참가하세요.", 17, {796, 378}, ui::Muted, leftAnchor);
        ui::panel(this, {796, 284, 408, 50}, Color4F(ui::Background), Color4F(ui::Border));
        address = cocos2d::ui::EditBox::create({392, 46}, "");
        address->setPosition({1000, 309});
        address->setInputMode(cocos2d::ui::EditBox::InputMode::SINGLE_LINE);
        address->setFont("Arial", 19);
        address->setFontColor(ui::Ink);
        address->setPlaceholderFont("Malgun Gothic", 18);
        address->setPlaceholderFontColor(ui::Muted);
        address->setPlaceHolder("방 코드");
        address->setMaxLength(7);
        address->setText(UserDefault::getInstance()->getStringForKey(roomCodeSetting).c_str());
        addChild(address);
        ui::button(buttons, "방 참가", {1000, 238}, {408, 50}, [this] { join(); }, false, 22);
    }

    void buildSummary() {
        ui::panel(this, {48, 46, 1184, 88}, Color4F(ui::Surface), Color4F(ui::Border));
        ui::label(this, "방을 만들 때 적용할 구성", 14, {76, 107}, ui::Muted, leftAnchor);
        matchFormat = ui::label(this, " ", 26, {76, 77}, ui::Ink, leftAnchor);
        notice = ui::label(this, "", 16, {1204, 90}, ui::teamColor(1), rightAnchor);
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
        openLobby(LanSession::host(config));
    }

    void join() {
        const std::string code = address->getText();
        const auto ipv4 = decodeRoomCode(code);
        if (ipv4.empty()) {
            notice->setString("방 코드 7자리를 확인해 주세요.");
            return;
        }
        auto* defaults = UserDefault::getInstance();
        defaults->setStringForKey(roomCodeSetting, code);
        defaults->flush();
        openLobby(LanSession::join(ipv4));
    }

    void openLobby(std::shared_ptr<LanSession> session) {
        if (!session) {
            notice->setString("연결을 열 수 없습니다.");
        } else if (!session->error().empty()) {
            notice->setString(session->error());
        } else if (auto* lobby = LobbyScene::create(std::move(session))) {
            Director::getInstance()->replaceScene(lobby);
        }
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
};

}

cocos2d::Scene* createStartScene() {
    return StartScene::create();
}

cocos2d::Scene* createMenuScene() {
    return SetupScene::create();
}

}
