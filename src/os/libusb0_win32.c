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

struct libusby_context
{
	usbyi_device_list_node devlist_head;

	HMODULE hKernel32;
	cancel_io_ex_t * cancel_io_ex;

	CRITICAL_SECTION ctx_mutex;

	usbyb_transfer * trani_first;
	usbyb_transfer * trani_last;
	int tran_count;

	usbyi_handle_list handle_list;

	HANDLE hReaperLock;
	HANDLE hEventLoopStopped;
	HANDLE hTransferListUpdated;
};

struct usbyb_device
{
	libusby_device pub;
	usbyi_device_list_node devnode;
	int devno;
	HANDLE hFile;
};

struct usbyb_device_handle
{
	libusby_device_handle pub;
};

struct usbyb_transfer
{
	usbyi_transfer intrn;
	HANDLE hCompletionEvent;
	OVERLAPPED overlapped;
	int submitted;
	libusby_transfer pub;
};

int const usbyb_context_size = sizeof(usbyb_context);
int const usbyb_device_size = sizeof(usbyb_device);
int const usbyb_device_handle_size = sizeof(usbyb_device_handle);
int const usbyb_transfer_size = sizeof(usbyb_transfer);
int const usbyb_transfer_pub_offset = offsetof(usbyb_transfer, pub);

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
	usbyi_init_devlist_head(&ctx->devlist_head);

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
	if (ctx->hTransferListUpdated)
		CloseHandle(ctx->hTransferListUpdated);
	if (ctx->hEventLoopStopped)
		CloseHandle(ctx->hEventLoopStopped);
	if (ctx->hReaperLock)
		CloseHandle(ctx->hReaperLock);
	FreeLibrary(ctx->hKernel32);
	return LIBUSBY_ERROR_NO_MEM;
}

void usbyb_exit(usbyb_context * ctx)
{
	assert(ctx->devlist_head.next == &ctx->devlist_head);
	DeleteCriticalSection(&ctx->ctx_mutex);
	CloseHandle(ctx->hTransferListUpdated);
	CloseHandle(ctx->hEventLoopStopped);
	CloseHandle(ctx->hReaperLock);
	FreeLibrary(ctx->hKernel32);
}

static int usbyb_get_descriptor_with_handle(HANDLE hFile, uint8_t desc_type, uint8_t desc_index, uint16_t langid, unsigned char * data, int length)
{
	libusb0_win32_request req = {0};
	req.descriptor.type = desc_type;
	req.descriptor.index = desc_index;
	req.descriptor.language_id = langid;
	return sync_device_io_control(hFile, LIBUSB_IOCTL_GET_DESCRIPTOR, &req, sizeof req, data, length);
}

int usbyb_get_descriptor_cached(usbyb_device * dev, uint8_t desc_type, uint8_t desc_index, uint16_t langid, unsigned char * data, int length)
{
	return usbyb_get_descriptor_with_handle(dev->hFile, desc_type, desc_index, langid, data, length);
}

int usbyb_get_descriptor(usbyb_device_handle * dev_handle, uint8_t desc_type, uint8_t desc_index, uint16_t langid, unsigned char * data, int length)
{
	return usbyb_get_descriptor_cached((usbyb_device *)dev_handle->pub.dev, desc_type, desc_index, langid, data, length);
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
		HANDLE hFile;
		usbyi_device_list_node * devnode = 0;

		WCHAR devname[32];
		wsprintf(devname, L"\\\\.\\libusb0-%04d", i);

		hFile = CreateFileW(devname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
		if (hFile == INVALID_HANDLE_VALUE)
			continue;

		EnterCriticalSection(&ctx->ctx_mutex);
		for (devnode = ctx->devlist_head.next; devnode != &ctx->devlist_head; devnode = devnode->next)
		{
			usbyb_device * dev = container_of(devnode, usbyb_device, devnode);

			if (dev->devno == i)
			{
				CloseHandle(hFile);
				if (usbyi_append_device_list(&devlist, &dev->pub) < 0)
					goto error_unlock;
				libusby_ref_device(&dev->pub);
				break;
			}
		}

		if (devnode == &ctx->devlist_head)
		{
			uint8_t cached_desc[sizeof(libusby_device_descriptor)];
			usbyb_device * dev = usbyi_alloc_device(ctx);
			if (!dev)
				goto error_unlock;

			dev->devno = i;
			dev->hFile = hFile;
			usbyi_insert_before_devlist_node(&dev->devnode, &ctx->devlist_head);

			if (usbyb_get_descriptor_with_handle(hFile, 1, 0, 0, cached_desc, sizeof cached_desc) != sizeof cached_desc
				|| usbyi_sanitize_device_desc(&dev->pub.device_desc, cached_desc) < 0
				|| usbyi_append_device_list(&devlist, &dev->pub) < 0)
			{
				LeaveCriticalSection(&ctx->ctx_mutex);
				libusby_unref_device(&dev->pub);
				goto error;
			}
		}

		LeaveCriticalSection(&ctx->ctx_mutex);
	}

	*list = devlist.list;
	return devlist.count;

error_unlock:
	LeaveCriticalSection(&ctx->ctx_mutex);

error:
	if (devlist.list)
		libusby_free_device_list(devlist.list, /*unref_devices=*/1);
	return LIBUSBY_ERROR_NO_MEM;
}

void usbyb_finalize_device(usbyb_device * dev)
{
	usbyb_context * ctx = dev->pub.ctx;

	EnterCriticalSection(&ctx->ctx_mutex);
	usbyi_remove_devlist_node(&dev->devnode);
	LeaveCriticalSection(&ctx->ctx_mutex);

	CloseHandle(dev->hFile);
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

static void usbyb_update_finished_transfer(usbyb_transfer * tran, DWORD dwError, DWORD dwTransferred)
{
	switch (dwError)
	{
	case ERROR_SUCCESS:
		if (tran->pub.type == LIBUSBY_TRANSFER_TYPE_CONTROL)
			tran->pub.actual_length = dwTransferred + 8;
		else
			tran->pub.actual_length = dwTransferred;
		tran->pub.status = LIBUSBY_TRANSFER_COMPLETED;
		break;

	case ERROR_OPERATION_ABORTED:
		tran->pub.actual_length = 0;
		tran->pub.status = LIBUSBY_TRANSFER_CANCELLED;
		break;

	default:
		tran->pub.actual_length = 0;
		tran->pub.status = LIBUSBY_TRANSFER_ERROR;
	}
}

static int usbyb_prepare_transfer_submission(usbyb_transfer * tran, DWORD * dwControlCode, libusb0_win32_request * req, uint8_t ** data_ptr, int * data_len)
{
	if (tran->pub.type == LIBUSBY_TRANSFER_TYPE_BULK || tran->pub.type == LIBUSBY_TRANSFER_TYPE_INTERRUPT)
	{
		req->endpoint.endpoint = tran->pub.endpoint;
		if (tran->pub.endpoint & 0x80)
			*dwControlCode = LIBUSB_IOCTL_INTERRUPT_OR_BULK_READ;
		else
			*dwControlCode = LIBUSB_IOCTL_INTERRUPT_OR_BULK_WRITE;
		*data_ptr = tran->pub.buffer;
		*data_len = tran->pub.length;
	}
	else if (tran->pub.type == LIBUSBY_TRANSFER_TYPE_CONTROL)
	{
		if (tran->pub.length < 8)
			return LIBUSBY_ERROR_INVALID_PARAM;

		*data_ptr = tran->pub.buffer + 8;
		*data_len = tran->pub.length - 8;

		if (tran->pub.buffer[0] & 0x80)
			*dwControlCode = LIBUSB_IOCTL_CONTROL_READ;
		else
			*dwControlCode = LIBUSB_IOCTL_CONTROL_WRITE;

		req->control.bmRequestType = tran->pub.buffer[0];
		req->control.bRequest = tran->pub.buffer[1];
		req->control.wValue = tran->pub.buffer[2] | (tran->pub.buffer[3] << 8);
		req->control.wIndex = tran->pub.buffer[4] | (tran->pub.buffer[5] << 8);
		req->control.wLength = tran->pub.buffer[6] | (tran->pub.buffer[7] << 8);

		if (req->control.wLength > *data_len)
			return LIBUSBY_ERROR_INVALID_PARAM;
	}
	else
	{
		return LIBUSBY_ERROR_INVALID_PARAM;
	}

	return LIBUSBY_SUCCESS;
}

int usbyb_perform_transfer(usbyb_transfer * tran)
{
	usbyb_device * dev = tran->pub.dev_handle->dev;

	DWORD dwControlCode = 0;
	libusb0_win32_request req = {0};
	uint8_t * data_ptr = 0;
	int data_len = 0;
	DWORD dwTransferred;
	DWORD dwError = ERROR_SUCCESS;

	int r = usbyb_prepare_transfer_submission(tran, &dwControlCode, &req, &data_ptr, &data_len);
	if (r < 0)
		return r;

	if (!DeviceIoControl(dev->hFile, dwControlCode, &req, sizeof req, data_ptr, data_len, &dwTransferred, &tran->overlapped))
	{
		dwError = GetLastError();
		if (dwError == ERROR_IO_PENDING)
		{
			if (!GetOverlappedResult(dev->hFile, &tran->overlapped, &dwTransferred, TRUE))
				dwError = GetLastError();
			else
				dwError = ERROR_SUCCESS;
		}
	}

	usbyb_update_finished_transfer(tran, dwError, dwTransferred);
	if (tran->pub.callback)
		tran->pub.callback(&tran->pub);
	return LIBUSBY_SUCCESS;
}

int usbyb_submit_transfer(usbyb_transfer * tran)
{
	usbyb_device * dev = tran->pub.dev_handle->dev;
	usbyb_context * ctx = tran->intrn.ctx;

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

	EnterCriticalSection(&ctx->ctx_mutex);

	res = DeviceIoControl(dev->hFile, dwControlCode, &req, sizeof req, data_ptr, data_len, &dwTransferred, &tran->overlapped);

	/* We might have completed synchronously, we still have to reap asynchronously though. */
	if (!res && GetLastError() != ERROR_IO_PENDING)
	{
		LeaveCriticalSection(&ctx->ctx_mutex);
		return LIBUSBY_ERROR_IO;
	}

	ResetEvent(tran->hCompletionEvent);

	tran->intrn.next = 0;
	tran->intrn.prev = ctx->trani_last;
	if (ctx->trani_last)
		ctx->trani_last->intrn.next = tran;
	ctx->trani_last = tran;
	if (!ctx->trani_first)
		ctx->trani_first = tran;
	++ctx->tran_count;
	tran->submitted = 1;
	SetEvent(ctx->hTransferListUpdated);

	LeaveCriticalSection(&ctx->ctx_mutex);
	return LIBUSBY_SUCCESS;
}

int usbyb_cancel_transfer(usbyb_transfer * tran)
{
	usbyb_context * ctxpriv = tran->intrn.ctx;

	EnterCriticalSection(&ctxpriv->ctx_mutex);
	if (tran->submitted)
	{
		usbyb_device * dev;
		assert(tran->pub.dev_handle);

		dev = tran->pub.dev_handle->dev;

		if (ctxpriv->cancel_io_ex)
		{
			ctxpriv->cancel_io_ex(dev->hFile, &tran->overlapped);
		}
		else
		{
			libusb0_win32_request req = {0};
			req.endpoint.endpoint = tran->pub.endpoint;
			sync_device_io_control(dev->hFile, LIBUSB_IOCTL_ABORT_ENDPOINT, &req, sizeof req, 0, 0);
		}
	}
	LeaveCriticalSection(&ctxpriv->ctx_mutex);

	return LIBUSBY_SUCCESS;
}

void usbyb_reap_transfer(usbyb_transfer * tran)
{
	usbyb_device * dev = tran->pub.dev_handle->dev;
	usbyb_context * ctx = tran->intrn.ctx;

	DWORD dwTransferred;
	BOOL res = GetOverlappedResult(dev->hFile, &tran->overlapped, &dwTransferred, TRUE);

	usbyb_update_finished_transfer(tran,  res? ERROR_SUCCESS: GetLastError(), dwTransferred);

	EnterCriticalSection(&ctx->ctx_mutex);
	if (tran->intrn.next)
		tran->intrn.next->intrn.prev = tran->intrn.prev;
	else
		ctx->trani_last = tran->intrn.prev;

	if (tran->intrn.prev)
		tran->intrn.prev->intrn.next = tran->intrn.next;
	else
		ctx->trani_first = tran->intrn.next;

	tran->intrn.next = 0;
	tran->intrn.prev = 0;
	--ctx->tran_count;

	tran->submitted = 0;
	LeaveCriticalSection(&ctx->ctx_mutex);

	if (tran->pub.callback)
		tran->pub.callback(&tran->pub);

	EnterCriticalSection(&ctx->ctx_mutex);
	if (!tran->submitted)
		SetEvent(tran->hCompletionEvent);
	LeaveCriticalSection(&ctx->ctx_mutex);
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

int usbyb_init_transfer(usbyb_transfer * tran)
{
	tran->hCompletionEvent = CreateEvent(NULL, TRUE, TRUE, NULL);
	if (!tran->hCompletionEvent)
		return LIBUSBY_ERROR_NO_MEM;

	tran->overlapped.hEvent = CreateEvent(NULL, TRUE, TRUE, NULL);
	if (!tran->overlapped.hEvent)
	{
		CloseHandle(tran->hCompletionEvent);
		return LIBUSBY_ERROR_NO_MEM;
	}

	tran->submitted = 0;
	return LIBUSBY_SUCCESS;
}

void usbyb_clear_transfer(usbyb_transfer * tran)
{
	CloseHandle(tran->overlapped.hEvent);
	CloseHandle(tran->hCompletionEvent);
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

static int usbyi_win32_reap_until_locked(usbyb_context * ctx, HANDLE hTarget)
{
	for (;;)
	{
		int handle_count = 2;
		int i;
		usbyb_transfer * trani_cur;
		HANDLE h;
		DWORD res;

		EnterCriticalSection(&ctx->ctx_mutex);
		res = usbyi_reserve_handle_list(&ctx->handle_list, 2 + ctx->tran_count);
		if (res < 0)
		{
			LeaveCriticalSection(&ctx->ctx_mutex);
			return res;
		}

		ctx->handle_list.handles[0] = hTarget;
		ctx->handle_list.handles[1] = ctx->hTransferListUpdated;

		trani_cur = ctx->trani_first;
		for (i = 0; i < ctx->tran_count && trani_cur != NULL; ++i, trani_cur = trani_cur->intrn.next)
		{
			if (trani_cur->overlapped.hEvent != hTarget)
				ctx->handle_list.handles[handle_count++] = trani_cur->overlapped.hEvent;
		}

		ResetEvent(ctx->hTransferListUpdated);
		LeaveCriticalSection(&ctx->ctx_mutex);

		res = WaitForMultipleObjects(handle_count, ctx->handle_list.handles, FALSE, INFINITE);
		if (res < WAIT_OBJECT_0 && res >= WAIT_OBJECT_0 + handle_count)
			return LIBUSBY_ERROR_IO;

		if (res == WAIT_OBJECT_0)
			return LIBUSBY_SUCCESS;

		if (res == WAIT_OBJECT_0 + 1)
			continue;

		h = ctx->handle_list.handles[res - WAIT_OBJECT_0];
		trani_cur = ctx->trani_first;
		for (i = 0; i < ctx->tran_count && trani_cur != NULL; ++i, trani_cur = trani_cur->intrn.next)
		{
			if (trani_cur->overlapped.hEvent == h)
			{
				usbyb_reap_transfer(trani_cur);
				break;
			}
		}
	}
}

static int usbyi_win32_reap_until(usbyb_context * ctx, HANDLE hTarget)
{
	HANDLE handles[2] = { hTarget, ctx->hReaperLock };
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
	SetEvent(ctx->hReaperLock);
	return res;
}

int usbyb_wait_for_transfer(usbyb_transfer * tran)
{
	return usbyi_win32_reap_until(tran->intrn.ctx, tran->hCompletionEvent);
}

int usbyb_run_event_loop(usbyb_context * ctx)
{
	return usbyi_win32_reap_until(ctx, ctx->hEventLoopStopped);
}

void usbyb_stop_event_loop(usbyb_context * ctx)
{
	SetEvent(ctx->hEventLoopStopped);
}

void usbyb_reset_event_loop(usbyb_context * ctx)
{
	ResetEvent(ctx->hEventLoopStopped);
}
