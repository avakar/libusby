#include "../libusbyi.h"
#include "libusb0_win32.h"

int libusby_init(libusby_context ** ctx)
{
	return usbyi_init(ctx, &libusb0_win32_backend);
}

int usbyi_init_os_transfer(usbyi_transfer * trani)
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

void usbyi_clear_os_transfer(usbyi_transfer * trani)
{
	CloseHandle(trani->os_priv.overlapped.hEvent);
	CloseHandle(trani->os_priv.hCompletionEvent);
}

void usbyi_win32_add_transfer(usbyi_transfer * trani)
{
	libusby_context * ctx = trani->ctx;
	usbyi_os_ctx * ctx_priv = &ctx->os_priv;

	EnterCriticalSection(&ctx_priv->ctx_mutex);
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
}

void usbyi_win32_remove_transfer(usbyi_transfer * trani)
{
	libusby_context * ctx = trani->ctx;
	usbyi_os_ctx * ctx_priv = &ctx->os_priv;

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
}

void usbyi_free_handle_list(usbyi_handle_list * handle_list)
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

int usbyi_init_os_ctx(libusby_context * ctx)
{
	usbyi_os_ctx * ctx_priv = &ctx->os_priv;

	ctx_priv->hReaperLock = CreateEvent(NULL, FALSE, TRUE, NULL);
	if (!ctx_priv->hReaperLock)
		return LIBUSBY_ERROR_NO_MEM;

	ctx_priv->hEventLoopStopped = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!ctx_priv->hEventLoopStopped)
	{
		CloseHandle(ctx_priv->hReaperLock);
		return LIBUSBY_ERROR_NO_MEM;
	}

	ctx_priv->hTransferListUpdated = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!ctx_priv->hTransferListUpdated)
	{
		CloseHandle(ctx_priv->hEventLoopStopped);
		CloseHandle(ctx_priv->hReaperLock);
		return LIBUSBY_ERROR_NO_MEM;
	}

	InitializeCriticalSection(&ctx_priv->ctx_mutex);
	return LIBUSBY_SUCCESS;
}

void usbyi_clear_os_ctx(libusby_context * ctx)
{
	DeleteCriticalSection(&ctx->os_priv.ctx_mutex);
	CloseHandle(ctx->os_priv.hTransferListUpdated);
	CloseHandle(ctx->os_priv.hReaperLock);
	CloseHandle(ctx->os_priv.hEventLoopStopped);
	usbyi_free_handle_list(&ctx->os_priv.handle_list);
}

static int usbyi_win32_reap_until_locked(libusby_context * ctx, HANDLE hTarget)
{
	usbyi_os_ctx * ctx_priv = &ctx->os_priv;

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
				ctx->backend->reap_transfer(trani_cur);
				break;
			}
		}
	}
}

static int usbyi_win32_reap_until(libusby_context * ctx, HANDLE hTarget)
{
	usbyi_os_ctx * ctx_priv = &ctx->os_priv;

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
	return usbyi_win32_reap_until(ctx, ctx->os_priv.hEventLoopStopped);
}

void libusby_stop_event_loop(libusby_context * ctx)
{
	SetEvent(ctx->os_priv.hEventLoopStopped);
}

void libusby_reset_event_loop(libusby_context * ctx)
{
	ResetEvent(ctx->os_priv.hEventLoopStopped);
}
