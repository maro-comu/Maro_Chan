#include "Ui_Maro.h"

#include <windows.h>

#include <array>
#include <filesystem>
#include <utility>

namespace maro::ui {

using namespace cocos2d;

const Color3B Background{16, 21, 30};
const Color3B Surface{24, 33, 48};
const Color3B Border{52, 68, 90};
const Color3B Ink{242, 247, 252};
const Color3B Muted{163, 178, 198};
const Color3B Accent{101, 229, 202};

namespace {

constexpr int buttonBackgroundTag = 1;

std::string systemFontPath() {
    std::array<wchar_t, MAX_PATH> directory{};
    const auto length = GetWindowsDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
    if (length == 0 || length >= directory.size()) {
        return {};
    }
    const auto path = std::filesystem::path(directory.data()) / L"Fonts" / L"malgun.ttf";
    auto font = path.generic_u8string();
    return FileUtils::getInstance()->isFileExist(font) ? font : std::string{};
}

Label* makeLabel(const std::string& text, float size) {
    static const auto font = systemFontPath();
    if (!font.empty()) {
        if (auto* node = Label::createWithTTF(text, font, size)) {
            return node;
        }
    }
    return Label::createWithSystemFont(text, "Malgun Gothic", size);
}

void drawPanel(DrawNode* shape, Rect rect, Color4F fill, Color4F border) {
    const Vec2 corner{rect.getMaxX(), rect.getMaxY()};
    if (fill.a > 0.0f) {
        shape->drawSolidRect(rect.origin, corner, fill);
    }
    if (border.a > 0.0f) {
        shape->drawRect(rect.origin, corner, border);
    }
}

}

Color3B teamColor(int team) {
    static const std::array<Color3B, 4> colors{{Accent, {255, 134, 122}, {141, 170, 255}, {255, 203, 115}}};
    return team >= 0 && team < static_cast<int>(colors.size()) ? colors[team] : Muted;
}

Label* label(Node* parent, const std::string& text, float size, Vec2 position,
             const Color3B& color, Vec2 anchor) {
    auto* node = makeLabel(text, size);
    node->setTextColor(Color4B(color));
    node->setPosition(position);
    node->setAnchorPoint(anchor);
    parent->addChild(node);
    return node;
}

void panel(Node* parent, Rect rect, Color4F fill, Color4F border) {
    auto* shape = DrawNode::create();
    drawPanel(shape, rect, fill, border);
    parent->addChild(shape);
}

Menu* menu(Node* parent) {
    auto* node = Menu::create();
    node->setPosition(Vec2::ZERO);
    parent->addChild(node);
    return node;
}

MenuItemLabel* button(Menu* menu, const std::string& text, Vec2 position, Size size,
                      std::function<void()> onClick, bool active, float fontSize) {
    auto* title = makeLabel(text, fontSize);
    auto* item = MenuItemLabel::create(title, [onClick = std::move(onClick)](Ref*) { onClick(); });
    item->setContentSize(size);
    item->setPosition(position);
    title->setAnchorPoint(Vec2::ANCHOR_MIDDLE);
    title->setPosition({size.width / 2.0f, size.height / 2.0f});
    item->addChild(DrawNode::create(), -1, buttonBackgroundTag);
    setButtonActive(item, active);
    menu->addChild(item);
    return item;
}

void setButtonActive(MenuItemLabel* item, bool active) {
    auto* title = static_cast<Label*>(item->getLabel());
    const Color4B textColor(active ? Background : Ink);
    if (title->getTextColor() == textColor) {
        return;
    }
    title->setTextColor(textColor);
    auto* shape = static_cast<DrawNode*>(item->getChildByTag(buttonBackgroundTag));
    const auto size = item->getContentSize();
    shape->clear();
    drawPanel(shape, {0.0f, 0.0f, size.width, size.height},
              Color4F(active ? Accent : Background), Color4F(active ? Accent : Border));
}

}
