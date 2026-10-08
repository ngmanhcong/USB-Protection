#include <ntddk.h>
#include <initguid.h>
#include <devpkey.h>

#include "HubFilter.h"

static NTSTATUS SendQueryId(_In_ PDEVICE_OBJECT Target,
                            _In_ BUS_QUERY_ID_TYPE IdType,
                            _Outptr_result_maybenull_ PWCHAR* Result);
static NTSTATUS SubmitUrbSynchronously(_In_ PDEVICE_OBJECT Target,
                                       _Inout_ PURB Urb);
static NTSTATUS GetDescriptor(_In_ PDEVICE_OBJECT Target,
                              _In_ UCHAR DescriptorType,
                              _Out_writes_bytes_(BufferLength) PVOID Buffer,
                              _In_ ULONG BufferLength,
                              _Out_opt_ PULONG Transferred);
static USBP_DEVICE_KIND ClassifyByQueryIds(_In_ PDEVICE_OBJECT Target);
static NTSTATUS InspectUsbTarget(_In_ PDEVICE_OBJECT Target,
                                 _Out_ PUSBP_DEVICE_KIND Kind);
static NTSTATUS BuildInstanceIdFromQueries(
    _In_ PDEVICE_OBJECT Target,
    _Out_writes_(OutputCount) PWCHAR Output,
    _In_ SIZE_T OutputCount);
static NTSTATUS GetCurrentInstanceId(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_writes_(OutputCount) PWCHAR Output,
    _In_ ULONG OutputCount);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, SendQueryId)
#pragma alloc_text(PAGE, SubmitUrbSynchronously)
#pragma alloc_text(PAGE, GetDescriptor)
#pragma alloc_text(PAGE, ClassifyByQueryIds)
#pragma alloc_text(PAGE, InspectUsbTarget)
#pragma alloc_text(PAGE, BuildInstanceIdFromQueries)
#pragma alloc_text(PAGE, GetCurrentInstanceId)
#pragma alloc_text(PAGE, HubInspectChildPdo)
#pragma alloc_text(PAGE, HubInspectCurrentDevice)
#endif

typedef struct _USBP_SYNC_CONTEXT {
    KEVENT Event;
} USBP_SYNC_CONTEXT, *PUSBP_SYNC_CONTEXT;

static NTSTATUS
SyncCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context
    )
{
    PUSBP_SYNC_CONTEXT syncContext = (PUSBP_SYNC_CONTEXT)Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent(&syncContext->Event, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
SendQueryId(
    _In_ PDEVICE_OBJECT Target,
    _In_ BUS_QUERY_ID_TYPE IdType,
    _Outptr_result_maybenull_ PWCHAR* Result
    )
{
    USBP_SYNC_CONTEXT syncContext;
    PIRP irp;
    PIO_STACK_LOCATION stack;
    NTSTATUS status;

    PAGED_CODE();

    *Result = NULL;
    irp = IoAllocateIrp(Target->StackSize, FALSE);
    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KeInitializeEvent(&syncContext.Event, NotificationEvent, FALSE);
    irp->RequestorMode = KernelMode;
    irp->Tail.Overlay.Thread = PsGetCurrentThread();
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    irp->IoStatus.Information = 0;
    stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = IRP_MJ_PNP;
    stack->MinorFunction = IRP_MN_QUERY_ID;
    stack->Parameters.QueryId.IdType = IdType;
    IoSetCompletionRoutine(irp,
                           SyncCompletion,
                           &syncContext,
                           TRUE,
                           TRUE,
                           TRUE);

    status = IoCallDriver(Target, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&syncContext.Event,
                              Executive,
                              KernelMode,
                              FALSE,
                              NULL);
    }

    status = irp->IoStatus.Status;
    if (NT_SUCCESS(status)) {
        *Result = (PWCHAR)irp->IoStatus.Information;
        if (*Result == NULL) {
            status = STATUS_INVALID_DEVICE_STATE;
        }
    }
    IoFreeIrp(irp);
    return status;
}

static NTSTATUS
SubmitUrbSynchronously(
    _In_ PDEVICE_OBJECT Target,
    _Inout_ PURB Urb
    )
{
    KEVENT event;
    IO_STATUS_BLOCK ioStatus;
    PIRP irp;
    PIO_STACK_LOCATION stack;
    NTSTATUS status;

    PAGED_CODE();

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    RtlZeroMemory(&ioStatus, sizeof(ioStatus));
    irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB,
                                        Target,
                                        NULL,
                                        0,
                                        NULL,
                                        0,
                                        TRUE,
                                        &event,
                                        &ioStatus);
    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    stack = IoGetNextIrpStackLocation(irp);
    stack->Parameters.Others.Argument1 = Urb;
    status = IoCallDriver(Target, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event,
                              Executive,
                              KernelMode,
                              FALSE,
                              NULL);
        status = ioStatus.Status;
    }
    return status;
}

static NTSTATUS
GetDescriptor(
    _In_ PDEVICE_OBJECT Target,
    _In_ UCHAR DescriptorType,
    _Out_writes_bytes_(BufferLength) PVOID Buffer,
    _In_ ULONG BufferLength,
    _Out_opt_ PULONG Transferred
    )
{
    struct _URB urb;
    NTSTATUS status;

    PAGED_CODE();

    RtlZeroMemory(&urb, sizeof(urb));
    UsbBuildGetDescriptorRequest(&urb,
                                 sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),
                                 DescriptorType,
                                 0,
                                 0,
                                 Buffer,
                                 NULL,
                                 BufferLength,
                                 NULL);
    status = SubmitUrbSynchronously(Target, &urb);
    if (Transferred != NULL) {
        *Transferred = NT_SUCCESS(status)
                           ? urb.UrbControlDescriptorRequest.TransferBufferLength
                           : 0;
    }
    return status;
}

static USBP_DEVICE_KIND
ClassifyConfiguration(
    _In_ const USB_DEVICE_DESCRIPTOR* DeviceDescriptor,
    _In_reads_bytes_opt_(ConfigurationLength) const UCHAR* Configuration,
    _In_ ULONG ConfigurationLength
    )
{
    BOOLEAN hasMassStorage = FALSE;
    BOOLEAN hasHid = FALSE;
    ULONG offset = 0;

    if (DeviceDescriptor->bDeviceClass == USB_DEVICE_CLASS_STORAGE) {
        return UsbpDeviceMassStorage;
    }
    if (DeviceDescriptor->bDeviceClass == USB_DEVICE_CLASS_HUB ||
        DeviceDescriptor->bDeviceClass == USB_DEVICE_CLASS_HUMAN_INTERFACE) {
        return UsbpDeviceSafeNonStorage;
    }
    if (DeviceDescriptor->bDeviceClass != 0) {
        return UsbpDeviceSafeNonStorage;
    }

    while (Configuration != NULL && offset < ConfigurationLength) {
        USB_COMMON_DESCRIPTOR commonDescriptor;
        UCHAR descriptorLength;
        UCHAR descriptorType;

        if (ConfigurationLength - offset < 2) {
            return UsbpDeviceUnknown;
        }
#pragma warning(suppress:6385) /* Remaining length is checked immediately above. */
        RtlCopyMemory(&commonDescriptor,
                      Configuration + offset,
                      sizeof(commonDescriptor));
        descriptorLength = commonDescriptor.bLength;
        descriptorType = commonDescriptor.bDescriptorType;

        if (descriptorLength < 2 || offset + descriptorLength > ConfigurationLength) {
            return UsbpDeviceUnknown;
        }

        if (descriptorType == USB_INTERFACE_DESCRIPTOR_TYPE &&
            descriptorLength >= sizeof(USB_INTERFACE_DESCRIPTOR)) {
            const USB_INTERFACE_DESCRIPTOR* interfaceDescriptor =
                (const USB_INTERFACE_DESCRIPTOR*)(Configuration + offset);

            if (interfaceDescriptor->bInterfaceClass == USB_DEVICE_CLASS_STORAGE) {
                hasMassStorage = TRUE;
            } else if (interfaceDescriptor->bInterfaceClass ==
                       USB_DEVICE_CLASS_HUMAN_INTERFACE) {
                hasHid = TRUE;
            }
        }
        offset += descriptorLength;
    }

    if (hasMassStorage && hasHid) {
        return UsbpDeviceMixedHidStorage;
    }
    if (hasMassStorage) {
        return UsbpDeviceMassStorage;
    }
    return UsbpDeviceSafeNonStorage;
}

static BOOLEAN
MultiSzContainsClass(
    _In_opt_ PCWSTR MultiString,
    _In_ PCWSTR ClassText
    )
{
    PCWSTR current;
    SIZE_T consumed = 0;

    if (MultiString == NULL) {
        return FALSE;
    }

    current = MultiString;
    while (consumed < 4096 && *current != L'\0') {
        SIZE_T length = 0;

        while (consumed + length < 4096 && current[length] != L'\0') {
            length++;
        }
        if (consumed + length >= 4096) {
            return FALSE;
        }
        if (wcsstr(current, ClassText) != NULL) {
            return TRUE;
        }
        current += length + 1;
        consumed += length + 1;
    }
    return FALSE;
}

static USBP_DEVICE_KIND
ClassifyByQueryIds(
    _In_ PDEVICE_OBJECT Target
    )
{
    PWCHAR hardwareIds = NULL;
    PWCHAR compatibleIds = NULL;
    BOOLEAN massStorage;
    BOOLEAN hid;
    BOOLEAN hub;

    PAGED_CODE();

    (VOID)SendQueryId(Target, BusQueryHardwareIDs, &hardwareIds);
    (VOID)SendQueryId(Target, BusQueryCompatibleIDs, &compatibleIds);

    massStorage = MultiSzContainsClass(hardwareIds, L"Class_08") ||
                  MultiSzContainsClass(compatibleIds, L"Class_08");
    hid = MultiSzContainsClass(hardwareIds, L"Class_03") ||
          MultiSzContainsClass(compatibleIds, L"Class_03");
    hub = MultiSzContainsClass(hardwareIds, L"Class_09") ||
          MultiSzContainsClass(compatibleIds, L"Class_09") ||
          MultiSzContainsClass(hardwareIds, L"ROOT_HUB") ||
          MultiSzContainsClass(compatibleIds, L"ROOT_HUB");

    if (hardwareIds != NULL) {
        ExFreePool(hardwareIds);
    }
    if (compatibleIds != NULL) {
        ExFreePool(compatibleIds);
    }

    if (hub || (hid && !massStorage)) {
        return UsbpDeviceSafeNonStorage;
    }
    if (massStorage && hid) {
        return UsbpDeviceMixedHidStorage;
    }
    if (massStorage) {
        return UsbpDeviceMassStorage;
    }
    return UsbpDeviceUnknown;
}

static NTSTATUS
InspectUsbTarget(
    _In_ PDEVICE_OBJECT Target,
    _Out_ PUSBP_DEVICE_KIND Kind
    )
{
    USB_DEVICE_DESCRIPTOR deviceDescriptor;
    USB_CONFIGURATION_DESCRIPTOR configurationHeader;
    PUSB_CONFIGURATION_DESCRIPTOR configuration = NULL;
    ULONG transferred = 0;
    ULONG configurationLength;
    NTSTATUS status;

    PAGED_CODE();

    *Kind = UsbpDeviceUnknown;
    RtlZeroMemory(&deviceDescriptor, sizeof(deviceDescriptor));
    status = GetDescriptor(Target,
                           USB_DEVICE_DESCRIPTOR_TYPE,
                           &deviceDescriptor,
                           sizeof(deviceDescriptor),
                           &transferred);
    if (!NT_SUCCESS(status) || transferred < sizeof(deviceDescriptor)) {
        *Kind = ClassifyByQueryIds(Target);
        return (*Kind == UsbpDeviceUnknown) ? status : STATUS_SUCCESS;
    }

    if (deviceDescriptor.bDeviceClass != 0) {
        *Kind = ClassifyConfiguration(&deviceDescriptor, NULL, 0);
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(&configurationHeader, sizeof(configurationHeader));
    status = GetDescriptor(Target,
                           USB_CONFIGURATION_DESCRIPTOR_TYPE,
                           &configurationHeader,
                           sizeof(configurationHeader),
                           &transferred);
    if (!NT_SUCCESS(status) || transferred < sizeof(configurationHeader) ||
        configurationHeader.wTotalLength < sizeof(configurationHeader)) {
        *Kind = ClassifyByQueryIds(Target);
        return (*Kind == UsbpDeviceUnknown) ? status : STATUS_SUCCESS;
    }

    configurationLength = configurationHeader.wTotalLength;
    configuration = (PUSB_CONFIGURATION_DESCRIPTOR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, configurationLength, USBP_HUB_POOL_TAG);
    if (configuration == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = GetDescriptor(Target,
                           USB_CONFIGURATION_DESCRIPTOR_TYPE,
                           configuration,
                           configurationLength,
                           &transferred);
    if (NT_SUCCESS(status) && transferred >= sizeof(*configuration)) {
        if (transferred > configurationLength) {
            transferred = configurationLength;
        }
        *Kind = ClassifyConfiguration(&deviceDescriptor,
                                      (const UCHAR*)configuration,
                                      transferred);
    } else {
        *Kind = ClassifyByQueryIds(Target);
    }

    ExFreePoolWithTag(configuration, USBP_HUB_POOL_TAG);
    return (*Kind == UsbpDeviceUnknown) ? status : STATUS_SUCCESS;
}

_Success_(return != FALSE)
static BOOLEAN
ValidIdLength(
    _In_ PCWSTR Value,
    _Out_ PSIZE_T Length
    )
{
    SIZE_T index;

    if (Length == NULL) {
        return FALSE;
    }
    *Length = 0;
    if (Value == NULL) {
        return FALSE;
    }
    for (index = 0; index < USBP_PNP_ID_MAX; index++) {
        WCHAR character = Value[index];

        if (character == L'\0') {
            *Length = index;
            return index != 0;
        }
        if (character <= 0x20 || character > 0x7f || character == L',') {
            return FALSE;
        }
    }
    return FALSE;
}

static NTSTATUS
BuildInstanceIdFromQueries(
    _In_ PDEVICE_OBJECT Target,
    _Out_writes_(OutputCount) PWCHAR Output,
    _In_ SIZE_T OutputCount
    )
{
    PWCHAR deviceId = NULL;
    PWCHAR instanceId = NULL;
    SIZE_T deviceLength;
    SIZE_T instanceLength;
    NTSTATUS status;

    PAGED_CODE();

    Output[0] = L'\0';
    status = SendQueryId(Target, BusQueryDeviceID, &deviceId);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    status = SendQueryId(Target, BusQueryInstanceID, &instanceId);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    if (deviceId == NULL || instanceId == NULL ||
        !ValidIdLength(deviceId, &deviceLength) ||
        !ValidIdLength(instanceId, &instanceLength) ||
        deviceLength + instanceLength + 2 > OutputCount) {
        status = STATUS_INVALID_DEVICE_STATE;
        goto Exit;
    }

    status = RtlStringCchPrintfW(Output,
                                 OutputCount,
                                 L"%ws\\%ws",
                                 deviceId,
                                 instanceId);

Exit:
    if (deviceId != NULL) {
        ExFreePool(deviceId);
    }
    if (instanceId != NULL) {
        ExFreePool(instanceId);
    }
    return status;
}

static NTSTATUS
GetCurrentInstanceId(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_writes_(OutputCount) PWCHAR Output,
    _In_ ULONG OutputCount
    )
{
    DEVPROPTYPE propertyType = 0;
    ULONG requiredSize = 0;
    NTSTATUS status;

    PAGED_CODE();

    Output[0] = L'\0';
    status = IoGetDevicePropertyData(PhysicalDeviceObject,
                                     &DEVPKEY_Device_InstanceId,
                                     LOCALE_NEUTRAL,
                                     0,
                                     OutputCount * sizeof(WCHAR),
                                     Output,
                                     &requiredSize,
                                     &propertyType);
    if (!NT_SUCCESS(status) || propertyType != DEVPROP_TYPE_STRING ||
        requiredSize < sizeof(WCHAR) ||
        requiredSize > OutputCount * sizeof(WCHAR)) {
        Output[0] = L'\0';
        return NT_SUCCESS(status) ? STATUS_OBJECT_TYPE_MISMATCH : status;
    }
    Output[OutputCount - 1] = L'\0';
    return STATUS_SUCCESS;
}

NTSTATUS
HubInspectChildPdo(
    _In_ PDEVICE_OBJECT ChildPdo,
    _Out_ PUSBP_DEVICE_INSPECTION Inspection
    )
{
    PDEVICE_OBJECT target;
    NTSTATUS classStatus;
    NTSTATUS identityStatus;

    PAGED_CODE();

    RtlZeroMemory(Inspection, sizeof(*Inspection));
    target = IoGetAttachedDeviceReference(ChildPdo);
    classStatus = InspectUsbTarget(target, &Inspection->Kind);
    identityStatus = GetCurrentInstanceId(
        ChildPdo,
        Inspection->InstanceId,
        RTL_NUMBER_OF(Inspection->InstanceId));
    if (!NT_SUCCESS(identityStatus)) {
        identityStatus = BuildInstanceIdFromQueries(
            target,
            Inspection->InstanceId,
            RTL_NUMBER_OF(Inspection->InstanceId));
    }
    ObDereferenceObject(target);

    if (NT_SUCCESS(identityStatus)) {
        Inspection->DeviceHash =
            UsbIdentityHashInstanceId(Inspection->InstanceId);
        Inspection->IdentityAvailable = Inspection->DeviceHash != 0;
    }
    return classStatus;
}

NTSTATUS
HubInspectCurrentDevice(
    _In_ PUSBP_HUB_DEVICE_EXTENSION Extension,
    _Out_ PUSBP_DEVICE_INSPECTION Inspection
    )
{
    NTSTATUS classStatus;
    NTSTATUS identityStatus;

    PAGED_CODE();

    RtlZeroMemory(Inspection, sizeof(*Inspection));
    classStatus = InspectUsbTarget(Extension->LowerDeviceObject,
                                   &Inspection->Kind);
    identityStatus = GetCurrentInstanceId(
        Extension->PhysicalDeviceObject,
        Inspection->InstanceId,
        RTL_NUMBER_OF(Inspection->InstanceId));
    if (NT_SUCCESS(identityStatus)) {
        Inspection->DeviceHash =
            UsbIdentityHashInstanceId(Inspection->InstanceId);
        Inspection->IdentityAvailable = Inspection->DeviceHash != 0;
    }
    return classStatus;
}
