#include "shared.h"

#include <initguid.h>
#include <winioctl.h>

static const WCHAR PT_UNK[] = L"   ";
static const WCHAR PT_MBR[] = L"MBR";
static const WCHAR PT_GPT[] = L"GPT";
static const WCHAR PT_RAW[] = L"RAW";

void resetErr(err_desc* e)
{
    e->title = NULL;
    e->msg = LocalFree(e->msg);
    e->error = 0;
}

static void resetPart(part_info* part)
{
    part->number = 0;
    part->fs = FS_NONE;
    resetErr(part->e);
}

static void resetDisk(disk_info* disk)
{
    while (disk->n_parts) {
        disk->n_parts--;
        resetPart(&disk->part[disk->n_parts]);
    }
    memcpy(disk->parttype, PT_UNK, sizeof(disk->parttype));
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

static void initOverlapped(OVERLAPPED* ov, LONGLONG offs)
{
    memset(ov, 0, sizeof(*ov));
    ov->Offset = (DWORD)offs;
    ov->OffsetHigh = (DWORD)(offs >> 32);
}

static int setFs(part_info* part, fs_type fs)
{
    part->fs = fs;
    return 0;
}

static BOOL isFat(const BYTE sector[512])
{
    // https://msdn.microsoft.com/en-us/windows/hardware/gg463080.aspx
    // This is only valid for well-formed boot sector.
    // But it doesn't matter: FAT is not a main target of the program
    if (RtlCompareMemory(sector + 82, "FAT32   ", 8) == 8 ||
        RtlCompareMemory(sector + 54, "FAT16   ", 8) == 8 ||
        RtlCompareMemory(sector + 54, "FAT12   ", 8) == 8 ||
        RtlCompareMemory(sector + 54, "FAT     ", 8) == 8)
        return TRUE;
    return FALSE;
}

static int getPartFs(HANDLE dev, part_info* part,
    const PARTITION_INFORMATION_EX* pinfo, BYTE *sector, DWORD size)
{
    OVERLAPPED ov[1];

    part->offset = pinfo->StartingOffset.QuadPart;
    part->size = pinfo->PartitionLength.QuadPart;

    ZeroMemory(sector, size);
    initOverlapped(ov, part->offset);

    DWORD n;
    if (!ReadFile(dev, sector, size, &n, ov))
        return setError(part->e, L"ReadFile failed");

    if (RtlCompareMemory(sector + 3, "NTFS    ", 8) == 8)
        return setFs(part, FS_NTFS);

    if (RtlCompareMemory(sector + 3, "EXFAT   ", 8) == 8)
        return setFs(part, FS_EXFAT);

    if (RtlCompareMemory(sector + 3, "-FVE-FS-", 8) == 8)
        return setFs(part, FS_BITLOCKER);

    if (isFat(sector))
        return setFs(part, FS_BITLOCKER);

    if (RtlCompareMemory(sector, "XFSB", 4) == 4)
        return setFs(part, FS_XFS);

    if (RtlCompareMemory(sector + 0x438, "\xEF\x53", 2) == 2)
        return setFs(part, FS_EXT4);

    ZeroMemory(sector, size);
    initOverlapped(ov, part->offset + 0x10000);

    if (!ReadFile(dev, sector, size, &n, NULL))
        return setError(part->e, L"ReadFile failed");

    if (RtlCompareMemory(sector + 8, "_BHRfS_M", 8) == 8)
        return setFs(part, FS_BTRFS);

    return 0;
}

// Container type for data read from physical disk
typedef struct disk_state {
    BYTE sector[4096];
    DRIVE_LAYOUT_INFORMATION_EX layout;
} disk_state;

static int getPartListWith(HANDLE dev, disk_info* disk, disk_state* state, DWORD size)
{
    PDRIVE_LAYOUT_INFORMATION_EX dest = &state->layout;
    DWORD ctl = IOCTL_DISK_GET_DRIVE_LAYOUT_EX;
    DWORD n;
    if (!DeviceIoControl(dev, ctl, NULL, 0, dest, size - sizeof(state->sector), &n, NULL))
        return setError(disk->e, L"IOCTL_DISK_GET_DRIVE_LAYOUT_EX failed");

    PCWCH pt = PT_UNK;
    switch (dest->PartitionStyle) {
    case PARTITION_STYLE_MBR: pt = PT_MBR; break;
    case PARTITION_STYLE_GPT: pt = PT_GPT; break;
    case PARTITION_STYLE_RAW: pt = PT_RAW; break;
    }
    memcpy(disk->parttype, pt, sizeof(disk->parttype));

    for (DWORD i = 0; i < dest->PartitionCount; ++i) {
        n = dest->PartitionEntry[i].PartitionNumber;
        if (!n)
            continue;

        part_info* part = &disk->part[disk->n_parts];
        disk->n_parts++;

        part->number = n;
        getPartFs(dev, part, &dest->PartitionEntry[i], state->sector, sizeof(state->sector));
    }
    return 0;
}

static int getPartList(HANDLE dev, disk_info* disk)
{
    disk->number = 0;

    DWORD ctl = IOCTL_STORAGE_GET_DEVICE_NUMBER;
    DWORD n;
    STORAGE_DEVICE_NUMBER dn;
    if (!DeviceIoControl(dev, ctl, NULL, 0, &dn, sizeof(dn), &n, NULL))
        return setError(disk->e, L"IOCTL_STORAGE_GET_DEVICE_NUMBER failed");

    disk->number = dn.DeviceNumber;

    DWORD size = sizeof(disk_state) + MAX_PARTS * sizeof(PARTITION_INFORMATION_EX);
    disk_state* state = LocalAlloc(0, size);
    if (!state)
        return setError(disk->e, L"PDRIVE_LAYOUT_INFORMATION_EX allocation failed");

    int rc = getPartListWith(dev, disk, state, size);
    LocalFree(state);
    return rc;
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

    HANDLE dev = CreateFileW(disk->dd->DevicePath,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (dev != INVALID_HANDLE_VALUE) {
        getPartList(dev, disk);
        CloseHandle(dev);
    } else
        setError(disk->e, L"CreateFile failed");
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
