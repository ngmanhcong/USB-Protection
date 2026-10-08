#pragma once

/*
 * Pure instance-ID hashing shared by user mode and kernel mode.  Keep this
 * byte-for-byte compatible with the original UsbDevicesHashInstanceId.
 */
#if defined(_KERNEL_MODE) || defined(_NTDDK_) || defined(_WDMDDK_)
#include <ntddk.h>
#else
#include <Windows.h>
#endif

static __forceinline ULONGLONG
UsbIdentityHashInstanceId(
    _In_opt_ const WCHAR* InstanceId
    )
{
    ULONGLONG hash = 14695981039346656037ULL;
    const WCHAR* cursor;

    if (InstanceId == NULL || InstanceId[0] == L'\0') {
        return 0;
    }

    for (cursor = InstanceId; *cursor != L'\0'; cursor++) {
        WCHAR character = *cursor;

        if (character >= L'a' && character <= L'z') {
            character = (WCHAR)(character - (L'a' - L'A'));
        }
        if (character > 0x7f) {
            return 0;
        }

        hash ^= (UCHAR)character;
        hash *= 1099511628211ULL;
    }

    return hash;
}
