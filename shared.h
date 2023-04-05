#pragma once

#include <windows.h>
#include <setupapi.h>

#define FS(OP)  \
    OP(NONE)    \
    OP(FAT32)   \
    OP(EXFAT)   \
    OP(NTFS)    \
    OP(BITLOCKER) \
    OP(EXT4)    \
    OP(XFS)     \
    OP(BTRFS)   \

#define ENUM(x) FS_ ## x,
typedef enum fs_type {
    FS(ENUM)
} fs_type;

// Dynamic allocations are good if we need lots of memory.
// But this a simple program. It has simple needs.
#define MAX_DISKS 32
#define MAX_PARTS 32

// Container for readable error message with a title
typedef struct err_desc {
    LPCWCH title;
    PWCHAR msg;
    DWORD error;
} err_desc;

typedef struct part_info {
    err_desc e[1];
    LONGLONG offset;
    LONGLONG size;
    DWORD number;
    fs_type fs;
} part_info;

typedef struct disk_info {
    err_desc e[1];
    PSP_DEVICE_INTERFACE_DETAIL_DATA dd;
    WCHAR parttype[4];
    DWORD number;
    DWORD n_parts;
    part_info part[MAX_PARTS];
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

static __inline part_info* getPart(disk_info* disk, DWORD i)
{
    return &disk->part[i];
}
