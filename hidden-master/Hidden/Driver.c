#include <fltKernel.h>
#include <Ntddk.h>
#include "ExcludeList.h"

#include "RegFilter.h"
#include "FsFilter.h"
#include "PsMonitor.h"
#include "Device.h"
#include "Driver.h"
#include "Configs.h"
#include "Helper.h"

#include <ntifs.h>
#include <ntddk.h>
#include <windef.h>
#include <ntdef.h>

#define DRIVER_ALLOC_TAG 'nddH'

PDRIVER_OBJECT g_driverObject = NULL;

volatile LONG g_driverActive = FALSE;

// =========================================================================================

VOID EnableDisableDriver(BOOLEAN enabled)
{
	InterlockedExchange(&g_driverActive, (LONG)enabled);
}

BOOLEAN IsDriverEnabled()
{
	return (g_driverActive ? TRUE : FALSE);
}

// =========================================================================================

ULONGLONG g_hiddenRegConfigId = 0;
ULONGLONG g_hiddenDriverFileId = 0;

NTSTATUS InitializeStealthMode(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
	PLDR_DATA_TABLE_ENTRY LdrEntry;
	UNICODE_STRING normalized;
	NTSTATUS status;

	if (!CfgGetStealthState())
		return STATUS_SUCCESS;
	
	LdrEntry = (PLDR_DATA_TABLE_ENTRY)DriverObject->DriverSection;

	normalized.Length = 0;
	normalized.MaximumLength = LdrEntry->FullModuleName.Length + NORMALIZE_INCREAMENT;
	normalized.Buffer = (PWCH)ExAllocatePoolWithQuotaTag(PagedPool, normalized.MaximumLength, DRIVER_ALLOC_TAG);
	
	if (!normalized.Buffer)
	{
		DbgPrint("FsFilter1!" __FUNCTION__ ": error, can't allocate buffer\n");
		return STATUS_MEMORY_NOT_ALLOCATED;
	}

	status = NormalizeDevicePath(&LdrEntry->FullModuleName, &normalized);
	if (!NT_SUCCESS(status))
	{
		DbgPrint("FsFilter1!" __FUNCTION__ ": path normalization failed with code:%08x, path:%wZ\n", status, &LdrEntry->FullModuleName);
		ExFreePoolWithTag(normalized.Buffer, DRIVER_ALLOC_TAG);
		return status;
	}

	status = AddHiddenFile(&normalized, &g_hiddenDriverFileId);
	if (!NT_SUCCESS(status))
		DbgPrint("FsFilter1!" __FUNCTION__ ": can't hide self registry key\n");

	ExFreePoolWithTag(normalized.Buffer, DRIVER_ALLOC_TAG);

	status = AddHiddenRegKey(RegistryPath, &g_hiddenRegConfigId);
	if (!NT_SUCCESS(status))
		DbgPrint("FsFilter1!" __FUNCTION__ ": can't hide self registry key\n");

	return STATUS_SUCCESS;
}

// =========================================================================================

_Function_class_(DRIVER_UNLOAD)
VOID DriverUnload(PDRIVER_OBJECT DriverObject)
{
	UNREFERENCED_PARAMETER(DriverObject);

	DestroyDevice();
	DestroyRegistryFilter();
	DestroyFSMiniFilter();
	DestroyPsMonitor();
}

#define READ_REQUEST CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_SPECIAL_ACCESS)
#define WRITE_REQUEST CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_SPECIAL_ACCESS)
#define SET_PID_REQUEST CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_SPECIAL_ACCESS)
#define GET_MODULE_REQUEST CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_SPECIAL_ACCESS)
#define IO_READ_REQUEST CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_SPECIAL_ACCESS)

ULONG PID;
DWORD64 MainModule;
PEPROCESS process;


NTSTATUS NTAPI MmCopyVirtualMemory(IN PEPROCESS  	SourceProcess,
	IN PVOID  	SourceAddress,
	IN PEPROCESS  	TargetProcess,
	OUT PVOID  	TargetAddress,
	IN SIZE_T  	BufferSize,
	IN KPROCESSOR_MODE  	PreviousMode,
	OUT PSIZE_T  	ReturnSize
);

NTKERNELAPI
PVOID
PsGetProcessSectionBaseAddress(
	__in PEPROCESS Process
);

typedef struct _READ_MEM
{
	DWORD64 address;
	DWORD64 response;
	ULONG size;

} READ_MEM, *PREAD_MEM;

typedef struct _WRITE_MEM
{
	DWORD64 address;
	float value;
	ULONG size;

} WRITE_MEM, *PWRITE_MEM;

NTSTATUS RPM(PVOID src, PVOID dest, SIZE_T size)
{
	PSIZE_T bytes;
	__try
	{
		// Checks if the memory address actually exists
		ProbeForRead(src, size, (ULONG)size);
		// Use MmCopyVirtualMemory to copy memory from the game to our usermode process
		if (NT_SUCCESS(MmCopyVirtualMemory(process, src, PsGetCurrentProcess(), dest, size, KernelMode, &bytes)))
			return STATUS_SUCCESS;
		else
			return STATUS_ACCESS_DENIED;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return STATUS_ACCESS_DENIED;
	}
}

NTSTATUS WPM(PVOID src, PVOID dest, SIZE_T size)
{
	PSIZE_T bytes;
	__try
	{
		// Checks if the memory address actually exists and is writable
		ProbeForWrite(dest, size, (ULONG)size);
		// Use MmCopyVirtualMemory to copy memory from our usermode process to the game
		if (NT_SUCCESS(MmCopyVirtualMemory(PsGetCurrentProcess(), src, process, dest, size, KernelMode, &bytes)))
			return STATUS_SUCCESS;
		else
			return STATUS_ACCESS_DENIED;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return STATUS_ACCESS_DENIED;
	}
}


typedef struct _KERNEL_READ_REQUEST
{
	ULONG ProtectedProgram;
	ULONG LSASS;
	ULONG CSRSS;
	ULONG CSRSS2;

} KERNEL_READ_REQUEST, *PKERNEL_READ_REQUEST;

ULONG ProtectedProgramPID = 0;
ULONG LsassPID = 0;
ULONG CsrssPID = 0;
ULONG CsrssSecondPID = 0;
NTSTATUS DriverDispatch(PDEVICE_OBJECT pDeviceObject, PIRP Irp)
{
	NTSTATUS status = STATUS_SUCCESS;
	ULONG bytes = 0;

	PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

	// Get the IOCTL code
	ULONG IOcode = stack->Parameters.DeviceIoControl.IoControlCode;


	if (IOcode == READ_REQUEST)
	{
		// Get the struct address from the irp buffer
		PREAD_MEM read = (PREAD_MEM)Irp->AssociatedIrp.SystemBuffer;

		// Check if address is in usermode virtual memory (the usermode app sometimes fuck up for some reason)
		if (read->address < 0x7FFFFFFFFFFF)
		{
			PsLookupProcessByProcessId((HANDLE)PID, &process);
			RPM(read->address, &read->response, read->size);
		}

		status = STATUS_SUCCESS;
		bytes = sizeof(PREAD_MEM);
	}
	else if (IOcode == IO_READ_REQUEST)
	{
		// Get the input buffer & format it to our struct
		PKERNEL_READ_REQUEST ReadInput = (PKERNEL_READ_REQUEST)Irp->AssociatedIrp.SystemBuffer;

		if (ReadInput->ProtectedProgram != 0)
		{
			ProtectedProgramPID = ReadInput->ProtectedProgram;
		}
		if (ReadInput->LSASS != 0)
		{
			LsassPID = ReadInput->LSASS;
		}

		if (ReadInput->CSRSS != 0)
		{
			CsrssPID = ReadInput->CSRSS;
		}

		if (ReadInput->CSRSS2 != 0)
		{
			CsrssSecondPID = ReadInput->CSRSS2;
		}

		status = STATUS_SUCCESS;
		bytes = sizeof(KERNEL_READ_REQUEST);
	}
	else if (IOcode == WRITE_REQUEST)
	{
		// Get the struct address from the irp buffer
		PWRITE_MEM write = (PWRITE_MEM)Irp->AssociatedIrp.SystemBuffer;

		// Check if address is in usermode virtual memory (the usermode app sometimes fuck up for some reason)
		if (write->address < 0x7FFFFFFFFFFF)
		{
			PsLookupProcessByProcessId((HANDLE)PID, &process);
			WPM(&write->value, write->address, write->size);
		}

		status = STATUS_SUCCESS;
		bytes = sizeof(PWRITE_MEM);
	}
	else if (IOcode == SET_PID_REQUEST)
	{
		// Get address of var where to read the PID from
		PULONG Input = (PULONG)Irp->AssociatedIrp.SystemBuffer;
		PID = *Input;

		status = STATUS_SUCCESS;
		bytes = sizeof(Input);
	}
	else if (IOcode == GET_MODULE_REQUEST)
	{
		// Get address of var where to store the main mdoule base address
		PDWORD64 Module = (PDWORD64)Irp->AssociatedIrp.SystemBuffer;
		PsLookupProcessByProcessId((HANDLE)PID, &process);

		// Attach to the process and get it's base
		KeAttachProcess((PKPROCESS)process);
		*Module = PsGetProcessSectionBaseAddress(process);
		KeDetachProcess();

		status = STATUS_SUCCESS;
		bytes = sizeof(Module);
	}

	// Finish our request
	Irp->IoStatus.Status = status;
	Irp->IoStatus.Information = bytes;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	return status;
}

_Function_class_(DRIVER_INITIALIZE)
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
	NTSTATUS status;

	UNREFERENCED_PARAMETER(RegistryPath);

	EnableDisableDriver(TRUE);

	status = InitializeConfigs(RegistryPath);
	if (!NT_SUCCESS(status))
		DbgPrint("FsFilter1!" __FUNCTION__ ": can't initialize configs\n");

	EnableDisableDriver(CfgGetDriverState());

	status = InitializePsMonitor(DriverObject);
	if (!NT_SUCCESS(status))
		DbgPrint("FsFilter1!" __FUNCTION__ ": object monitor didn't start\n");

	status = InitializeFSMiniFilter(DriverObject);
	if (!NT_SUCCESS(status))
		DbgPrint("FsFilter1!" __FUNCTION__ ": file-system mini-filter didn't start\n");

	status = InitializeRegistryFilter(DriverObject);
	if (!NT_SUCCESS(status))
		DbgPrint("FsFilter1!" __FUNCTION__ ": registry filter didn't start\n");

	status = InitializeDevice(DriverObject);
	if (!NT_SUCCESS(status))
		DbgPrint("FsFilter1!" __FUNCTION__ ": can't create device\n");

	status = InitializeStealthMode(DriverObject, RegistryPath);
	if (!NT_SUCCESS(status))
		DbgPrint("FsFilter1!" __FUNCTION__ ": can't activate stealth mode\n");

	DestroyConfigs();

	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DriverDispatch;

	DriverObject->DriverUnload = DriverUnload;
	g_driverObject = DriverObject;

	return STATUS_SUCCESS;
}

