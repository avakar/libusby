#include "libusby.h"
#include "libusbyi.h"
#include <stdlib.h>
#include <memory.h>
#include <assert.h>

int usbyi_append_device_list(struct usbyi_device_list * devices, libusby_device * dev)
{
	assert(devices->count <= devices->capacity);
	assert(devices->list || devices->capacity == 0);

	if (!devices->count || devices->count+1 == devices->capacity)
	{
		libusby_device ** dev_list = devices->list;
		int new_capacity = (devices->count + 1) * 2;
		dev_list = realloc(dev_list, new_capacity * sizeof(libusby_device *));
		if (!dev_list)
			return LIBUSBY_ERROR_NO_MEM;
		devices->capacity = new_capacity;
		devices->list = dev_list;
	}

	assert(devices->count+1 < devices->capacity);
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

		if (libusby_get_device_descriptor(handle, &desc) < 0
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

int libusby_get_device_descriptor(libusby_device_handle * dev_handle, libusby_device_descriptor * desc)
{
	if (libusby_get_device_descriptor_cached(dev_handle->dev, desc) == LIBUSBY_SUCCESS)
		return LIBUSBY_SUCCESS;

	{
		uint8_t rawdesc[sizeof(libusby_device_descriptor)];
		int r = libusby_get_descriptor(dev_handle, 1, 0, rawdesc, sizeof rawdesc);
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

int libusby_control_transfer(libusby_device_handle * dev_handle, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, uint8_t * data, uint16_t wLength, libusby_timeout_t timeout)
{
	int r;

	uint8_t * buffer = 0;
	libusby_transfer * tran = libusby_alloc_transfer(dev_handle->dev->ctx, 0);
	if (!tran)
		return LIBUSBY_ERROR_NO_MEM;

	buffer = malloc(wLength + 8);
	if (!buffer)
	{
		libusby_free_transfer(tran);
		return LIBUSBY_ERROR_NO_MEM;
	}

	buffer[0] = bmRequestType;
	buffer[1] = bRequest;
	buffer[2] = (uint8_t)wValue;
	buffer[3] = (uint8_t)(wValue >> 8);
	buffer[4] = (uint8_t)wIndex;
	buffer[5] = (uint8_t)(wIndex >> 8);
	buffer[6] = (uint8_t)wLength;
	buffer[7] = (uint8_t)(wLength >> 8);

	libusby_fill_control_transfer(tran, dev_handle, buffer, NULL, 0, timeout);
	r = libusby_perform_transfer(tran);

	if (r >= 0)
	{
		if (tran->status != LIBUSBY_TRANSFER_COMPLETED)
		{
			r = LIBUSBY_ERROR_IO;
		}
		else
		{
			assert(tran->actual_length >= 8);
			memcpy(data, buffer + 8, tran->actual_length - 8);
			r = tran->actual_length - 8;
		}
	}

	free(buffer);
	libusby_free_transfer(tran);

	return r;
}

int libusby_get_descriptor(libusby_device_handle * dev_handle, uint8_t desc_type, uint8_t desc_index, unsigned char * data, int length)
{
	int r = LIBUSBY_ERROR_NOT_SUPPORTED;
	if (dev_handle->dev->ctx->backend->get_descriptor)
		r = dev_handle->dev->ctx->backend->get_descriptor(dev_handle, desc_type, desc_index, data, length);
	if (r == LIBUSBY_ERROR_NOT_SUPPORTED)
		r = libusby_control_transfer(dev_handle, 0x80, 6/*GET_DESCRIPTOR*/, desc_index | (desc_type << 8), 0, data, length, 0);
	return r;
}

void libusby_fill_control_transfer(libusby_transfer * transfer, libusby_device_handle * dev_handle, uint8_t * buffer, libusby_transfer_cb_fn callback, void * user_data, libusby_timeout_t timeout)
{
	transfer->dev_handle = dev_handle;
	transfer->buffer = buffer;
	transfer->callback = callback;
	transfer->user_data = user_data;
	transfer->timeout = timeout;
	transfer->endpoint = 0;
	transfer->length = (buffer[6] | (buffer[7] << 8)) + 8;
	transfer->type = LIBUSBY_TRANSFER_TYPE_CONTROL;
}

int libusby_get_string_descriptor(libusby_device_handle * dev_handle, uint8_t desc_index, uint16_t langid, unsigned char * data, int length)
{
	return libusby_control_transfer(dev_handle, 0x80, 6/*GET_DESCRIPTOR*/, desc_index | 0x300, langid, data, length, 0);
}

static int usbyi_sanitize_config_descriptor(libusby_config_descriptor ** config, unsigned char * rawdesc, uint16_t wTotalLength)
{
	libusby_config_descriptor * res;
	libusby_interface * intf = 0;
	libusby_interface_descriptor * intf_desc = 0;
	int endp_desc_index = 0;

	if (wTotalLength < 9 || rawdesc[1] != 2/*CONFIGURATION*/ || rawdesc[0] != 9)
		return LIBUSBY_ERROR_IO;

	res = malloc(sizeof(libusby_config_descriptor));
	if (!res)
		return LIBUSBY_ERROR_NO_MEM;

	memcpy(res, rawdesc, 9);
	rawdesc += 9;
	wTotalLength -= 9;

	res->interface = malloc(res->bNumInterfaces * sizeof(libusby_interface));
	if (!res->interface)
		goto error;
	memset(res->interface, 0, res->bNumInterfaces * sizeof(libusby_interface));

	while (wTotalLength != 0)
	{
		uint8_t desclen = rawdesc[0];
		if (desclen > wTotalLength || desclen < 2)
			goto error;

		if (rawdesc[1] == 4/*INTERFACE*/)
		{
			uint8_t bInterfaceNumber;
			uint8_t bAlternateSettings;
			libusby_interface_descriptor * altsetting;

			if (intf_desc && endp_desc_index != intf_desc->bNumEndpoints)
				goto error;

			if (rawdesc[0] != 9)
				goto error;

			bInterfaceNumber = rawdesc[2];
			bAlternateSettings = rawdesc[3];

			if (bInterfaceNumber >= res->bNumInterfaces)
				goto error;

			intf = &res->interface[bInterfaceNumber];
			if (bAlternateSettings != intf->num_altsetting)
				goto error;

			altsetting = realloc(intf->altsetting, sizeof(libusby_interface_descriptor) * (bAlternateSettings+1));
			if (!altsetting)
				goto error;

			intf->altsetting = altsetting;
			intf_desc = &intf->altsetting[bAlternateSettings];
			memcpy(intf_desc, rawdesc, 9);
			++intf->num_altsetting;

			intf_desc->endpoint = malloc(intf_desc->bNumEndpoints * sizeof(libusby_endpoint_descriptor));
			if (!intf_desc->endpoint)
				goto error;
			memset(intf_desc->endpoint, 0, intf_desc->bNumEndpoints * sizeof(libusby_endpoint_descriptor));
			endp_desc_index = 0;
		}

		if (rawdesc[1] == 5/*ENDPOINT*/)
		{
			if (rawdesc[0] != 7)
				goto error;

			memcpy(&intf_desc->endpoint[endp_desc_index++], rawdesc, 7);
		}

		wTotalLength -= desclen;
		rawdesc += desclen;
	}

	if (intf_desc && endp_desc_index != intf_desc->bNumEndpoints)
		goto error;

	{
		int i;
		for (i = 0; i < res->bNumInterfaces; ++i)
		{
			if (res->interface[i].num_altsetting == 0)
				goto error;
		}
	}

	*config = res;
	return LIBUSBY_SUCCESS;

error:
	libusby_free_config_descriptor(res);
	return LIBUSBY_ERROR_IO;
}

int libusby_get_config_descriptor(libusby_device_handle * dev_handle, uint8_t config_index, libusby_config_descriptor ** config)
{
	int r;
	unsigned char header[6];
	uint16_t wTotalLength;
	unsigned char * rawdesc;

	r = libusby_get_descriptor(dev_handle, 2, config_index, header, sizeof header);
	if (r < 0)
		return r;

	wTotalLength = (header[5] << 8) | header[4];

	rawdesc = malloc(wTotalLength);
	if (!rawdesc)
		return LIBUSBY_ERROR_NO_MEM;

	r = libusby_get_descriptor(dev_handle, 2, config_index, rawdesc, wTotalLength);
	if (r >= 0)
		r = usbyi_sanitize_config_descriptor(config, rawdesc, r);

	free(rawdesc);
	return r;
}

void libusby_free_config_descriptor(libusby_config_descriptor * config)
{
	int i, j;

	if (config->interface)
	{
		for (i = 0; i < config->bNumInterfaces; ++i)
		{
			if (config->interface[i].altsetting)
			{
				for (j = 0; j < config->interface[i].num_altsetting; ++j)
				{
					if (config->interface[i].altsetting[j].endpoint)
						free(config->interface[i].altsetting[j].endpoint);
				}

				free(config->interface[i].altsetting);
			}
		}

		free(config->interface);
	}

	free(config);
}

int libusby_get_active_config_descriptor(libusby_device_handle * dev_handle, libusby_config_descriptor ** config)
{
	int active_config;
	int r = libusby_get_configuration(dev_handle, &active_config);
	if (r < 0)
		return r;

	return libusby_get_config_descriptor_by_value(dev_handle, active_config, config);
}

int libusby_get_config_descriptor_by_value(libusby_device_handle * dev_handle, uint8_t config_value, libusby_config_descriptor ** config)
{
	libusby_device_descriptor desc;
	int i;

	int r = libusby_get_device_descriptor(dev_handle, &desc);
	if (r < 0)
		return r;

	for (i = 0; i < desc.bNumConfigurations; ++i)
	{
		libusby_config_descriptor * config_desc;
		r = libusby_get_config_descriptor(dev_handle, i, &config_desc);
		if (r < 0)
			return r;

		if (config_desc->bConfigurationValue == config_value)
		{
			*config = config_desc;
			return LIBUSBY_SUCCESS;
		}

		libusby_free_config_descriptor(config_desc);
	}

	return LIBUSBY_ERROR_NOT_FOUND;
}

int libusby_get_configuration(libusby_device_handle * dev_handle, int * config_value)
{
	int r = LIBUSBY_ERROR_NOT_SUPPORTED;
	if (dev_handle->dev->ctx->backend->get_configuration)
		r = dev_handle->dev->ctx->backend->get_configuration(dev_handle, config_value, /*cached_only=*/0);

	if (r == LIBUSBY_ERROR_NOT_SUPPORTED)
	{
		uint8_t data;
		r = libusby_control_transfer(dev_handle, 0x80, 8/*GET_CONFIGURATION*/, 0, 0, &data, 1, 0);
		if (r >= 0 && r != 1)
			r = LIBUSBY_ERROR_IO;

		if (r == 1)
		{
			*config_value = data;
			r = LIBUSBY_SUCCESS;
		}
	}

	return r;
}

int libusby_get_configuration_cached(libusby_device_handle * dev_handle, int * config_value)
{
	int r = LIBUSBY_ERROR_NOT_SUPPORTED;
	if (dev_handle->dev->ctx->backend->get_configuration)
		r = dev_handle->dev->ctx->backend->get_configuration(dev_handle, config_value, /*cached_only=*/1);
	return r;
}
