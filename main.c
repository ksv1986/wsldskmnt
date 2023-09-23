#include "resource.h"
#include "shared.h"

#include <windows.h>
#include <objbase.h>
#include <shlwapi.h>
#include <strsafe.h>

#define MAX_CMD (MAX_DISKS * MAX_PARTS)

enum {
    APP_NOTIFY = WM_APP + 1, // Tray icon notification callback message
    MENU_EXIT = 40001,
    MENU_COPY = 41000,
    MENU_MOUNT = 42000,
    MENU_UNMOUNT = 43000,
    MENU_PART = 44000,
};
// Tray icon will be identified by guid
static const GUID GUID_NOTIFY = {
    0xd9cbd4ab,
    0x346b,
    0x4905,
    { 0xb2, 0xf, 0xfc, 0xe3, 0x7e, 0x80, 0xd7, 0xb3 }
};

static const WCHAR* WSL_PATH = L"C:\\Windows\\System32\\wsl.exe";

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

static DWORD onWslRunAs(HWND hwnd, DWORD exitCode)
{
    if (!exitCode)
        return 0;

    WCHAR text[128];
    wnsprintfW(text, ARRAYSIZE(text), L"wsl.exe exit code: %d", exitCode);
    showWarning(hwnd, text, L"Failed to run wsl.exe");
    return exitCode;
}

static void onWslRunFailure(HWND hwnd, DWORD code)
{
    err_desc e[1] = { ERRINIT() };
    setErrorCode(e, L"Failed to start wsl.exe", code);
    showWarning(hwnd, e->text, e->title);
    resetErr(e);
}

static DWORD runWslAs(HWND hwnd, WCHAR* cmd)
{
    SHELLEXECUTEINFO sei = {
        .cbSize = sizeof(sei),
        .fMask = SEE_MASK_NOCLOSEPROCESS,
        .lpVerb = L"runas",
        .lpFile = WSL_PATH,
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

        return onWslRunAs(hwnd, exitCode);
    }
    else {
        DWORD code = GetLastError();
        if (code != ERROR_CANCELLED)
            onWslRunFailure(hwnd, code);
        return code;
    }
}

static void closeIfOpen(HANDLE* ph)
{
    if (*ph)
        CloseHandle(*ph);
    *ph = INVALID_HANDLE_VALUE;
}

static void closeStdHandles(STARTUPINFO* si, HANDLE* pout)
{
    closeIfOpen(&si->hStdOutput);
    closeIfOpen(pout);
    si->dwFlags &= ~STARTF_USESTDHANDLES;
}

static void openStdHandles(STARTUPINFO* si, HANDLE* pout)
{
    SECURITY_ATTRIBUTES sa = {
        .nLength = sizeof(sa),
        .bInheritHandle = TRUE,
    };
    si->hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si->hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!CreatePipe(pout, &si->hStdOutput, &sa, 0)) {
        closeStdHandles(si, pout);
        return;
    }
    SetHandleInformation(*pout, HANDLE_FLAG_INHERIT, 0);
    si->dwFlags |= STARTF_USESTDHANDLES;
}

typedef DWORD (*cmd_cb)(HWND hwnd, DWORD exitCode, const WCHAR* text, DWORD cch);

static DWORD execWslAndThen(HWND hwnd, WCHAR* cmd, cmd_cb cb)
{
    PROCESS_INFORMATION pi;
    HANDLE out = 0;
    STARTUPINFO si = { .cb = sizeof(si), };
    // command line must start with executable name
    WCHAR text[1024];
    wnsprintfW(text, ARRAYSIZE(text), L"wsl.exe %s", cmd);
    cmd = text;

    openStdHandles(&si, &out);
    if (!CreateProcessW(WSL_PATH,
        cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
    {
        onWslRunFailure(hwnd, GetLastError());
        closeStdHandles(&si, &out);
        return ~0u;
    }
    closeIfOpen(&si.hStdOutput);

    BYTE* buf = (BYTE*)text;
    BYTE* p = buf;
    DWORD sz = sizeof(text) - sizeof(WCHAR);
    do {
        DWORD n;
        if (!ReadFile(out, p, sz, &n, NULL))
            break;
        sz -= n;
        p += n;
    } while (sz);
    closeIfOpen(&out);
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    closeStdHandles(&si, &out);

    const DWORD cch = (DWORD)(p - buf) / sizeof(WCHAR);
    text[cch] = 0;

    return cb(hwnd, exitCode, text, cch);
}

static DWORD onWslExit(HWND hwnd, DWORD exitCode, const WCHAR* text, DWORD cch)
{
    UNREFERENCED_PARAMETER(cch);
    if (exitCode) {
        WCHAR title[128];
        wnsprintfW(title, ARRAYSIZE(title), L"wsl.exe exit code: %d", exitCode);
        showWarning(hwnd, text, title);
    }
    return exitCode;
}

static void execWsl(HWND hwnd, WCHAR* cmd)
{
    execWslAndThen(hwnd, cmd, onWslExit);
}

static void onMountClicked(HWND hwnd, DWORD i)
{
    state* st = getState(hwnd);
    disk_info* disk = getDisk(st, i);

    WCHAR cmd[MAX_PATH];
    wnsprintfW(cmd, ARRAYSIZE(cmd), L"--mount \"%s\" --bare", disk->path);

    runWslAs(hwnd, cmd);
}

static BOOL directoryExists(const WCHAR *path)
{
    DWORD dwAttrib = GetFileAttributesW(path);

    return (dwAttrib != INVALID_FILE_ATTRIBUTES &&
        (dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}

static void onPartClicked(HWND hwnd, DWORD n)
{
    state* st = getState(hwnd);
    DWORD i = n / MAX_PARTS;
    DWORD j = n % MAX_PARTS;
    disk_info* disk = getDisk(st, i);
    part_info* part = getPart(disk, j);
    DWORD p = part->index + 1;
    WCHAR path[MAX_PATH];

    const WCHAR* name = NULL;
    for (const WCHAR* c = disk->path; *c; ++c)
        if (*c == L'\\')
            name = c + 1;

    wnsprintfW(path, ARRAYSIZE(path), L"\\\\wsl$\\%s\\mnt\\wsl\\%sp%u", st->dist, name, p);
    if (!directoryExists(path)) {
        WCHAR cmd[MAX_PATH];
        wnsprintfW(cmd, ARRAYSIZE(cmd), L"--mount %s --partition %u", disk->path, p);

        if (runWslAs(hwnd, cmd) != 0)
            return;
    }

    SHELLEXECUTEINFO sei = {
        .cbSize = sizeof(sei),
        .lpVerb = L"open",
        .lpFile = path,
        .hwnd = hwnd,
        .nShow = SW_NORMAL,
    };
    ShellExecuteExW(&sei);
}

static void onUnmountClicked(HWND hwnd, DWORD i)
{
    state* st = getState(hwnd);
    disk_info* disk = getDisk(st, i);

    WCHAR cmd[MAX_PATH];
    wnsprintfW(cmd, ARRAYSIZE(cmd), L"--unmount %s", disk->path);

    execWsl(hwnd, cmd);
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
        {MENU_PART,     onPartClicked},
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
            if (cmd >= d->cmd && cmd < d->cmd + MAX_CMD) {
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
    WCHAR text[256] = L"";
    DWORD letters = 0;

    if (disk->e->error)
        appendError(menu, disk->e);
    else {
        AppendMenuW(menu, MF_STRING, MENU_COPY + i, L"&Copy device path");
        AppendMenuW(menu, MF_STRING, MENU_MOUNT + i, L"&Mount --bare");
        if (shield)
            SetMenuItemBitmaps(menu, MENU_MOUNT + i, MF_BYCOMMAND, shield, shield);

        if (disk->e_parts->error)
            appendError(menu, disk->e_parts);
        else
            for (DWORD j = 0; j < disk->n_parts; ++j) {
                part_info* part = getPart(disk, j);

                WCHAR letter[8] = L"";
                if (part->letter) {
                    letters ++;
                    wnsprintfW(letter, 8, L" (%c)", part->letter);
                }

                const WCHAR* suffix = L"MB";
                ULONGLONG hi = part->size >> 10;
                if (hi > (1000<<10)) {
                    suffix = L"GB";
                    hi >>= 10;
                }
                DWORD lo = hi % 1000;
                hi /= 1000;

                if (hi < 10 && lo > 100)
                    wnsprintfW(text, ARRAYSIZE(text), L"Part %u%s: %llu.%u%s",
                        part->index, letter, hi, lo / 10, suffix);
                else
                    wnsprintfW(text, ARRAYSIZE(text), L"Part %u%s: %llu%s",
                        part->index, letter, hi, suffix);

                const DWORD n = MENU_PART + i * MAX_PARTS + j;
                AppendMenuW(menu, MF_STRING, n, text);
                if (shield)
                    SetMenuItemBitmaps(menu, n, MF_BYCOMMAND, shield, shield);
            }

        AppendMenuW(menu, MF_STRING, MENU_UNMOUNT + i, L"&Unmount");
    }
    wnsprintfW(text, ARRAYSIZE(text), L"&%u: %s %u/%u parts",
        disk->index, disk->model, letters, disk->n_parts);
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

static DWORD parseDistroList(HWND hwnd, DWORD exitCode, const WCHAR* text, DWORD cch)
{
    state* st = getState(hwnd);
    st->dist[0] = 0;

    if (exitCode)
        return onWslExit(hwnd, exitCode, text, cch);

    for (;;) {
        switch (*text) {
        case 0:
            return 0;

        case L'*': // found
            // skip "* "
            ++text;
            ++text;
            // copy name of the distro
            const WCHAR * sep = text;
            WCHAR *p = st->dist;
            while (*sep && *sep != L'\t' && *sep != L' ')
                *p++ = *sep++;
            *p = 0;
            return 0;

        default:
            // goto next line
            while (*text && *text != L'\r' && *text != '\n')
                ++text;
            while (*text && (*text == L'\r' || *text == L'\n'))
                ++text;
        }
    }
}

static void getDefaultDistribution(HWND hwnd, state *st)
{
    WCHAR cmd[MAX_PATH];
    wnsprintfW(cmd, ARRAYSIZE(cmd), L"--list -v");
    execWslAndThen(hwnd, cmd, parseDistroList);
    const WCHAR* text = L"No distribution";
    if (st->dist[0])
        text = st->dist;
    InsertMenuW(st->menu, 0, MF_BYPOSITION | MF_STRING | MF_DISABLED, 0, st->dist);
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

    getDefaultDistribution(hwnd, st);
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
