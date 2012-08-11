#include "../libspy.h"
#include "spy.h"
#include "libpolly_win32.h"
#include <windows.h>
#include <setupapi.h>
#include <DEVPKEY.H>
#include <assert.h>

typedef BOOL WINAPI CancelIoEx_t(HANDLE hFile, LPOVERLAPPED lpOverlapped);
typedef BOOL WINAPI CancelSynchronousIo_t(HANDLE hThread);

struct spyb_context
{
	libspy_context pub;
	HMODULE hKernel32;
	CancelIoEx_t * CancelIoEx;
	CancelSynchronousIo_t * CancelSynchronousIo;
};

struct libspy_device_handle
{
	spyb_context * ctx;
	HANDLE hFile;
};

struct spyb_transfer
{
	libspy_transfer pub;
	CRITICAL_SECTION transfer_mutex;
	libpolly_task * cancel_task;
	HANDLE hCompletedEvent;
	OVERLAPPED o;
};

struct libspy_open_future
{
	spyb_context * ctx;
	WCHAR * path;
	libspy_open_callback callback;
	void * callback_data;
	HANDLE hOpenThread;
	DWORD dwError;
	DCB dcb;
	int timeout;
};

int const spyb_context_size = sizeof(spyb_context);
int const spyb_transfer_size = sizeof(spyb_transfer);

static WCHAR * to_utf16(char const * utf8)
{
	int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, 0, 0);
	WCHAR * res = malloc(len*2+2);
	if (!res)
		return 0;
	len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, res, len+1);
	res[len] = 0;
	return res;
}

static char * to_utf8(WCHAR const * utf16)
{
	int src_len = wcslen(utf16);
	int buf_size = WideCharToMultiByte(CP_UTF8, 0, utf16, src_len, 0, 0, 0, 0);
	char * res = malloc(buf_size+1);
	if (!res)
		return 0;

	buf_size = WideCharToMultiByte(CP_UTF8, 0, utf16, src_len, res, buf_size, 0, 0);
	res[buf_size] = 0;
	return res;
}

int spyb_init(spyb_context * ctx)
{
	ctx->hKernel32 = LoadLibraryW(L"kernel32.dll");
	if (ctx->hKernel32)
	{
		ctx->CancelIoEx = (CancelIoEx_t *)GetProcAddress(ctx->hKernel32, "CancelIoEx");
		ctx->CancelSynchronousIo = (CancelSynchronousIo_t *)GetProcAddress(ctx->hKernel32, "CancelSynchronousIo");
	}

	return LIBSPY_SUCCESS;
}

void spyb_exit(spyb_context * ctx)
{
	if (ctx->hKernel32)
		FreeLibrary(ctx->hKernel32);
}

void spyi_cancel_transfer_callback(void * user_data)
{
	spyb_transfer * transfer = user_data;
	EnterCriticalSection(&transfer->transfer_mutex);
	if (transfer->pub.handle)
		CancelIo(transfer->pub.handle->hFile);
	LeaveCriticalSection(&transfer->transfer_mutex);
}

void spyb_cancel_transfer(spyb_transfer * transfer)
{
	EnterCriticalSection(&transfer->transfer_mutex);
	if (transfer->pub.handle)
	{
		transfer->pub.flags &= ~spyi_tf_resubmit;
		if (transfer->pub.ctx->CancelIoEx)
		{
			transfer->pub.ctx->CancelIoEx(transfer->pub.handle->hFile, &transfer->o);
		}
		else if (transfer->cancel_task)
		{
			libpolly_submit_task(transfer->cancel_task, &spyi_cancel_transfer_callback, transfer);
			transfer->cancel_task = 0;
		}
	}
	LeaveCriticalSection(&transfer->transfer_mutex);
}

void libspy_close(libspy_device_handle * handle)
{
	CloseHandle(handle->hFile);
	free(handle);
}

int libspy_write(libspy_device_handle * handle, uint8_t const * buf, int len)
{
	DWORD dwTransferred;
	OVERLAPPED o = {0};

	o.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!o.hEvent)
		return LIBSPY_ERROR_NO_MEM;

	if (!WriteFile(handle->hFile, buf, len, &dwTransferred, &o))
	{
		DWORD dwError = GetLastError();
		if (dwError == ERROR_IO_PENDING)
		{
			if (!GetOverlappedResult(handle->hFile, &o, &dwTransferred, TRUE))
				dwError = GetLastError();
			else
				dwError = 0;
		}

		if (dwError)
		{
			CloseHandle(o.hEvent);
			return LIBSPY_ERROR_NO_MEM; // XXX
		}
	}

	CloseHandle(o.hEvent);
	return dwTransferred;
}

int spyb_init_transfer(spyb_transfer * transfer)
{
	transfer->hCompletedEvent = CreateEvent(NULL, TRUE, TRUE, NULL);
	if (!transfer->hCompletedEvent)
		return LIBSPY_ERROR_NO_MEM;

	transfer->o.hEvent = CreateEvent(NULL, TRUE, TRUE, NULL);
	if (!transfer->o.hEvent)
	{
		CloseHandle(transfer->hCompletedEvent);
		return LIBSPY_ERROR_NO_MEM;
	}

	InitializeCriticalSection(&transfer->transfer_mutex);
	return LIBSPY_SUCCESS;
}

void spyb_destroy_transfer(spyb_transfer * transfer)
{
	DeleteCriticalSection(&transfer->transfer_mutex);
	CloseHandle(transfer->o.hEvent);
	CloseHandle(transfer->hCompletedEvent);
}

void spyb_wait_for_transfer(spyb_transfer * transfer)
{
	WaitForSingleObject(transfer->hCompletedEvent, INFINITE);
}

int libspy_get_device_list(libspy_context * ctx, libspy_device const ** device_list)
{
	static GUID const guid_comport = { 0x86e0d1e0L, 0x8089, 0x11d0, 0x9c, 0xe4, 0x08, 0x00, 0x3e, 0x30, 0x1f, 0x73 };

	libspy_device * res = 0;
	int res_size = 0;
	int res_capacity = 0;
	int r = LIBSPY_SUCCESS;

	HDEVINFO hDevInfoSet = SetupDiGetClassDevsA(&guid_comport, 0, 0, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
	int i;

	(void *)ctx;

	for (i = 0;; ++i)
	{
		char * portName8 = 0;
		char * friendlyName8 = 0;
		char * path8 = 0;
		HKEY hKey;
		DWORD dwSize;
		SP_DEVINFO_DATA sdd = {0};

		sdd.cbSize = sizeof sdd;
		if (!SetupDiEnumDeviceInfo(hDevInfoSet, i, &sdd))
			break;

		if (!SetupDiGetDeviceRegistryPropertyW(hDevInfoSet, &sdd, SPDRP_FRIENDLYNAME, 0, 0, 0, &dwSize)
			&& GetLastError() == ERROR_INSUFFICIENT_BUFFER)
		{
			WCHAR * friendlyName16 = malloc(dwSize);
			if (friendlyName16 && SetupDiGetDeviceRegistryPropertyW(hDevInfoSet, &sdd, SPDRP_FRIENDLYNAME, 0, (PBYTE)friendlyName16, dwSize, &dwSize))
				friendlyName8 = to_utf8(friendlyName16);
			free(friendlyName16);
		}

		hKey = SetupDiOpenDevRegKey(hDevInfoSet, &sdd, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_QUERY_VALUE);
		if (hKey)
		{
			DWORD dwType;
			if (RegQueryValueExW(hKey, L"PortName", 0, &dwType, 0, &dwSize) == ERROR_SUCCESS
				&& dwType == REG_SZ)
			{
				WCHAR * portName16 = malloc(dwSize);
				if (portName16 && RegQueryValueExW(hKey, L"PortName", 0, &dwType, (LPBYTE)portName16, &dwSize) == ERROR_SUCCESS
					&& dwType == REG_SZ)
				{
					portName8 = to_utf8(portName16);
				}
				free(portName16);
			}

			RegCloseKey(hKey);
		}

		if (res_size + 1 >= res_capacity)
		{
			int new_capacity = res_capacity? res_capacity * 2: 4;
			libspy_device * new_res = realloc(res, new_capacity*sizeof(libspy_device));
			if (!new_res)
			{
				r = LIBSPY_ERROR_NO_MEM;
				goto out;
			}
			res = new_res;

			memset(res + res_capacity, 0, (new_capacity - res_capacity)*sizeof(libspy_device));
			res_capacity = new_capacity;
		}

		path8 = malloc(strlen(portName8) + 5);
		if (!path8)
		{
			free(friendlyName8);
			free(portName8);
			r = LIBSPY_ERROR_NO_MEM;
			goto out;
		}

		strcpy(path8, "\\\\.\\");
		strcat(path8, portName8);
		res[res_size].path = path8;
		res[res_size].friendly_name = friendlyName8;
		res[res_size].name = portName8;
		++res_size;
	}

	SetupDiDestroyDeviceInfoList(hDevInfoSet);
	*device_list = res;
	return res_size;

out:
	SetupDiDestroyDeviceInfoList(hDevInfoSet);
	libspy_free_device_list(res);
	return r;
}

static void spyi_submit_read_callback(void * user_data);

static void spyb_complete_transfer_locked(spyb_transfer * transfer)
{
	if (transfer->cancel_task)
	{
		libpolly_cancel_task(transfer->cancel_task);
		transfer->cancel_task = 0;
	}
	SetEvent(transfer->hCompletedEvent);
}

static void spyb_read_completed(HANDLE handle, void * user_data)
{
	spyb_transfer * transfer = user_data;

	DWORD dwTransferred;
	if (!GetOverlappedResult(handle, &transfer->o, &dwTransferred, TRUE))
	{
		DWORD dwError = GetLastError();
		if (transfer->pub.callback)
			transfer->pub.callback(&transfer->pub, dwError == ERROR_CANCELLED? libspy_transfer_canceled: libspy_transfer_error);
	}
	else
	{
		transfer->pub.actual_length = dwTransferred;
		if (transfer->pub.callback)
			transfer->pub.callback(&transfer->pub, libspy_transfer_completed);
	}

	EnterCriticalSection(&transfer->transfer_mutex);
	if (transfer->pub.flags & spyi_tf_resubmit)
		spyi_submit_read_callback(transfer);
	else
		spyb_complete_transfer_locked(transfer);
	LeaveCriticalSection(&transfer->transfer_mutex);
}

static void spyi_submit_read_callback(void * user_data)
{
	spyb_transfer * transfer = user_data;
	spyb_context * ctx = transfer->pub.ctx;
	DWORD dwTransferred;
	if (!ReadFile(transfer->pub.handle->hFile, transfer->pub.buf, transfer->pub.length, &dwTransferred, &transfer->o))
	{
		DWORD dwError = GetLastError();
		if (dwError != ERROR_IO_PENDING)
		{
			if (transfer->pub.callback)
				transfer->pub.callback(&transfer->pub, libspy_transfer_error);
		}
		else
		{
			if (libpolly_win32_add_handle(ctx->pub.polly, transfer->o.hEvent, &spyb_read_completed, transfer) < 0)
			{
				CancelIo(transfer->pub.handle->hFile);
				if (!GetOverlappedResult(transfer->pub.handle->hFile, &transfer->o, &dwTransferred, TRUE))
				{
					if (transfer->pub.callback)
						transfer->pub.callback(&transfer->pub, libspy_transfer_error);
				}
				else
				{
					transfer->pub.actual_length = dwTransferred;
					if (transfer->pub.callback)
						transfer->pub.callback(&transfer->pub, libspy_transfer_completed);
				}
				EnterCriticalSection(&transfer->transfer_mutex);
				spyb_complete_transfer_locked(transfer);
				LeaveCriticalSection(&transfer->transfer_mutex);
			}
		}
	}
	else
	{
		transfer->pub.actual_length = dwTransferred;
		if (transfer->pub.callback)
			transfer->pub.callback(&transfer->pub, libspy_transfer_completed);

		EnterCriticalSection(&transfer->transfer_mutex);
		if (transfer->pub.flags & spyi_tf_resubmit)
		{
			int r = libpolly_submit_task_direct(transfer->pub.ctx->pub.polly, &spyi_submit_read_callback, transfer);
			if (r < 0)
			{
				LeaveCriticalSection(&transfer->transfer_mutex);
				if (transfer->pub.callback)
					transfer->pub.callback(&transfer->pub, libspy_transfer_error);
				EnterCriticalSection(&transfer->transfer_mutex);
				spyb_complete_transfer_locked(transfer);
			}
		}
		else
		{
			spyb_complete_transfer_locked(transfer);
		}
		LeaveCriticalSection(&transfer->transfer_mutex);
	}
}

int spyb_submit_read(spyb_transfer * transfer)
{
	int r;

	EnterCriticalSection(&transfer->transfer_mutex);
	assert(!transfer->cancel_task);
	if (!transfer->pub.ctx->CancelIoEx)
	{
		r = libpolly_prepare_task(transfer->pub.ctx->pub.polly, &transfer->cancel_task);
		if (r < 0)
			goto out;
	}

	ResetEvent(transfer->hCompletedEvent);
	r = libpolly_submit_task_direct(transfer->pub.ctx->pub.polly, &spyi_submit_read_callback, transfer);
	if (r < 0)
	{
		SetEvent(transfer->hCompletedEvent);
		if (transfer->cancel_task)
		{
			libpolly_cancel_task(transfer->cancel_task);
			transfer->cancel_task = 0;
		}
	}

out:
	LeaveCriticalSection(&transfer->transfer_mutex);
	return r;
}

static DWORD WINAPI open_thread_proc(LPVOID param)
{
	libspy_open_future * future = param;
	libspy_device_handle * handle = 0;

	HANDLE hFile = CreateFile(future->path, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		future->dwError = GetLastError();
	}
	else
	{
		future->dwError = 0;
		handle = malloc(sizeof(libspy_device_handle));
		if (!handle)
		{
			CloseHandle(hFile);
			future->dwError = ERROR_NOT_ENOUGH_MEMORY;
		}
		else
		{
			handle->ctx = future->ctx;
			handle->hFile = hFile;
		}
	}

	if (handle)
	{
		COMMTIMEOUTS timeouts;
		timeouts.ReadIntervalTimeout = MAXDWORD;
		timeouts.ReadTotalTimeoutConstant = future->timeout;
		timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
		timeouts.WriteTotalTimeoutConstant = 0;
		timeouts.WriteTotalTimeoutMultiplier = 0;
		SetCommTimeouts(handle->hFile, &timeouts);

		SetCommState(handle->hFile, &future->dcb);
	}

	future->callback(future, future->callback_data, handle);
	return 0;
}

int libspy_begin_open(libspy_context * ctx, char const * path, libspy_device_settings const * settings, libspy_open_callback callback, void * user_data, libspy_open_future ** future)
{
	DWORD dwThreadId;
	libspy_open_future * res;

	assert(settings);

	res = malloc(sizeof(libspy_open_future));
	if (!res)
		return LIBUSBY_ERROR_NO_MEM;
	memset(res, 0, sizeof *res);

	res->ctx = (spyb_context *)ctx;
	res->callback = callback;
	res->callback_data = user_data;
	res->path = to_utf16(path);
	if (!res->path)
	{
		free(res);
		return LIBUSBY_ERROR_NO_MEM;
	}

	memset(&res->dcb, 0, sizeof res->dcb);
	res->dcb.DCBlength = sizeof res->dcb;
	res->dcb.BaudRate = settings->baud_rate;
	res->dcb.fBinary = TRUE;
	res->dcb.fParity = (settings->parity != libspy_parity_none);
	res->dcb.ByteSize = settings->bits;
	switch (settings->parity)
	{
	case libspy_parity_none:
		res->dcb.Parity = NOPARITY;
		break;
	case libspy_parity_odd:
		res->dcb.Parity = ODDPARITY;
		break;
	case libspy_parity_even:
		res->dcb.Parity = EVENPARITY;
		break;
	case libspy_parity_mark:
		res->dcb.Parity = MARKPARITY;
		break;
	case libspy_parity_space:
		res->dcb.Parity = SPACEPARITY;
		break;
	}

	switch (settings->stopbits)
	{
	case libspy_stopbits_1:
		res->dcb.StopBits = ONESTOPBIT;
		break;
	case libspy_stopbits_1_5:
		res->dcb.StopBits = ONE5STOPBITS;
		break;
	case libspy_stopbits_2:
		res->dcb.StopBits = TWOSTOPBITS;
		break;
	}

	if (settings->timeout < 0)
		res->timeout = MAXDWORD;
	else
		res->timeout = settings->timeout;

	res->hOpenThread = CreateThread(0, 0, &open_thread_proc, res, 0, &dwThreadId);
	if (!res->hOpenThread)
	{
		free(res->path);
		free(res);
		return LIBUSBY_ERROR_NO_MEM;
	}

	*future = res;
	return LIBUSBY_SUCCESS;
}

int libspy_is_open_cancelable(libspy_context * ctx)
{
	spyb_context * ctxb = (spyb_context *)ctx;
	return ctxb->CancelSynchronousIo != 0;
}

void libspy_cancel_open(libspy_open_future * future)
{
	if (future->ctx->CancelSynchronousIo)
		future->ctx->CancelSynchronousIo(future->hOpenThread);
}

void libspy_wait_for_open(libspy_open_future * future)
{
	WaitForSingleObject(future->hOpenThread, INFINITE);
}

void libspy_free_open_future(libspy_open_future * future)
{
	CloseHandle(future->hOpenThread);
	free(future->path);
	free(future);
}
