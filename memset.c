#include <windows.h>

// Workaround for
// Error	LNK2001	unresolved external symbol _memset

// Compiler inserts calls to memset() when large structs are initialized.
// Usually memset() is provided by CRT, but we opt out of it by /NODEFAULTLIB.
// So, instead we will provide implementation based on what is available in system libraries.

#undef RtlFillMemory
void RtlFillMemory(
    _Out_writes_bytes_all_(len) void* d,
    _In_ size_t len,
    _In_ int v
);

#pragma function(memset)
_Post_equal_to_(_Dst)
_At_buffer_(
    (unsigned char*)_Dst,
    _Iter_,
    _Size,
    _Post_satisfies_(((unsigned char*)_Dst)[_Iter_] == _Val)
)
void* __cdecl memset(
    _Out_writes_bytes_all_(_Size) void* _Dst,
    _In_                          int    _Val,
    _In_                          size_t _Size
)
{
    RtlFillMemory(_Dst, _Size, _Val);
    return _Dst;
}
