#include "Driver.h"
#include "Callbacks.h"
#include "Communication.h"
#include "UsbDetection.h"

#include <stdarg.h>

#pragma prefast(disable:__WARNING_ENCODE_MEMBER_FUNCTION_POINTER, "Not valid for kernel mode drivers")

PFLT_FILTER gUsbProtectFilter = NULL;
PFLT_PORT gUsbProtectServerPort = NULL;
volatile LONG gUsbProtectEnabled = 1;
volatile LONG gUsbProtectExecutableBlockingEnabled = 1;
volatile LONG gUsbProtectApprovedOnlyEnabled = 0;

static KSPIN_LOCK gUsbProtectApprovedDevicesLock;
static ULONGLONG gUsbProtectApprovedDevices[USB_PROTECTION_MAX_APPROVED_DEVICES];
static ULONG gUsbProtectApprovedDeviceCount = 0;

/*
bảng đăng kí operation mà driver muốn theo dõi
*/
CONST FLT_OPERATION_REGISTRATION gUsbProtectCallbacks[] = {
    { IRP_MJ_CREATE, 0, UsbProtectPreCreate, NULL },
    { IRP_MJ_READ, 0, UsbProtectPreRead, NULL },
    { IRP_MJ_WRITE, 0, UsbProtectPreWrite, NULL },
    { IRP_MJ_SET_INFORMATION, 0, UsbProtectPreSetInformation, NULL },
    { IRP_MJ_ACQUIRE_FOR_SECTION_SYNCHRONIZATION,
      0,
      UsbProtectPreAcquireForSectionSynchronization,
      NULL },
    { IRP_MJ_OPERATION_END }
};

/*
driver sử dụng context gắn với từng minifilter instance
*/
CONST FLT_CONTEXT_REGISTRATION gUsbProtectContexts[] = {
    {
        FLT_INSTANCE_CONTEXT,
        0,
        UsbProtectInstanceContextCleanup,
        sizeof(USBPROTECT_INSTANCE_CONTEXT),
        USBP_TAG
    },
    { FLT_CONTEXT_END }
};

/*
mô tả cho filter manager biết driver có gì
*/
CONST FLT_REGISTRATION gUsbProtectRegistration = {
    sizeof(FLT_REGISTRATION),
    FLT_REGISTRATION_VERSION,
    0,
    gUsbProtectContexts,
    gUsbProtectCallbacks,
    UsbProtectUnload,
    UsbProtectInstanceSetup,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

/*
hàm thực hiện atomically để kiểm tra xem protection có đang bật hay không
*/
BOOLEAN
UsbProtectIsProtectionEnabled(
    VOID
    )
{
    return (InterlockedCompareExchange((volatile LONG *)&gUsbProtectEnabled, 0, 0) != 0);
}

VOID
UsbProtectLog(
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...
    )
{
    va_list args;

    va_start(args, Format);
    vDbgPrintExWithPrefix("[UsbProtect] ",
                          DPFLTR_IHVDRIVER_ID,
                          DPFLTR_INFO_LEVEL,
                          Format,
                          args);
    va_end(args);

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "\n");
}

/*
bật tắt protection, atomically
*/
VOID
UsbProtectSetProtectionEnabled(
    _In_ BOOLEAN Enabled
    )
{
    InterlockedExchange((volatile LONG *)&gUsbProtectEnabled, Enabled ? 1 : 0);
}

BOOLEAN
UsbProtectIsExecutableBlockingEnabled(
    VOID
    )
{
    return (InterlockedCompareExchange(
                (volatile LONG *)&gUsbProtectExecutableBlockingEnabled,
                0,
                0) != 0);
}

VOID
UsbProtectSetExecutableBlockingEnabled(
    _In_ BOOLEAN Enabled
    )
{
    InterlockedExchange((volatile LONG *)&gUsbProtectExecutableBlockingEnabled,
                        Enabled ? 1 : 0);
}

BOOLEAN
UsbProtectIsApprovedOnlyEnabled(
    VOID
    )
{
    return (InterlockedCompareExchange(
                (volatile LONG *)&gUsbProtectApprovedOnlyEnabled,
                0,
                0) != 0);
}

VOID
UsbProtectSetApprovedOnlyEnabled(
    _In_ BOOLEAN Enabled
    )
{
    InterlockedExchange((volatile LONG *)&gUsbProtectApprovedOnlyEnabled,
                        Enabled ? 1 : 0);
}

NTSTATUS
UsbProtectAddApprovedDevice(
    _In_ ULONGLONG DeviceHash
    )
{
    KIRQL oldIrql;
    ULONG index;
    NTSTATUS status = STATUS_SUCCESS;

    if (DeviceHash == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireSpinLock(&gUsbProtectApprovedDevicesLock, &oldIrql);

    for (index = 0; index < gUsbProtectApprovedDeviceCount; index++) {
        if (gUsbProtectApprovedDevices[index] == DeviceHash) {
            goto Exit;
        }
    }

    if (gUsbProtectApprovedDeviceCount >= USB_PROTECTION_MAX_APPROVED_DEVICES) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    gUsbProtectApprovedDevices[gUsbProtectApprovedDeviceCount++] = DeviceHash;

Exit:
    KeReleaseSpinLock(&gUsbProtectApprovedDevicesLock, oldIrql);
    return status;
}

BOOLEAN
UsbProtectRemoveApprovedDevice(
    _In_ ULONGLONG DeviceHash
    )
{
    KIRQL oldIrql;
    ULONG index;
    BOOLEAN removed = FALSE;

    KeAcquireSpinLock(&gUsbProtectApprovedDevicesLock, &oldIrql);

    for (index = 0; index < gUsbProtectApprovedDeviceCount; index++) {
        if (gUsbProtectApprovedDevices[index] == DeviceHash) {
            gUsbProtectApprovedDeviceCount--;
            gUsbProtectApprovedDevices[index] =
                gUsbProtectApprovedDevices[gUsbProtectApprovedDeviceCount];
            gUsbProtectApprovedDevices[gUsbProtectApprovedDeviceCount] = 0;
            removed = TRUE;
            break;
        }
    }

    KeReleaseSpinLock(&gUsbProtectApprovedDevicesLock, oldIrql);
    return removed;
}

VOID
UsbProtectClearApprovedDevices(
    VOID
    )
{
    KIRQL oldIrql;

    KeAcquireSpinLock(&gUsbProtectApprovedDevicesLock, &oldIrql);
    RtlZeroMemory(gUsbProtectApprovedDevices, sizeof(gUsbProtectApprovedDevices));
    gUsbProtectApprovedDeviceCount = 0;
    KeReleaseSpinLock(&gUsbProtectApprovedDevicesLock, oldIrql);
}

BOOLEAN
UsbProtectIsDeviceApproved(
    _In_ ULONGLONG DeviceHash
    )
{
    KIRQL oldIrql;
    ULONG index;
    BOOLEAN approved = FALSE;

    if (DeviceHash == 0) {
        return FALSE;
    }

    KeAcquireSpinLock(&gUsbProtectApprovedDevicesLock, &oldIrql);
    for (index = 0; index < gUsbProtectApprovedDeviceCount; index++) {
        if (gUsbProtectApprovedDevices[index] == DeviceHash) {
            approved = TRUE;
            break;
        }
    }
    KeReleaseSpinLock(&gUsbProtectApprovedDevicesLock, oldIrql);

    return approved;
}

ULONG
UsbProtectGetApprovedDeviceCount(
    VOID
    )
{
    KIRQL oldIrql;
    ULONG count;

    KeAcquireSpinLock(&gUsbProtectApprovedDevicesLock, &oldIrql);
    count = gUsbProtectApprovedDeviceCount;
    KeReleaseSpinLock(&gUsbProtectApprovedDevicesLock, oldIrql);
    return count;
}

BOOLEAN
UsbProtectGetApprovedDeviceAt(
    _In_ ULONG Index,
    _Out_ PULONGLONG DeviceHash
    )
{
    KIRQL oldIrql;
    BOOLEAN found = FALSE;

    if (DeviceHash == NULL) {
        return FALSE;
    }

    KeAcquireSpinLock(&gUsbProtectApprovedDevicesLock, &oldIrql);
    if (Index < gUsbProtectApprovedDeviceCount) {
        *DeviceHash = gUsbProtectApprovedDevices[Index];
        found = TRUE;
    }
    KeReleaseSpinLock(&gUsbProtectApprovedDevicesLock, oldIrql);
    return found;
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(RegistryPath);

    UsbProtectSetProtectionEnabled(TRUE);
    UsbProtectSetExecutableBlockingEnabled(TRUE);
    UsbProtectSetApprovedOnlyEnabled(FALSE);
    KeInitializeSpinLock(&gUsbProtectApprovedDevicesLock);
    RtlZeroMemory(gUsbProtectApprovedDevices, sizeof(gUsbProtectApprovedDevices));
    gUsbProtectApprovedDeviceCount = 0;

    status = FltRegisterFilter(DriverObject, &gUsbProtectRegistration, &gUsbProtectFilter);
    if (!NT_SUCCESS(status)) {
        USBP_LOG("FltRegisterFilter failed: 0x%08X", status);
        return status;
    }

    USBP_LOG("Filter registered");

    status = UsbProtectCreateCommunicationPort();
    if (!NT_SUCCESS(status)) {
        USBP_LOG("FltCreateCommunicationPort failed: 0x%08X", status);
        FltUnregisterFilter(gUsbProtectFilter);
        gUsbProtectFilter = NULL;
        return status;
    }

    status = FltStartFiltering(gUsbProtectFilter);
    if (!NT_SUCCESS(status)) {
        USBP_LOG("FltStartFiltering failed: 0x%08X", status);
        UsbProtectCloseCommunicationPort();
        FltUnregisterFilter(gUsbProtectFilter);
        gUsbProtectFilter = NULL;
        return status;
    }

    USBP_LOG("Filtering started");
    USBP_LOG("Driver loaded");

    return STATUS_SUCCESS;
}

NTSTATUS
UsbProtectUnload(
    _In_ FLT_FILTER_UNLOAD_FLAGS Flags
    )
{
    UNREFERENCED_PARAMETER(Flags);

    UsbProtectCloseCommunicationPort();

    if (gUsbProtectFilter != NULL) {
        FltUnregisterFilter(gUsbProtectFilter);
        gUsbProtectFilter = NULL;
    }

    USBP_LOG("Driver unloaded");

    return STATUS_SUCCESS;
}

NTSTATUS
UsbProtectInstanceSetup(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_SETUP_FLAGS Flags,
    _In_ DEVICE_TYPE VolumeDeviceType,
    _In_ FLT_FILESYSTEM_TYPE VolumeFilesystemType
    )
{
    NTSTATUS status;
    PUSBPROTECT_INSTANCE_CONTEXT context = NULL;
    BOOLEAN isUsb = FALSE;
    BOOLEAN isRemovable = FALSE;
    ULONGLONG deviceHash = 0;

    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(VolumeDeviceType);
    UNREFERENCED_PARAMETER(VolumeFilesystemType);

    if (FltObjects == NULL || FltObjects->Instance == NULL || FltObjects->Volume == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    status = FltAllocateContext(gUsbProtectFilter,
                                FLT_INSTANCE_CONTEXT,
                                sizeof(USBPROTECT_INSTANCE_CONTEXT),
                                NonPagedPoolNx,
                                &context);
    if (!NT_SUCCESS(status)) {
        USBP_LOG("FltAllocateContext failed: 0x%08X", status);
        return status;
    }

    RtlZeroMemory(context, sizeof(USBPROTECT_INSTANCE_CONTEXT));

    status = UsbProtectQueryVolumeUsbState(FltObjects->Volume,
                                           &isUsb,
                                           &isRemovable,
                                           &deviceHash);
    if (!NT_SUCCESS(status)) {
        /*
         * Detection failure should not prevent the filter from attaching.
         * Unknown volumes are treated as non-USB so local disks remain usable.
         */
        USBP_LOG("USB detection failed, attaching as non-removable: 0x%08X", status);
        isUsb = FALSE;
        isRemovable = FALSE;
        deviceHash = 0;
    }

    context->IsUsb = isUsb;
    context->IsRemovable = isRemovable;
    context->DeviceHash = deviceHash;

    status = FltSetInstanceContext(FltObjects->Instance,
                                   FLT_SET_CONTEXT_KEEP_IF_EXISTS,
                                   context,
                                   NULL);
    FltReleaseContext(context);

    if (!NT_SUCCESS(status) && status != STATUS_FLT_CONTEXT_ALREADY_DEFINED) {
        USBP_LOG("FltSetInstanceContext failed: 0x%08X", status);
        return status;
    }

    USBP_LOG("Instance attached");

    if (isUsb || isRemovable) {
        USBP_LOG("USB/removable volume detected, hash=0x%I64X", deviceHash);
    }

    return STATUS_SUCCESS;
}

VOID
UsbProtectInstanceContextCleanup(
    _In_ PFLT_CONTEXT Context,
    _In_ FLT_CONTEXT_TYPE ContextType
    )
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(ContextType);
}
