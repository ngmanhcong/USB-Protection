#include "HubFilter.h"

static NTSTATUS HandleStartDevice(
    _In_ PUSBP_HUB_DEVICE_EXTENSION Extension,
    _Inout_ PIRP Irp);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, HandleStartDevice)
#pragma alloc_text(PAGE, HubFilterRelationsWorker)
#endif

static NTSTATUS
ReleaseRemoveLockCompletion(
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

static NTSTATUS
BlockedStateCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context
    )
{
    PUSBP_HUB_DEVICE_EXTENSION extension =
        (PUSBP_HUB_DEVICE_EXTENSION)Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    if (NT_SUCCESS(Irp->IoStatus.Status) &&
        InterlockedCompareExchange(&extension->BlockedMassStorage, 0, 0) != 0) {
        Irp->IoStatus.Information |= PNP_DEVICE_FAILED;
    }
    if (Irp->PendingReturned) {
        IoMarkIrpPending(Irp);
    }
    IoReleaseRemoveLock(&extension->RemoveLock, Irp);
    return STATUS_CONTINUE_COMPLETION;
}

static NTSTATUS
RelationsCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context
    )
{
    PUSBP_RELATIONS_WORK work = (PUSBP_RELATIONS_WORK)Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    IoQueueWorkItem(work->WorkItem,
                    HubFilterRelationsWorker,
                    DelayedWorkQueue,
                    work);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
ForwardWithRemoveLockCompletion(
    _In_ PUSBP_HUB_DEVICE_EXTENSION Extension,
    _Inout_ PIRP Irp,
    _In_ PIO_COMPLETION_ROUTINE Completion
    )
{
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           Completion,
                           Extension,
                           TRUE,
                           TRUE,
                           TRUE);
    return IoCallDriver(Extension->LowerDeviceObject, Irp);
}

static NTSTATUS
CompleteBlockedStart(
    _In_ PUSBP_HUB_DEVICE_EXTENSION Extension,
    _Inout_ PIRP Irp,
    _In_ const USBP_DEVICE_INSPECTION* Inspection,
    _In_ PCSTR Reason
    )
{
    InterlockedExchange(&Extension->BlockedMassStorage, 1);
    USBP_HUB_LOG("START blocked id=%ws hash=%016I64X reason=%s",
                 Inspection->IdentityAvailable ? Inspection->InstanceId : L"<unknown>",
                 Inspection->DeviceHash,
                 Reason);
    Irp->IoStatus.Status = STATUS_ACCESS_DENIED;
    Irp->IoStatus.Information = 0;
    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_ACCESS_DENIED;
}

static NTSTATUS
HandleStartDevice(
    _In_ PUSBP_HUB_DEVICE_EXTENSION Extension,
    _Inout_ PIRP Irp
    )
{
    USBP_DEVICE_INSPECTION inspection;
    PCSTR reason;
    NTSTATUS inspectStatus;

    PAGED_CODE();

    InterlockedExchange(&Extension->BlockedMassStorage, 0);
    if (!HubFilterRegistryEnabled()) {
        return ForwardWithRemoveLockCompletion(Extension,
                                               Irp,
                                               ReleaseRemoveLockCompletion);
    }

    inspectStatus = HubInspectCurrentDevice(Extension, &inspection);
    if (!NT_SUCCESS(inspectStatus) || inspection.Kind == UsbpDeviceUnknown) {
        USBP_HUB_LOG("START pass-through: device class unknown status=0x%08X pdo=%p",
                     inspectStatus,
                     Extension->PhysicalDeviceObject);
        return ForwardWithRemoveLockCompletion(Extension,
                                               Irp,
                                               ReleaseRemoveLockCompletion);
    }
    if (inspection.Kind == UsbpDeviceSafeNonStorage ||
        inspection.Kind == UsbpDeviceMixedHidStorage) {
        if (inspection.Kind == UsbpDeviceMixedHidStorage) {
            USBP_HUB_LOG("START allow mixed HID/storage parent; child interfaces will be evaluated id=%ws",
                         inspection.IdentityAvailable ? inspection.InstanceId : L"<unknown>");
        }
        return ForwardWithRemoveLockCompletion(Extension,
                                               Irp,
                                               ReleaseRemoveLockCompletion);
    }

    if (!HubPolicyAllows(inspection.DeviceHash,
                         inspection.IdentityAvailable,
                         &reason)) {
        return CompleteBlockedStart(Extension, Irp, &inspection, reason);
    }

    USBP_HUB_LOG("START allow mass-storage id=%ws hash=%016I64X reason=%s",
                 inspection.InstanceId,
                 inspection.DeviceHash,
                 reason);
    return ForwardWithRemoveLockCompletion(Extension,
                                           Irp,
                                           ReleaseRemoveLockCompletion);
}

VOID
HubFilterRelationsWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PVOID Context
    )
{
    PUSBP_RELATIONS_WORK work = (PUSBP_RELATIONS_WORK)Context;
    PUSBP_HUB_DEVICE_EXTENSION extension;
    PDEVICE_RELATIONS relations;
    ULONG index;

    UNREFERENCED_PARAMETER(DeviceObject);
    PAGED_CODE();

    extension = (PUSBP_HUB_DEVICE_EXTENSION)
        work->FilterDeviceObject->DeviceExtension;
    relations = (PDEVICE_RELATIONS)work->Irp->IoStatus.Information;

    if (NT_SUCCESS(work->Irp->IoStatus.Status) && relations != NULL &&
        HubFilterRegistryEnabled()) {
        index = 0;
        while (index < relations->Count) {
            USBP_DEVICE_INSPECTION inspection;
            NTSTATUS inspectStatus;
            PCSTR reason;
            BOOLEAN remove = FALSE;

            inspectStatus = HubInspectChildPdo(relations->Objects[index],
                                               &inspection);
            if (!NT_SUCCESS(inspectStatus) ||
                inspection.Kind == UsbpDeviceUnknown) {
                USBP_HUB_LOG("BusRelations allow unknown class pdo=%p status=0x%08X",
                             relations->Objects[index],
                             inspectStatus);
            } else if (inspection.Kind == UsbpDeviceMixedHidStorage) {
                USBP_HUB_LOG("BusRelations allow mixed HID/storage parent id=%ws; awaiting interface relations",
                             inspection.IdentityAvailable
                                 ? inspection.InstanceId
                                 : L"<unknown>");
            } else if (inspection.Kind == UsbpDeviceMassStorage) {
                remove = !HubPolicyAllows(inspection.DeviceHash,
                                          inspection.IdentityAvailable,
                                          &reason);
                USBP_HUB_LOG("BusRelations %s id=%ws hash=%016I64X reason=%s",
                             remove ? "block" : "allow",
                             inspection.IdentityAvailable
                                 ? inspection.InstanceId
                                 : L"<unknown>",
                             inspection.DeviceHash,
                             reason);
            }

            if (remove) {
                ULONG moveCount = relations->Count - index - 1;

                ObDereferenceObject(relations->Objects[index]);
                if (moveCount != 0) {
                    RtlMoveMemory(&relations->Objects[index],
                                  &relations->Objects[index + 1],
                                  moveCount * sizeof(relations->Objects[0]));
                }
                relations->Count--;
                continue;
            }
            index++;
        }
    } else if (relations != NULL) {
        USBP_HUB_LOG0("BusRelations pass-through: emergency switch is off");
    }

    IoReleaseRemoveLock(&extension->RemoveLock, work->Irp);
    IoFreeWorkItem(work->WorkItem);
    IoCompleteRequest(work->Irp, IO_NO_INCREMENT);
    ExFreePoolWithTag(work, USBP_HUB_RELATIONS_TAG);
}

NTSTATUS
UsbHubDispatchPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    PUSBP_HUB_DEVICE_EXTENSION extension;
    PIO_STACK_LOCATION stack;
    NTSTATUS status;

    if (DeviceObject == gUsbHubControlDevice) {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    extension = (PUSBP_HUB_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    status = IoAcquireRemoveLock(&extension->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    stack = IoGetCurrentIrpStackLocation(Irp);
    switch (stack->MinorFunction) {
    case IRP_MN_START_DEVICE:
        return HandleStartDevice(extension, Irp);

    case IRP_MN_QUERY_PNP_DEVICE_STATE:
        return ForwardWithRemoveLockCompletion(extension,
                                               Irp,
                                               BlockedStateCompletion);

    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (stack->Parameters.QueryDeviceRelations.Type == BusRelations) {
            PUSBP_RELATIONS_WORK work;

            work = (PUSBP_RELATIONS_WORK)ExAllocatePool2(
                POOL_FLAG_NON_PAGED,
                sizeof(*work),
                USBP_HUB_RELATIONS_TAG);
            if (work != NULL) {
                work->WorkItem = IoAllocateWorkItem(DeviceObject);
                if (work->WorkItem != NULL) {
                    work->FilterDeviceObject = DeviceObject;
                    work->Irp = Irp;
                    IoMarkIrpPending(Irp);
                    IoCopyCurrentIrpStackLocationToNext(Irp);
                    IoSetCompletionRoutine(Irp,
                                           RelationsCompletion,
                                           work,
                                           TRUE,
                                           TRUE,
                                           TRUE);
                    (VOID)IoCallDriver(extension->LowerDeviceObject, Irp);
                    return STATUS_PENDING;
                }
                ExFreePoolWithTag(work, USBP_HUB_RELATIONS_TAG);
            }
        }
        return ForwardWithRemoveLockCompletion(extension,
                                               Irp,
                                               ReleaseRemoveLockCompletion);

    case IRP_MN_SURPRISE_REMOVAL:
        USBP_HUB_LOG("Surprise removal filter=%p pdo=%p",
                     DeviceObject,
                     extension->PhysicalDeviceObject);
        return ForwardWithRemoveLockCompletion(extension,
                                               Irp,
                                               ReleaseRemoveLockCompletion);

    case IRP_MN_REMOVE_DEVICE:
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(extension->LowerDeviceObject, Irp);
        IoReleaseRemoveLockAndWait(&extension->RemoveLock, Irp);
        IoDetachDevice(extension->LowerDeviceObject);
        USBP_HUB_LOG("Detached filter=%p pdo=%p",
                     DeviceObject,
                     extension->PhysicalDeviceObject);
        IoDeleteDevice(DeviceObject);
        return status;

    default:
        return ForwardWithRemoveLockCompletion(extension,
                                               Irp,
                                               ReleaseRemoveLockCompletion);
    }
}

