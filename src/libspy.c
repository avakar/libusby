#include "libspy.h"
#include <stdlib.h>
#include <string.h>

int libspy_init(libspy_context ** ctx)
{
	libpolly_context * polly;
	int r = libpolly_init(&polly);
	if (r < 0)
		return r;

	r = libspy_init_with_polly(ctx, polly);
	libpolly_unref_context(polly);
	return r;
}

void libspy_free_device_list(libspy_device const * device_list)
{
	libspy_device const * cur = device_list;
	for (; cur->path; ++cur)
	{
		free((void *)cur->friendly_name);
		free((void *)cur->name);
		free((void *)cur->path);
	}

	free((void *)device_list);
}

static void spyi_sync_open_callback(libspy_open_future * future, void * user_data, libspy_device_handle * handle)
{
	libspy_device_handle ** h = user_data;
	*h = handle;
}

int libspy_open(libspy_context * ctx, char const * path, libspy_device_settings const * settings, libspy_device_handle ** handle)
{
	libspy_device_handle * res;
	libspy_open_future * future;
	int r = libspy_begin_open(ctx, path, settings, &spyi_sync_open_callback, &res, &future);
	if (r < 0)
		return r;
	libspy_wait_for_open(future);
	libspy_free_open_future(future);
	*handle = res;
	return res? LIBUSBY_SUCCESS: LIBUSBY_ERROR_NO_DEVICE;
}

static void spyi_sync_read_callback(libspy_transfer * transfer, libspy_transfer_status status)
{
	int * actual_length = libspy_get_user_data(transfer);
	if (status == libspy_transfer_completed)
		*actual_length = libspy_get_transfer_length(transfer);
	else
		*actual_length = LIBUSBY_ERROR_IO;
}

int libspy_read(libspy_device_handle * handle, uint8_t * buf, int len)
{
	int actual_length;
	libspy_transfer * tran = libspy_alloc_transfer(libspy_get_context(handle));
	libspy_set_user_data(tran, &actual_length);
	libspy_submit_read(tran, handle, buf, len, &spyi_sync_read_callback);
	libspy_wait_for_transfer(tran);
	libspy_free_transfer(tran);
	return actual_length;
}
