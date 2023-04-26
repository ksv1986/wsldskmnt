#include "resource.h"
#include "shared.h"

#include <windows.h>
#include <objbase.h>
#include <shlwapi.h>
#include <strsafe.h>

enum {
    APP_NOTIFY = WM_APP + 1, // Tray icon notification callback message
    MENU_EXIT = 40001,
    MENU_COPY = 40100,
    MENU_MOUNT = 40200,
    MENU_UNMOUNT = 40300,
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
    nid.uFlags |= NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = APP_NOTIFY;
    nid.hIcon = LoadIconW(getInst(hwnd), MAKEINTRESOURCEW(IDI_MAIN));
    state* st = getState(hwnd);

    wnsprintfW(nid.szTip, ARRAYSIZE(nid.szTip), L"Disks: %u", st->n_disks);
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

static void execWsl(HWND hwnd, WCHAR* cmd, int cch)
{
    SHELLEXECUTEINFO sei = {
        .cbSize = sizeof(sei),
        .fMask = SEE_MASK_NOCLOSEPROCESS,
        .lpVerb = L"runas",
        .lpFile = L"C:\\Windows\\System32\\wsl.exe",
        .lpParameters = cmd,
        .hwnd = hwnd,
        .nShow = SW_NORMAL,
    };
    if (ShellExecuteExW(&sei)) {
        HANDLE proc = sei.hProcess;
        WaitForSingleObject(proc, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(proc, &exitCode);
        CloseHandle(proc);

        if (exitCode) {
            wnsprintfW(cmd, cch, L"wsl.exe exit code: %d", exitCode);
            showWarning(hwnd, cmd, L"Failed to run wsl.exe");
        }
    }
    else {
        DWORD code = GetLastError();
        if (code == ERROR_CANCELLED)
            return;

        err_desc e[1] = { ERRINIT() };
        setErrorCode(e, L"Failed to start wsl.exe", code);
        showWarning(hwnd, e->text, e->title);
        resetErr(e);
    }
}

static void onMountClicked(HWND hwnd, DWORD i)
{
    state* st = getState(hwnd);
    disk_info* disk = getDisk(st, i);

    WCHAR cmd[MAX_PATH];
    wnsprintfW(cmd, ARRAYSIZE(cmd), L"--mount \"%s\" --bare", disk->path);

    execWsl(hwnd, cmd, ARRAYSIZE(cmd));
}

static void onUnmountClicked(HWND hwnd, DWORD i)
{
    state* st = getState(hwnd);
    disk_info* disk = getDisk(st, i);

    WCHAR cmd[MAX_PATH];
    wnsprintfW(cmd, ARRAYSIZE(cmd), L"--unmount \"%s\"", disk->path);

    execWsl(hwnd, cmd, ARRAYSIZE(cmd));
}

static void copyToClipboard(HGLOBAL hdst)
{
    if (!OpenClipboard(NULL))
        return;

    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, hdst);
    CloseClipboard();
}

static void onCopyClicked(HWND hwnd, DWORD i)
{
    state* st = getState(hwnd);
    disk_info* disk = getDisk(st, i);

    const int cch = lstrlenW(disk->path) + 1;
    HGLOBAL hdst = GlobalAlloc(GMEM_MOVEABLE, cch * sizeof(WCHAR));
    if (!hdst)
        return;

    PWSTR dst = (LPWSTR)GlobalLock(hdst);
    if (dst) {
        StringCchCopyW(dst, cch, disk->path);
        GlobalUnlock(hdst);

        copyToClipboard(hdst);
    }
    GlobalFree(hdst);
}

static LRESULT onMenuCommand(HWND hwnd, WPARAM wparam, LPARAM lparam)
{
    typedef struct {
        UINT cmd;
        void (*cb)(HWND, DWORD);
    } dispatch;

    static const dispatch table[] = {
        {MENU_COPY,     onCopyClicked},
        {MENU_MOUNT,    onMountClicked},
        {MENU_UNMOUNT,  onUnmountClicked},
        {0, NULL}
    };

    const UINT cmd = LOWORD(wparam);
    switch (cmd)
    {
    case MENU_EXIT:
        DestroyWindow(hwnd);
        return 0;
    default:
        for (const dispatch* d = table; d->cmd; ++d) {
            if (cmd >= d->cmd && cmd < d->cmd + MAX_DISKS) {
                d->cb(hwnd, cmd - d->cmd);
                return 0;
            }
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
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, e->text);
}

static void createDiskMenu(HMENU parent, DWORD i, disk_info* disk, HBITMAP shield)
{
    HMENU menu = CreatePopupMenu();
    if (disk->e->error)
        appendError(menu, disk->e);
    else {
        AppendMenuW(menu, MF_STRING, MENU_COPY + i, L"&Copy device path");
        AppendMenuW(menu, MF_STRING, MENU_MOUNT + i, L"&Mount --bare");
        AppendMenuW(menu, MF_STRING, MENU_UNMOUNT + i, L"&Unmount");
        if (shield) {
            SetMenuItemBitmaps(menu, MENU_MOUNT + i, MF_BYCOMMAND, shield, shield);
            SetMenuItemBitmaps(menu, MENU_UNMOUNT + i, MF_BYCOMMAND, shield, shield);
        }
    }
    WCHAR text[256] = L"";
    wnsprintfW(text, ARRAYSIZE(text), L"&%u: %s %u parts",
        disk->index, disk->model, disk->n_parts);
    text[ARRAYSIZE(text) - 1] = 0;
    AppendMenuW(parent, MF_STRING | MF_POPUP, (UINT_PTR)menu, text);
}

static void createDisksMenu(state* st)
{
    if (st->e->error)
        appendError(st->menu, st->e);

    for (DWORD i = 0; i < st->n_disks; i++)
        createDiskMenu(st->menu, i, getDisk(st, i), st->shield);

    AppendMenuW(st->menu, MF_STRING, MENU_EXIT, L"&Exit");
}

static HBITMAP convertToBitmap(HICON icon)
{
    ICONINFOEX ii = { .cbSize = sizeof(ii), };
    if (!GetIconInfoExW(icon, &ii))
        return 0;

    return CopyImage(ii.hbmColor, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
}

static HBITMAP createShieldBitmap(void)
{
    SHSTOCKICONINFO ssii = { .cbSize = sizeof(ssii), };
    HRESULT hr = SHGetStockIconInfo(SIID_SHIELD, SHGSI_ICON | SHGSI_SMALLICON, &ssii);
    if (FAILED(hr))
        return 0;

    HBITMAP bitmap = convertToBitmap(ssii.hIcon);
    DestroyIcon(ssii.hIcon);
    return bitmap;
}

static LRESULT onCreate(HWND hwnd, LPARAM lparam)
{
    LPCREATESTRUCTW cs = (LPCREATESTRUCTW)lparam;
    state* st = cs->lpCreateParams;
    setState(hwnd, st);

    switch (CoInitializeEx(NULL, COINIT_MULTITHREADED)) {
    case S_OK:
    case S_FALSE:
        break;
    default:
        return GetLastError();
    }

    listDisks(st);

    st->menu = CreatePopupMenu();
    if (!st->menu)
        return GetLastError(); // without menu program is useless

    st->shield = createShieldBitmap();
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
    DeleteObject(st->shield);

    resetDisks(st);

    CoUninitialize();
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
        return onMenuCommand(hwnd, wparam, lparam);
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

    TerminateProcess(GetCurrentProcess(), (UINT)msg->wParam);
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
