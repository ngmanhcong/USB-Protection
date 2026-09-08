#include "Callbacks.h"

static
BOOLEAN
UsbProtectIsProtectedTarget(
    _In_ PCFLT_RELATED_OBJECTS FltObjects
    )
{
    NTSTATUS status;
    PUSBPROTECT_INSTANCE_CONTEXT context = NULL;
    BOOLEAN protectedTarget = FALSE;

    if (FltObjects == NULL || FltObjects->Instance == NULL) {
        return FALSE;
    }

    status = FltGetInstanceContext(FltObjects->Instance, &context);
    if (!NT_SUCCESS(status)) {
        return FALSE;
    }

    protectedTarget = (context->IsUsb || context->IsRemovable);
    FltReleaseContext(context);

    return protectedTarget;
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
    UNREFERENCED_PARAMETER(CompletionContext);

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
UsbProtectPreWrite(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
    )
{
    UNREFERENCED_PARAMETER(CompletionContext);

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
