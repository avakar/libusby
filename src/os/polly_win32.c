#include "libpolly_win32.h"
#include <windows.h>
#include <assert.h>

struct pollyi_entry
{
	HANDLE handle;
	libpolly_win32_callback callback;
	void * user_data;
};

struct libpolly_task
{
	libpolly_context * ctx;
	libpolly_task_callback callback;
	void * user_data;
	libpolly_task * next;
};

struct libpolly_context
{
	LONG refcount;

	HANDLE hReaperLock;
	HANDLE hHandleListUpdated;

	CRITICAL_SECTION entry_list_mutex;
	struct pollyi_entry * entry_list;
	int entry_list_size;
	int entry_list_capacity;

	libpolly_task * task_first;
	libpolly_task * task_last;

	HANDLE * handle_list;
	int handle_list_capacity;

	HANDLE hThread;
	HANDLE hEventLoopStopped;
	DWORD dwThreadId;
};

static DWORD WINAPI event_loop_thread_proc(void * param);

int libpolly_init(libpolly_context ** ctx)
{
	libpolly_context * res = malloc(sizeof(libpolly_context));
	if (!res)
		return LIBUSBY_ERROR_NO_MEM;
	memset(res, 0, sizeof(libpolly_context));

	InitializeCriticalSection(&res->entry_list_mutex);
	res->refcount = 1;

	res->hReaperLock = CreateEvent(NULL, FALSE, TRUE, NULL);
	if (!res->hReaperLock)
		goto error;

	res->hHandleListUpdated = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!res->hHandleListUpdated)
		goto error;

	res->hEventLoopStopped = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!res->hEventLoopStopped)
		goto error;

	res->hThread = CreateThread(0, 0, &event_loop_thread_proc, res, 0, &res->dwThreadId);
	if (!res->hThread)
		goto error;

	*ctx = res;
	return LIBUSBY_SUCCESS;

error:
	if (res->hThread)
		CloseHandle(res->hThread);
	if (res->hEventLoopStopped)
		CloseHandle(res->hEventLoopStopped);
	if (res->hHandleListUpdated)
		CloseHandle(res->hHandleListUpdated);
	if (res->hReaperLock)
		CloseHandle(res->hReaperLock);
	DeleteCriticalSection(&res->entry_list_mutex);
	free(res);
	return LIBUSBY_ERROR_NO_MEM;
}

void libpolly_ref_context(libpolly_context * ctx)
{
	InterlockedIncrement(&ctx->refcount);
}

void libpolly_unref_context(libpolly_context * ctx)
{
	if (InterlockedDecrement(&ctx->refcount) == 0)
	{
		assert(ctx->entry_list_size == 0);
		assert(ctx->task_first == 0);
		assert(ctx->task_last == 0);

		SetEvent(ctx->hEventLoopStopped);
		WaitForSingleObject(ctx->hThread, INFINITE);
		CloseHandle(ctx->hThread);

		CloseHandle(ctx->hEventLoopStopped);
		CloseHandle(ctx->hReaperLock);
		CloseHandle(ctx->hHandleListUpdated);
		free(ctx->entry_list);
		free(ctx->handle_list);
		DeleteCriticalSection(&ctx->entry_list_mutex);
		free(ctx);
	}
}

int libpolly_win32_add_handle(libpolly_context * ctx, HANDLE handle, libpolly_win32_callback callback, void * user_data)
{
	EnterCriticalSection(&ctx->entry_list_mutex);

	if (ctx->entry_list_capacity == ctx->entry_list_size)
	{
		int new_capacity = ctx->entry_list_capacity? ctx->entry_list_capacity * 2: 4;
		struct pollyi_entry * new_entry_list = realloc(ctx->entry_list, new_capacity * sizeof(struct pollyi_entry));
		if (!new_entry_list)
		{
			LeaveCriticalSection(&ctx->entry_list_mutex);
			return LIBUSBY_ERROR_NO_MEM;
		}

		ctx->entry_list = new_entry_list;
		ctx->entry_list_capacity = new_capacity;
	}

	assert(ctx->entry_list_capacity > ctx->entry_list_size);
	ctx->entry_list[ctx->entry_list_size].handle = handle;
	ctx->entry_list[ctx->entry_list_size].callback = callback;
	ctx->entry_list[ctx->entry_list_size].user_data = user_data;
	++ctx->entry_list_size;
	SetEvent(ctx->hHandleListUpdated);

	LeaveCriticalSection(&ctx->entry_list_mutex);
	return LIBUSBY_SUCCESS;
}

static int libpolly_win32_wait_until_locked(libpolly_context * ctx, HANDLE handle)
{
	int done = 0;
	while (!done)
	{
		int i;
		DWORD res;
		int viable_entry_count;
		int original_entry_count;
		int selected_entry_index;
		struct pollyi_entry selected_entry;

		EnterCriticalSection(&ctx->entry_list_mutex);
		while (ctx->task_first)
		{
			libpolly_task * task = ctx->task_first;
			ctx->task_first = task->next;
			if (!ctx->task_first)
				ctx->task_last = 0;
			LeaveCriticalSection(&ctx->entry_list_mutex);
			task->callback(task->user_data);
			free(task);
			EnterCriticalSection(&ctx->entry_list_mutex);
		}

		if (ctx->handle_list_capacity < 2 + ctx->entry_list_size)
		{
			int new_capacity = ctx->handle_list_capacity * 2;
			HANDLE * new_handle_list;

			if (new_capacity < 2 + ctx->entry_list_size)
				new_capacity = 2 + ctx->entry_list_size;

			new_handle_list = realloc(ctx->handle_list, new_capacity * sizeof(HANDLE));
			if (!new_handle_list)
			{
				LeaveCriticalSection(&ctx->entry_list_mutex);
				return LIBUSBY_ERROR_NO_MEM;
			}

			ctx->handle_list = new_handle_list;
			ctx->handle_list_capacity = new_capacity;
		}

		assert(ctx->handle_list_capacity >= 2 + ctx->entry_list_size);

		ctx->handle_list[0] = handle;
		ctx->handle_list[1] = ctx->hHandleListUpdated;

		original_entry_count = viable_entry_count = ctx->entry_list_size;
		for (i = 0; i < viable_entry_count; )
		{
			assert(ctx->entry_list[i].handle);
			if (ctx->entry_list[i].handle != handle)
			{
				ctx->handle_list[i+2] = ctx->entry_list[i].handle;
				++i;
			}
			else if (i+1 != viable_entry_count)
			{
				struct pollyi_entry temp = ctx->entry_list[i];
				ctx->entry_list[i] = ctx->entry_list[viable_entry_count-1];
				ctx->entry_list[viable_entry_count-1] = temp;
				--viable_entry_count;
			}
		}

		ResetEvent(ctx->hHandleListUpdated);
		LeaveCriticalSection(&ctx->entry_list_mutex);

		res = WaitForMultipleObjects(2+viable_entry_count, ctx->handle_list, FALSE, INFINITE);
		if (res < WAIT_OBJECT_0 && res >= WAIT_OBJECT_0 + 2+viable_entry_count)
			return LIBUSBY_ERROR_IO;

		if (res == WAIT_OBJECT_0 + 1)
			continue;

		if (res == WAIT_OBJECT_0)
		{
			if (viable_entry_count == original_entry_count)
				return LIBUSBY_SUCCESS;

			selected_entry_index = viable_entry_count;
			done = 1;
		}
		else
		{
			selected_entry_index = res - WAIT_OBJECT_0 - 2;
		}

		EnterCriticalSection(&ctx->entry_list_mutex);
		assert(ctx->entry_list_size > 0 && selected_entry_index < ctx->entry_list_size);

		selected_entry = ctx->entry_list[selected_entry_index];
		ctx->entry_list[selected_entry_index] = ctx->entry_list[ctx->entry_list_size-1];
		--ctx->entry_list_size;
		LeaveCriticalSection(&ctx->entry_list_mutex);

		selected_entry.callback(selected_entry.handle, selected_entry.user_data);
	}

	return LIBUSBY_SUCCESS;
}

int libpolly_win32_wait_until(libpolly_context * ctx, HANDLE handle)
{
	HANDLE handles[2] = { handle, ctx->hReaperLock };
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

	res = libpolly_win32_wait_until_locked(ctx, handle);
	SetEvent(ctx->hReaperLock);
	return res;
}

static DWORD WINAPI event_loop_thread_proc(void * param)
{
	libpolly_context * ctx = param;
	return libpolly_win32_wait_until(ctx, ctx->hEventLoopStopped);
}

void libpolly_submit_task(libpolly_task * task, libpolly_task_callback callback, void * user_data)
{
	libpolly_context * ctx = task->ctx;
	task->callback = callback;
	task->user_data = user_data;
	task->next = 0;

	EnterCriticalSection(&ctx->entry_list_mutex);

	if (ctx->task_last)
	{
		assert(ctx->task_first);
		ctx->task_last->next = task;
	}
	else
	{
		assert(!ctx->task_first);
		ctx->task_first = task;
	}

	ctx->task_last = task;
	SetEvent(ctx->hHandleListUpdated);

	LeaveCriticalSection(&ctx->entry_list_mutex);
}

int libpolly_prepare_task(libpolly_context * ctx, libpolly_task ** task)
{
	libpolly_task * res = malloc(sizeof(libpolly_task));
	if (!res)
		return LIBUSBY_ERROR_NO_MEM;
	res->ctx = ctx;
	*task = res;
	return LIBUSBY_SUCCESS;
}

void libpolly_cancel_task(libpolly_task * task)
{
	free(task);
}

int libpolly_submit_task_direct(libpolly_context * ctx, libpolly_task_callback callback, void * user_data)
{
	libpolly_task * task;
	int r = libpolly_prepare_task(ctx, &task);
	if (r < 0)
		return r;
	libpolly_submit_task(task, callback, user_data);
	return LIBUSBY_SUCCESS;
}
