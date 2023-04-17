#pragma once

#include <windows.h>

// Dynamic allocations are good if we need lots of memory.
// But this a simple program. It has simple needs.
#define MAX_DISKS 32
#define MAX_DRIVE_PATH 24

// Container for readable error message with a title
typedef struct err_desc {
    PCWCH title;
    PWCHAR text;
    DWORD error; // windows error code
} err_desc;

#define ERRINIT() { .text = L"" }

typedef struct disk_info {
    err_desc e[1];
    DWORD index;
    PWCHAR model;
    WCHAR path[MAX_DRIVE_PATH];
    DWORD n_parts;
} disk_info;

// Global program state
typedef struct state {
    HINSTANCE hinst;
    HWND hwnd;
    HMENU menu;
    HBITMAP shield;

    err_desc e[1]; // if there was a problem to enumerate disks
    DWORD n_disks;
    disk_info disk[MAX_DISKS];
} state;

// Free resources used by error
void resetErr(err_desc* e);


DWORD setError(err_desc* e, PCWCH title);
DWORD setErrorCode(err_desc* e, PCWCH title, DWORD code);

// Enumerate physical disks and fill disk_info array.
// Returns 0 on success and GetLastError() on failure.
HRESULT listDisks(state* st);
void resetDisks(state* st);

static __inline disk_info* getDisk(state* st, DWORD i)
{
    return &st->disk[i];
}
