#include "HubFilter.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, UsbHubAddDevice)
#pragma alloc_text(PAGE, UsbHubUnload)
#endif

PDEVICE_OBJECT gUsbHubControlDevice = NULL;

static NTSTATUS
CompleteIrp(
    _Inout_ PIRP Irp,
    _In_ NTSTATUS Status,
    _In_ ULONG_PTR Information
    )
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS
PassThroughCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context
    )
{
    PUSBP_HUB_DEVICE_EXTENSION extension =
        (PUSBP_HUB_DEVICE_EXTENSION)Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    if (Irp->PendingReturned) {
        IoMarkIrpPending(Irp);
    }
    IoReleaseRemoveLock(&extension->RemoveLock, Irp);
    return STATUS_CONTINUE_COMPLETION;
}

NTSTATUS
UsbHubDispatchCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    if (DeviceObject != gUsbHubControlDevice) {
        return UsbHubDispatchPassThrough(DeviceObject, Irp);
    }
    return CompleteIrp(Irp, STATUS_SUCCESS, 0);
}

NTSTATUS
UsbHubDispatchDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack;
    NTSTATUS status;

    if (DeviceObject != gUsbHubControlDevice) {
        return UsbHubDispatchPassThrough(DeviceObject, Irp);
    }

    stack = IoGetCurrentIrpStackLocation(Irp);
    if (stack->Parameters.DeviceIoControl.IoControlCode !=
        IOCTL_USB_PROTECTION_SET_HUB_POLICY) {
        return CompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }

    status = HubPolicyUpdate(
        (const USB_PROTECTION_HUB_POLICY_MESSAGE*)Irp->AssociatedIrp.SystemBuffer,
        stack->Parameters.DeviceIoControl.InputBufferLength);
    return CompleteIrp(Irp, status, 0);
}

NTSTATUS
UsbHubDispatchPassThrough(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    PUSBP_HUB_DEVICE_EXTENSION extension;
    NTSTATUS status;

    if (DeviceObject == gUsbHubControlDevice) {
        return CompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }

    extension = (PUSBP_HUB_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    status = IoAcquireRemoveLock(&extension->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {
        return CompleteIrp(Irp, status, 0);
    }

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           PassThroughCompletion,
                           extension,
                           TRUE,
                           TRUE,
                           TRUE);
    return IoCallDriver(extension->LowerDeviceObject, Irp);
}

NTSTATUS
UsbHubDispatchPower(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    PUSBP_HUB_DEVICE_EXTENSION extension;
    NTSTATUS status;

    if (DeviceObject == gUsbHubControlDevice) {
        return CompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }

    extension = (PUSBP_HUB_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    status = IoAcquireRemoveLock(&extension->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {
        return CompleteIrp(Irp, status, 0);
    }

    PoStartNextPowerIrp(Irp);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           PassThroughCompletion,
                           extension,
                           TRUE,
                           TRUE,
                           TRUE);
    return PoCallDriver(extension->LowerDeviceObject, Irp);
}

NTSTATUS
UsbHubAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject
    )
{
    PDEVICE_OBJECT filterDeviceObject = NULL;
    PUSBP_HUB_DEVICE_EXTENSION extension;
    NTSTATUS status;

    PAGED_CODE();

    status = IoCreateDevice(DriverObject,
                            sizeof(USBP_HUB_DEVICE_EXTENSION),
                            NULL,
                            FILE_DEVICE_UNKNOWN,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &filterDeviceObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    extension = (PUSBP_HUB_DEVICE_EXTENSION)filterDeviceObject->DeviceExtension;
    RtlZeroMemory(extension, sizeof(*extension));
    extension->Self = filterDeviceObject;
    extension->PhysicalDeviceObject = PhysicalDeviceObject;
    IoInitializeRemoveLock(&extension->RemoveLock,
                           USBP_HUB_POOL_TAG,
                           0,
                           0);

    extension->LowerDeviceObject =
        IoAttachDeviceToDeviceStack(filterDeviceObject, PhysicalDeviceObject);
    if (extension->LowerDeviceObject == NULL) {
        IoDeleteDevice(filterDeviceObject);
        return STATUS_NO_SUCH_DEVICE;
    }

    filterDeviceObject->Flags |=
        extension->LowerDeviceObject->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO);
    filterDeviceObject->DeviceType = extension->LowerDeviceObject->DeviceType;
    filterDeviceObject->Characteristics =
        extension->LowerDeviceObject->Characteristics;
    filterDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    USBP_HUB_LOG("Attached filter=%p pdo=%p lower=%p",
                 filterDeviceObject,
                 PhysicalDeviceObject,
                 extension->LowerDeviceObject);
    return STATUS_SUCCESS;
}

VOID
UsbHubUnload(
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    UNREFERENCED_PARAMETER(DriverObject);
    PAGED_CODE();

    HubPolicyUninitialize();
    if (gUsbHubControlDevice != NULL) {
        IoDeleteDevice(gUsbHubControlDevice);
        gUsbHubControlDevice = NULL;
    }
    USBP_HUB_LOG0("Driver unloaded");
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    UNICODE_STRING controlName;
    ULONG major;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(RegistryPath);

#pragma warning(push)
#pragma warning(disable:28168) /* One generic pass-through is intentional. */
    for (major = 0; major <= IRP_MJ_MAXIMUM_FUNCTION; major++) {
        DriverObject->MajorFunction[major] = UsbHubDispatchPassThrough;
    }
#pragma warning(pop)
    DriverObject->MajorFunction[IRP_MJ_CREATE] = UsbHubDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = UsbHubDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = UsbHubDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] =
        UsbHubDispatchDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_PNP] = UsbHubDispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = UsbHubDispatchPower;
    DriverObject->DriverExtension->AddDevice = UsbHubAddDevice;
    DriverObject->DriverUnload = UsbHubUnload;

    status = HubPolicyInitialize();
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlInitUnicodeString(&controlName, USB_PROTECTION_HUBFILTER_NT_NAME);
    status = IoCreateDevice(DriverObject,
                            0,
                            &controlName,
                            FILE_DEVICE_UNKNOWN,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &gUsbHubControlDevice);
    if (!NT_SUCCESS(status)) {
        HubPolicyUninitialize();
        return status;
    }
    gUsbHubControlDevice->Flags |= DO_BUFFERED_IO;
    gUsbHubControlDevice->Flags &= ~DO_DEVICE_INITIALIZING;

    USBP_HUB_LOG0("Driver loaded; emergency switch is registry controlled");
    return STATUS_SUCCESS;
}

