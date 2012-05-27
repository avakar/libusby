#include "libusby.h"
#include "libusbyi.h"
#include <stdlib.h>
#include <memory.h>
#include <assert.h>

int usbyi_append_device_list(struct usbyi_device_list * devices, libusby_device * dev)
{
	assert(devices->count <= devices->capacity);
	assert(devices->list || devices->capacity == 0);

	if (devices->count == devices->capacity)
	{
		libusby_device ** dev_list = devices->list;
		int new_capacity = (devices->count + 1) * 2;
		dev_list = realloc(dev_list, new_capacity * sizeof(libusby_device *));
		if (!dev_list)
			return LIBUSBY_ERROR_NO_MEM;
		devices->capacity = new_capacity;
		devices->list = dev_list;
	}

	assert(devices->count < devices->capacity);
	devices->list[devices->count] = dev;
	devices->list[devices->count+1] = 0;
	++devices->count;
	return LIBUSBY_SUCCESS;
}

libusby_device * usbyi_alloc_device(libusby_context * ctx)
{
	int alloc_size = sizeof(libusby_device) + ctx->backend->device_private_size;
	libusby_device * res = malloc(alloc_size);
	memset(res, 0, alloc_size);

	res->ctx = ctx;
	res->ref_count = 1;

	if (usbyi_append_device_list(&ctx->devices, res) < 0)
	{
		free(res);
		return 0;
	}

	return res;
}

void * usbyi_ctx_to_priv(libusby_context * ctx)
{
	return ctx + 1;
}

struct usbyi_transfer * usbyi_tran_to_trani(libusby_transfer * tran)
{
	return (struct usbyi_transfer *)tran - 1;
}

libusby_transfer * usbyi_trani_to_tran(struct usbyi_transfer * trani)
{
	return (libusby_transfer *)(trani + 1);
}

void * usbyi_dev_to_devpriv(libusby_device * dev)
{
	return dev + 1;
}

void * usbyi_handle_to_handlepriv(libusby_device_handle * handle)
{
	return handle + 1;
}

int usbyi_init(libusby_context ** ctx, struct usbyi_backend const * backend)
{
	int r;

	size_t alloc_size = sizeof(libusby_context) + backend->context_private_size;
	libusby_context * res = malloc(alloc_size);
	if (!res)
		return LIBUSBY_ERROR_NO_MEM;
	memset(res, 0, alloc_size);

	res->backend = backend;
	r = usbyi_init_os_ctx(res);
	if (r < 0)
	{
		free(res);
		return r;
	}

	r = backend->init(res);
	if (r < 0)
	{
		usbyi_clear_os_ctx(res);
		free(res);
		return r;
	}

	*ctx = res;
	return LIBUSBY_SUCCESS;
}

void libusby_exit(libusby_context * ctx)
{
	ctx->backend->exit(ctx);
	usbyi_clear_os_ctx(ctx);
	free(ctx->devices.list);
	free(ctx);
}

libusby_transfer * libusby_alloc_transfer(libusby_context * ctx, int iso_packets)
{
	size_t alloc_size = sizeof(struct usbyi_transfer) + sizeof(libusby_transfer) + (sizeof(libusby_iso_packet_descriptor)*(iso_packets-1));
	struct usbyi_transfer * res = malloc(alloc_size);
	if (!res)
		return NULL;

	memset(res, 0, alloc_size);
	if (usbyi_init_os_transfer(res) < 0)
	{
		free(res);
		return NULL;
	}

	res->ctx = ctx;
	res->num_iso_packets = iso_packets;
	return usbyi_trani_to_tran(res);
}

void libusby_free_transfer(libusby_transfer * transfer)
{
	struct usbyi_transfer * trani = usbyi_tran_to_trani(transfer);
	usbyi_clear_os_transfer(trani);
	free(trani);
}

void libusby_fill_bulk_transfer(libusby_transfer * transfer, libusby_device_handle * dev_handle, libusby_endpoint_t endpoint, uint8_t * buffer, int length,
	libusby_transfer_cb_fn callback, void * user_data, libusby_timeout_t timeout)
{
	transfer->dev_handle = dev_handle;
	transfer->flags = 0;
	transfer->endpoint = endpoint;
	transfer->type = LIBUSBY_TRANSFER_TYPE_BULK;
	transfer->status = LIBUSBY_TRANSFER_COMPLETED;
	transfer->length = length;
	transfer->actual_length = 0;
	transfer->callback = callback;
	transfer->user_data = user_data;
	transfer->buffer = buffer;
	transfer->timeout = timeout;
	if (transfer->num_iso_packets)
		memset(transfer->iso_packet_desc, 0, sizeof(libusby_iso_packet_descriptor)*transfer->num_iso_packets);
}

int libusby_perform_transfer(libusby_transfer * tran)
{
	if (tran->dev_handle->dev->ctx->backend->perform_transfer)
	{
		return tran->dev_handle->dev->ctx->backend->perform_transfer(tran);
	}
	else
	{
		int r = libusby_submit_transfer(tran);
		if (r < 0)
			return r;
		return libusby_wait_for_transfer(tran);
	}
}

int libusby_bulk_transfer(libusby_device_handle * dev_handle, libusby_endpoint_t endpoint, uint8_t * data, int length, int * transferred, libusby_timeout_t timeout)
{
	int r;

	libusby_transfer * tran = libusby_alloc_transfer(dev_handle->dev->ctx, 0);
	if (!tran)
		return LIBUSBY_ERROR_NO_MEM;
	libusby_fill_bulk_transfer(tran, dev_handle, endpoint, data, length, 0, 0, timeout);

	r = libusby_perform_transfer(tran);
	if (r >= 0)
		*transferred = tran->actual_length;

	libusby_free_transfer(tran);
	return r;
}

int libusby_submit_transfer(libusby_transfer * transfer)
{
	return transfer->dev_handle->dev->ctx->backend->submit_transfer(transfer);
}

int libusby_cancel_transfer(libusby_transfer * transfer)
{
	struct usbyi_transfer * trani = usbyi_tran_to_trani(transfer);
	return trani->ctx->backend->cancel_transfer(transfer);
}

int libusby_claim_interface(libusby_device_handle * dev_handle, int interface_number)
{
	return dev_handle->dev->ctx->backend->claim_interface(dev_handle, interface_number);
}

int libusby_release_interface(libusby_device_handle * dev_handle, int interface_number)
{
	return dev_handle->dev->ctx->backend->release_interface(dev_handle, interface_number);
}

libusby_device * libusby_get_device(libusby_device_handle * dev_handle)
{
	return dev_handle->dev;
}

void libusby_free_device_list(libusby_device ** list, int unref_devices)
{
	if (list && unref_devices)
	{
		libusby_device ** cur = list;
		for (; *cur; ++cur)
			libusby_unref_device(*cur);
	}

	free(list);
}

libusby_device * libusby_ref_device(libusby_device * dev)
{
	++dev->ref_count;
	return dev;
}

void libusby_unref_device(libusby_device * dev)
{
	if (--dev->ref_count)
		return;

	{
		int i;
		for (i = 0; i < dev->ctx->devices.count; ++i)
		{
			if (dev->ctx->devices.list[i] == dev)
			{
				dev->ctx->devices.list[i] = dev->ctx->devices.list[--dev->ctx->devices.count];
				dev->ctx->devices.list[dev->ctx->devices.count] = 0;
				break;
			}
		}
	}
	free(dev);
}

int libusby_get_device_descriptor_cached(libusby_device * dev, libusby_device_descriptor * desc)
{
	if (dev->device_desc.bLength != sizeof(libusby_device_descriptor))
		return LIBUSBY_ERROR_IO;

	memcpy(desc, &dev->device_desc, sizeof dev->device_desc);
	return LIBUSBY_SUCCESS;
}

int libusby_open(libusby_device * dev, libusby_device_handle ** dev_handle)
{
	libusby_context * ctx = dev->ctx;
	int alloc_size = sizeof(libusby_device_handle) + ctx->backend->device_handle_private_size;

	libusby_device_handle * res = malloc(alloc_size);
	if (!res)
		return LIBUSBY_ERROR_NO_MEM;
	memset(res, 0, alloc_size);

	res->dev = dev;

	if (dev->ctx->backend->open)
	{
		int r = dev->ctx->backend->open(res);
		if (r < 0)
		{
			free(res);
			return r;
		}
	}

	libusby_ref_device(dev);
	*dev_handle = res;
	return LIBUSBY_SUCCESS;
}

libusby_device_handle * libusby_open_device_with_vid_pid(libusby_context * ctx, uint16_t vendor_id, uint16_t product_id)
{
	libusby_device ** device_list = 0;
	libusby_device_handle * handle = 0;
	int i;

	int cnt = libusby_get_device_list(ctx, &device_list);
	if (cnt < 0)
		return 0;

	for (i = 0; handle == 0 && i < cnt; ++i)
	{
		libusby_device_descriptor desc;

		if (libusby_get_device_descriptor_cached(device_list[i], &desc) < 0)
			continue;

		if (desc.idVendor != vendor_id || desc.idProduct != product_id)
			continue;

		if (libusby_open(device_list[i], &handle) < 0)
			handle = 0;
	}

	for (i = 0; handle == 0 && i < cnt; ++i)
	{
		libusby_device_descriptor desc;

		if (libusby_open(device_list[i], &handle) < 0)
		{
			handle = 0;
			continue;
		}

		if (libusby_get_device_descriptor(handle, &desc, 1000) < 0
			|| desc.idVendor != vendor_id || desc.idProduct != product_id)
		{
			libusby_close(handle);
			handle = 0;
		}
	}

	libusby_free_device_list(device_list, 1);
	return handle;
}

void libusby_close(libusby_device_handle * dev_handle)
{
	if (dev_handle->dev->ctx->backend->close)
		dev_handle->dev->ctx->backend->close(dev_handle);
	libusby_unref_device(dev_handle->dev);
	free(dev_handle);
}

int libusby_get_device_list(libusby_context * ctx, libusby_device *** list)
{
	return ctx->backend->get_device_list(ctx, list);
}

int libusby_get_device_descriptor(libusby_device_handle * dev_handle, libusby_device_descriptor * desc, libusby_timeout_t timeout)
{
	if (libusby_get_device_descriptor_cached(dev_handle->dev, desc) == LIBUSBY_SUCCESS)
		return LIBUSBY_SUCCESS;

	{
		uint8_t rawdesc[sizeof(libusby_device_descriptor)];
		//int r = libusby_control_transfer(dev_handle, 0x80, 6/*GET_DESCRIPTOR*/, 1, 0, rawdesc, sizeof rawdesc, timeout);
		int r = LIBUSBY_ERROR_IO;
		if (r < 0)
			return r;

		r = usbyi_sanitize_device_desc(&dev_handle->dev->device_desc, rawdesc);
		if (r < 0)
			return r;
	}

	memcpy(desc, &dev_handle->dev->device_desc, sizeof dev_handle->dev->device_desc);
	return LIBUSBY_SUCCESS;
}

int usbyi_sanitize_device_desc(libusby_device_descriptor * desc, uint8_t * rawdesc)
{
	if (rawdesc[0] != sizeof(libusby_device_descriptor))
		return LIBUSBY_ERROR_IO;
	memcpy(desc, rawdesc, sizeof(libusby_device_descriptor));
	// FIXME: endianity
	return LIBUSBY_SUCCESS;
}
