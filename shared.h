#pragma once

#include <windows.h>

// Global program state
typedef struct state {
    HINSTANCE hinst;
    HWND hwnd;
    HMENU menu;
} state;

