#include "pch.h"
#include "ThreadSchedule.h"

#define DO_TASK(key, type) \
	const UINT fid = static_cast<UINT>(key); \
	if (fid == g_exitCode) break; \
	if (fid >= g_testArgs.fileCount) ExitProcess(-5); \
	if (type == THREAD_TASK_READ_CALL) \
		ReadCallTaskWork(fid); \
	else \
		ComputeTaskWork(fid)

#define WAIT_AND_DO_TASK(pRet, pKey, pLpov, type) \
	GetQueuedCompletionStatus(type == THREAD_TASK_READ_CALL ? g_globalTaskQueue : g_globalWaitingQueue, pRet, pKey, pLpov, INFINITE); \
	DO_TASK(*pKey, type)

#define SAFE_CLOSE_HANDLE(h) if (h != INVALID_HANDLE_VALUE) CloseHandle(h)

using namespace Concurrency::diagnostic;
using namespace ThreadSchedule;

TestArgument g_testArgs;
TestResult g_testResult;

UINT g_currentReadCallCount;
UINT g_currentComputeCount;
BOOL g_currentTaskMode;
UINT g_completeFileCount;

// Thread works.
HANDLE g_globalTaskQueue;
HANDLE g_globalWaitingQueue;
HANDLE* g_threadHandleAry;
HANDLE* g_threadIocpAry;

// SRW Locks.
SRWLOCK g_srwFileStatus;
SRWLOCK g_srwFileLock;
SRWLOCK g_srwFileHandle;
SRWLOCK g_srwFileIocp;
SRWLOCK g_srwFileBuffer;
SRWLOCK g_srwFileFinish;
SRWLOCK g_srwTaskMode;

// File cache data.
std::unordered_map<UINT, UINT> g_fileStatusMap;
std::unordered_map<UINT, FileLock*> g_fileLockMap;
std::unordered_map<UINT, HANDLE> g_fileHandleMap;
std::unordered_map<UINT, HANDLE> g_fileIocpMap;
std::unordered_map<UINT, BYTE*> g_fileBufferMap;
std::unordered_map<UINT, UINT> g_fileBufferSizeMap;

void ThreadSchedule::ReadCallTaskWork(const UINT fid)
{
#ifdef _DEBUG
	SERIES_INIT((std::to_wstring(GetCurrentThreadId()) + L" Read Call Task").c_str());
	SPAN_INIT;
	SPAN_START(0, _T("Create File (%d)", fid), fid);
#endif

	const HANDLE fileHandle = 
		CreateFileW(
			(L"dummy\\" + std::to_wstring(fid)).c_str(), 
			GENERIC_READ, 
			FILE_SHARE_READ, 
			NULL, 
			OPEN_EXISTING, 
			g_fileFlag, 
			NULL);

#ifdef _DEBUG
	SPAN_END;
	SPAN_START(0, _T("Get File Size (%d)"), fid);
#endif

	LARGE_INTEGER fileByteSize;
	GetFileSizeEx(fileHandle, &fileByteSize);

	const DWORD alignedFileByteSize = GetAlignedByteSize(&fileByteSize, 512u);

#ifdef _DEBUG
	SPAN_END;
	SPAN_START(0, _T("Create IOCP Handle (%d)"), fid);
#endif

	HANDLE fileIOCP = NULL;
	if (g_simType == SIM_UNIVERSAL_THREAD)
	{
		fileIOCP = CreateIoCompletionPort(fileHandle, NULL, 0, 0);
	}
	else if (g_simType == SIM_ROLE_SPECIFIED_THREAD)
	{
		// All IOCP is same, we don't need to save.
		CreateIoCompletionPort(fileHandle, g_globalWaitingQueue, fid, 0);
	}

#ifdef _DEBUG
	SPAN_END;
	SPAN_START(0, _T("Buffer Allocation (%d)"), fid);
#endif

	BYTE* fileBuffer = static_cast<BYTE*>(VirtualAlloc(NULL, alignedFileByteSize, MEM_COMMIT, PAGE_READWRITE));

#ifdef _DEBUG
	SPAN_END;
	SPAN_START(0, _T("Write to Map (%d)"), fid);
#endif

	AcquireSRWLockExclusive(&g_srwFileHandle);
	{
		if (FALSE == g_fileHandleMap.contains(fid))
			g_fileHandleMap[fid] = fileHandle;
	}
	ReleaseSRWLockExclusive(&g_srwFileHandle);

	if (g_simType == SIM_UNIVERSAL_THREAD)
	{
		AcquireSRWLockExclusive(&g_srwFileIocp);
		{
			if (FALSE == g_fileIocpMap.contains(fid))
				g_fileIocpMap[fid] = fileIOCP;
		}
		ReleaseSRWLockExclusive(&g_srwFileIocp);
	}

	AcquireSRWLockExclusive(&g_srwFileBuffer);
	{
		if (FALSE == g_fileBufferMap.contains(fid))
			g_fileBufferMap[fid] = fileBuffer;
		
		if (FALSE == g_fileBufferSizeMap.contains(fid))
			g_fileBufferSizeMap[fid] = fileByteSize.QuadPart;

		g_testResult.totalFileSize += fileByteSize.QuadPart;
	}
	ReleaseSRWLockExclusive(&g_srwFileBuffer);

#ifdef _DEBUG
	SPAN_END;
	SPAN_START(0, _T("ReadFile Call (%d)"), fid);
#endif

	OVERLAPPED ov = { 0 };
	if (FALSE == ReadFile(fileHandle, fileBuffer, alignedFileByteSize, NULL, &ov) && GetLastError() != ERROR_IO_PENDING)
		ExitProcess(-1);

#ifdef _DEBUG
	SPAN_END;
#endif
}

void ThreadSchedule::CompletionTaskWork(const UINT fid)
{
#ifdef _DEBUG
	SERIES_INIT((std::to_wstring(GetCurrentThreadId()) + L" Completion Task").c_str());
	SPAN_INIT;
	SPAN_START(1, _T("Completion (%d)"), fid);
#endif

	HANDLE fileIocp = NULL;
	AcquireSRWLockShared(&g_srwFileIocp);
	{
		fileIocp = g_fileIocpMap[fid];
	}
	ReleaseSRWLockShared(&g_srwFileIocp);

	DWORD ret;
	ULONG_PTR key;
	LPOVERLAPPED lpov;

	GetQueuedCompletionStatus(fileIocp, &ret, &key, &lpov, INFINITE);

#ifdef _DEBUG
	SPAN_END;
#endif
}

void ThreadSchedule::ComputeTaskWork(const UINT fid)
{
#ifdef _DEBUG
	SERIES_INIT((std::to_wstring(GetCurrentThreadId()) + L" Compute Task").c_str());
	SPAN_INIT;
	SPAN_START(2, _T("Compute (%d)"), fid);
#endif

	TIMER_INIT;
	TIMER_START;

	BYTE* bufferAddress = nullptr;
	UINT bufferSize = 0;
	AcquireSRWLockShared(&g_srwFileBuffer);
	{
		bufferAddress = g_fileBufferMap[fid];
		bufferSize = g_fileBufferSizeMap[fid];
	}
	ReleaseSRWLockShared(&g_srwFileBuffer);

	if (g_testArgs.enableComputeTime)
	{
		UINT timeOverMicroSeconds;
		BOOL exit = FALSE;
		memcpy(&timeOverMicroSeconds, bufferAddress, sizeof(UINT));
		
		TIMER_STOP;
		while (el * 1000 * 1000 <= timeOverMicroSeconds)
		{
			// Calculate checksum.
			{
				int checkSum = 0;
				int sum = 0;

				for (int i = 0; i < bufferSize; i++)
				{
					sum += bufferAddress[i];

					TIMER_STOP;
					if (el * 1000 * 1000 > timeOverMicroSeconds)
					{
						exit = TRUE;
						break;
					}
				}

				if (exit)
					break;

				checkSum = sum;
				checkSum = checkSum & 0xFF;
				checkSum = ~checkSum + 1;

				int res = checkSum + sum;
				res = res & 0xFF;
			}
		}
	}
	else
	{
		// Calculate checksum.
		for (int x = 0; x < g_computeLoopCount; x++)
		{
			int checkSum = 0;
			int sum = 0;

			for (int i = 0; i < bufferSize; i++)
			{
				sum += bufferAddress[i];
			}

			checkSum = sum;
			checkSum = checkSum & 0xFF;
			checkSum = ~checkSum + 1;

			int res = checkSum + sum;
			res = res & 0xFF;
		}
	}

#ifdef _DEBUG
	SPAN_END;
	SPAN_START(2, _T("Release (%d)"), fid);
#endif

	if (bufferAddress != nullptr)
		VirtualFree(bufferAddress, 0, MEM_RELEASE);

	if (g_simType == SIM_UNIVERSAL_THREAD)
	{
		AcquireSRWLockShared(&g_srwFileIocp);
		if (g_fileIocpMap.contains(fid))
			SAFE_CLOSE_HANDLE(g_fileIocpMap[fid]);
		ReleaseSRWLockShared(&g_srwFileIocp);
	}

	AcquireSRWLockShared(&g_srwFileHandle);
	if (g_fileHandleMap.contains(fid))
		SAFE_CLOSE_HANDLE(g_fileHandleMap[fid]);
	ReleaseSRWLockShared(&g_srwFileHandle);

	AcquireSRWLockExclusive(&g_srwFileFinish);
	g_completeFileCount++;
	if (g_completeFileCount == g_testArgs.fileCount)
	{
		std::cout << "Finished" << "\n";
		for (UINT t = 0; t < g_testArgs.threadCount; t++)
		{
			OVERLAPPED ov = { 0 };
			PostQueuedCompletionStatus(g_globalTaskQueue, 0, g_exitCode, &ov);
			PostQueuedCompletionStatus(g_globalWaitingQueue, 0, g_exitCode, &ov);
		}
	}
	ReleaseSRWLockExclusive(&g_srwFileFinish);

#ifdef _DEBUG
	SPAN_END;
#endif
}

void ThreadSchedule::FileMapTaskWork(UINT fid)
{
#ifdef _DEBUG
	SERIES_INIT((std::to_wstring(GetCurrentThreadId()) + L" FileMap").c_str());
	SPAN_INIT;
	SPAN_START(0, _T("Create File (%d)"), fid);
#endif

	const HANDLE fileHandle =
		CreateFileW(
			(L"dummy\\" + std::to_wstring(fid)).c_str(),
			GENERIC_READ,
			0,
			NULL,
			OPEN_EXISTING,
			FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED,
			NULL);

	if (fileHandle == INVALID_HANDLE_VALUE)
		ExitProcess(9);

#ifdef _DEBUG
	SPAN_END;
#endif

	LARGE_INTEGER fileByteSize;
	GetFileSizeEx(fileHandle, &fileByteSize);

#ifdef _DEBUG
	SPAN_START(0, _T("CreateFileMapping (%d)"), fid);
#endif

	const HANDLE mapHandle =
		CreateFileMapping(
			fileHandle,
			NULL,
			PAGE_READONLY,
			0,
			0,
			NULL);

	if (mapHandle == 0)
		ExitProcess(9);

#ifdef _DEBUG
	SPAN_END;
	SPAN_START(0, _T("MapViewOfFile (%d)"), fid);
#endif

	const LPVOID mapView =
		MapViewOfFile(
			mapHandle,
			FILE_MAP_READ,
			0,
			0,
			0
		);

	if (mapView == NULL)
		ExitProcess(9);

#ifdef _DEBUG
	SPAN_END;
	SPAN_START(2, _T("Compute (%d)"), fid);
#endif

	BYTE* ptr = (BYTE*)mapView;

	// Calculate checksum.
	for (int x = 0; x < g_computeLoopCount; x++)
	{
		int checkSum = 0;
		int sum = 0;

		for (int i = 0; i < fileByteSize.QuadPart; i++)
		{
			sum += ptr[i];
		}

		checkSum = sum;
		checkSum = checkSum & 0xFF;
		checkSum = ~checkSum + 1;

		int res = checkSum + sum;
		res = res & 0xFF;
	}

#ifdef _DEBUG
	SPAN_END;
#endif

	UnmapViewOfFile(mapView);
	SAFE_CLOSE_HANDLE(mapHandle);
	SAFE_CLOSE_HANDLE(fileHandle);

	AcquireSRWLockExclusive(&g_srwFileFinish);
	g_completeFileCount++;
	g_testResult.totalFileSize += fileByteSize.QuadPart;
	if (g_completeFileCount == g_testArgs.fileCount)
	{
		std::cout << "Finished" << "\n";
		for (UINT t = 0; t < g_testArgs.threadCount; t++)
		{
			OVERLAPPED ov = { 0 };
			PostQueuedCompletionStatus(g_globalTaskQueue, 0, g_exitCode, &ov);
		}
	}
	ReleaseSRWLockExclusive(&g_srwFileFinish);
}

void ThreadSchedule::SyncTaskWork(UINT fid)
{
#ifdef _DEBUG
	SERIES_INIT((std::to_wstring(GetCurrentThreadId()) + L" Sync").c_str());
	SPAN_INIT;
	SPAN_START(0, _T("Create File (%d)"), fid);
#endif

	const HANDLE fileHandle =
		CreateFileW(
			(L"dummy\\" + std::to_wstring(fid)).c_str(),
			GENERIC_READ,
			0,
			NULL,
			OPEN_EXISTING,
			FILE_FLAG_NO_BUFFERING,
			NULL);

	if (fileHandle == INVALID_HANDLE_VALUE)
		ExitProcess(9);

#ifdef _DEBUG
	SPAN_END;
	SPAN_START(0, _T("Buffer Allocation (%d)"), fid);
#endif

	LARGE_INTEGER fileByteSize;
	GetFileSizeEx(fileHandle, &fileByteSize);
	const DWORD alignedFileByteSize = GetAlignedByteSize(&fileByteSize, 512u);

	BYTE* fileBuffer = static_cast<BYTE*>(VirtualAlloc(NULL, alignedFileByteSize, MEM_COMMIT, PAGE_READWRITE));

#ifdef _DEBUG
	SPAN_END;
	SPAN_START(1, _T("ReadFile (%d)"), fid);
#endif

	if (FALSE == ReadFile(fileHandle, fileBuffer, alignedFileByteSize, NULL, NULL) && GetLastError() != ERROR_IO_PENDING)
		ExitProcess(-1);

#ifdef _DEBUG
	SPAN_END;
	SPAN_START(2, _T("Compute (%d)"), fid);
#endif

	TIMER_INIT;
	TIMER_START;

	if (g_testArgs.enableComputeTime)
	{
		UINT timeOverMicroSeconds;
		BOOL exit = FALSE;
		memcpy(&timeOverMicroSeconds, fileBuffer, sizeof(UINT));

		TIMER_STOP;
		while (el * 1000 * 1000 <= timeOverMicroSeconds)
		{
			// Calculate checksum.
			{
				int checkSum = 0;
				int sum = 0;

				for (int i = 0; i < fileByteSize.QuadPart; i++)
				{
					sum += fileBuffer[i];

					TIMER_STOP;
					if (el * 1000 * 1000 > timeOverMicroSeconds)
					{
						exit = TRUE;
						break;
					}
				}

				if (exit)
					break;

				checkSum = sum;
				checkSum = checkSum & 0xFF;
				checkSum = ~checkSum + 1;

				int res = checkSum + sum;
				res = res & 0xFF;
			}
		}
	}
	else
	{
		// Calculate checksum.
		for (int x = 0; x < g_computeLoopCount; x++)
		{
			int checkSum = 0;
			int sum = 0;

			for (int i = 0; i < fileByteSize.QuadPart; i++)
			{
				sum += fileBuffer[i];
			}

			checkSum = sum;
			checkSum = checkSum & 0xFF;
			checkSum = ~checkSum + 1;

			int res = checkSum + sum;
			res = res & 0xFF;
		}
	}

#ifdef _DEBUG
	SPAN_END;
#endif

	VirtualFree(fileBuffer, 0, MEM_RELEASE);
	SAFE_CLOSE_HANDLE(fileHandle);

	AcquireSRWLockExclusive(&g_srwFileFinish);
	g_completeFileCount++;
	g_testResult.totalFileSize += fileByteSize.QuadPart;
	if (g_completeFileCount == g_testArgs.fileCount)
	{
		std::cout << "Finished" << "\n";
		for (UINT t = 0; t < g_testArgs.threadCount; t++)
		{
			OVERLAPPED ov = { 0 };
			PostQueuedCompletionStatus(g_globalTaskQueue, 0, g_exitCode, &ov);
		}
	}
	ReleaseSRWLockExclusive(&g_srwFileFinish);
}

// Failed to get lock, which means another thread is processing task.
DWORD ThreadSchedule::HandleLockAcquireFailure(const UINT fid, const UINT threadTaskType)
{
	const HANDLE* fileSemAry = g_fileLockMap[fid]->sem;
	const HANDLE* taskEndEvAry = g_fileLockMap[fid]->taskEndEv;

	UINT fileStatus = 0;
	AcquireSRWLockShared(&g_srwFileStatus);
	{
		fileStatus = g_fileStatusMap[fid];
	}
	ReleaseSRWLockShared(&g_srwFileStatus);

	// Another thread is processing pre-require task.
	if (fileStatus < threadTaskType * 3)
	{
		if (g_waitDependencyFront)
		{
			// Waiting until pre-require task ends.
			const DWORD result = WaitForSingleObject(fileSemAry[threadTaskType], INFINITE);

			// Do your job!
			return result;
		}
		// If g_waitDependencyFront false, we'll check later.
		
		// Ignore your job!
		return WAIT_TIMEOUT;
	}

	// Another thread is processing requested task.
	if (threadTaskType * 3 <= fileStatus && fileStatus < (threadTaskType + 1) * 3)
	{
		if (g_waitDependencyFront)
		{
			// Waiting until requested task ends.
			WaitForSingleObject(taskEndEvAry[threadTaskType], INFINITE);
		}
		// If g_waitDependencyFront false, we'll check later.

		// Ignore your job!
		return WAIT_TIMEOUT;
	}

	// Another thread is processing post-require task.
	if ((threadTaskType + 1) * 3 <= fileStatus)
	{
		// Ignore your job!
		return WAIT_TIMEOUT;
	}

	return WAIT_TIMEOUT;
}

void ThreadSchedule::DoThreadTask(ThreadTaskArgs* args, const UINT threadTaskType)
{
#ifdef _DEBUG
	SERIES_INIT(std::to_wstring(GetCurrentThreadId()).c_str());
	SPAN_INIT;
#endif

	const UINT fid = args->FID;
	const HANDLE* fileSemAry = g_fileLockMap[fid]->sem;
	const HANDLE* taskEndEvAry = g_fileLockMap[fid]->taskEndEv;

	DWORD waitResult = WaitForSingleObject(fileSemAry[threadTaskType], 0L);
	if (waitResult == WAIT_TIMEOUT)	// If failed to get lock...
	{
		waitResult = HandleLockAcquireFailure(fid, threadTaskType);
		if (waitResult == WAIT_TIMEOUT)
		{
			HeapFree(GetProcessHeap(), 0, args);
			return;
		}
	}

	AcquireSRWLockExclusive(&g_srwFileStatus);
	InterlockedIncrement(&g_fileStatusMap[fid]);
	ReleaseSRWLockExclusive(&g_srwFileStatus);

	const LPCTSTR format =
		threadTaskType == THREAD_TASK_READ_CALL ? L"Read Call Task (%d)" :
		threadTaskType == THREAD_TASK_COMPLETION ? L"Completion Task (%d)" :
		L"Compute Task (%d)";

#ifdef _DEBUG
	SPAN_START(threadTaskType, format, fid);
#endif

	switch (threadTaskType)
	{
	case THREAD_TASK_READ_CALL:
		ReadCallTaskWork(fid);
		break;
	case THREAD_TASK_COMPLETION:
		CompletionTaskWork(fid);
		break;
	case THREAD_TASK_COMPUTE:
		ComputeTaskWork(fid);
		break;
	}

#ifdef _DEBUG
	SPAN_END;
#endif

	AcquireSRWLockExclusive(&g_srwFileStatus);
	InterlockedIncrement(&g_fileStatusMap[fid]);
	ReleaseSRWLockExclusive(&g_srwFileStatus);

	if (threadTaskType < THREAD_TASK_COMPUTE)
	{
		ReleaseSemaphore(fileSemAry[threadTaskType + 1], 1, NULL);

		AcquireSRWLockExclusive(&g_srwFileStatus);
		InterlockedIncrement(&g_fileStatusMap[fid]);
		ReleaseSRWLockExclusive(&g_srwFileStatus);
	}

	SetEvent(taskEndEvAry[threadTaskType]);
	HeapFree(GetProcessHeap(), 0, args);
}

DWORD WINAPI ThreadSchedule::UniversalThreadFunc(const LPVOID param)
{
	const HANDLE threadIOCPHandle = param;

	OVERLAPPED_ENTRY entryAry[g_taskRemoveCount];
	ULONG entRemoved;

	while (TRUE)
	{
		GetQueuedCompletionStatusEx(threadIOCPHandle, entryAry, g_taskRemoveCount, &entRemoved, INFINITE, FALSE);

		for (UINT i = 0; i < entRemoved; i++)
		{
			const ULONG_PTR key = entryAry[i].lpCompletionKey;
			const LPOVERLAPPED lpov = entryAry[i].lpOverlapped;

			// If exit code received, terminate thread.
			if (key == g_exitCode)
				return 0;

			ThreadTaskArgs* args = reinterpret_cast<ThreadTaskArgs*>(lpov);
			DoThreadTask(args, static_cast<UINT>(key));
		}
	}

	return 0;
}

DWORD ThreadSchedule::RoleSpecifiedThreadFunc(const LPVOID param)
{
#ifdef _DEBUG
	SERIES_INIT(std::to_wstring(GetCurrentThreadId()).c_str());
	SPAN_INIT;
#endif

	const UINT threadRole = reinterpret_cast<UINT>(param);

	DWORD ret;
	ULONG_PTR key;
	LPOVERLAPPED lpov;

	if (threadRole == THREAD_ROLE_BOTH)
	{
		while (TRUE)
		{
			if (FALSE == g_currentTaskMode)
			{
				// Read call task exists...
				if (TRUE == GetQueuedCompletionStatus(g_globalTaskQueue, &ret, &key, &lpov, 0L))
				{
					if (key == g_exitCode) break;

					AcquireSRWLockExclusive(&g_srwTaskMode);
					if (g_currentReadCallCount < g_testArgs.readCallLimit)
					{
						g_currentReadCallCount++;
						if (g_currentReadCallCount == g_testArgs.readCallLimit)
						{
							g_currentTaskMode = TRUE;
							g_currentReadCallCount = 0;
						}
						ReleaseSRWLockExclusive(&g_srwTaskMode);

						DO_TASK(key, THREAD_TASK_READ_CALL);
					}
					else
					{
						ReleaseSRWLockExclusive(&g_srwTaskMode);
					}
				}
				else
				{
					if (TRUE == GetQueuedCompletionStatus(g_globalWaitingQueue, &ret, &key, &lpov, 0L))
					{
						DO_TASK(key, THREAD_TASK_COMPUTE);
					}
				}
			}
			else
			{
				if (TRUE == GetQueuedCompletionStatus(g_globalWaitingQueue, &ret, &key, &lpov, INFINITE))
				{
					AcquireSRWLockExclusive(&g_srwTaskMode);
					if (g_currentComputeCount < g_testArgs.computeLimit)
					{
						g_currentComputeCount++;
						if (g_currentComputeCount == g_testArgs.computeLimit)
						{
							g_currentTaskMode = FALSE;
							g_currentComputeCount = 0;
						}
						ReleaseSRWLockExclusive(&g_srwTaskMode);

						DO_TASK(key, THREAD_TASK_COMPUTE);
					}
					else
					{
						ReleaseSRWLockExclusive(&g_srwTaskMode);
					}
				}
			}
		}
	}
	else if (threadRole == THREAD_ROLE_COMPUTE_ONLY)
	{
		while (TRUE)
		{
			WAIT_AND_DO_TASK(&ret, &key, &lpov, THREAD_TASK_COMPUTE);
		}
	}
	else if (threadRole == THREAD_ROLE_COMPUTE_AND_READFILE)
	{
		while (TRUE)
		{
			if (FALSE == GetQueuedCompletionStatus(g_globalWaitingQueue, &ret, &key, &lpov, 0L))
			{
				// If no compute task exists...
				// Check read call task exists immediately.
				if (TRUE == GetQueuedCompletionStatus(g_globalTaskQueue, &ret, &key, &lpov, 0L))
				{
					// If exists, do read call task.
					DO_TASK(key, THREAD_TASK_READ_CALL);
				}
				else
				{
					WAIT_AND_DO_TASK(&ret, &key, &lpov, THREAD_TASK_COMPUTE);
				}
			}
			else
			{
				DO_TASK(key, THREAD_TASK_COMPUTE);
			}
		}
	}
	else if (threadRole == THREAD_ROLE_READFILE_CALL_ONLY)
	{
		while (TRUE)
		{
			WAIT_AND_DO_TASK(&ret, &key, &lpov, THREAD_TASK_READ_CALL);
		}
	}

	return 0;
}

DWORD ThreadSchedule::FileMapThreadFunc(const LPVOID param)
{
	UNREFERENCED_PARAMETER(param);

	DWORD ret;
	ULONG_PTR key;
	LPOVERLAPPED lpov;

	while (TRUE)
	{
		GetQueuedCompletionStatus(g_globalTaskQueue, &ret, &key, &lpov, INFINITE);

		if (key == g_exitCode) break;
		if (key >= g_testArgs.fileCount) ExitProcess(-5);

		FileMapTaskWork(key);
	}
}

DWORD ThreadSchedule::SyncThreadFunc(LPVOID param)
{
	UNREFERENCED_PARAMETER(param);

	DWORD ret;
	ULONG_PTR key;
	LPOVERLAPPED lpov;

	while (TRUE)
	{
		GetQueuedCompletionStatus(g_globalTaskQueue, &ret, &key, &lpov, INFINITE);

		if (key == g_exitCode) break;
		if (key >= g_testArgs.fileCount) ExitProcess(-5);

		SyncTaskWork(key);
	}
}

TestResult ThreadSchedule::StartThreadTasks(TestArgument args)
{
#ifdef _DEBUG
	SERIES_INIT(_T("Main Thread"));
	SPAN_INIT;
#endif

	g_testResult = { 0 };
	g_testArgs = args;

	if (g_simType == SIM_UNIVERSAL_THREAD)
	{
		InitializeSRWLock(&g_srwFileStatus);
		InitializeSRWLock(&g_srwFileLock);
		InitializeSRWLock(&g_srwFileIocp);
	}

	InitializeSRWLock(&g_srwFileHandle);
	InitializeSRWLock(&g_srwFileBuffer);
	InitializeSRWLock(&g_srwTaskMode);
	InitializeSRWLock(&g_srwFileFinish);

	// Initialize global IOCP.
	if (g_simType == SIM_ROLE_SPECIFIED_THREAD)
	{
		g_globalTaskQueue = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, g_testArgs.threadCount);
		g_globalWaitingQueue = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, g_testArgs.threadCount);
	}

	g_threadHandleAry = new HANDLE[g_testArgs.threadCount];
	g_threadIocpAry = new HANDLE[g_testArgs.threadCount];

	// Create threads.
	for (UINT t = 0; t < g_testArgs.threadCount; t++)
	{
		DWORD tid = 0;
		HANDLE threadHandle = NULL;

		if (g_simType == SIM_UNIVERSAL_THREAD)
		{
			g_threadIocpAry[t] = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
			threadHandle = CreateThread(NULL, 0, UniversalThreadFunc, g_threadIocpAry[t], CREATE_SUSPENDED, &tid);
		}
		else if (g_simType == SIM_ROLE_SPECIFIED_THREAD)
		{
			// Assign thread roles.
			UINT threadType = (t < g_testArgs.threadRole[0]) ? 0 : (g_testArgs.threadRole[0] <= t && t < g_testArgs.threadRole[0] + g_testArgs.threadRole[1]) ? 1 : (g_testArgs.threadRole[0] + g_testArgs.threadRole[1] <= t && t < g_testArgs.threadRole[0] + g_testArgs.threadRole[1] + g_testArgs.threadRole[2]) ? 2 : 3;
			threadHandle = CreateThread(NULL, 0, RoleSpecifiedThreadFunc, reinterpret_cast<LPVOID>(threadType), 0, &tid);
		}

		if (threadHandle == NULL)
			ExitProcess(3);

		g_threadHandleAry[t] = threadHandle;
	}

	if (g_simType == SIM_UNIVERSAL_THREAD)
	{
		for (UINT t = 0; t < g_testArgs.threadCount; t++)
			ResumeThread(g_threadHandleAry[t]);
	}

	// Create root file locking & status objects.
	if (g_simType == SIM_UNIVERSAL_THREAD)
	{
		for (UINT i = 0; i < args.fileCount; i++)
		{
			const UINT rootFID = i;
			g_fileLockMap[rootFID] = new FileLock(rootFID);
			g_fileStatusMap[rootFID] = 0;
		}
	}
	else if (g_simType == SIM_ROLE_SPECIFIED_THREAD)
	{
		// Don't need to create lock object.
	}

#ifdef _DEBUG
	SPAN_START(0, _T("Loading Time"));
#endif

	TIMER_INIT;
	TIMER_START;

	// Post tasks.
	if (g_simType == SIM_UNIVERSAL_THREAD)
	{
		// 1. Uniformly distribute.
		/*for (UINT i = 0; i < rootFIDAryCount; i++)
		{
			PostThreadTask(i % g_testArgs.threadCount, rootFIDAry[i], THREAD_TASK_READ_CALL);
			PostThreadTask(i % g_testArgs.threadCount, rootFIDAry[i], THREAD_TASK_COMPLETION);
			PostThreadTask(i % g_testArgs.threadCount, rootFIDAry[i], THREAD_TASK_COMPUTE);
		}*/

		// 2. Define thread tasks.
		for (UINT i = 0; i < args.fileCount/4; i++)
		{
			PostThreadTask(0, i*4, THREAD_TASK_READ_CALL);
			PostThreadTask(1, i*4+1, THREAD_TASK_READ_CALL);
			PostThreadTask(2, i*4+2, THREAD_TASK_READ_CALL);
			PostThreadTask(3, i*4+3, THREAD_TASK_READ_CALL);
		}

		for (UINT i = 0; i < args.fileCount/2; i++)
		{
			PostThreadTask(4, i*2, THREAD_TASK_COMPLETION);
			PostThreadTask(5, i*2+1, THREAD_TASK_COMPLETION);
		}

		for (UINT i = 0; i < args.fileCount/8; i++)
		{
			PostThreadTask(6, i*8, THREAD_TASK_COMPUTE);
			PostThreadTask(7, i*8+1, THREAD_TASK_COMPUTE);
			PostThreadTask(8, i*8+2, THREAD_TASK_COMPUTE);
			PostThreadTask(9, i*8+3, THREAD_TASK_COMPUTE);
			PostThreadTask(10, i*8+4, THREAD_TASK_COMPUTE);
			PostThreadTask(11, i*8+5, THREAD_TASK_COMPUTE);
			PostThreadTask(12, i*8+6, THREAD_TASK_COMPUTE);
			PostThreadTask(13, i*8+7, THREAD_TASK_COMPUTE);
		}
	}
	else if (g_simType == SIM_ROLE_SPECIFIED_THREAD)
	{
		for (UINT i = 0; i < args.fileCount; i++)
		{
			OVERLAPPED ov = { 0 };
			PostQueuedCompletionStatus(g_globalTaskQueue, 0, i, &ov);
		}
	}
	
	// Post thread termination.
	if (g_simType == SIM_UNIVERSAL_THREAD)
	{
		// Post exit to all threads.
		for (UINT t = 0; t < g_testArgs.threadCount; t++)
			PostThreadExit(t);
	}
	else if (g_simType == SIM_ROLE_SPECIFIED_THREAD)
	{
		// Post exit on last compute task.
	}

	WaitForMultipleObjects(g_testArgs.threadCount, g_threadHandleAry, TRUE, INFINITE);
	TIMER_STOP;

#ifdef _DEBUG
	SPAN_END;
#endif

	// Release cached data.
	{
		for (UINT i = 0; i < g_fileLockMap.size(); i++)
			delete g_fileLockMap[i];

		g_fileBufferMap.clear();
		g_fileBufferSizeMap.clear();
		g_fileIocpMap.clear();
		g_fileHandleMap.clear();
		g_fileLockMap.clear();
	}

	// Close global IOCP.
	if (g_simType == SIM_ROLE_SPECIFIED_THREAD)
	{
		SAFE_CLOSE_HANDLE(g_globalTaskQueue);
		SAFE_CLOSE_HANDLE(g_globalWaitingQueue);
	}

	// Release thread handle/IOCP.
	for (UINT t = 0; t < g_testArgs.threadCount; t++)
	{
		if (g_simType == SIM_UNIVERSAL_THREAD)
			SAFE_CLOSE_HANDLE(g_threadIocpAry[t]);

		SAFE_CLOSE_HANDLE(g_threadHandleAry[t]);
	}

	delete[] g_threadHandleAry;
	delete[] g_threadIocpAry;

	PROCESS_MEMORY_COUNTERS memCounter;
	GetProcessMemoryInfo(GetCurrentProcess(), &memCounter, sizeof(memCounter));

	g_currentReadCallCount = 0;
	g_currentComputeCount = 0;
	g_currentTaskMode = FALSE;
	g_completeFileCount = 0;

	g_testResult.elapsedMS = el * 1000;
	g_testResult.peakMem = memCounter.PeakPagefileUsage;

	return g_testResult;
}

TestResult ThreadSchedule::StartThreadTasksFileMap(TestArgument args)
{
#ifdef _DEBUG
	SERIES_INIT(_T("Main Thread"));
	SPAN_INIT;
#endif

	g_testResult = { 0 };
	g_testArgs = args;

	InitializeSRWLock(&g_srwFileFinish);

	g_globalTaskQueue = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, g_testArgs.threadCount);

	g_threadHandleAry = new HANDLE[g_testArgs.threadCount];
	g_threadIocpAry = new HANDLE[g_testArgs.threadCount];

	// Create threads.
	for (UINT t = 0; t < g_testArgs.threadCount; t++)
	{
		DWORD tid = 0;
		HANDLE threadHandle = CreateThread(NULL, 0, FileMapThreadFunc, NULL, 0, &tid);

		if (threadHandle == NULL)
			ExitProcess(3);

		g_threadHandleAry[t] = threadHandle;
	}

#ifdef _DEBUG
	SPAN_START(0, _T("Loading Time"));
#endif

	TIMER_INIT;
	TIMER_START;

	for (UINT i = 0; i < args.fileCount; i++)
	{
		OVERLAPPED ov = { 0 };
		PostQueuedCompletionStatus(g_globalTaskQueue, 0, i, &ov);
	}

	WaitForMultipleObjects(g_testArgs.threadCount, g_threadHandleAry, TRUE, INFINITE);

	TIMER_STOP;

#ifdef _DEBUG
	SPAN_END;
#endif

	SAFE_CLOSE_HANDLE(g_globalTaskQueue);

	for (UINT t = 0; t < g_testArgs.threadCount; t++)
		SAFE_CLOSE_HANDLE(g_threadHandleAry[t]);

	delete[] g_threadHandleAry;
	delete[] g_threadIocpAry;

	PROCESS_MEMORY_COUNTERS memCounter;
	GetProcessMemoryInfo(GetCurrentProcess(), &memCounter, sizeof(memCounter));

	g_currentReadCallCount = 0;
	g_currentComputeCount = 0;
	g_currentTaskMode = FALSE;
	g_completeFileCount = 0;

	g_testResult.elapsedMS = el * 1000;
	g_testResult.peakMem = memCounter.PeakPagefileUsage;

	return g_testResult;
}

TestResult ThreadSchedule::StartSyncThreadTasks(TestArgument args)
{
#ifdef _DEBUG
	SERIES_INIT(_T("Main Thread"));
	SPAN_INIT;
#endif

	g_testResult = { 0 };
	g_testArgs = args;

	InitializeSRWLock(&g_srwFileFinish);

	g_globalTaskQueue = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, g_testArgs.threadCount);

	g_threadHandleAry = new HANDLE[g_testArgs.threadCount];
	g_threadIocpAry = new HANDLE[g_testArgs.threadCount];

	// Create threads.
	for (UINT t = 0; t < g_testArgs.threadCount; t++)
	{
		DWORD tid = 0;
		HANDLE threadHandle = CreateThread(NULL, 0, SyncThreadFunc, NULL, 0, &tid);

		if (threadHandle == NULL)
			ExitProcess(3);

		g_threadHandleAry[t] = threadHandle;
	}

#ifdef _DEBUG
	SPAN_START(0, _T("Loading Time"));
#endif

	TIMER_INIT;
	TIMER_START;

	for (UINT i = 0; i < args.fileCount; i++)
	{
		OVERLAPPED ov = { 0 };
		PostQueuedCompletionStatus(g_globalTaskQueue, 0, i, &ov);
	}

	WaitForMultipleObjects(g_testArgs.threadCount, g_threadHandleAry, TRUE, INFINITE);

	TIMER_STOP;

#ifdef _DEBUG
	SPAN_END;
#endif

	SAFE_CLOSE_HANDLE(g_globalTaskQueue);

	for (UINT t = 0; t < g_testArgs.threadCount; t++)
		SAFE_CLOSE_HANDLE(g_threadHandleAry[t]);

	delete[] g_threadHandleAry;
	delete[] g_threadIocpAry;

	PROCESS_MEMORY_COUNTERS memCounter;
	GetProcessMemoryInfo(GetCurrentProcess(), &memCounter, sizeof(memCounter));

	g_currentReadCallCount = 0;
	g_currentComputeCount = 0;
	g_currentTaskMode = FALSE;
	g_completeFileCount = 0;

	g_testResult.elapsedMS = el * 1000;
	g_testResult.peakMem = memCounter.PeakPagefileUsage;

	return g_testResult;
}

DWORD ThreadSchedule::GetAlignedByteSize(const PLARGE_INTEGER fileByteSize, const DWORD sectorSize)
{
	return ((fileByteSize->QuadPart / sectorSize) + 1u) * sectorSize;
}

void ThreadSchedule::PostThreadTask(const UINT t, const UINT fid, const UINT threadTaskType)
{
	ThreadTaskArgs* args = static_cast<ThreadTaskArgs*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ThreadTaskArgs)));
	args->FID = fid;

	if (FALSE == PostQueuedCompletionStatus(g_threadIocpAry[t], 0, threadTaskType, reinterpret_cast<LPOVERLAPPED>(args)))
		ExitProcess(9);
}

void ThreadSchedule::PostThreadExit(const UINT t)
{
	if (FALSE == PostQueuedCompletionStatus(g_threadIocpAry[t], 0, g_exitCode, NULL))
		ExitProcess(9);
}

void ThreadSchedule::InsertThreadTaskFront(const UINT t, const UINT* fidAry, const UINT* threadTaskTypeAry, const UINT count)
{
	OVERLAPPED_ENTRY entAry[1000];
	ULONG entRemoved;

	GetQueuedCompletionStatusEx(g_threadIocpAry[t], entAry, 1000, &entRemoved, 0L, FALSE);

	for (UINT i = 0; i < count; i++)
		PostThreadTask(t, fidAry[i], threadTaskTypeAry[i]);

	for (UINT i = 0; i < entRemoved; i++)
	{
		if (entAry[i].lpCompletionKey == g_exitCode)
		{
			PostThreadExit(t);
			continue;
		}

		const ThreadTaskArgs* args = reinterpret_cast<ThreadTaskArgs*>(entAry[i].lpOverlapped);
		PostThreadTask(t, args->FID, static_cast<UINT>(entAry[i].lpCompletionKey));
	}
}