#include <windows.h>

// https://devblogs.microsoft.com/oldnewthing/20230328-00/?p=107978
// https://skanthak.homepage.t-online.de/snafu.html

DWORD_PTR __security_cookie = 3141592653589793241ULL >> 16;

const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used = {
    .Size = sizeof(_load_config_used),
    .MajorVersion = _MSC_VER / 100,
    .MinorVersion = _MSC_VER % 100,
    .SecurityCookie = (ULONGLONG) & __security_cookie,
    .GuardFlags = LOAD_LIBRARY_SEARCH_SYSTEM32,
};
