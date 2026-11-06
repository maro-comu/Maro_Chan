#pragma once

#include <string>
#include <windows.h>

namespace maro {

std::string normalizeRoomCode(const std::string& code);
std::string encodeRoomCode(const std::string& address);
std::string decodeRoomCode(const std::string& code);
bool copyRoomCode(const std::string& code);
std::string clipboardRoomCode();
void installDarkInputs(HWND window);

}
