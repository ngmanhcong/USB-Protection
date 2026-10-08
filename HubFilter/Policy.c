#include "HubFilter.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, HubPolicyUpdate)
#pragma alloc_text(PAGE, HubFilterRegistryEnabled)
#pragma alloc_text(PAGE, HubPolicyAllows)
#endif

typedef struct _USBP_HUB_POLICY_STATE {
    BOOLEAN Valid;
    BOOLEAN ApprovedOnlyEnabled;
    ULONG ApprovedDeviceCount;
    ULONGLONG ApprovedDevices[USB_PROTECTION_MAX_APPROVED_DEVICES];
} USBP_HUB_POLICY_STATE, *PUSBP_HUB_POLICY_STATE;

static EX_PUSH_LOCK gPolicyLock;
static PUSBP_HUB_POLICY_STATE gPolicy;

static NTSTATUS
QueryRegistryValue(
    _In_ HANDLE Key,
    _In_ PCWSTR Name,
    _Outptr_result_maybenull_ PKEY_VALUE_PARTIAL_INFORMATION* Information
    )
{
    UNICODE_STRING valueName;
    ULONG length = 0;
    NTSTATUS status;
    PKEY_VALUE_PARTIAL_INFORMATION information;

    *Information = NULL;
    RtlInitUnicodeString(&valueName, Name);
    status = ZwQueryValueKey(Key,
                             &valueName,
                             KeyValuePartialInformation,
                             NULL,
                             0,
                             &length);
    if (status != STATUS_BUFFER_TOO_SMALL && status != STATUS_BUFFER_OVERFLOW) {
        return status;
    }

    information = (PKEY_VALUE_PARTIAL_INFORMATION)ExAllocatePool2(
        POOL_FLAG_PAGED, length, USBP_HUB_POOL_TAG);
    if (information == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = ZwQueryValueKey(Key,
                             &valueName,
                             KeyValuePartialInformation,
                             information,
                             length,
                             &length);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(information, USBP_HUB_POOL_TAG);
        return status;
    }

    *Information = information;
    return STATUS_SUCCESS;
}

static NTSTATUS
OpenAbsoluteKey(
    _In_ PCWSTR Path,
    _Out_ PHANDLE Key
    )
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES attributes;

    RtlInitUnicodeString(&path, Path);
    InitializeObjectAttributes(&attributes,
                               &path,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);
    return ZwOpenKey(Key, KEY_QUERY_VALUE, &attributes);
}

static VOID
ReplacePolicy(
    _In_opt_ PUSBP_HUB_POLICY_STATE NewPolicy
    )
{
    PUSBP_HUB_POLICY_STATE oldPolicy;

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&gPolicyLock);
    oldPolicy = gPolicy;
    gPolicy = NewPolicy;
    ExReleasePushLockExclusive(&gPolicyLock);
    KeLeaveCriticalRegion();

    if (oldPolicy != NULL) {
        ExFreePoolWithTag(oldPolicy, USBP_HUB_POOL_TAG);
    }
}

static NTSTATUS
LoadInitialPolicy(
    VOID
    )
{
    static const WCHAR policyPath[] =
        L"\\Registry\\Machine\\SOFTWARE\\UsbProtection";
    PUSBP_HUB_POLICY_STATE policy;
    PKEY_VALUE_PARTIAL_INFORMATION approvedOnly = NULL;
    PKEY_VALUE_PARTIAL_INFORMATION approvedDevices = NULL;
    HANDLE key = NULL;
    NTSTATUS status;
    BOOLEAN listValid = FALSE;

    policy = (PUSBP_HUB_POLICY_STATE)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(*policy), USBP_HUB_POOL_TAG);
    if (policy == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    policy->ApprovedOnlyEnabled = TRUE;
    status = OpenAbsoluteKey(policyPath, &key);
    if (!NT_SUCCESS(status)) {
        ReplacePolicy(policy);
        USBP_HUB_LOG("Policy registry unavailable, fail-closed status=0x%08X",
                     status);
        return STATUS_SUCCESS;
    }

    status = QueryRegistryValue(key, L"ApprovedOnly", &approvedOnly);
    if (NT_SUCCESS(status) && approvedOnly != NULL &&
        approvedOnly->Type == REG_DWORD &&
        approvedOnly->DataLength == sizeof(ULONG)) {
        policy->ApprovedOnlyEnabled =
            (*(UNALIGNED ULONG*)approvedOnly->Data != 0) ? TRUE : FALSE;
    }

    status = QueryRegistryValue(key, L"ApprovedDevices", &approvedDevices);
    if (NT_SUCCESS(status) && approvedDevices != NULL &&
        approvedDevices->Type == REG_BINARY &&
        (approvedDevices->DataLength % sizeof(ULONGLONG)) == 0 &&
        approvedDevices->DataLength <= sizeof(policy->ApprovedDevices)) {
        policy->ApprovedDeviceCount =
            approvedDevices->DataLength / sizeof(ULONGLONG);
        if (approvedDevices->DataLength != 0) {
            RtlCopyMemory(policy->ApprovedDevices,
                          approvedDevices->Data,
                          approvedDevices->DataLength);
        }
        listValid = TRUE;
    }

    policy->Valid = (approvedOnly != NULL &&
                     (policy->ApprovedOnlyEnabled == FALSE || listValid));

    if (approvedOnly != NULL) {
        ExFreePoolWithTag(approvedOnly, USBP_HUB_POOL_TAG);
    }
    if (approvedDevices != NULL) {
        ExFreePoolWithTag(approvedDevices, USBP_HUB_POOL_TAG);
    }
    ZwClose(key);

    USBP_HUB_LOG("Initial policy valid=%u approvedOnly=%u count=%lu",
                 policy->Valid,
                 policy->ApprovedOnlyEnabled,
                 policy->ApprovedDeviceCount);
    ReplacePolicy(policy);
    return STATUS_SUCCESS;
}

NTSTATUS
HubPolicyInitialize(
    VOID
    )
{
    ExInitializePushLock(&gPolicyLock);
    gPolicy = NULL;
    return LoadInitialPolicy();
}

VOID
HubPolicyUninitialize(
    VOID
    )
{
    ReplacePolicy(NULL);
}

NTSTATUS
HubPolicyUpdate(
    _In_ const USB_PROTECTION_HUB_POLICY_MESSAGE* Message,
    _In_ ULONG InputLength
    )
{
    PUSBP_HUB_POLICY_STATE policy;
    ULONG count;

    PAGED_CODE();

    if (Message == NULL || InputLength < sizeof(*Message) ||
        Message->Command != (USBP_UINT32)UsbProtectionSetHubFilterPolicy ||
        Message->Version != USB_PROTECTION_HUB_POLICY_VERSION ||
        Message->ApprovedDeviceCount > USB_PROTECTION_MAX_APPROVED_DEVICES) {
        return STATUS_INVALID_PARAMETER;
    }

    policy = (PUSBP_HUB_POLICY_STATE)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(*policy), USBP_HUB_POOL_TAG);
    if (policy == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    policy->Valid = TRUE;
    policy->ApprovedOnlyEnabled =
        Message->ApprovedOnlyEnabled != 0 ? TRUE : FALSE;
    count = Message->ApprovedDeviceCount;
    policy->ApprovedDeviceCount = count;
    if (count != 0) {
        RtlCopyMemory(policy->ApprovedDevices,
                      Message->ApprovedDevices,
                      count * sizeof(ULONGLONG));
    }

    USBP_HUB_LOG("Service policy received approvedOnly=%u count=%lu",
                 policy->ApprovedOnlyEnabled,
                 policy->ApprovedDeviceCount);
    ReplacePolicy(policy);
    return STATUS_SUCCESS;
}

BOOLEAN
HubFilterRegistryEnabled(
    VOID
    )
{
    static const WCHAR parametersPath[] =
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\"
        L"UsbProtectionHubFilter\\Parameters";
    PKEY_VALUE_PARTIAL_INFORMATION value = NULL;
    HANDLE key = NULL;
    NTSTATUS status;
    BOOLEAN enabled = FALSE;

    PAGED_CODE();

    status = OpenAbsoluteKey(parametersPath, &key);
    if (NT_SUCCESS(status)) {
        status = QueryRegistryValue(key, L"HubFilterEnabled", &value);
        if (NT_SUCCESS(status) && value != NULL &&
            value->Type == REG_DWORD &&
            value->DataLength == sizeof(ULONG)) {
            enabled = (*(UNALIGNED ULONG*)value->Data != 0) ? TRUE : FALSE;
        }
        ZwClose(key);
    }

    if (value != NULL) {
        ExFreePoolWithTag(value, USBP_HUB_POOL_TAG);
    }
    return enabled;
}

BOOLEAN
HubPolicyAllows(
    _In_ ULONGLONG DeviceHash,
    _In_ BOOLEAN IdentityAvailable,
    _Out_ PCSTR* Reason
    )
{
    BOOLEAN allowed = FALSE;
    ULONG index;

    PAGED_CODE();

    *Reason = "policy unavailable (fail-closed)";
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&gPolicyLock);

    if (gPolicy == NULL || !gPolicy->Valid) {
        allowed = FALSE;
    } else if (!gPolicy->ApprovedOnlyEnabled) {
        *Reason = "approved-only disabled";
        allowed = TRUE;
    } else if (!IdentityAvailable || DeviceHash == 0) {
        *Reason = "identity unavailable (fail-closed)";
        allowed = FALSE;
    } else {
        *Reason = "hash absent from approved list";
        for (index = 0; index < gPolicy->ApprovedDeviceCount; index++) {
            if (gPolicy->ApprovedDevices[index] == DeviceHash) {
                *Reason = "approved hash";
                allowed = TRUE;
                break;
            }
        }
    }

    ExReleasePushLockShared(&gPolicyLock);
    KeLeaveCriticalRegion();
    return allowed;
}
