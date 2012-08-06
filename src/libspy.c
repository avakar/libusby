#include "libspy.h"
#include "os/spy.h"
#include <stdlib.h>
#include <string.h>

int libspy_init_with_polly(libspy_context ** ctx, libpolly_context * polly)
{
	int r;
	libspy_context * res = malloc(spyb_context_size);
	if (!res)
		return LIBSPY_ERROR_NO_MEM;
	memset(res, 0, spyb_context_size);

	res->polly = polly;

	r = spyb_init((spyb_context *)res);
	if (r < 0)
	{
		free(res);
		return r;
	}

	libpolly_ref_context(polly);
	*ctx = res;
	return LIBSPY_SUCCESS;
}

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

void libspy_exit(libspy_context * ctx)
{
	spyb_exit((spyb_context *)ctx);
	libpolly_unref_context(ctx->polly);
	free(ctx);
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

int libspy_read(libspy_device_handle * handle, uint8_t * buf, int len);

libspy_transfer * libspy_alloc_transfer(libspy_context * ctx)
{
	libspy_transfer * res = malloc(spyb_transfer_size);
	if (!res)
		return 0;
	memset(res, 0, spyb_transfer_size);

	res->ctx = (spyb_context *)ctx;

	if (spyb_init_transfer((spyb_transfer *)res) < 0)
	{
		free(res);
		return 0;
	}

	return res;
}

void libspy_free_transfer(libspy_transfer * transfer)
{
	spyb_destroy_transfer((spyb_transfer *)transfer);
	free(transfer);
}

void libspy_set_user_data(libspy_transfer * transfer, void * user_data)
{
	transfer->user_data = user_data;
}

void * libspy_get_user_data(libspy_transfer * transfer)
{
	return transfer->user_data;
}

void libspy_wait_for_transfer(libspy_transfer * transfer)
{
	spyb_wait_for_transfer((spyb_transfer *)transfer);
}

int libspy_submit_continuous_read(libspy_transfer * transfer,
	libspy_device_handle * handle, uint8_t * buf, int len,
	void (* callback)(libspy_transfer * transfer, libspy_transfer_status status))
{
	transfer->callback = callback;
	transfer->handle = handle;
	transfer->flags = spyi_tf_resubmit;
	transfer->buf = buf;
	transfer->length = len;
	return spyb_submit_read((spyb_transfer *)transfer);
}

int libspy_get_transfer_length(libspy_transfer * transfer)
{
	return transfer->actual_length;
}

void spyi_init_list(spyi_list * list)
{
	list->next = list;
	list->prev = list;
}

void libspy_cancel_transfer(libspy_transfer * transfer)
{
	spyb_cancel_transfer((spyb_transfer *)transfer);
}
