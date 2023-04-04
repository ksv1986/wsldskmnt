#include <windows.h>

int WINAPI wWinMain(_In_ HINSTANCE hinst, _In_opt_ HINSTANCE hprev, _In_ PWSTR argv, _In_ int show)
{
    UNREFERENCED_PARAMETER(hinst);
    UNREFERENCED_PARAMETER(hprev);
    UNREFERENCED_PARAMETER(argv);
    UNREFERENCED_PARAMETER(show);

    return 0;
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
