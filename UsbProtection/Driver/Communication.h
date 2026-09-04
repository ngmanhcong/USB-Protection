#pragma once

#include "Driver.h"

NTSTATUS
UsbProtectCreateCommunicationPort(
    VOID
    );

VOID
UsbProtectCloseCommunicationPort(
    VOID
    );

NTSTATUS
UsbProtectConnectNotify(
    _In_ PFLT_PORT ClientPort,
    _In_opt_ PVOID ServerPortCookie,
    _In_reads_bytes_opt_(SizeOfContext) PVOID ConnectionContext,
    _In_ ULONG SizeOfContext,
    _Outptr_result_maybenull_ PVOID *ConnectionCookie
    );

VOID
UsbProtectDisconnectNotify(
    _In_opt_ PVOID ConnectionCookie
    );

NTSTATUS
UsbProtectMessageNotify(
    _In_opt_ PVOID PortCookie,
    _In_reads_bytes_opt_(InputBufferLength) PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ReturnOutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _Out_ PULONG ReturnOutputBufferLength
    );
