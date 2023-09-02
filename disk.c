#include "shared.h"

#include <windows.h>
#include <wbemidl.h>
#include <Shlwapi.h>
#include <strsafe.h>

static IWbemStatusCodeText* pCode;

static void ignore(HRESULT hr) { UNREFERENCED_PARAMETER(hr); }

void resetErr(err_desc* e)
{
    e->title = NULL;
    e->text = LocalFree(e->text);
    e->error = 0;
}

static void resetPart(part_info* part)
{
    part->index = 0;
    part->size = 0;
}

static void resetDisk(disk_info* disk)
{
    disk->model = LocalFree(disk->model);
    while (disk->n_parts) {
        disk->n_parts--;
        resetPart(getPart(disk, disk->n_parts));
    }
    disk->n_parts = 0;
    resetErr(disk->e);
    resetErr(disk->e_parts);
}

void resetDisks(state* st)
{
    while (st->n_disks) {
        st->n_disks--;
        resetDisk(getDisk(st, st->n_disks));
    }
    resetErr(st->e);

    if (pCode) {
        pCode->lpVtbl->Release(pCode);
        pCode = NULL;
    }
}

static DWORD returnErr(err_desc* e)
{
    // Make sure e->text is set to something
    if (!e->text) {
        static const WCHAR unknown[] = L"error";
        e->text = LocalAlloc(0, sizeof(unknown));
        StringCchCopyW(e->text, ARRAYSIZE(unknown), unknown);
        return e->error;
    }

    // Windows error messages can be too lengthy.
    // Leave only one line
    for (PWCHAR p = e->text; ; ++p) {
        switch (*p) {
        case 0: case L'\r': case L'\n':
            *p = 0;
            return e->error;
        }
    }
}

DWORD setErrorCode(err_desc* e, PCWCH title, DWORD code)
{
    e->title = title;
    e->error = code;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, code, 0, (wchar_t*)&e->text, 0, NULL);
    return returnErr(e);
}

DWORD setError(err_desc* e, PCWCH title)
{
    return setErrorCode(e, title, GetLastError());
}

static HRESULT setHresult(err_desc* e, PCWCH title, HRESULT hr)
{
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32)
        return setErrorCode(e, title, HRESULT_CODE(hr));

    e->title = title;
    e->error = hr;

    // https://learn.microsoft.com/en-us/windows/win32/com/error-handling-in-com
    if (!pCode)
        ignore(CoCreateInstance(&CLSID_WbemStatusCodeText, 0,
            CLSCTX_INPROC_SERVER, &IID_IWbemStatusCodeText, (LPVOID*)&pCode));

    if (pCode) {
        BSTR text = NULL;
        pCode->lpVtbl->GetErrorCodeText(pCode, hr, 0, 0, &text);
        if (text) {
            e->text = StrDupW(text);
            SysFreeString(text);
        }
    }
    return returnErr(e);
}

static ULONGLONG wtou64(const WCHAR *s)
{
    ULONGLONG r = 0;
    for (;;) {
        switch (*s) {
        case 0:
            return r;
        case L'0': case L'1': case L'2': case L'3': case L'4':
        case L'5': case L'6': case L'7': case L'8': case L'9':
            r = r * 10 + (*s - L'0');
            s++;
            break;
        default:
            return 0;
        }
    }
}

static void initPart(part_info* part, IWbemClassObject* pCls)
{
    VARIANT v[1];
    VariantInit(v);
#define GET(name, copy)                                                     \
    do {                                                                    \
        HRESULT hr = pCls->lpVtbl->Get(pCls, L#name, 0, v, NULL, NULL);     \
        if (!FAILED(hr))                                                    \
            copy;                                                           \
        VariantClear(v);                                                    \
    } while (0)

    GET(Index, part->index = v->uintVal);
    // for some reason uint64 value is returned as string
    GET(Size, part->size = wtou64(v->bstrVal));
#undef GET
}

static HRESULT listParts(disk_info* disk, IWbemServices* pSvc)
{
    WCHAR query[64];
    wnsprintfW(query, ARRAYSIZE(query), L"SELECT * from Win32_DiskPartition WHERE DiskIndex = %u", disk->index);

    IEnumWbemClassObject* pEnum = NULL;
    HRESULT hr = pSvc->lpVtbl->ExecQuery(pSvc, L"WQL", query, 0, NULL, &pEnum);
    if (FAILED(hr))
        return setHresult(disk->e_parts, L"IWbemServices::ExecQuery failed", hr);

    DWORD i = 0;
    IWbemClassObject* pCls = NULL;
    ULONG nr = 0;
    for (pEnum->lpVtbl->Next(pEnum, WBEM_INFINITE, 1, &pCls, &nr); nr;
        pEnum->lpVtbl->Next(pEnum, WBEM_INFINITE, 1, &pCls, &nr))
    {
        initPart(getPart(disk, i), pCls);
        pCls->lpVtbl->Release(pCls);

        i++;
        if (i == MAX_PARTS || i == disk->n_parts)
            break;
    }

    pEnum->lpVtbl->Release(pEnum);
    return 0;
}

static HRESULT initDisk(disk_info* disk, IWbemClassObject* pCls)
{
    // https://learn.microsoft.com/en-us/windows/win32/cimwin32prov/win32-diskdrive
    VARIANT v[1];
    VariantInit(v);

#define GET(name, copy)                                                     \
    do {                                                                    \
        HRESULT hr = pCls->lpVtbl->Get(pCls, L#name, 0, v, NULL, NULL);     \
        if (FAILED(hr))                                                     \
            return setHresult(disk->e, L"Failed to get disk " L#name, hr);  \
        copy;                                                               \
        VariantClear(v);                                                    \
    } while (0)

    GET(Index,      disk->index = v->uintVal);
    GET(Model,      disk->model = StrDupW(v->bstrVal));
    GET(DeviceID,   StringCchCopyW(disk->path, ARRAYSIZE(disk->path), v->bstrVal));
    GET(Partitions, disk->n_parts = v->uintVal);

#undef GET
    return 0;
}

static HRESULT servicesListDisks(state* st, IWbemServices * pSvc)
{
    // https://learn.microsoft.com/en-us/windows/win32/wmisdk/example--getting-wmi-data-from-the-local-computer
    // Without proxy blanket ExecQuery returns 0x80041003 "Access denied"
    HRESULT hr = CoSetProxyBlanket(
        (IUnknown*)pSvc,             // Indicates the proxy to set
        RPC_C_AUTHN_WINNT,
        RPC_C_AUTHZ_NONE,
        NULL,                        // Server principal name
        RPC_C_AUTHN_LEVEL_CALL,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL,                        // client identity
        EOAC_NONE                    // proxy capabilities
    );
    if (FAILED(hr))
        return setHresult(st->e, L"CoSetProxyBlanket failed", hr);

    IEnumWbemClassObject* pEnum = NULL;
    hr = pSvc->lpVtbl->ExecQuery(pSvc, L"WQL", L"SELECT * from Win32_DiskDrive", 0, NULL, &pEnum);
    if (FAILED(hr))
        return setHresult(st->e, L"IWbemServices::ExecQuery failed", hr);

    IWbemClassObject* pCls = NULL;
    ULONG nr = 0;
    for (pEnum->lpVtbl->Next(pEnum, WBEM_INFINITE, 1, &pCls, &nr); nr;
         pEnum->lpVtbl->Next(pEnum, WBEM_INFINITE, 1, &pCls, &nr))
    {
        disk_info* disk = getDisk(st, st->n_disks);
        st->n_disks++;

        initDisk(disk, pCls);

        pCls->lpVtbl->Release(pCls);

        listParts(disk, pSvc);

        if (st->n_disks == MAX_DISKS)
            break;
    }

    pEnum->lpVtbl->Release(pEnum);
    return 0;
}

static HRESULT locatorListDisks(state* st, IWbemLocator* pLoc)
{
    IWbemServices* pSvc = NULL;
    HRESULT hr = pLoc->lpVtbl->ConnectServer(pLoc, L"ROOT\\CIMV2", NULL, NULL, 0, 0, 0, 0, &pSvc);
    if (FAILED(hr))
        return setHresult(st->e, L"IWbemLocator::ConnectServer failed", hr);

    hr = servicesListDisks(st, pSvc);
    pSvc->lpVtbl->Release(pSvc);
    return hr;
}

static void swapDisks(disk_info* l, disk_info* r)
{
    disk_info t[1];
    *t = *l;
    *l = *r;
    *r = *t;
}

static void sortDisks(state* st)
{
    if (st->n_disks < 2)
        return;

    for (DWORD x = 0; x < st->n_disks - 1; x++) {
        for (DWORD y = 0; y < st->n_disks - x - 1; y++) {
            disk_info* l = getDisk(st, y);
            disk_info* r = getDisk(st, y + 1);
            if (l->index > r->index)
                swapDisks(l, r);
        }
    }
}

HRESULT listDisks(state* st)
{
    IWbemLocator* pLoc = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_WbemLocator, 0,
        CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (LPVOID*)&pLoc);

    if (FAILED(hr))
        return setHresult(st->e, L"Failed to create IWbemLocator instance", hr);

    hr = locatorListDisks(st, pLoc);
    pLoc->lpVtbl->Release(pLoc);

    sortDisks(st);
    return hr;
}
