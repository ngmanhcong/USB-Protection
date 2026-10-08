#pragma once

#include <ntddk.h>
#include <ntstrsafe.h>
#include <usb.h>
#include <usbioctl.h>
#include <usbdlib.h>

#include "../Driver/SharedProtocol.h"
#include "../Common/UsbIdentityHash.h"

#define USBP_HUB_POOL_TAG 'HbsU'
#define USBP_HUB_RELATIONS_TAG 'RbsU'
#define USBP_HUB_ID_MAX 512
#define USBP_PNP_ID_MAX 200

#define USBP_HUB_LOG(_format_, ...)                                           \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,                        \
               "[UsbProtectionHub] " _format_ "\n", __VA_ARGS__)
#define USBP_HUB_LOG0(_message_)                                              \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,                        \
               "[UsbProtectionHub] " _message_ "\n")

typedef enum _USBP_DEVICE_KIND {
    UsbpDeviceUnknown = 0,
    UsbpDeviceSafeNonStorage,
    UsbpDeviceMassStorage,
    UsbpDeviceMixedHidStorage
} USBP_DEVICE_KIND, *PUSBP_DEVICE_KIND;

typedef struct _USBP_DEVICE_INSPECTION {
    USBP_DEVICE_KIND Kind;
    WCHAR InstanceId[USBP_HUB_ID_MAX];
    ULONGLONG DeviceHash;
    BOOLEAN IdentityAvailable;
} USBP_DEVICE_INSPECTION, *PUSBP_DEVICE_INSPECTION;

typedef struct _USBP_HUB_DEVICE_EXTENSION {
    PDEVICE_OBJECT Self;
    PDEVICE_OBJECT LowerDeviceObject;
    PDEVICE_OBJECT PhysicalDeviceObject;
    IO_REMOVE_LOCK RemoveLock;
    volatile LONG BlockedMassStorage;
} USBP_HUB_DEVICE_EXTENSION, *PUSBP_HUB_DEVICE_EXTENSION;

typedef struct _USBP_RELATIONS_WORK {
    PIO_WORKITEM WorkItem;
    PDEVICE_OBJECT FilterDeviceObject;
    PIRP Irp;
} USBP_RELATIONS_WORK, *PUSBP_RELATIONS_WORK;

extern PDEVICE_OBJECT gUsbHubControlDevice;

DRIVER_INITIALIZE DriverEntry;
DRIVER_ADD_DEVICE UsbHubAddDevice;
DRIVER_UNLOAD UsbHubUnload;

_Dispatch_type_(IRP_MJ_CREATE)
_Dispatch_type_(IRP_MJ_CLOSE)
_Dispatch_type_(IRP_MJ_CLEANUP)
DRIVER_DISPATCH UsbHubDispatchCreateClose;
_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
DRIVER_DISPATCH UsbHubDispatchDeviceControl;
_Dispatch_type_(IRP_MJ_PNP)
DRIVER_DISPATCH UsbHubDispatchPnp;
_Dispatch_type_(IRP_MJ_POWER)
DRIVER_DISPATCH UsbHubDispatchPower;
DRIVER_DISPATCH UsbHubDispatchPassThrough;

NTSTATUS HubPolicyInitialize(VOID);
VOID HubPolicyUninitialize(VOID);
NTSTATUS HubPolicyUpdate(_In_ const USB_PROTECTION_HUB_POLICY_MESSAGE* Message,
                         _In_ ULONG InputLength);
BOOLEAN HubFilterRegistryEnabled(VOID);
BOOLEAN HubPolicyAllows(_In_ ULONGLONG DeviceHash,
                        _In_ BOOLEAN IdentityAvailable,
                        _Out_ PCSTR* Reason);

NTSTATUS HubInspectChildPdo(_In_ PDEVICE_OBJECT ChildPdo,
                            _Out_ PUSBP_DEVICE_INSPECTION Inspection);
NTSTATUS HubInspectCurrentDevice(
    _In_ PUSBP_HUB_DEVICE_EXTENSION Extension,
    _Out_ PUSBP_DEVICE_INSPECTION Inspection);

VOID HubFilterRelationsWorker(_In_ PDEVICE_OBJECT DeviceObject,
                              _In_ PVOID Context);

