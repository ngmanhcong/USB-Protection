#pragma once

#include <Windows.h>

#include "../Driver/SharedProtocol.h"

#define USBP_POLICY_REGISTRY_PATH L"SOFTWARE\\UsbProtection"

typedef struct _USBP_SAVED_POLICY {
    DWORD DataLeakProtectionEnabled;
    DWORD ExecutableBlockingEnabled;
    DWORD ApprovedOnlyEnabled;
    DWORD ApprovedDeviceCount;
    ULONGLONG ApprovedDevices[USB_PROTECTION_MAX_APPROVED_DEVICES];
} USBP_SAVED_POLICY, *PUSBP_SAVED_POLICY;

void UsbPolicySetDefaults(PUSBP_SAVED_POLICY policy);
BOOL UsbPolicyLoad(PUSBP_SAVED_POLICY policy);
BOOL UsbPolicySave(const USBP_SAVED_POLICY* policy);
BOOL UsbPolicyContainsDevice(const USBP_SAVED_POLICY* policy, ULONGLONG deviceHash);
BOOL UsbPolicyAddDevice(PUSBP_SAVED_POLICY policy, ULONGLONG deviceHash);
BOOL UsbPolicyRemoveDevice(PUSBP_SAVED_POLICY policy, ULONGLONG deviceHash);
