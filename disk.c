#include "shared.h"

#include <initguid.h>
#include <winioctl.h>

void resetErr(err_desc* e)
{
    e->title = NULL;
    e->msg = LocalFree(e->msg);
    e->error = 0;
}

static void resetDisk(disk_info* disk)
{
    disk->dd = LocalFree(disk->dd);
    resetErr(disk->e);
}

static void resetDisks(state* st)
{
    while (st->n_disks) {
        st->n_disks--;
        resetDisk(&st->disk[st->n_disks]);
    }
    resetErr(st->e);
}

DWORD setError(err_desc* e, LPCWCH title)
{
    e->title = title;
    e->error = GetLastError();
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, e->error, 0, (wchar_t*)&e->msg, 0, NULL);

    if (!e->msg) {
        static const WCHAR unknown[] = L"error";
        e->msg = LocalAlloc(0, sizeof(unknown));
        memcpy(e->msg, unknown, sizeof(unknown));
        return e->error;
    }

    // Windows error messages can be too lengthy.
    // Leave only one line
    for (PWCHAR p = e->msg; ; ++p) {
        switch (*p) {
        case 0: case L'\r': case L'\n':
            *p = 0;
            return e->error;
        }
    }
}

static int readDisk(HDEVINFO dset,
    PSP_DEVICE_INTERFACE_DATA devData,
    disk_info* disk, DWORD size)
{
    if (!(SetupDiGetDeviceInterfaceDetailW(dset, devData, disk->dd, size, NULL, NULL))) {
        resetDisk(disk);
        return GetLastError();
    }
    // Save actual string size
    disk->dd->cbSize = size - sizeof(disk->dd->cbSize);
    return 0;
}

DWORD listDisks(state* st)
{
    const GUID* guid = &GUID_DEVINTERFACE_DISK;
    HDEVINFO dset = SetupDiGetClassDevsW(guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dset == INVALID_HANDLE_VALUE)
        return setError(st->e, L"SetupDiGetClassDevs failed");

    DWORD devIndex = 0;
    SP_DEVICE_INTERFACE_DATA devData[1] = { {.cbSize = sizeof(devData), } };
    while (SetupDiEnumDeviceInterfaces(dset, NULL, guid, devIndex, devData)) {
        ++devIndex;

        DWORD size;
        SetupDiGetDeviceInterfaceDetailW(dset, devData, NULL, 0, &size, NULL);

        disk_info* disk = getDisk(st, st->n_disks);
        disk->dd = LocalAlloc(0, size);
        if (!disk->dd) {
            setError(st->e, L"Failed to allocate disk details");
            break;
        }

        ZeroMemory(disk->dd, size);
        disk->dd->cbSize = sizeof(*disk->dd);

        if (readDisk(dset, devData, disk, size))
            continue;

        st->n_disks++;
    }
    SetupDiDestroyDeviceInfoList(dset);

    return st->e->error;
}
