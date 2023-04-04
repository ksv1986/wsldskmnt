#include <windows.h>

#include "resource.h"

// Global program state
typedef struct state {
    HINSTANCE hinst;
    HWND hwnd;
    HMENU menu;
    HMENU popup;
} state;

enum {
    APP_NOTIFY = WM_APP + 1, // Tray icon notification callback message
};
// Tray icon will be identified by guid
static const GUID GUID_NOTIFY = {
    0xd9cbd4ab,
    0x346b,
    0x4905,
    { 0xb2, 0xf, 0xfc, 0xe3, 0x7e, 0x80, 0xd7, 0xb3 }
};

#define NIDINIT(name, hwnd) {       \
        .cbSize = sizeof(name),     \
        .hWnd = hwnd,               \
        .guidItem = GUID_NOTIFY,    \
    }

static state g_state[1];

static state* getState(HWND hwnd)
{
    return (state*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

static void setState(HWND hwnd, state *st)
{
    st->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
}

static HMENU getMenu(HWND hwnd)
{
    return getState(hwnd)->popup;
}

static HINSTANCE getInst(HWND hwnd)
{
    return (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
}

static BOOL addTrayIcon(HWND hwnd)
{
    NOTIFYICONDATA nid = NIDINIT(nid, hwnd);
    nid.uFlags |= NIF_ICON | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = APP_NOTIFY;
    nid.hIcon = LoadIconW(getInst(hwnd), MAKEINTRESOURCEW(IDI_MAIN));
    return Shell_NotifyIconW(NIM_ADD, &nid);
}

static void removeTrayIcon(HWND hwnd)
{
    NOTIFYICONDATA nid = NIDINIT(nid, hwnd);
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void showContextMenu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);

    // our window must be foreground before calling TrackPopupMenu or the menu will not disappear when the user clicks away
    SetForegroundWindow(hwnd);
    // respect menu drop alignment
    UINT flags = TPM_RIGHTBUTTON;
    if (GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0)
        flags |= TPM_RIGHTALIGN;
    else
        flags |= TPM_LEFTALIGN;

    TrackPopupMenuEx(getMenu(hwnd), flags, pt.x, pt.y, hwnd, NULL);
}

static LRESULT onCommand(HWND hwnd, WPARAM wparam, LPARAM lparam)
{
    switch (LOWORD(wparam))
    {
    case ID_MAIN_EXIT:
        DestroyWindow(hwnd);
        return 0;
    default:
        return DefWindowProc(hwnd, WM_COMMAND, wparam, lparam);
    }
}

static LRESULT onTrayCallback(HWND hwnd, WPARAM wparam, LPARAM lparam)
{
    UNREFERENCED_PARAMETER(wparam);

    switch (lparam)
    {
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        showContextMenu(hwnd);
    }
    return 0;
}

static LRESULT onCreate(HWND hwnd, LPARAM lparam)
{
    LPCREATESTRUCTW cs = (LPCREATESTRUCTW)lparam;
    state* st = cs->lpCreateParams;

    setState(hwnd, st);
    st->menu = LoadMenuW(st->hinst, MAKEINTRESOURCEW(IDR_MENU));
    st->popup = GetSubMenu(st->menu, 0);
    if (!addTrayIcon(hwnd))
        return GetLastError();
    return 0;
}

static void onDestroy(HWND hwnd)
{
    state* st = getState(hwnd);

    removeTrayIcon(hwnd);
    DestroyMenu(st->menu);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT umsg, WPARAM wparam, LPARAM lparam)
{
    switch (umsg)
    {
    case WM_CREATE:
        return onCreate(hwnd, lparam);
    case WM_DESTROY:
        onDestroy(hwnd);
        PostQuitMessage(0);
        return 0;
    case WM_COMMAND:
        return onCommand(hwnd, wparam, lparam);
    case APP_NOTIFY:
        return onTrayCallback(hwnd, wparam, lparam);
    }
    return DefWindowProcW(hwnd, umsg, wparam, lparam);
}

int WINAPI wWinMain(_In_ HINSTANCE hinst, _In_opt_ HINSTANCE hprev, _In_ PWSTR argv, _In_ int show)
{
    UNREFERENCED_PARAMETER(hprev);
    UNREFERENCED_PARAMETER(argv);
    UNREFERENCED_PARAMETER(show);

    state* st = g_state;
    st->hinst = hinst;

    // The program will use only tray icon popup menu,
    // but it's simpler to use window handle to process messages.
    // No need to show and paint this window though
    LPCWCH dummy = L"DummyWindow";
    WNDCLASSEX wcex = {
        .cbSize = sizeof(wcex),
        .lpszClassName = dummy,
        .style = CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc = WndProc,
        .hInstance = hinst,
    };
    // window class will be unregistered on exit, no need to call UnregisterClass()
    RegisterClassExW(&wcex);

    // Create the window.
    HWND hwnd = CreateWindowExW(
        0,                  // Optional window styles.
        dummy,              // Window class
        dummy,              // Window text
        WS_OVERLAPPED,      // Window style
        // Size and position
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,

        NULL,               // Parent window
        NULL,               // Menu
        hinst,              // Instance handle
        st                  // Additional application data
    );
    if (hwnd == NULL)
        return 1;

    MSG msg[1];
    while (GetMessageW(msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(msg);
        DispatchMessageW(msg);
    }

    return (int)msg->wParam;
}

int __stdcall
wWinMainCRTStartup()
{
    return wWinMain(
        GetModuleHandleW(NULL),
        NULL,
        GetCommandLineW(),
        SW_SHOW);
}
