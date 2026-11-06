#include "Menu_Maro.h"
#include "Settings_Maro.h"
#include "cocos2d.h"

#include <algorithm>
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

GLFWmonitor* monitorFor(GLFWwindow* window) {
    if (auto* monitor = glfwGetWindowMonitor(window)) {
        return monitor;
    }

    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    glfwGetWindowPos(window, &x, &y);
    glfwGetWindowSize(window, &width, &height);
    int count = 0;
    auto** monitors = glfwGetMonitors(&count);
    auto* nearest = glfwGetPrimaryMonitor();
    int bestOverlap = 0;
    for (int i = 0; i < count; ++i) {
        const auto* mode = glfwGetVideoMode(monitors[i]);
        if (!mode) {
            continue;
        }
        int mx = 0;
        int my = 0;
        glfwGetMonitorPos(monitors[i], &mx, &my);
        const int overlapWidth = std::max(0, std::min(x + width, mx + mode->width) - std::max(x, mx));
        const int overlapHeight = std::max(0, std::min(y + height, my + mode->height) - std::max(y, my));
        const int overlap = overlapWidth * overlapHeight;
        if (overlap > bestOverlap) {
            nearest = monitors[i];
            bestOverlap = overlap;
        }
    }
    return nearest;
}

struct WindowRect {
    int x = 0;
    int y = 0;
    int width = 1152;
    int height = 720;
};

WindowRect windowed;

WindowRect fitWindow(GLFWmonitor* monitor, WindowRect target) {
    if (!monitor) {
        return target;
    }
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    glfwGetMonitorWorkarea(monitor, &x, &y, &width, &height);
    if (width > 0 && height > 0) {
        const float scale = std::min({1.0f, width / static_cast<float>(target.width),
                                           height / static_cast<float>(target.height)});
        target.width = static_cast<int>(target.width * scale);
        target.height = static_cast<int>(target.height * scale);
        target.x = x + (width - target.width) / 2;
        target.y = y + (height - target.height) / 2;
    }
    return target;
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
        const auto settings = maro::loadSettings();
        auto* window = view->getWindow();
        glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);
        windowed = fitWindow(glfwGetPrimaryMonitor(), windowed);
        glfwSetWindowSize(window, windowed.width, windowed.height);
        glfwSetWindowPos(window, windowed.x, windowed.y);
        maro::setGameFullscreen(settings.fullscreen);
        director->setDisplayStats(settings.showFps);
        director->setAnimationInterval(1.0 / 60.0);

        auto* files = cocos2d::FileUtils::getInstance();
        const auto resourceRoot = std::filesystem::u8path(files->getDefaultResourceRootPath());
        const auto assets = (resourceRoot / "../Assets").lexically_normal();
        files->addSearchPath(assets.generic_u8string(), true);
        director->runWithScene(maro::createStartScene());
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

void maro::setGameFullscreen(bool fullscreen) {
    auto* view = static_cast<cocos2d::GLViewImpl*>(cocos2d::Director::getInstance()->getOpenGLView());
    if (!view || !view->getWindow()) {
        return;
    }
    auto* window = view->getWindow();
    if ((glfwGetWindowMonitor(window) != nullptr) == fullscreen) {
        return;
    }
    auto* monitor = monitorFor(window);
    if (fullscreen) {
        const auto* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
        if (!mode) {
            return;
        }
        glfwGetWindowPos(window, &windowed.x, &windowed.y);
        glfwGetWindowSize(window, &windowed.width, &windowed.height);
        glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        windowed = fitWindow(monitor, windowed);
        glfwSetWindowMonitor(window, nullptr, windowed.x, windowed.y,
                             windowed.width, windowed.height, GLFW_DONT_CARE);
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    MaroApplication app;
    return app.run();
}
