#pragma once

#include <Windows.h>
#include <string>

struct KeystrokesKey {
    bool isSpacebar = false;
    DWORD vk = 0;
    std::string label;
    int x = 0;
    int y = 0;
    int w = 60;
    int h = 60;
    bool showCps = false;
};
