#include "Battle_Maro.h"

#include "Arena_Maro.h"
#include "Lan_Maro.h"
#include "Menu_Maro.h"
#include "Settings_Maro.h"
#include "Ui_Maro.h"
#include "cocos2d.h"
#include "platform/desktop/CCGLViewImpl-desktop.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <new>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <xaudio2.h>

namespace maro {
namespace {

using cocos2d::Color3B;
using cocos2d::Color4B;
using cocos2d::Color4F;
using cocos2d::DrawNode;
using cocos2d::Label;
using cocos2d::Menu;
using cocos2d::Node;
using cocos2d::Rect;
using cocos2d::Scene;
using cocos2d::Sprite;
using cocos2d::Vec2;
using Key = cocos2d::EventKeyboard::KeyCode;
using Mouse = cocos2d::EventMouse::MouseButton;

constexpr float Pi = 3.14159265358979323846f;
const Vec2 FieldOrigin(100, 116);
const Rect FieldRect(FieldOrigin.x, FieldOrigin.y, Arena::Width, Arena::Height);
const Vec2 LeftAnchor(0, 0.5f);
const Vec2 RightAnchor(1, 0.5f);
const Color4F Grid(0.12f, 0.17f, 0.23f, 0.9f);

Vec2 point(Vec vector) {
    return {vector.x, vector.y};
}

Color4F tint(Color3B color, float alpha = 1.0f) {
    return {color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, alpha};
}

void rectangle(DrawNode* drawing, float x, float y, float width, float height, Color4F color) {
    if (width > 0 && height > 0) {
        drawing->drawSolidRect({x, y}, {x + width, y + height}, color);
    }
}

void text(Label* label, const std::string& value) {
    if (label->getString() != value) {
        label->setString(value);
    }
}

std::string clockText(float seconds) {
    const auto wholeSeconds = static_cast<int>(seconds);
    std::ostringstream clock;
    clock << std::setfill('0') << std::setw(2) << wholeSeconds / 60
          << ':' << std::setw(2) << wholeSeconds % 60;
    return clock.str();
}

std::string matchText(const MatchConfig& config) {
    std::string name = std::to_string(config.teamSizes[0]);
    for (int i = 1; i < config.teams; ++i) {
        name += "대" + std::to_string(config.teamSizes[i]);
    }
    return name + "  /  LAN 경기";
}

enum class Overlay { None, Connecting, Augments, Waiting, Menu, Finished, Disconnected };
enum class Sound { Hit, Parry, Blast };

class BattleAudio {
public:
    BattleAudio() {
        if (FAILED(XAudio2Create(&engine_))) {
            return;
        }
        if (FAILED(engine_->CreateMasteringVoice(&output_))) {
            engine_->Release();
            engine_ = nullptr;
        }
    }

    ~BattleAudio() {
        for (auto& effect : effects_) {
            if (effect.voice) {
                effect.voice->DestroyVoice();
            }
        }
        if (output_) {
            output_->DestroyVoice();
            engine_->Release();
        }
    }

    void play(Sound sound, float volume) {
        if (!output_) {
            return;
        }
        constexpr std::array<const char*, 3> Paths{"hit_Maro.wav", "parry_Maro.wav", "blast_Maro.wav"};
        const auto index = static_cast<std::size_t>(sound);
        auto& effect = effects_[index];
        if (!effect.voice) {
            effect.data = cocos2d::FileUtils::getInstance()->getDataFromFile(Paths[index]);
            if (effect.data.getSize() <= WaveHeaderSize
                || std::memcmp(effect.data.getBytes() + 36, "data", 4) != 0) {
                return;
            }
            const WAVEFORMATEX format{WAVE_FORMAT_PCM, 1, 22050, 44100, 2, 16, 0};
            if (FAILED(engine_->CreateSourceVoice(&effect.voice, &format))) {
                return;
            }
        }
        XAUDIO2_BUFFER buffer{};
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        buffer.AudioBytes = static_cast<UINT32>(effect.data.getSize() - WaveHeaderSize);
        buffer.pAudioData = effect.data.getBytes() + WaveHeaderSize;
        effect.voice->Stop();
        effect.voice->FlushSourceBuffers();
        effect.voice->SetVolume(volume);
        if (SUCCEEDED(effect.voice->SubmitSourceBuffer(&buffer))) {
            effect.voice->Start();
        }
    }

private:
    static constexpr int WaveHeaderSize = 44;

    struct Effect {
        cocos2d::Data data;
        IXAudio2SourceVoice* voice = nullptr;
    };

    IXAudio2* engine_ = nullptr;
    IXAudio2MasteringVoice* output_ = nullptr;
    std::array<Effect, 3> effects_;
};

class BattleScene final : public Scene {
public:
    BattleScene(std::shared_ptr<LanSession> session, ControlMode controls)
        : session_(std::move(session)), config_(session_->config()), settings_(loadSettings()) {
        settings_.controls = controls;
    }

    bool initBattle();
    void onExit() override;
    void update(float dt) override;

private:
    void makeField();
    void makeHud();
    void listen();
    void setKey(Key key, bool down);
    bool handleShortcut(Key key);
    void moveCursor(cocos2d::EventMouse* event);
    void mouseDown(cocos2d::EventMouse* event);
    void mouseUp(cocos2d::EventMouse* event);
    void clearInput();
    bool acceptingInput() const;
    const MatchFrame& world() const;
    const Actor* player() const;
    Input battleInput();
    void stepHost(float dt);
    void collectFrame();
    void drawWorld();
    void drawFighter(const Actor& actor, std::size_t index);
    void drawHud();
    void updateCursor();
    void updateOverlay();
    void showAugments(Menu* buttons);
    void showSettings();
    void openMainMenu();
    void resumeFromMain();
    bool canResume() const;
    void showResult(Menu* buttons);
    void showStatus(Menu* buttons, const std::string& title, const std::string& detail, bool canLeave);
    void choose(int index);
    void setPaused(bool paused);
    void updateFeedback(float dt);
    void playEffect(Sound sound, float intensity);

    std::shared_ptr<LanSession> session_;
    MatchConfig config_;
    Settings settings_;
    std::unique_ptr<BattleAudio> audio_;
    std::unique_ptr<Arena> arena_;
    MatchFrame hostFrame_;
    Node* field_ = nullptr;
    Node* overlayNode_ = nullptr;
    Node* mainMenu_ = nullptr;
    DrawNode* combat_ = nullptr;
    DrawNode* meters_ = nullptr;
    Menu* topMenu_ = nullptr;
    Label* time_ = nullptr;
    Label* health_ = nullptr;
    Label* level_ = nullptr;
    Label* levelHint_ = nullptr;
    Label* kills_ = nullptr;
    Label* you_ = nullptr;
    Sprite* cursor_ = nullptr;
    std::vector<Label*> teamLabels_;
    std::vector<Sprite*> fighters_;
    Vec aim_;
    Vec destination_;
    Vec2 pointer_;
    Overlay overlay_ = Overlay::None;
    int offeredLevel_ = 0;
    int submittedLevel_ = 0;
    float publishTimer_ = 0.0f;
    float destinationMarker_ = 0.0f;
    float shakeTime_ = 0.0f;
    float previousHp_ = -1.0f;
    float previousParry_ = 0.0f;
    float previousBlast_ = 0.0f;
    float endedBehindMain_ = 0.0f;
    bool haveDestination_ = false;
    bool haveAim_ = false;
    bool frameDirty_ = false;
    bool cursorHidden_ = false;
    bool menuOpen_ = false;
    bool up_ = false;
    bool down_ = false;
    bool left_ = false;
    bool right_ = false;
    bool mouseFire_ = false;
    bool mouseGuard_ = false;
    bool keyFire_ = false;
    bool keyGuard_ = false;
    bool firePressed_ = false;
    bool parryPressed_ = false;
};

bool BattleScene::initBattle() {
    if (!Scene::init()) {
        return false;
    }
    if (session_->hosting()) {
        arena_ = std::make_unique<Arena>(config_);
        collectFrame();
        session_->publish(hostFrame_);
    }
    ui::panel(this, {0, 0, 1280, 800}, tint(ui::Background));
    makeField();
    makeHud();
    listen();
    updateOverlay();
    drawWorld();
    drawHud();
    scheduleUpdate();
    return true;
}

void BattleScene::makeField() {
    field_ = Node::create();
    field_->setPosition(FieldOrigin);
    addChild(field_, 1);

    ui::panel(field_, {0, 0, Arena::Width, Arena::Height}, {0.055f, 0.076f, 0.104f, 1}, tint(ui::Border));
    auto* geometry = DrawNode::create();
    field_->addChild(geometry);
    for (int x = 54; x < Arena::Width; x += 54) {
        geometry->drawLine({static_cast<float>(x), 1}, {static_cast<float>(x), Arena::Height - 1}, Grid);
    }
    for (int y = 56; y < Arena::Height; y += 56) {
        geometry->drawLine({1, static_cast<float>(y)}, {Arena::Width - 1, static_cast<float>(y)}, Grid);
    }
    for (const auto& cover : Arena::obstacles()) {
        const Vec2 vertices[] = {{cover.left, cover.bottom}, {cover.right, cover.bottom},
                                 {cover.right, cover.top}, {cover.left, cover.top}};
        geometry->drawPolygon(vertices, 4, tint(ui::Surface), 0.9f, tint(ui::Border));
    }

    combat_ = DrawNode::create();
    field_->addChild(combat_, 1);
    you_ = ui::label(field_, "나", 13, {}, ui::Accent);
    you_->setVisible(false);
    you_->setLocalZOrder(3);
    cursor_ = Sprite::create("cursor_Maro.png");
    if (cursor_) {
        cursor_->setVisible(false);
        addChild(cursor_, 5);
    }
}

void BattleScene::makeHud() {
    ui::label(this, "멀티 사격 게임", 31, {100, 751}, ui::Ink, LeftAnchor);
    ui::label(this, matchText(config_), 13, {102, 713}, ui::Muted, LeftAnchor);
    time_ = ui::label(this, "00:00", 29, {640, 753}, ui::Ink);
    for (int team = 0; team < config_.teams; ++team) {
        const float x = 640 + (team - (config_.teams - 1) / 2.0f) * 116;
        ui::panel(this, {x - 51, 697, 102, 28}, tint(ui::Surface), tint(ui::Border));
        teamLabels_.push_back(ui::label(this, "", 13, {x, 711}, ui::teamColor(team)));
    }
    topMenu_ = ui::menu(this);
    ui::button(topMenu_, "설정  /  ESC", {1101, 745}, {158, 42}, [this] { setPaused(true); });

    meters_ = DrawNode::create();
    addChild(meters_);
    ui::label(this, "체력", 15, {100, 91}, ui::Muted, LeftAnchor);
    health_ = ui::label(this, "", 15, {352, 91}, ui::Ink, RightAnchor);
    level_ = ui::label(this, "", 15, {402, 91}, ui::Accent, LeftAnchor);
    levelHint_ = ui::label(this, "", 14, {750, 91}, ui::Muted, RightAnchor);
    kills_ = ui::label(this, "", 19, {1180, 83}, ui::Ink, RightAnchor);
}

void BattleScene::listen() {
    auto* keys = cocos2d::EventListenerKeyboard::create();
    keys->onKeyPressed = [this](Key key, cocos2d::Event*) {
        if (!mainMenu_ && !handleShortcut(key)) {
            setKey(key, true);
        }
    };
    keys->onKeyReleased = [this](Key key, cocos2d::Event*) { setKey(key, false); };
    _eventDispatcher->addEventListenerWithSceneGraphPriority(keys, this);

    auto* mouse = cocos2d::EventListenerMouse::create();
    mouse->onMouseMove = [this](cocos2d::EventMouse* event) { moveCursor(event); };
    mouse->onMouseDown = [this](cocos2d::EventMouse* event) { mouseDown(event); };
    mouse->onMouseUp = [this](cocos2d::EventMouse* event) { mouseUp(event); };
    _eventDispatcher->addEventListenerWithSceneGraphPriority(mouse, this);

    auto* focus = cocos2d::EventListenerCustom::create(cocos2d::GLViewImpl::EVENT_WINDOW_UNFOCUSED,
        [this](cocos2d::EventCustom*) { clearInput(); });
    _eventDispatcher->addEventListenerWithSceneGraphPriority(focus, this);
}

bool BattleScene::handleShortcut(Key key) {
    if (key == Key::KEY_ESCAPE) {
        setPaused(!menuOpen_);
        return true;
    }
    if (overlay_ == Overlay::Augments && key >= Key::KEY_1 && key <= Key::KEY_3) {
        choose(static_cast<int>(key) - static_cast<int>(Key::KEY_1));
        return true;
    }
    return false;
}

void BattleScene::setKey(Key key, bool down) {
    if (down && !acceptingInput()) {
        return;
    }
    if (settings_.controls == ControlMode::Moba) {
        switch (key) {
        case Key::KEY_Q: keyFire_ = down; firePressed_ |= down; break;
        case Key::KEY_E: keyGuard_ = down; break;
        case Key::KEY_LEFT_SHIFT:
        case Key::KEY_RIGHT_SHIFT: parryPressed_ |= down; break;
        default: break;
        }
        return;
    }
    switch (key) {
    case Key::KEY_W: up_ = down; break;
    case Key::KEY_S: down_ = down; break;
    case Key::KEY_A: left_ = down; break;
    case Key::KEY_D: right_ = down; break;
    default: break;
    }
}

void BattleScene::moveCursor(cocos2d::EventMouse* event) {
    if (mainMenu_) {
        return;
    }
    pointer_ = {event->getCursorX(), event->getCursorY()};
    if (FieldRect.containsPoint(pointer_)) {
        const auto offset = pointer_ - FieldOrigin;
        aim_ = {offset.x, offset.y};
        haveAim_ = true;
    }
    updateCursor();
}

void BattleScene::mouseDown(cocos2d::EventMouse* event) {
    moveCursor(event);
    if (!acceptingInput() || !FieldRect.containsPoint(pointer_)) {
        return;
    }
    const auto button = event->getMouseButton();
    if (settings_.controls == ControlMode::Moba) {
        if (button == Mouse::BUTTON_LEFT) {
            destination_ = aim_;
            haveDestination_ = true;
            destinationMarker_ = 0.9f;
        }
    } else if (button == Mouse::BUTTON_RIGHT) {
        mouseGuard_ = true;
        parryPressed_ |= mouseFire_;
    } else if (button == Mouse::BUTTON_LEFT) {
        mouseFire_ = true;
        firePressed_ |= !mouseGuard_;
        parryPressed_ |= mouseGuard_;
    }
}

void BattleScene::mouseUp(cocos2d::EventMouse* event) {
    moveCursor(event);
    if (event->getMouseButton() == Mouse::BUTTON_LEFT) {
        mouseFire_ = false;
    } else if (event->getMouseButton() == Mouse::BUTTON_RIGHT) {
        mouseGuard_ = false;
    }
}

void BattleScene::clearInput() {
    up_ = down_ = left_ = right_ = false;
    mouseFire_ = mouseGuard_ = keyFire_ = keyGuard_ = false;
    firePressed_ = parryPressed_ = haveDestination_ = false;
    destinationMarker_ = 0.0f;
}

const MatchFrame& BattleScene::world() const {
    return arena_ ? hostFrame_ : session_->frame();
}

const Actor* BattleScene::player() const {
    const auto& actors = world().actors;
    const auto slot = session_->localSlot();
    return slot >= 0 && static_cast<std::size_t>(slot) < actors.size() ? &actors[slot] : nullptr;
}

bool BattleScene::acceptingInput() const {
    const auto* actor = player();
    return !mainMenu_ && overlay_ == Overlay::None && actor && actor->alive();
}

Input BattleScene::battleInput() {
    Input input;
    input.aim = aim_;
    if (!acceptingInput()) {
        return input;
    }
    input.parry = parryPressed_;
    if (settings_.controls == ControlMode::Moba) {
        input.moveTo = haveDestination_;
        input.destination = destination_;
        input.attack = keyFire_;
        input.attackPressed = firePressed_;
        input.guard = keyGuard_;
    } else {
        input.movement = {static_cast<float>(right_ - left_), static_cast<float>(up_ - down_)};
        input.attack = !mouseGuard_ && mouseFire_;
        input.attackPressed = !mouseGuard_ && firePressed_;
        input.guard = mouseGuard_;
    }
    firePressed_ = parryPressed_ = false;
    return input;
}

void BattleScene::stepHost(float dt) {
    const float previousTime = arena_->elapsed();
    for (const int slot : session_->takeDepartures()) {
        arena_->removePlayer(slot);
        frameDirty_ = true;
    }
    for (const auto& [slot, option] : session_->takeChoices()) {
        frameDirty_ |= arena_->chooseAugment(slot, option);
    }
    auto inputs = session_->takeInputs();
    inputs.resize(arena_->actors().size());
    inputs.front() = battleInput();
    arena_->update(dt, inputs);
    frameDirty_ |= arena_->elapsed() != previousTime;
    if (frameDirty_) {
        collectFrame();
    }
    publishTimer_ -= dt;
    if (frameDirty_ && publishTimer_ <= 0) {
        session_->publish(hostFrame_);
        frameDirty_ = false;
        publishTimer_ = 1.0f / 30;
    }
}

void BattleScene::collectFrame() {
    hostFrame_.actors = arena_->actors();
    hostFrame_.bullets = arena_->bullets();
    hostFrame_.blasts = arena_->blasts();
    hostFrame_.elapsed = arena_->elapsed();
    hostFrame_.result = arena_->result();
    hostFrame_.winner = arena_->winner();
    for (int slot = 0; slot < static_cast<int>(hostFrame_.actors.size()); ++slot) {
        hostFrame_.choosing[slot] = arena_->choosingAugment(slot);
        hostFrame_.augments[slot] = arena_->choiceIds(slot);
        hostFrame_.levelTime[slot] = arena_->timeToNextLevel(slot);
    }
}

void BattleScene::drawFighter(const Actor& actor, std::size_t index) {
    auto* sprite = fighters_[index];
    if (sprite) {
        sprite->setVisible(actor.alive());
    }
    if (!actor.alive()) {
        return;
    }

    const auto center = point(actor.position);
    const auto facing = point(actor.facing);
    const auto color = ui::teamColor(actor.team);
    const auto angle = std::atan2(facing.y, facing.x);
    if (sprite) {
        sprite->setPosition(center);
        sprite->setRotation(-angle * 180.0f / Pi);
    } else {
        combat_->drawDot(center, actor.radius, tint(color));
        combat_->drawSegment(center, center + facing * (actor.radius + 6), 3, tint(ui::Ink));
    }
    if (actor.guarding || actor.parryTime > 0) {
        const bool parrying = actor.parryTime > 0;
        const float arc = actor.wideGuard && !parrying ? 1.69f : 1.27f;
        const float radius = actor.radius + 8;
        const auto shield = parrying ? tint(ui::Ink) : tint(color, 0.85f);
        Vec2 last;
        for (int i = 0; i <= 10; ++i) {
            const float theta = angle + arc * (i / 5.0f - 1);
            const Vec2 next = center + Vec2(std::cos(theta), std::sin(theta)) * radius;
            if (i > 0) {
                combat_->drawSegment(last, next, parrying ? 2.3f : 1.4f, shield);
            }
            last = next;
        }
    }
    rectangle(combat_, center.x - 15, center.y + 21, 30, 4, tint(ui::Border));
    rectangle(combat_, center.x - 15, center.y + 21, 30 * actor.hp / actor.maxHp, 4, tint(color));
    if (static_cast<int>(index) == session_->localSlot()) {
        you_->setPosition(center.x, center.y - 27);
        you_->setTextColor(Color4B(color));
    }
}

void BattleScene::drawWorld() {
    combat_->clear();
    const auto& frame = world();
    const auto& actors = frame.actors;
    const auto* local = player();
    you_->setVisible(local && local->alive());
    while (fighters_.size() < actors.size()) {
        const auto& actor = actors[fighters_.size()];
        auto* sprite = Sprite::create("fighter_Maro.png");
        if (sprite) {
            sprite->setColor(ui::teamColor(actor.team));
            field_->addChild(sprite, 2);
        }
        fighters_.push_back(sprite);
    }
    if (destinationMarker_ > 0 && local) {
        combat_->drawCircle(point(destination_), 8, 0, 16, false, tint(ui::teamColor(local->team), 0.7f));
    }
    for (const auto& blast : frame.blasts) {
        const float remaining = std::clamp(blast.remaining / Arena::BlastDuration, 0.0f, 1.0f);
        const auto color = ui::teamColor(blast.team);
        const auto center = point(blast.position);
        combat_->drawSolidCircle(center, blast.radius * (1.0f - remaining * 0.7f),
                                 0, 28, tint(color, remaining * 0.22f));
        combat_->drawCircle(center, blast.radius * (1.0f - remaining * 0.7f),
                            0, 28, false, tint(color, remaining * 0.85f));
    }
    for (const auto& bullet : frame.bullets) {
        const auto head = point(bullet.position);
        const auto tail = head - point(bullet.velocity).getNormalized() * 14;
        combat_->drawSegment(tail, head, bullet.radius, tint(ui::teamColor(bullet.team), 0.8f));
        const Color3B tip = bullet.effects & Bullet::Explosive ? Color3B(255, 190, 93)
            : bullet.effects & Bullet::Slow ? Color3B(143, 223, 255) : ui::Ink;
        combat_->drawDot(head, bullet.effects & Bullet::Charged ? 2.6f : 1.5f, tint(tip));
    }
    for (std::size_t i = 0; i < actors.size(); ++i) {
        drawFighter(actors[i], i);
    }
}

void BattleScene::playEffect(Sound sound, float intensity) {
    const float volume = settings_.masterVolume * settings_.effectsVolume * intensity;
    if (!mainMenu_ && volume > 0.0f) {
        if (!audio_) {
            audio_ = std::make_unique<BattleAudio>();
        }
        audio_->play(sound, volume);
    }
}

void BattleScene::updateFeedback(float dt) {
    const auto* local = player();
    if (!local) {
        return;
    }
    shakeTime_ = std::max(0.0f, shakeTime_ - dt);
    float nearbyBlast = 0.0f;
    for (const auto& blast : world().blasts) {
        if (point(blast.position).distanceSquared(point(local->position)) < 220.0f * 220.0f) {
            nearbyBlast = std::max(nearbyBlast, world().elapsed + blast.remaining);
        }
    }
    if (previousHp_ >= 0.0f) {
        if (local->hp < previousHp_) {
            playEffect(Sound::Hit, 0.7f);
            shakeTime_ = 0.16f;
        }
        if (local->parryTime > previousParry_ + 0.04f) {
            playEffect(Sound::Parry, 0.65f);
        }
        if (nearbyBlast > previousBlast_ + 0.02f) {
            playEffect(Sound::Blast, 0.75f);
        }
    }
    previousHp_ = local->hp;
    previousParry_ = local->parryTime;
    previousBlast_ = nearbyBlast;
    const float shake = settings_.screenShake ? shakeTime_ / 0.16f : 0.0f;
    field_->setPosition(FieldOrigin + Vec2(std::sin(shake * 32.0f) * 5.0f, std::cos(shake * 25.0f) * 3.0f) * shake);
}

void BattleScene::drawHud() {
    const auto& frame = world();
    const auto* actor = player();
    if (!actor) {
        return;
    }
    const auto& local = *actor;
    std::array<int, 4> alive{};
    for (const auto& fighter : frame.actors) {
        alive[fighter.team] += fighter.alive();
    }
    text(time_, clockText(frame.elapsed));
    for (int team = 0; team < config_.teams; ++team) {
        text(teamLabels_[team], std::to_string(team + 1) + "팀  /  " + std::to_string(alive[team]) + "명");
        teamLabels_[team]->setTextColor(Color4B(alive[team] ? ui::teamColor(team) : ui::Muted));
    }
    text(health_, std::to_string(static_cast<int>(std::ceil(local.hp))) + " / "
        + std::to_string(static_cast<int>(local.maxHp)));
    text(level_, "레벨  " + std::to_string(local.level) + " / " + std::to_string(Arena::MaxLevel));
    text(kills_, std::to_string(local.team + 1) + "팀  /  " + std::to_string(local.kills) + " 처치");
    const float seconds = frame.levelTime[session_->localSlot()];
    if (!local.alive()) {
        text(levelHint_, "관전 중");
    } else if (local.level == Arena::MaxLevel) {
        text(levelHint_, "최대 레벨");
    } else {
        text(levelHint_, "처치 또는 " + std::to_string(static_cast<int>(std::ceil(seconds))) + "초 후 성장");
    }

    meters_->clear();
    const float health = std::clamp(local.hp / local.maxHp, 0.0f, 1.0f);
    rectangle(meters_, 100, 67, 252, 8, tint(ui::Border));
    rectangle(meters_, 100, 67, 252 * health, 8, tint(ui::teamColor(local.team)));
    rectangle(meters_, 402, 67, 348, 8, tint(ui::Border));
    if (local.alive()) {
        const float progress = local.level == Arena::MaxLevel ? 1.0f
            : std::clamp(1 - seconds / Arena::LevelInterval, 0.0f, 1.0f);
        rectangle(meters_, 402, 67, 348 * progress, 8, tint(ui::Accent, 0.7f));
    }
}

void BattleScene::updateCursor() {
    const bool hide = cursor_ && acceptingInput() && FieldRect.containsPoint(pointer_);
    if (cursor_) {
        cursor_->setPosition(pointer_);
        cursor_->setVisible(hide);
    }
    if (hide != cursorHidden_) {
        cocos2d::Director::getInstance()->getOpenGLView()->setCursorVisible(!hide);
        cursorHidden_ = hide;
    }
}

void BattleScene::updateOverlay() {
    Overlay next = Overlay::None;
    const auto& frame = world();
    const auto* local = player();
    const int slot = session_->localSlot();
    if (menuOpen_) {
        next = Overlay::Menu;
    } else if (local && frame.result != MatchResult::Playing) {
        next = Overlay::Finished;
    } else if (!session_->connected()) {
        next = Overlay::Disconnected;
    } else if (!local) {
        next = Overlay::Connecting;
    } else if (frame.choosing[slot] && submittedLevel_ != local->level) {
        next = Overlay::Augments;
    } else if (std::any_of(frame.choosing.begin(), frame.choosing.end(), [](bool pending) { return pending; })) {
        next = Overlay::Waiting;
    }
    const int offeredLevel = next == Overlay::Augments ? local->level : 0;
    if (next == overlay_ && offeredLevel == offeredLevel_) {
        return;
    }
    if (overlayNode_) {
        overlayNode_->removeFromParent();
        overlayNode_ = nullptr;
    }
    overlay_ = next;
    offeredLevel_ = offeredLevel;
    topMenu_->setEnabled(next == Overlay::None && !mainMenu_);
    updateCursor();
    if (next == Overlay::None) {
        return;
    }
    clearInput();
    if (next == Overlay::Menu) {
        showSettings();
        return;
    }
    overlayNode_ = Node::create();
    addChild(overlayNode_, 10);
    ui::panel(overlayNode_, {0, 0, 1280, 800}, {0.025f, 0.035f, 0.052f, 0.8f});
    auto* buttons = ui::menu(overlayNode_);
    buttons->setLocalZOrder(2);
    switch (next) {
    case Overlay::Augments:
        showAugments(buttons);
        break;
    case Overlay::Menu:
        break;
    case Overlay::Finished:
        showResult(buttons);
        break;
    case Overlay::Connecting:
        showStatus(buttons, "경기 준비 중", "방장에게서 경기 정보를 받는 중입니다.", true);
        break;
    case Overlay::Waiting:
        showStatus(buttons, "증강 선택 대기 중", "모든 참가자가 증강을 선택하면 경기가 이어집니다.", false);
        break;
    case Overlay::Disconnected:
        showStatus(buttons, "연결이 끊겼습니다",
            session_->error().empty() ? "방과의 연결을 확인해 주세요." : session_->error(), true);
        break;
    case Overlay::None:
        break;
    }
}

void BattleScene::showAugments(Menu* buttons) {
    ui::panel(overlayNode_, {132, 226, 1016, 348}, tint(ui::Background), tint(ui::Border));
    ui::label(overlayNode_, "레벨 " + std::to_string(offeredLevel_) + " / " + std::to_string(Arena::MaxLevel),
              15, {640, 535}, ui::Accent);
    ui::label(overlayNode_, "증강을 선택하세요", 34, {640, 493}, ui::Ink);
    ui::label(overlayNode_, "선택한 강화 효과는 이번 경기 동안 유지됩니다.", 16, {640, 458}, ui::Muted);
    const auto& choices = world().augments[session_->localSlot()];
    for (int index = 0; index < 3; ++index) {
        const float x = 322 + index * 318;
        const auto& choice = Arena::augment(choices[index]);
        ui::panel(overlayNode_, {x - 147, 263, 294, 164}, tint(ui::Surface), tint(ui::Border));
        ui::label(overlayNode_, choice.name, 22, {x, 397}, ui::Ink);
        auto* description = ui::label(overlayNode_, choice.description, 15, {x, 355}, ui::Muted);
        description->setWidth(270);
        description->setAlignment(cocos2d::TextHAlignment::CENTER);
        ui::button(buttons, "선택  /  " + std::to_string(index + 1),
                   {x, 296}, {242, 38}, [this, index] { choose(index); });
    }
}

void BattleScene::showSettings() {
    std::function<void()> onMain;
    if (canResume()) {
        onMain = [this] { openMainMenu(); };
    }
    overlayNode_ = createSettingsOverlay(settings_, [this](const Settings& settings) {
        const bool fullscreenChanged = settings_.fullscreen != settings.fullscreen;
        settings_ = settings;
        if (fullscreenChanged) {
            setGameFullscreen(settings_.fullscreen);
        }
    }, [this] { setPaused(false); }, [] {
        cocos2d::Director::getInstance()->replaceScene(createMenuScene());
    }, session_->hosting() ? "경기 종료" : "경기 나가기", std::move(onMain));
    if (overlayNode_) {
        addChild(overlayNode_, 10);
    }
}

bool BattleScene::canResume() const {
    return session_->connected() && world().result == MatchResult::Playing;
}

void BattleScene::openMainMenu() {
    if (!canResume() || mainMenu_) {
        return;
    }
    mainMenu_ = createMenuLayer([this] { resumeFromMain(); }, [] {
        cocos2d::Director::getInstance()->replaceScene(createMenuScene());
    });
    if (mainMenu_) {
        addChild(mainMenu_, 20);
        topMenu_->setEnabled(false);
        setPaused(false);
    }
}

void BattleScene::resumeFromMain() {
    if (!canResume()) {
        cocos2d::Director::getInstance()->replaceScene(createMenuScene());
        return;
    }
    mainMenu_->removeFromParent();
    mainMenu_ = nullptr;
    settings_ = loadSettings();
    topMenu_->setEnabled(overlay_ == Overlay::None);
    updateCursor();
}

void BattleScene::showResult(Menu* buttons) {
    const auto& frame = world();
    const auto& local = *player();
    const bool draw = frame.result == MatchResult::Draw;
    const bool won = frame.winner == local.team;
    const auto title = draw ? "무승부" : won ? "우리 팀 승리" : std::to_string(frame.winner + 1) + "팀 승리";
    const auto color = draw ? ui::Muted : ui::teamColor(frame.winner);
    ui::panel(overlayNode_, {352, 242, 576, 316}, tint(ui::Background), tint(ui::Border));
    ui::label(overlayNode_, title, 38, {640, 497}, color);
    ui::label(overlayNode_, clockText(frame.elapsed) + "   /   " + std::to_string(local.kills)
        + " 처치   /   레벨 " + std::to_string(local.level), 16, {640, 451}, ui::Muted);
    ui::button(buttons, "설정으로 돌아가기", {640, 343}, {330, 48},
               [] { cocos2d::Director::getInstance()->replaceScene(createMenuScene()); }, true);
}

void BattleScene::showStatus(Menu* buttons, const std::string& title, const std::string& detail, bool canLeave) {
    ui::panel(overlayNode_, {344, 280, 592, 240}, tint(ui::Background), tint(ui::Border));
    ui::label(overlayNode_, title, 30, {640, 466}, ui::Ink);
    ui::label(overlayNode_, detail, 16, {640, 415}, ui::Muted);
    if (canLeave) {
        ui::button(buttons, "설정으로 돌아가기", {640, 340}, {314, 46},
                   [] { cocos2d::Director::getInstance()->replaceScene(createMenuScene()); }, true);
    }
}

void BattleScene::choose(int index) {
    const auto* local = player();
    if (overlay_ != Overlay::Augments || !local || offeredLevel_ != local->level
        || submittedLevel_ == offeredLevel_) {
        return;
    }
    if (arena_) {
        if (!arena_->chooseAugment(session_->localSlot(), index)) {
            return;
        }
        frameDirty_ = true;
    } else {
        session_->sendChoice(index);
    }
    submittedLevel_ = offeredLevel_;
    clearInput();
}

void BattleScene::setPaused(bool paused) {
    clearInput();
    menuOpen_ = paused;
    updateOverlay();
}

void BattleScene::onExit() {
    if (cursorHidden_) {
        cocos2d::Director::getInstance()->getOpenGLView()->setCursorVisible(true);
    }
    Scene::onExit();
}

void BattleScene::update(float dt) {
    session_->poll();
    if (!haveAim_) {
        if (const auto* local = player()) {
            aim_ = {local->position.x + local->facing.x * 60, local->position.y + local->facing.y * 60};
            haveAim_ = true;
        }
    }
    updateOverlay();
    if (arena_) {
        stepHost(dt);
    } else if (session_->connected() && player() && world().result == MatchResult::Playing) {
        session_->sendInput(battleInput());
    }
    if (const auto* local = player(); local && !local->alive()) {
        clearInput();
    }
    if (mainMenu_ && !canResume()) {
        endedBehindMain_ += dt;
        if (endedBehindMain_ >= 0.25f) {
            cocos2d::Director::getInstance()->replaceScene(createMenuScene());
            return;
        }
    }
    destinationMarker_ = std::max(0.0f, destinationMarker_ - dt);
    updateOverlay();
    updateCursor();
    updateFeedback(dt);
    drawWorld();
    drawHud();
}

}

cocos2d::Scene* createBattleScene(std::shared_ptr<LanSession> session, ControlMode controls) {
    if (!session) {
        return nullptr;
    }
    auto* battle = new (std::nothrow) BattleScene(std::move(session), controls);
    if (battle && battle->initBattle()) {
        battle->autorelease();
        return battle;
    }
    delete battle;
    return nullptr;
}

}
