#pragma once

#include <windows.h>
#include <setupapi.h>

// Dynamic allocations are good if we need lots of memory.
// But this a simple program. It has simple needs.
#define MAX_DISKS 32

// Container for readable error message with a title
typedef struct err_desc {
    LPCWCH title;
    PWCHAR msg;
    DWORD error;
} err_desc;

typedef struct disk_info {
    err_desc e[1];
    PSP_DEVICE_INTERFACE_DETAIL_DATA dd;
} disk_info;

// Global program state
typedef struct state {
    HINSTANCE hinst;
    HWND hwnd;
    HMENU menu;

    err_desc e[1]; // if there was a problem to enumerate disks
    DWORD n_disks;
    disk_info disk[MAX_DISKS];
} state;

#define ERRINIT() { .msg = NULL }

// Save GetLastError() and it's description into *e with given title
// Returns saved error code value
DWORD setError(err_desc* e, LPCWCH title);
// Free resources used by error
void resetErr(err_desc* e);

// Enumerate physical disks and fill disk_info array.
// Returns 0 on success and GetLastError() on failure.
DWORD listDisks(state* st);

static __inline disk_info* getDisk(state* st, DWORD i)
{
    return &st->disk[i];
}
