#pragma once

#include "Driver.h"

NTSTATUS
UsbProtectQueryVolumeUsbState(
    _In_ PFLT_VOLUME Volume,
    _Out_ PBOOLEAN IsUsb,
    _Out_ PBOOLEAN IsRemovable
    );
