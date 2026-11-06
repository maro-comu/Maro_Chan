#include "Menu_Maro.h"
#include "cocos2d.h"

#include <commctrl.h>
#include <filesystem>

namespace {

LRESULT CALLBACK mapMissingKeyScanCode(HWND window, UINT message, WPARAM key, LPARAM data,
                                       UINT_PTR subclass, DWORD_PTR) {
    switch (message) {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        if ((HIWORD(data) & 0x1ff) == 0) {
            const auto scanCode = MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC);
            data |= static_cast<LPARAM>(scanCode) << 16;
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, mapMissingKeyScanCode, subclass);
        break;
    }
    return DefSubclassProc(window, message, key, data);
}

class MaroApplication final : public cocos2d::Application {
public:
    bool applicationDidFinishLaunching() override
    {
        auto* director = cocos2d::Director::getInstance();
        auto* view = cocos2d::GLViewImpl::createWithRect(
            "멀티 사격 게임", cocos2d::Rect(0, 0, 1152, 720), 1.0f, true);
        if (!view) {
            MessageBoxW(nullptr, L"OpenGL 게임 창을 열 수 없습니다.", L"멀티 사격 게임", MB_OK | MB_ICONERROR);
            return false;
        }

        SetWindowSubclass(view->getWin32Window(), mapMissingKeyScanCode, 1, 0);
        director->setOpenGLView(view);
        view->setDesignResolutionSize(1280, 800, ResolutionPolicy::SHOW_ALL);
        director->setDisplayStats(false);
        director->setAnimationInterval(1.0 / 60.0);

        auto* files = cocos2d::FileUtils::getInstance();
        const auto resourceRoot = std::filesystem::u8path(files->getDefaultResourceRootPath());
        const auto assets = (resourceRoot / "../Assets").lexically_normal();
        files->addSearchPath(assets.generic_u8string(), true);
        director->runWithScene(maro::createMenuScene());
        return true;
    }

    void applicationDidEnterBackground() override
    {
        cocos2d::Director::getInstance()->stopAnimation();
    }

    void applicationWillEnterForeground() override
    {
        cocos2d::Director::getInstance()->startAnimation();
    }
};

}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    MaroApplication app;
    return app.run();
}
