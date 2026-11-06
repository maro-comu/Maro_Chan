#pragma once

#include "cocos2d.h"

#include <functional>
#include <string>

namespace maro::ui {

extern const cocos2d::Color3B Background;
extern const cocos2d::Color3B Surface;
extern const cocos2d::Color3B Border;
extern const cocos2d::Color3B Ink;
extern const cocos2d::Color3B Muted;
extern const cocos2d::Color3B Accent;

cocos2d::Color3B teamColor(int team);
cocos2d::Label* label(cocos2d::Node* parent, const std::string& text, float size,
                     cocos2d::Vec2 position, const cocos2d::Color3B& color = Ink,
                     cocos2d::Vec2 anchor = {0.5f, 0.5f});
void panel(cocos2d::Node* parent, cocos2d::Rect rect, cocos2d::Color4F fill,
           cocos2d::Color4F border = {0.0f, 0.0f, 0.0f, 0.0f});
cocos2d::Menu* menu(cocos2d::Node* parent);
cocos2d::MenuItemLabel* button(cocos2d::Menu* menu, const std::string& text,
                             cocos2d::Vec2 position, cocos2d::Size size,
                             std::function<void()> onClick, bool active = false,
                             float fontSize = 18.0f);
void setButtonActive(cocos2d::MenuItemLabel* item, bool active);

}
