#include "Menu_Maro.h"
#include "RoomCode_Maro.h"
#include "Settings_Maro.h"
#include "cocos2d.h"

#include <algorithm>
#include <array>
#include <commctrl.h>
#include <filesystem>
#include <windowsx.h>

namespace {

bool fullscreenActive = false;

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
    case WM_NCHITTEST:
        if (!fullscreenActive) {
            RECT bounds{};
            GetWindowRect(window, &bounds);
            const POINT point{GET_X_LPARAM(data), GET_Y_LPARAM(data)};
            constexpr int Border = 8;
            const bool left = point.x >= bounds.left && point.x < bounds.left + Border;
            const bool right = point.x < bounds.right && point.x >= bounds.right - Border;
            const bool top = point.y >= bounds.top && point.y < bounds.top + Border;
            const bool bottom = point.y < bounds.bottom && point.y >= bounds.bottom - Border;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
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
    int width = 1600;
    int height = 1000;
};

WindowRect windowed;
int appliedWindowedSize = -1;
const std::array<WindowRect, 5> WindowSizes{{
    {0, 0, 960, 600}, {0, 0, 1280, 800}, {0, 0, 1440, 900},
    {0, 0, 1600, 1000}, {0, 0, 1920, 1200}
}};

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
        const float scale = std::min({1.0f, width * 0.94f / static_cast<float>(target.width),
                                           height * 0.94f / static_cast<float>(target.height)});
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
            "멀티 사격 게임", cocos2d::Rect(0, 0, 1600, 1000), 1.0f, true);
        if (!view) {
            MessageBoxW(nullptr, L"OpenGL 게임 창을 열 수 없습니다.", L"멀티 사격 게임", MB_OK | MB_ICONERROR);
            return false;
        }

        SetWindowSubclass(view->getWin32Window(), mapMissingKeyScanCode, 1, 0);
        maro::installDarkInputs(view->getWin32Window());
        director->setOpenGLView(view);
        view->setDesignResolutionSize(1280, 800, ResolutionPolicy::SHOW_ALL);
        const auto settings = maro::loadSettings();
        auto* window = view->getWindow();
        glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);
        glfwSetWindowSizeLimits(window, 640, 400, GLFW_DONT_CARE, GLFW_DONT_CARE);
        maro::applyDisplaySettings(settings);

        auto* files = cocos2d::FileUtils::getInstance();
        const auto resourceRoot = std::filesystem::u8path(files->getDefaultResourceRootPath());
        const auto assets = (resourceRoot / "../Assets").lexically_normal();
        files->addSearchPath(assets.generic_u8string(), true);
        director->runWithScene(maro::createMenuScene());
        return true;
    }

    void applicationDidEnterBackground() override
    {
    }

    void applicationWillEnterForeground() override
    {
    }
};

}

void maro::setGameFullscreen(bool fullscreen) {
    auto settings = loadSettings();
    settings.fullscreen = fullscreen;
    applyDisplaySettings(settings);
}

void maro::applyDisplaySettings(const Settings& settings) {
    auto* view = static_cast<cocos2d::GLViewImpl*>(cocos2d::Director::getInstance()->getOpenGLView());
    if (!view || !view->getWindow()) {
        return;
    }
    auto* window = view->getWindow();
    const bool fullscreen = glfwGetWindowMonitor(window) != nullptr;
    auto* monitor = monitorFor(window);
    const int size = std::clamp(settings.windowSize, 0, 4);
    if (settings.fullscreen && !fullscreen) {
        const auto* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
        if (!mode) {
            return;
        }
        glfwGetWindowPos(window, &windowed.x, &windowed.y);
        glfwGetWindowSize(window, &windowed.width, &windowed.height);
        glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else if (!settings.fullscreen && (fullscreen || size != appliedWindowedSize)) {
        if (size != appliedWindowedSize) {
            windowed = fitWindow(monitor, WindowSizes[size]);
        }
        if (fullscreen) {
            glfwSetWindowMonitor(window, nullptr, windowed.x, windowed.y,
                                 windowed.width, windowed.height, GLFW_DONT_CARE);
        } else {
            glfwSetWindowSize(window, windowed.width, windowed.height);
            glfwSetWindowPos(window, windowed.x, windowed.y);
        }
        appliedWindowedSize = size;
    }
    fullscreenActive = glfwGetWindowMonitor(window) != nullptr;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(settings.vSync ? 1 : 0);
    const int limit = settings.frameLimit == 120 || settings.frameLimit == 144 ? settings.frameLimit : 60;
    cocos2d::Director::getInstance()->setAnimationInterval(1.0 / limit);
    cocos2d::Director::getInstance()->setDisplayStats(settings.showFps);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int result = 0;
    {
        MaroApplication app;
        result = app.run();
    }
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    return result;
}
