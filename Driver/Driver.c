#include "Driver.h"
#include "Callbacks.h"
#include "Communication.h"
#include "UsbDetection.h"

#include <stdarg.h>

#pragma prefast(disable:__WARNING_ENCODE_MEMBER_FUNCTION_POINTER, "Not valid for kernel mode drivers")

PFLT_FILTER gUsbProtectFilter = NULL;
PFLT_PORT gUsbProtectServerPort = NULL;
volatile LONG gUsbProtectEnabled = 1;

CONST FLT_OPERATION_REGISTRATION gUsbProtectCallbacks[] = {
    { IRP_MJ_CREATE, 0, UsbProtectPreCreate, NULL },
    { IRP_MJ_WRITE, 0, UsbProtectPreWrite, NULL },
    { IRP_MJ_SET_INFORMATION, 0, UsbProtectPreSetInformation, NULL },
    { IRP_MJ_OPERATION_END }
};

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

VOID
UsbProtectSetProtectionEnabled(
    _In_ BOOLEAN Enabled
    )
{
    InterlockedExchange((volatile LONG *)&gUsbProtectEnabled, Enabled ? 1 : 0);
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

    status = UsbProtectQueryVolumeUsbState(FltObjects->Volume, &isUsb, &isRemovable);
    if (!NT_SUCCESS(status)) {
        /*
         * Detection failure should not prevent the filter from attaching.
         * Unknown volumes are treated as non-USB so local disks remain usable.
         */
        USBP_LOG("USB detection failed, attaching as non-removable: 0x%08X", status);
        isUsb = FALSE;
        isRemovable = FALSE;
    }

    context->IsUsb = isUsb;
    context->IsRemovable = isRemovable;

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
        USBP_LOG("USB/removable volume detected");
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
