#include "Callbacks.h"

static
BOOLEAN
UsbProtectGetTargetState(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Out_ PBOOLEAN ProtectedTarget,
    _Out_ PULONGLONG DeviceHash
    )
{
    NTSTATUS status;
    PUSBPROTECT_INSTANCE_CONTEXT context = NULL;

    if (ProtectedTarget == NULL || DeviceHash == NULL) {
        return FALSE;
    }

    *ProtectedTarget = FALSE;
    *DeviceHash = 0;

    if (FltObjects == NULL || FltObjects->Instance == NULL) {
        return FALSE;
    }

    status = FltGetInstanceContext(FltObjects->Instance, &context);
    if (!NT_SUCCESS(status)) {
        return FALSE;
    }

    *ProtectedTarget = (context->IsUsb || context->IsRemovable);
    *DeviceHash = context->DeviceHash;
    FltReleaseContext(context);

    return TRUE;
}

static
BOOLEAN
UsbProtectIsProtectedTarget(
    _In_ PCFLT_RELATED_OBJECTS FltObjects
    )
{
    BOOLEAN protectedTarget;
    ULONGLONG deviceHash;

    if (!UsbProtectGetTargetState(FltObjects, &protectedTarget, &deviceHash)) {
        return FALSE;
    }

    UNREFERENCED_PARAMETER(deviceHash);
    return protectedTarget;
}

static
BOOLEAN
UsbProtectShouldBlockUnapprovedDevice(
    _In_ PCFLT_RELATED_OBJECTS FltObjects
    )
{
    BOOLEAN protectedTarget;
    ULONGLONG deviceHash;

    if (!UsbProtectIsApprovedOnlyEnabled()) {
        return FALSE;
    }

    if (!UsbProtectGetTargetState(FltObjects, &protectedTarget, &deviceHash) ||
        !protectedTarget) {
        return FALSE;
    }

    return !UsbProtectIsDeviceApproved(deviceHash);
}

static
BOOLEAN
UsbProtectIsDirectVolumeOpen(
    _In_ PCFLT_RELATED_OBJECTS FltObjects
    )
{
    if (FltObjects == NULL || FltObjects->FileObject == NULL) {
        return TRUE;
    }

    if (FltObjects->FileObject->FileName.Length == 0) {
        return TRUE;
    }

    return (FltObjects->FileObject->RelatedFileObject == NULL &&
            FltObjects->FileObject->FileName.Length == sizeof(WCHAR) &&
            FltObjects->FileObject->FileName.Buffer != NULL &&
            FltObjects->FileObject->FileName.Buffer[0] == L'\\');
}

static
BOOLEAN
UsbProtectHasSuffix(
    _In_ PCUNICODE_STRING FileName,
    _In_ PCUNICODE_STRING Suffix
    )
{
    if (FileName == NULL || FileName->Buffer == NULL ||
        FileName->Length < Suffix->Length) {
        return FALSE;
    }

    return RtlSuffixUnicodeString(Suffix, FileName, TRUE);
}

static
BOOLEAN
UsbProtectIsScriptExtension(
    _In_ PCUNICODE_STRING FileName
    )
{
    UNICODE_STRING bat = RTL_CONSTANT_STRING(L".BAT");
    UNICODE_STRING cmd = RTL_CONSTANT_STRING(L".CMD");
    UNICODE_STRING ps1 = RTL_CONSTANT_STRING(L".PS1");
    UNICODE_STRING psm1 = RTL_CONSTANT_STRING(L".PSM1");
    UNICODE_STRING vbs = RTL_CONSTANT_STRING(L".VBS");
    UNICODE_STRING vbe = RTL_CONSTANT_STRING(L".VBE");
    UNICODE_STRING js = RTL_CONSTANT_STRING(L".JS");
    UNICODE_STRING jse = RTL_CONSTANT_STRING(L".JSE");
    UNICODE_STRING wsf = RTL_CONSTANT_STRING(L".WSF");
    UNICODE_STRING wsh = RTL_CONSTANT_STRING(L".WSH");
    UNICODE_STRING hta = RTL_CONSTANT_STRING(L".HTA");

    return UsbProtectHasSuffix(FileName, &bat) ||
           UsbProtectHasSuffix(FileName, &cmd) ||
           UsbProtectHasSuffix(FileName, &ps1) ||
           UsbProtectHasSuffix(FileName, &psm1) ||
           UsbProtectHasSuffix(FileName, &vbs) ||
           UsbProtectHasSuffix(FileName, &vbe) ||
           UsbProtectHasSuffix(FileName, &js) ||
           UsbProtectHasSuffix(FileName, &jse) ||
           UsbProtectHasSuffix(FileName, &wsf) ||
           UsbProtectHasSuffix(FileName, &wsh) ||
           UsbProtectHasSuffix(FileName, &hta);
}

static
BOOLEAN
UsbProtectIsNativeExecutableExtension(
    _In_ PCUNICODE_STRING FileName
    )
{
    UNICODE_STRING exe = RTL_CONSTANT_STRING(L".EXE");
    UNICODE_STRING com = RTL_CONSTANT_STRING(L".COM");
    UNICODE_STRING scr = RTL_CONSTANT_STRING(L".SCR");
    UNICODE_STRING cpl = RTL_CONSTANT_STRING(L".CPL");
    UNICODE_STRING dll = RTL_CONSTANT_STRING(L".DLL");
    UNICODE_STRING sys = RTL_CONSTANT_STRING(L".SYS");
    UNICODE_STRING msi = RTL_CONSTANT_STRING(L".MSI");
    UNICODE_STRING msp = RTL_CONSTANT_STRING(L".MSP");

    return UsbProtectHasSuffix(FileName, &exe) ||
           UsbProtectHasSuffix(FileName, &com) ||
           UsbProtectHasSuffix(FileName, &scr) ||
           UsbProtectHasSuffix(FileName, &cpl) ||
           UsbProtectHasSuffix(FileName, &dll) ||
           UsbProtectHasSuffix(FileName, &sys) ||
           UsbProtectHasSuffix(FileName, &msi) ||
           UsbProtectHasSuffix(FileName, &msp);
}

static
BOOLEAN
UsbProtectCreateRequestsExecute(
    _In_ PFLT_CALLBACK_DATA Data
    )
{
    ACCESS_MASK desiredAccess;

    if (Data->Iopb->Parameters.Create.SecurityContext == NULL) {
        return FALSE;
    }

    desiredAccess = Data->Iopb->Parameters.Create.SecurityContext->DesiredAccess;
    return ((desiredAccess & (FILE_EXECUTE | GENERIC_EXECUTE | GENERIC_ALL)) != 0);
}

static
BOOLEAN
UsbProtectCreateMayModifyFile(
    _In_ PFLT_CALLBACK_DATA Data
    )
{
    ACCESS_MASK desiredAccess;
    ULONG options;
    ULONG createDisposition;
    ULONG createOptions;

    if (Data->Iopb->Parameters.Create.SecurityContext == NULL) {
        return FALSE;
    }

    desiredAccess = Data->Iopb->Parameters.Create.SecurityContext->DesiredAccess;
    options = Data->Iopb->Parameters.Create.Options;
    createDisposition = (options >> 24) & 0x000000FF;
    createOptions = options & 0x00FFFFFF;

    if ((desiredAccess & (FILE_WRITE_DATA |
                          FILE_APPEND_DATA |
                          FILE_WRITE_EA |
                          FILE_WRITE_ATTRIBUTES |
                          DELETE |
                          WRITE_DAC |
                          WRITE_OWNER |
                          GENERIC_WRITE |
                          GENERIC_ALL)) != 0) {
        return TRUE;
    }

    if ((createOptions & FILE_DELETE_ON_CLOSE) != 0) {
        return TRUE;
    }

    switch (createDisposition) {
    case FILE_SUPERSEDE:
    case FILE_CREATE:
    case FILE_OPEN_IF:
    case FILE_OVERWRITE:
    case FILE_OVERWRITE_IF:
        return TRUE;

    default:
        break;
    }

    return FALSE;
}

static
BOOLEAN
UsbProtectSetInformationMayModifyFile(
    _In_ PFLT_CALLBACK_DATA Data
    )
{
    FILE_INFORMATION_CLASS informationClass;
    PVOID informationBuffer;

    informationClass = Data->Iopb->Parameters.SetFileInformation.FileInformationClass;
    informationBuffer = Data->Iopb->Parameters.SetFileInformation.InfoBuffer;

    switch (informationClass) {
    case FileBasicInformation:
    case FileAllocationInformation:
    case FileEndOfFileInformation:
    case FileRenameInformation:
    case FileLinkInformation:
    case FileShortNameInformation:
    case FileValidDataLengthInformation:
    case FileRenameInformationEx:
    case FileLinkInformationEx:
        return TRUE;

    case FileDispositionInformation:
        if (informationBuffer != NULL) {
            PFILE_DISPOSITION_INFORMATION disposition;

            disposition = (PFILE_DISPOSITION_INFORMATION)informationBuffer;
            return disposition->DeleteFile ? TRUE : FALSE;
        }
        return TRUE;

    case FileDispositionInformationEx:
        if (informationBuffer != NULL) {
            PFILE_DISPOSITION_INFORMATION_EX dispositionEx;

            dispositionEx = (PFILE_DISPOSITION_INFORMATION_EX)informationBuffer;
            return ((dispositionEx->Flags & FILE_DISPOSITION_DELETE) != 0) ? TRUE : FALSE;
        }
        return TRUE;

    default:
        break;
    }

    return FALSE;
}

static
FLT_PREOP_CALLBACK_STATUS
UsbProtectCompleteAccessDenied(
    _Inout_ PFLT_CALLBACK_DATA Data
    )
{
    Data->IoStatus.Status = STATUS_ACCESS_DENIED;
    Data->IoStatus.Information = 0;
    return FLT_PREOP_COMPLETE;
}

FLT_PREOP_CALLBACK_STATUS
UsbProtectPreSetInformation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
    )
{
    UNREFERENCED_PARAMETER(CompletionContext);

    if (UsbProtectShouldBlockUnapprovedDevice(FltObjects)) {
        USBP_LOG("Unapproved USB file information change blocked");
        return UsbProtectCompleteAccessDenied(Data);
    }

    if (!UsbProtectIsProtectionEnabled()) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (!UsbProtectSetInformationMayModifyFile(Data)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (!UsbProtectIsProtectedTarget(FltObjects)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    USBP_LOG("USB file information change blocked");
    return UsbProtectCompleteAccessDenied(Data);
}

FLT_PREOP_CALLBACK_STATUS
UsbProtectPreCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
    )
{
    PUNICODE_STRING fileName;

    UNREFERENCED_PARAMETER(CompletionContext);

    if (UsbProtectShouldBlockUnapprovedDevice(FltObjects) &&
        !UsbProtectIsDirectVolumeOpen(FltObjects)) {
        USBP_LOG("Unapproved USB access blocked");
        return UsbProtectCompleteAccessDenied(Data);
    }

    if (UsbProtectIsExecutableBlockingEnabled() &&
        UsbProtectIsProtectedTarget(FltObjects) &&
        FltObjects != NULL &&
        FltObjects->FileObject != NULL) {
        fileName = &FltObjects->FileObject->FileName;

        /*
         * Script interpreters normally request read access rather than
         * FILE_EXECUTE, so script types must be denied at open time. Native
         * images are also enforced later when an executable section is made.
         */
        if (UsbProtectIsScriptExtension(fileName) ||
            (UsbProtectIsNativeExecutableExtension(fileName) &&
             UsbProtectCreateRequestsExecute(Data))) {
            USBP_LOG("USB executable open blocked");
            return UsbProtectCompleteAccessDenied(Data);
        }
    }

    if (!UsbProtectIsProtectionEnabled()) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (!UsbProtectCreateMayModifyFile(Data)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (!UsbProtectIsProtectedTarget(FltObjects)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    USBP_LOG("USB write blocked");
    return UsbProtectCompleteAccessDenied(Data);
}

FLT_PREOP_CALLBACK_STATUS
UsbProtectPreRead(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
    )
{
    UNREFERENCED_PARAMETER(CompletionContext);

    if (UsbProtectShouldBlockUnapprovedDevice(FltObjects)) {
        USBP_LOG("Unapproved USB read blocked");
        return UsbProtectCompleteAccessDenied(Data);
    }

    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

FLT_PREOP_CALLBACK_STATUS
UsbProtectPreWrite(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
    )
{
    UNREFERENCED_PARAMETER(CompletionContext);

    if (UsbProtectShouldBlockUnapprovedDevice(FltObjects)) {
        USBP_LOG("Unapproved USB write blocked");
        return UsbProtectCompleteAccessDenied(Data);
    }

    if (!UsbProtectIsProtectionEnabled()) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (!UsbProtectIsProtectedTarget(FltObjects)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    /*
     * This includes cached, non-cached, and paging writes. The attribute check
     * is cached in the instance context, so no passive-only storage query occurs
     * in the write hot path.
     */
    USBP_LOG("USB write blocked");
    Data->IoStatus.Status = STATUS_ACCESS_DENIED;
    Data->IoStatus.Information = 0;
    return FLT_PREOP_COMPLETE;
}

FLT_PREOP_CALLBACK_STATUS
UsbProtectPreAcquireForSectionSynchronization(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
    )
{
    ULONG pageProtection;

    UNREFERENCED_PARAMETER(CompletionContext);

    if (UsbProtectShouldBlockUnapprovedDevice(FltObjects)) {
        USBP_LOG("Unapproved USB section creation blocked");
        return UsbProtectCompleteAccessDenied(Data);
    }

    if (!UsbProtectIsExecutableBlockingEnabled() ||
        !UsbProtectIsProtectedTarget(FltObjects)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (Data->Iopb->Parameters.AcquireForSectionSynchronization.SyncType !=
        SyncTypeCreateSection) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    pageProtection =
        Data->Iopb->Parameters.AcquireForSectionSynchronization.PageProtection;

    if ((pageProtection & (PAGE_EXECUTE |
                           PAGE_EXECUTE_READ |
                           PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY)) == 0) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    USBP_LOG("USB executable image mapping blocked");
    return UsbProtectCompleteAccessDenied(Data);
}
