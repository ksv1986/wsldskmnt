#pragma once

#include <windows.h>

// Dynamic allocations are good if we need lots of memory.
// But this a simple program. It has simple needs.
#define MAX_DISKS 32
#define MAX_PARTS 16
#define MAX_PART_TYPE 64
#define MAX_DRIVE_PATH 24

// Container for readable error message with a title
typedef struct err_desc {
    PCWCH title;
    PWCHAR text;
    DWORD error; // windows error code
} err_desc;

#define ERRINIT() { .text = L"" }

typedef struct part_info {
    DWORD index;
    ULONGLONG size;
} part_info;

typedef struct disk_info {
    err_desc e[1];
    DWORD index;
    PWCHAR model;
    WCHAR path[MAX_DRIVE_PATH];
    DWORD n_parts;
    err_desc e_parts[1];
    part_info part[MAX_PARTS];
} disk_info;

// Global program state
typedef struct state {
    HINSTANCE hinst;
    HWND hwnd;
    HMENU menu;
    HBITMAP shield;

    WCHAR dist[256]; // default wsl distribution name

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

static __inline part_info* getPart(disk_info* disk, DWORD i)
{
    return &disk->part[i];
}
