#include <windows.h>
#include <shlwapi.h>
#include <strsafe.h>

#include "resource.h"
#include "shared.h"

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
    return getState(hwnd)->menu;
}

static HINSTANCE getInst(HWND hwnd)
{
    return (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
}

static BOOL showNotify(HWND hwnd, PCWCH text, PCWCH title, DWORD niif)
{
    NOTIFYICONDATA nid = NIDINIT(nid, hwnd);
    nid.uFlags |= NIF_INFO;
    nid.dwInfoFlags = niif;
    StringCchCopyW(nid.szInfo, ARRAYSIZE(nid.szInfo), text);
    StringCchCopyW(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), title);
    // Show the notification.
    return Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static BOOL showWarning(HWND hwnd, PCWCH text, PCWCH title)
{
    return showNotify(hwnd, text, title, NIIF_WARNING);
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

static void copyToClipboard(HGLOBAL hdst)
{
    if (!OpenClipboard(NULL))
        return;

    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, hdst);
    CloseClipboard();
}

static void onDiskClicked(HWND hwnd, DWORD i)
{
    state* st = getState(hwnd);
    disk_info* disk = getDisk(st, i);

    HGLOBAL hdst = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, disk->dd->cbSize);
    if (!hdst)
        return;

    LPWSTR dst = (LPWSTR)GlobalLock(hdst);
    if (dst) {
        memcpy(dst, disk->dd->DevicePath, disk->dd->cbSize);
        GlobalUnlock(hdst);
        copyToClipboard(hdst);
    }
    GlobalFree(hdst);
}

static void onMountClicked(HWND hwnd, DWORD i)
{
    state* st = getState(hwnd);
    disk_info* disk = getDisk(st, i);

    WCHAR cmd[MAX_PATH];
    wnsprintfW(cmd, ARRAYSIZE(cmd), L"--mount \"%s\" --bare",
        disk->dd->DevicePath);

    PROCESS_INFORMATION pi;
    STARTUPINFO si = { .cb = sizeof(si), };
    if (!CreateProcessW(L"C:\\Windows\\System32\\wsl.exe",
        cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        err_desc e[1] = { ERRINIT() };
        setError(e, L"Failed to start wsl.exe");
        showWarning(hwnd, e->msg, e->title);
        resetErr(e);
        return;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode) {
        wnsprintfW(cmd, ARRAYSIZE(cmd), L"wsl.exe exit code: %d", exitCode);
        showWarning(hwnd, cmd, L"Failed to run wsl.exe");
    }
}

static LRESULT onCommand(HWND hwnd, WPARAM wparam, LPARAM lparam)
{
    UINT cmd = LOWORD(wparam);
    switch (cmd)
    {
    case ID_MAIN_EXIT:
        DestroyWindow(hwnd);
        return 0;
    default:
        if (cmd >= ID_MAIN_DISK && cmd < ID_MAIN_DISK + MAX_DISKS) {
            onDiskClicked(hwnd, cmd - ID_MAIN_DISK);
            return 0;
        }
        else if (cmd >= ID_MAIN_MOUNT && cmd < ID_MAIN_MOUNT + MAX_DISKS) {
            onMountClicked(hwnd, cmd - ID_MAIN_MOUNT);
            return 0;
        }
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

static void appendError(HMENU menu, err_desc* e)
{
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, e->title);
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, e->msg);
}

static void createDiskMenu(HMENU parent, DWORD i, disk_info* disk)
{
    HMENU menu = CreatePopupMenu();
    if (disk->e->error)
        appendError(menu, disk->e);
    else {
        AppendMenuW(menu, MF_STRING, ID_MAIN_DISK + i, disk->dd->DevicePath);
        AppendMenuW(menu, MF_STRING, ID_MAIN_MOUNT + i, L"&Mount");
    }

    WCHAR text[512];
    wnsprintfW(text, ARRAYSIZE(text), L"Disk &%u", i);
    AppendMenuW(parent, MF_STRING | MF_POPUP, (UINT_PTR)menu, text);
}

static void createDisksMenu(state* st)
{
    if (st->e->error)
        appendError(st->menu, st->e);

    for (DWORD i = 0; i < st->n_disks; i++)
        createDiskMenu(st->menu, i, getDisk(st, i));

    AppendMenuW(st->menu, MF_STRING, ID_MAIN_EXIT, L"&Exit");
}

static LRESULT onCreate(HWND hwnd, LPARAM lparam)
{
    LPCREATESTRUCTW cs = (LPCREATESTRUCTW)lparam;
    state* st = cs->lpCreateParams;

    listDisks(st);

    setState(hwnd, st);
    st->menu = CreatePopupMenu();
    if (!st->menu)
        return GetLastError(); // without menu program is useless

    createDisksMenu(st);

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
