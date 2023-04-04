#include <windows.h>

static LRESULT CALLBACK WndProc(HWND hwnd, UINT umsg, WPARAM wparam, LPARAM lparam)
{
    switch (umsg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, umsg, wparam, lparam);
}

int WINAPI wWinMain(_In_ HINSTANCE hinst, _In_opt_ HINSTANCE hprev, _In_ PWSTR argv, _In_ int show)
{
    UNREFERENCED_PARAMETER(hprev);
    UNREFERENCED_PARAMETER(argv);
    UNREFERENCED_PARAMETER(show);

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
        NULL                // Additional application data
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
