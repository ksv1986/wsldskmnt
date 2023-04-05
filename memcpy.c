#include <windows.h>

#undef RtlCopyMemory
void RtlCopyMemory(
    _Out_writes_bytes_all_(len) void* d,
    _In_ const void *src,
    _In_ size_t len
);

#pragma function(memcpy)
_Post_equal_to_(_Dst)
_At_buffer_(
    (unsigned char*)_Dst,
    _Iter_,
    _Size,
    _Post_satisfies_(((unsigned char*)_Dst)[_Iter_] == ((unsigned char*)_Src)[_Iter_])
)
void* __cdecl memcpy(
    _Out_writes_bytes_all_(_Size) void* _Dst,
    _In_reads_bytes_(_Size)       void const* _Src,
    _In_                          size_t      _Size
)
{
    RtlCopyMemory(_Dst, _Src, _Size);
    return _Dst;
}
