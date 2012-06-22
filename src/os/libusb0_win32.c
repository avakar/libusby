#include "libusb0_win32.h"
#include "os.h"
#include "../libusbyi.h"
#include <windows.h>
#include <stdio.h>
#include <assert.h>

#include "libusb0_win32_intf.h"

typedef BOOL WINAPI cancel_io_ex_t(HANDLE hFile, LPOVERLAPPED lpOverlapped);

typedef struct usbyi_handle_list
{
	int capacity;
	HANDLE * handles;
} usbyi_handle_list;

struct usbyb_context
{
	libusby_context pub;

	HMODULE hKernel32;
	cancel_io_ex_t * cancel_io_ex;

	CRITICAL_SECTION ctx_mutex;

	usbyi_transfer * trani_first;
	usbyi_transfer * trani_last;
	int tran_count;

	usbyi_handle_list handle_list;

	HANDLE hReaperLock;
	HANDLE hEventLoopStopped;
	HANDLE hTransferListUpdated;
};

struct usbyb_device
{
	libusby_device pub;
	int devno;
	HANDLE hFile;
};

struct usbyb_device_handle
{
	libusby_device_handle pub;
};

struct usbyb_transfer
{
	usbyi_transfer pub;
	HANDLE hCompletionEvent;
	OVERLAPPED overlapped;
	int submitted;
};

int const usbyb_context_size = sizeof(usbyb_context);
int const usbyb_device_size = sizeof(usbyb_device);
int const usbyb_device_handle_size = sizeof(usbyb_device_handle);
int const usbyb_transfer_size = sizeof(usbyb_transfer);

static int sync_device_io_control(HANDLE hFile, DWORD dwControlCode, void const * in_data, int in_len, void * out_data, int out_len)
{
	DWORD dwTransferred = 0;
	DWORD dwError = ERROR_SUCCESS;

	OVERLAPPED o = {0};
	o.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!o.hEvent)
		return LIBUSBY_ERROR_NO_MEM;

	if (!DeviceIoControl(hFile, dwControlCode, (void *)in_data, in_len, out_data, out_len, &dwTransferred, &o))
	{
		dwError = GetLastError();
		if (dwError == ERROR_IO_PENDING)
		{
			if (!GetOverlappedResult(hFile, &o, &dwTransferred, TRUE))
				dwError = GetLastError();
			else
				dwError = ERROR_SUCCESS;
		}
	}

	CloseHandle(o.hEvent);

	switch (dwError)
	{
	case ERROR_SUCCESS:
		return dwTransferred;
	case ERROR_BUSY:
		return LIBUSBY_ERROR_BUSY;
	default:
		return LIBUSBY_ERROR_IO;
	}
}

int usbyb_init(usbyb_context * ctx)
{
	ctx->hKernel32 = LoadLibraryW(L"kernel32.dll");
	if (!ctx->hKernel32)
		return LIBUSBY_ERROR_INVALID_PARAM;
	ctx->cancel_io_ex = (cancel_io_ex_t *)GetProcAddress(ctx->hKernel32, "CancelIoEx");

	ctx->hReaperLock = CreateEvent(NULL, FALSE, TRUE, NULL);
	if (!ctx->hReaperLock)
		goto error;

	ctx->hEventLoopStopped = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!ctx->hEventLoopStopped)
		goto error;

	ctx->hTransferListUpdated = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!ctx->hTransferListUpdated)
		goto error;

	InitializeCriticalSection(&ctx->ctx_mutex);
	return LIBUSBY_SUCCESS;

error:
	if (ctx->hEventLoopStopped)
		CloseHandle(ctx->hEventLoopStopped);
	if (ctx->hReaperLock)
		CloseHandle(ctx->hReaperLock);
	FreeLibrary(ctx->hKernel32);
	return LIBUSBY_ERROR_NO_MEM;
}

void usbyb_exit(usbyb_context * ctx)
{
	DeleteCriticalSection(&ctx->ctx_mutex);
	CloseHandle(ctx->hEventLoopStopped);
	CloseHandle(ctx->hReaperLock);
	FreeLibrary(ctx->hKernel32);
}

static int usbyb_get_descriptor_with_handle(HANDLE hFile, uint8_t desc_type, uint8_t desc_index, unsigned char * data, int length)
{
	libusb0_win32_request req = {0};
	req.descriptor.type = desc_type;
	req.descriptor.index = desc_index;
	return sync_device_io_control(hFile, LIBUSB_IOCTL_GET_DESCRIPTOR, &req, sizeof req, data, length);
}

int usbyb_get_descriptor_cached(usbyb_device * dev, uint8_t desc_type, uint8_t desc_index, unsigned char * data, int length)
{
	return usbyb_get_descriptor_with_handle(dev->hFile, desc_type, desc_index, data, length);
}

int usbyb_get_descriptor(usbyb_device_handle * dev_handle, uint8_t desc_type, uint8_t desc_index, unsigned char * data, int length)
{
	return usbyb_get_descriptor_cached((usbyb_device *)dev_handle->pub.dev, desc_type, desc_index, data, length);
}

int usbyb_get_configuration(usbyb_device_handle * dev_handle, int * config_value, int cached_only)
{
	usbyb_device * dev = (usbyb_device *)dev_handle->pub.dev;
	libusb0_win32_request req = {0};
	uint8_t res;

	int r = sync_device_io_control(dev->hFile, LIBUSB_IOCTL_GET_CACHED_CONFIGURATION, &req, sizeof req, &res, sizeof res);
	if (r != 1 && !cached_only)
		r = sync_device_io_control(dev->hFile, LIBUSB_IOCTL_GET_CONFIGURATION, &req, sizeof req, &res, sizeof res);

	if (r >= 0)
		*config_value = res;
	return r;
}

int usbyb_set_configuration(usbyb_device_handle * dev_handle, int config_value)
{
	usbyb_device * dev = (usbyb_device *)dev_handle->pub.dev;
	libusb0_win32_request req = {0};

	req.configuration.configuration = config_value;
	return sync_device_io_control(dev->hFile, LIBUSB_IOCTL_SET_CONFIGURATION, &req, sizeof req, 0, 0);
}

int usbyb_get_device_list(usbyb_context * ctx, libusby_device *** list)
{
	usbyi_device_list devlist = {0};
	int i;
	for (i = 1; i < LIBUSB_MAX_NUMBER_OF_DEVICES; ++i)
	{
		int j;
		HANDLE hFile;

		WCHAR devname[32];
		wsprintf(devname, L"\\\\.\\libusb0-%04d", i);

		hFile = CreateFileW(devname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
		if (hFile == INVALID_HANDLE_VALUE)
			continue;

		for (j = 0; j < ctx->pub.devices.count; ++j)
		{
			usbyb_device * dev = (usbyb_device *)ctx->pub.devices.list[j];

			if (dev->devno == i)
			{
				if (usbyi_append_device_list(&devlist, ctx->pub.devices.list[j]) < 0)
					goto error;
				libusby_ref_device(ctx->pub.devices.list[j]);
				break;
			}
		}

		if (j == ctx->pub.devices.count)
		{
			uint8_t cached_desc[sizeof(libusby_device_descriptor)];
			usbyb_device * newdev = usbyi_alloc_device(&ctx->pub);

			if (!newdev)
				continue;

			if (usbyb_get_descriptor_with_handle(hFile, 1, 0, cached_desc, sizeof cached_desc) != sizeof cached_desc
				|| usbyi_sanitize_device_desc(&newdev->pub.device_desc, cached_desc) < 0
				|| usbyi_append_device_list(&devlist, &newdev->pub) < 0)
			{
				libusby_unref_device(&newdev->pub);
				CloseHandle(hFile);
			}
			else
			{
				newdev->devno = i;
				newdev->hFile = hFile;
			}
		}
	}

	*list = devlist.list;
	return devlist.count;

error:
	if (devlist.list)
		libusby_free_device_list(devlist.list, 1);
	return LIBUSBY_ERROR_NO_MEM;
}

int usbyb_claim_interface(usbyb_device_handle * dev_handle, int interface_number)
{
	usbyb_device * dev = dev_handle->pub.dev;

	libusb0_win32_request req = {0};
	req.intf.interface_number = interface_number;

	return sync_device_io_control(dev->hFile, LIBUSB_IOCTL_CLAIM_INTERFACE, &req, sizeof req, 0, 0);
}

int usbyb_release_interface(usbyb_device_handle * dev_handle, int interface_number)
{
	usbyb_device * dev = dev_handle->pub.dev;

	libusb0_win32_request req = {0};
	req.intf.interface_number = interface_number;

	return sync_device_io_control(dev->hFile, LIBUSB_IOCTL_RELEASE_INTERFACE, &req, sizeof req, 0, 0);
}

static void usbyb_update_finished_transfer(libusby_transfer * tran, DWORD dwError, DWORD dwTransferred)
{
	switch (dwError)
	{
	case ERROR_SUCCESS:
		if (tran->type == LIBUSBY_TRANSFER_TYPE_CONTROL)
			tran->actual_length = dwTransferred + 8;
		else
			tran->actual_length = dwTransferred;
		tran->status = LIBUSBY_TRANSFER_COMPLETED;
		break;

	case ERROR_OPERATION_ABORTED:
		tran->actual_length = 0;
		tran->status = LIBUSBY_TRANSFER_CANCELLED;
		break;

	default:
		tran->actual_length = 0;
		tran->status = LIBUSBY_TRANSFER_ERROR;
	}
}

static int usbyb_prepare_transfer_submission(libusby_transfer * tran, DWORD * dwControlCode, libusb0_win32_request * req, uint8_t ** data_ptr, int * data_len)
{
	if (tran->type == LIBUSBY_TRANSFER_TYPE_BULK || tran->type == LIBUSBY_TRANSFER_TYPE_INTERRUPT)
	{
		req->endpoint.endpoint = tran->endpoint;
		if (tran->endpoint & 0x80)
			*dwControlCode = LIBUSB_IOCTL_INTERRUPT_OR_BULK_READ;
		else
			*dwControlCode = LIBUSB_IOCTL_INTERRUPT_OR_BULK_WRITE;
		*data_ptr = tran->buffer;
		*data_len = tran->length;
	}
	else if (tran->type == LIBUSBY_TRANSFER_TYPE_CONTROL)
	{
		if (tran->length < 8)
			return LIBUSBY_ERROR_INVALID_PARAM;

		*data_ptr = tran->buffer + 8;
		*data_len = tran->length - 8;

		if (tran->buffer[0] & 0x80)
			*dwControlCode = LIBUSB_IOCTL_CONTROL_READ;
		else
			*dwControlCode = LIBUSB_IOCTL_CONTROL_WRITE;

		req->control.bmRequestType = tran->buffer[0];
		req->control.bRequest = tran->buffer[1];
		req->control.wValue = tran->buffer[2] | (tran->buffer[3] << 8);
		req->control.wIndex = tran->buffer[4] | (tran->buffer[5] << 8);
		req->control.wLength = tran->buffer[6] | (tran->buffer[7] << 8);

		if (req->control.wLength > *data_len)
			return LIBUSBY_ERROR_INVALID_PARAM;
	}
	else
	{
		return LIBUSBY_ERROR_INVALID_PARAM;
	}

	return LIBUSBY_SUCCESS;
}

int usbyb_perform_transfer(libusby_transfer * tran)
{
	usbyi_transfer * trani = usbyi_tran_to_trani(tran);
	usbyb_device * dev = tran->dev_handle->dev;

	DWORD dwControlCode = 0;
	libusb0_win32_request req = {0};
	uint8_t * data_ptr = 0;
	int data_len = 0;
	DWORD dwTransferred;
	DWORD dwError = ERROR_SUCCESS;

	int r = usbyb_prepare_transfer_submission(tran, &dwControlCode, &req, &data_ptr, &data_len);
	if (r < 0)
		return r;

	if (!DeviceIoControl(dev->hFile, dwControlCode, &req, sizeof req, data_ptr, data_len, &dwTransferred, &trani->os_priv.overlapped))
	{
		dwError = GetLastError();
		if (dwError == ERROR_IO_PENDING)
		{
			if (!GetOverlappedResult(dev->hFile, &trani->os_priv.overlapped, &dwTransferred, TRUE))
				dwError = GetLastError();
			else
				dwError = ERROR_SUCCESS;
		}
	}

	usbyb_update_finished_transfer(tran, dwError, dwTransferred);
	if (tran->callback)
		tran->callback(tran);
	return LIBUSBY_SUCCESS;
}

int usbyb_submit_transfer(libusby_transfer * tran)
{
	usbyi_transfer * trani = usbyi_tran_to_trani(tran);
	usbyb_device * dev = tran->dev_handle->dev;
	libusby_context * ctx = trani->ctx;
	usbyb_context * ctx_priv = container_of(ctx, usbyb_context, pub);

	DWORD dwControlCode = 0;
	DWORD dwTransferred;
	BOOL res;

	/* Note that we don't need to keep this around during the whole operation. It is copied
	 * by the kernel into a temporary buffer. */
	libusb0_win32_request req = {0};
	uint8_t * data_ptr = 0;
	int data_len = 0;

	int r = usbyb_prepare_transfer_submission(tran, &dwControlCode, &req, &data_ptr, &data_len);
	if (r < 0)
		return r;

	EnterCriticalSection(&ctx_priv->ctx_mutex);

	res = DeviceIoControl(dev->hFile, dwControlCode, &req, sizeof req, data_ptr, data_len, &dwTransferred, &trani->os_priv.overlapped);

	/* We might have completed synchronously, we still have to reap asynchronously though. */
	if (!res && GetLastError() != ERROR_IO_PENDING)
	{
		LeaveCriticalSection(&ctx_priv->ctx_mutex);
		return LIBUSBY_ERROR_IO;
	}

	ResetEvent(trani->os_priv.hCompletionEvent);

	trani->next = 0;
	trani->prev = ctx_priv->trani_last;
	if (ctx_priv->trani_last)
		ctx_priv->trani_last->next = trani;
	ctx_priv->trani_last = trani;
	if (!ctx_priv->trani_first)
		ctx_priv->trani_first = trani;
	++ctx_priv->tran_count;
	trani->os_priv.submitted = 1;
	SetEvent(ctx_priv->hTransferListUpdated);

	LeaveCriticalSection(&ctx_priv->ctx_mutex);
	return LIBUSBY_SUCCESS;
}

int usbyb_cancel_transfer(libusby_transfer * tran)
{
	usbyi_transfer * trani = usbyi_tran_to_trani(tran);
	usbyb_context * ctxpriv = container_of(trani->ctx, usbyb_context, pub);

	EnterCriticalSection(&ctxpriv->ctx_mutex);
	if (trani->os_priv.submitted)
	{
		usbyb_device * dev;
		assert(tran->dev_handle);

		dev = tran->dev_handle->dev;

		if (ctxpriv->cancel_io_ex)
		{
			ctxpriv->cancel_io_ex(dev->hFile, &trani->os_priv.overlapped);
		}
		else
		{
			libusb0_win32_request req = {0};
			req.endpoint.endpoint = tran->endpoint;
			sync_device_io_control(dev->hFile, LIBUSB_IOCTL_ABORT_ENDPOINT, &req, sizeof req, 0, 0);
		}
	}
	LeaveCriticalSection(&ctxpriv->ctx_mutex);

	return LIBUSBY_SUCCESS;
}

void usbyb_reap_transfer(usbyi_transfer * trani)
{
	libusby_transfer * tran = usbyi_trani_to_tran(trani);
	usbyi_os_transfer * tranos = &trani->os_priv;
	usbyb_device * dev = tran->dev_handle->dev;
	libusby_context * ctx = trani->ctx;
	usbyb_context * ctx_priv = container_of(ctx, usbyb_context, pub);

	DWORD dwTransferred;
	BOOL res = GetOverlappedResult(dev->hFile, &trani->os_priv.overlapped, &dwTransferred, TRUE);

	usbyb_update_finished_transfer(tran,  res? ERROR_SUCCESS: GetLastError(), dwTransferred);

	EnterCriticalSection(&ctx_priv->ctx_mutex);
	if (trani->next)
		trani->next->prev = trani->prev;
	else
		ctx_priv->trani_last = trani->prev;

	if (trani->prev)
		trani->prev->next = trani->next;
	else
		ctx_priv->trani_first = trani->next;

	trani->next = 0;
	trani->prev = 0;
	--ctx_priv->tran_count;

	trani->os_priv.submitted = 0;
	LeaveCriticalSection(&ctx_priv->ctx_mutex);

	if (tran->callback)
		tran->callback(tran);

	EnterCriticalSection(&ctx_priv->ctx_mutex);
	if (!tranos->submitted)
		SetEvent(tranos->hCompletionEvent);
	LeaveCriticalSection(&ctx_priv->ctx_mutex);
}

int usbyb_open(usbyb_device_handle * dev_handle)
{
	(void)dev_handle;
	return LIBUSBY_SUCCESS;
}

void usbyb_close(usbyb_device_handle * dev_handle)
{
	(void)dev_handle;
}

int usbyb_init_transfer(usbyi_transfer * trani)
{
	trani->os_priv.hCompletionEvent = CreateEvent(NULL, TRUE, TRUE, NULL);
	if (!trani->os_priv.hCompletionEvent)
		return LIBUSBY_ERROR_NO_MEM;

	trani->os_priv.overlapped.hEvent = CreateEvent(NULL, TRUE, TRUE, NULL);
	if (!trani->os_priv.overlapped.hEvent)
	{
		CloseHandle(trani->os_priv.hCompletionEvent);
		return LIBUSBY_ERROR_NO_MEM;
	}

	trani->os_priv.submitted = 0;
	return LIBUSBY_SUCCESS;
}

void usbyb_clear_transfer(usbyi_transfer * trani)
{
	CloseHandle(trani->os_priv.overlapped.hEvent);
	CloseHandle(trani->os_priv.hCompletionEvent);
}

static void usbyi_free_handle_list(usbyi_handle_list * handle_list)
{
	free(handle_list->handles);
	handle_list->handles = 0;
	handle_list->capacity = 0;
}

static int usbyi_reserve_handle_list(usbyi_handle_list * handle_list, int new_capacity)
{
	if (new_capacity < 4)
		new_capacity = 4;

	if (handle_list->capacity / 4 > new_capacity || handle_list->capacity < new_capacity)
	{
		HANDLE * new_handles;

		new_capacity = new_capacity * 3 / 2;
		new_handles = realloc(handle_list->handles, new_capacity * sizeof(HANDLE));
		if (!new_handles)
			return LIBUSBY_ERROR_NO_MEM;

		handle_list->capacity = new_capacity;
		handle_list->handles = new_handles;
	}

	return LIBUSBY_SUCCESS;
}

static int usbyi_win32_reap_until_locked(libusby_context * ctx, HANDLE hTarget)
{
	usbyb_context * ctx_priv = container_of(ctx, usbyb_context, pub);

	for (;;)
	{
		int handle_count = 2;
		int i;
		usbyi_transfer * trani_cur;
		HANDLE h;
		DWORD res;

		EnterCriticalSection(&ctx_priv->ctx_mutex);
		res = usbyi_reserve_handle_list(&ctx_priv->handle_list, 2 + ctx_priv->tran_count);
		if (res < 0)
		{
			LeaveCriticalSection(&ctx_priv->ctx_mutex);
			return res;
		}

		ctx_priv->handle_list.handles[0] = hTarget;
		ctx_priv->handle_list.handles[1] = ctx_priv->hTransferListUpdated;

		trani_cur = ctx_priv->trani_first;
		for (i = 0; i < ctx_priv->tran_count && trani_cur != NULL; ++i, trani_cur = trani_cur->next)
		{
			if (trani_cur->os_priv.overlapped.hEvent != hTarget)
				ctx_priv->handle_list.handles[handle_count++] = trani_cur->os_priv.overlapped.hEvent;
		}

		ResetEvent(ctx_priv->hTransferListUpdated);
		LeaveCriticalSection(&ctx_priv->ctx_mutex);

		res = WaitForMultipleObjects(handle_count, ctx_priv->handle_list.handles, FALSE, INFINITE);
		if (res < WAIT_OBJECT_0 && res >= WAIT_OBJECT_0 + handle_count)
			return LIBUSBY_ERROR_IO;

		if (res == WAIT_OBJECT_0)
			return LIBUSBY_SUCCESS;

		if (res == WAIT_OBJECT_0 + 1)
			continue;

		h = ctx_priv->handle_list.handles[res - WAIT_OBJECT_0];
		trani_cur = ctx_priv->trani_first;
		for (i = 0; i < ctx_priv->tran_count && trani_cur != NULL; ++i, trani_cur = trani_cur->next)
		{
			if (trani_cur->os_priv.overlapped.hEvent == h)
			{
				usbyb_reap_transfer(trani_cur);
				break;
			}
		}
	}
}

static int usbyi_win32_reap_until(libusby_context * ctx, HANDLE hTarget)
{
	usbyb_context * ctx_priv = container_of(ctx, usbyb_context, pub);

	HANDLE handles[2] = { hTarget, ctx_priv->hReaperLock };
	int res = WaitForMultipleObjects(2, handles, FALSE, INFINITE);

	switch (res)
	{
	case WAIT_OBJECT_0:
		return LIBUSBY_SUCCESS;
	case WAIT_OBJECT_0 + 1:
		break;
	default:
		return LIBUSBY_ERROR_IO;
	}

	res = usbyi_win32_reap_until_locked(ctx, hTarget);
	SetEvent(ctx_priv->hReaperLock);
	return res;
}

int libusby_wait_for_transfer(libusby_transfer * transfer)
{
	usbyi_transfer * trani = usbyi_tran_to_trani(transfer);
	return usbyi_win32_reap_until(trani->ctx, trani->os_priv.hCompletionEvent);
}

int libusby_run_event_loop(libusby_context * ctx)
{
	usbyb_context * ctxpriv = container_of(ctx, usbyb_context, pub);
	return usbyi_win32_reap_until(ctx, ctxpriv->hEventLoopStopped);
}

void libusby_stop_event_loop(libusby_context * ctx)
{
    usbyb_context * ctxpriv = container_of(ctx, usbyb_context, pub);
	SetEvent(ctxpriv->hEventLoopStopped);
}

void libusby_reset_event_loop(libusby_context * ctx)
{
    usbyb_context * ctxpriv = container_of(ctx, usbyb_context, pub);
	ResetEvent(ctxpriv->hEventLoopStopped);
}
