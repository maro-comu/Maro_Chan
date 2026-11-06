#include "RoomCode_Maro.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <commctrl.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string_view>

namespace maro {
namespace {

constexpr std::string_view Alphabet = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
constexpr UINT_PTR DarkInputSubclass = 0x4D41524F;
constexpr COLORREF Background = RGB(16, 21, 30);
constexpr COLORREF Text = RGB(242, 247, 252);
HWND clipboardOwner = nullptr;

bool editControl(HWND window) {
    std::array<wchar_t, 16> name{};
    return GetClassNameW(window, name.data(), static_cast<int>(name.size())) > 0
        && _wcsicmp(name.data(), L"Edit") == 0;
}

void removeNativeBorder(HWND window) {
    if (!editControl(window)) {
        return;
    }
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    const auto extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    SetWindowLongPtrW(window, GWL_STYLE, style & ~static_cast<LONG_PTR>(WS_BORDER));
    SetWindowLongPtrW(window, GWL_EXSTYLE, extended & ~static_cast<LONG_PTR>(WS_EX_CLIENTEDGE));
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
        SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    InvalidateRect(window, nullptr, TRUE);
}

BOOL CALLBACK darkExistingInput(HWND window, LPARAM) {
    removeNativeBorder(window);
    return TRUE;
}

LRESULT CALLBACK darkInputProcedure(HWND window, UINT message, WPARAM parameter, LPARAM data,
                                   UINT_PTR identifier, DWORD_PTR brush) {
    switch (message) {
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        if (editControl(reinterpret_cast<HWND>(data))) {
            const auto context = reinterpret_cast<HDC>(parameter);
            SetTextColor(context, Text);
            SetBkColor(context, Background);
            SetBkMode(context, OPAQUE);
            return static_cast<LRESULT>(brush);
        }
        break;
    case WM_PARENTNOTIFY:
        if (LOWORD(parameter) == WM_CREATE) {
            removeNativeBorder(reinterpret_cast<HWND>(data));
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, darkInputProcedure, identifier);
        DeleteObject(reinterpret_cast<HBRUSH>(brush));
        if (clipboardOwner == window) {
            clipboardOwner = nullptr;
        }
        break;
    }
    return DefSubclassProc(window, message, parameter, data);
}

bool openClipboard() {
    const auto owner = IsWindow(clipboardOwner) ? clipboardOwner : GetActiveWindow();
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (OpenClipboard(owner)) {
            return true;
        }
        if (attempt < 2) {
            Sleep(2);
        }
    }
    return false;
}

}

std::string normalizeRoomCode(const std::string& code) {
    if (code.size() > 128) {
        return {};
    }
    std::string normalized;
    normalized.reserve(code.size());
    for (const unsigned char character : code) {
        if (std::isspace(character) || character == '-') {
            continue;
        }
        const auto upper = static_cast<char>(std::toupper(character));
        if (Alphabet.find(upper) == std::string_view::npos) {
            return {};
        }
        normalized.push_back(upper);
    }
    return normalized;
}

std::string encodeRoomCode(const std::string& address) {
    std::array<unsigned, 4> components{};
    const char* cursor = address.data();
    const char* end = cursor + address.size();
    for (std::size_t index = 0; index < components.size(); ++index) {
        const auto parsed = std::from_chars(cursor, end, components[index]);
        if (parsed.ec != std::errc{} || parsed.ptr == cursor || components[index] > 255) {
            return {};
        }
        cursor = parsed.ptr;
        if (index + 1 < components.size()) {
            if (cursor == end || *cursor != '.') {
                return {};
            }
            ++cursor;
        }
    }
    if (cursor != end) {
        return {};
    }
    auto value = (std::uint32_t{components[0]} << 24) | (std::uint32_t{components[1]} << 16)
        | (std::uint32_t{components[2]} << 8) | components[3];
    std::string code(7, Alphabet.front());
    for (auto digit = code.rbegin(); digit != code.rend(); ++digit) {
        *digit = Alphabet[value & 31];
        value >>= 5;
    }
    return code;
}

std::string decodeRoomCode(const std::string& code) {
    const auto normalized = normalizeRoomCode(code);
    if (normalized.size() != 7) {
        return {};
    }
    std::uint64_t value = 0;
    for (const auto character : normalized) {
        value = (value << 5) | Alphabet.find(character);
    }
    if (value > UINT32_MAX) {
        return {};
    }
    return std::to_string(value >> 24) + "." + std::to_string((value >> 16) & 255)
        + "." + std::to_string((value >> 8) & 255) + "." + std::to_string(value & 255);
}

bool copyRoomCode(const std::string& code) {
    const auto normalized = normalizeRoomCode(code);
    if (normalized.empty() || normalized.size() > 64) {
        return false;
    }
    const std::wstring text(normalized.begin(), normalized.end());
    const auto bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        return false;
    }
    auto* destination = GlobalLock(memory);
    if (!destination) {
        GlobalFree(memory);
        return false;
    }
    std::memcpy(destination, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!openClipboard()) {
        GlobalFree(memory);
        return false;
    }
    const bool copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    CloseClipboard();
    if (!copied) {
        GlobalFree(memory);
    }
    return copied;
}

std::string clipboardRoomCode() {
    if (!openClipboard()) {
        return {};
    }
    const auto memory = GetClipboardData(CF_UNICODETEXT);
    const auto bytes = memory ? GlobalSize(memory) : 0;
    const auto* text = memory && bytes >= sizeof(wchar_t)
        ? static_cast<const wchar_t*>(GlobalLock(memory)) : nullptr;
    std::string result;
    if (text) {
        const auto count = (std::min)(static_cast<std::size_t>(bytes / sizeof(wchar_t)), std::size_t{128});
        const auto* end = static_cast<const wchar_t*>(std::wmemchr(text, L'\0', count));
        if (end) {
            for (auto* character = text; character != end; ++character) {
                if (*character > 127) {
                    result.clear();
                    break;
                }
                result.push_back(static_cast<char>(*character));
            }
        }
        GlobalUnlock(memory);
    }
    CloseClipboard();
    return normalizeRoomCode(result);
}

void installDarkInputs(HWND window) {
    if (!IsWindow(window)) {
        return;
    }
    clipboardOwner = window;
    DWORD_PTR existing = 0;
    if (GetWindowSubclass(window, darkInputProcedure, DarkInputSubclass, &existing)) {
        return;
    }
    const auto brush = CreateSolidBrush(Background);
    if (!brush) {
        return;
    }
    if (!SetWindowSubclass(window, darkInputProcedure, DarkInputSubclass, reinterpret_cast<DWORD_PTR>(brush))) {
        DeleteObject(brush);
        return;
    }
    EnumChildWindows(window, darkExistingInput, 0);
}

}
