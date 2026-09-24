#include "PolicyStore.h"

static void ReadDword(HKEY key, const wchar_t* name, DWORD* value)
{
    DWORD type = 0;
    DWORD size = sizeof(*value);
    DWORD storedValue = 0;

    if (RegQueryValueExW(key,
                         name,
                         NULL,
                         &type,
                         (LPBYTE)&storedValue,
                         &size) == ERROR_SUCCESS &&
        type == REG_DWORD && size == sizeof(storedValue)) {
        *value = storedValue != 0 ? 1 : 0;
    }
}

void UsbPolicySetDefaults(PUSBP_SAVED_POLICY policy)
{
    if (policy == NULL) {
        return;
    }

    ZeroMemory(policy, sizeof(*policy));
    policy->DataLeakProtectionEnabled = 1;
    policy->ExecutableBlockingEnabled = 1;
    policy->ApprovedOnlyEnabled = 0;
}

BOOL UsbPolicyLoad(PUSBP_SAVED_POLICY policy)
{
    HKEY key = NULL;
    LONG result;
    DWORD type = 0;
    DWORD size;

    if (policy == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    UsbPolicySetDefaults(policy);

    result = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                           USBP_POLICY_REGISTRY_PATH,
                           0,
                           KEY_QUERY_VALUE,
                           &key);
    if (result == ERROR_FILE_NOT_FOUND) {
        return TRUE;
    }
    if (result != ERROR_SUCCESS) {
        SetLastError((DWORD)result);
        return FALSE;
    }

    ReadDword(key, L"DataLeakProtection", &policy->DataLeakProtectionEnabled);
    ReadDword(key, L"ExecutableBlocking", &policy->ExecutableBlockingEnabled);
    ReadDword(key, L"ApprovedOnly", &policy->ApprovedOnlyEnabled);

    size = sizeof(policy->ApprovedDevices);
    result = RegQueryValueExW(key,
                              L"ApprovedDevices",
                              NULL,
                              &type,
                              (LPBYTE)policy->ApprovedDevices,
                              &size);
    if (result == ERROR_SUCCESS && type == REG_BINARY &&
        (size % sizeof(ULONGLONG)) == 0) {
        policy->ApprovedDeviceCount = size / sizeof(ULONGLONG);
        if (policy->ApprovedDeviceCount > USB_PROTECTION_MAX_APPROVED_DEVICES) {
            policy->ApprovedDeviceCount = USB_PROTECTION_MAX_APPROVED_DEVICES;
        }
    } else {
        ZeroMemory(policy->ApprovedDevices, sizeof(policy->ApprovedDevices));
        policy->ApprovedDeviceCount = 0;
    }

    RegCloseKey(key);
    return TRUE;
}

BOOL UsbPolicySave(const USBP_SAVED_POLICY* policy)
{
    HKEY key = NULL;
    DWORD disposition;
    LONG result;
    DWORD count;

    if (policy == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    result = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                             USBP_POLICY_REGISTRY_PATH,
                             0,
                             NULL,
                             REG_OPTION_NON_VOLATILE,
                             KEY_SET_VALUE,
                             NULL,
                             &key,
                             &disposition);
    if (result != ERROR_SUCCESS) {
        SetLastError((DWORD)result);
        return FALSE;
    }

    UNREFERENCED_PARAMETER(disposition);
    count = policy->ApprovedDeviceCount;
    if (count > USB_PROTECTION_MAX_APPROVED_DEVICES) {
        count = USB_PROTECTION_MAX_APPROVED_DEVICES;
    }

    result = RegSetValueExW(key,
                            L"DataLeakProtection",
                            0,
                            REG_DWORD,
                            (const BYTE*)&policy->DataLeakProtectionEnabled,
                            sizeof(DWORD));
    if (result == ERROR_SUCCESS) {
        result = RegSetValueExW(key,
                                L"ExecutableBlocking",
                                0,
                                REG_DWORD,
                                (const BYTE*)&policy->ExecutableBlockingEnabled,
                                sizeof(DWORD));
    }
    if (result == ERROR_SUCCESS) {
        result = RegSetValueExW(key,
                                L"ApprovedOnly",
                                0,
                                REG_DWORD,
                                (const BYTE*)&policy->ApprovedOnlyEnabled,
                                sizeof(DWORD));
    }
    if (result == ERROR_SUCCESS) {
        result = RegSetValueExW(key,
                                L"ApprovedDevices",
                                0,
                                REG_BINARY,
                                (const BYTE*)policy->ApprovedDevices,
                                count * sizeof(ULONGLONG));
    }

    RegCloseKey(key);

    if (result != ERROR_SUCCESS) {
        SetLastError((DWORD)result);
        return FALSE;
    }

    return TRUE;
}

BOOL UsbPolicyContainsDevice(const USBP_SAVED_POLICY* policy, ULONGLONG deviceHash)
{
    DWORD index;

    if (policy == NULL || deviceHash == 0) {
        return FALSE;
    }

    for (index = 0; index < policy->ApprovedDeviceCount; index++) {
        if (policy->ApprovedDevices[index] == deviceHash) {
            return TRUE;
        }
    }

    return FALSE;
}

BOOL UsbPolicyAddDevice(PUSBP_SAVED_POLICY policy, ULONGLONG deviceHash)
{
    if (policy == NULL || deviceHash == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (UsbPolicyContainsDevice(policy, deviceHash)) {
        return TRUE;
    }

    if (policy->ApprovedDeviceCount >= USB_PROTECTION_MAX_APPROVED_DEVICES) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    policy->ApprovedDevices[policy->ApprovedDeviceCount++] = deviceHash;
    return TRUE;
}

BOOL UsbPolicyRemoveDevice(PUSBP_SAVED_POLICY policy, ULONGLONG deviceHash)
{
    DWORD index;

    if (policy == NULL || deviceHash == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    for (index = 0; index < policy->ApprovedDeviceCount; index++) {
        if (policy->ApprovedDevices[index] == deviceHash) {
            policy->ApprovedDeviceCount--;
            policy->ApprovedDevices[index] =
                policy->ApprovedDevices[policy->ApprovedDeviceCount];
            policy->ApprovedDevices[policy->ApprovedDeviceCount] = 0;
            return TRUE;
        }
    }

    SetLastError(ERROR_NOT_FOUND);
    return FALSE;
}
