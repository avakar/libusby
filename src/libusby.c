#include "libusby.h"
#include "libusbyi.h"
#include <stdlib.h>
#include <memory.h>
#include <assert.h>
#include "os/os.h"

void usbyi_init_devlist_head(usbyi_device_list_node * head)
{
	head->next = head;
	head->prev = head;
}

usbyi_device_list_node * usbyi_remove_devlist_node(usbyi_device_list_node * dev_node)
{
	usbyi_device_list_node * next = dev_node->next;
	dev_node->next->prev = dev_node->prev;
	dev_node->prev->next = dev_node->next;
	return next;
}

void usbyi_insert_before_devlist_node(usbyi_device_list_node * node, usbyi_device_list_node * next)
{
	node->next = next;
	node->prev = next->prev;
	next->prev = node;
	node->prev->next = node;
}

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

usbyb_device * usbyi_alloc_device(libusby_context * ctx)
{
	libusby_device * res = malloc(usbyb_device_size);
	memset(res, 0, usbyb_device_size);

	res->ctx = ctx;
	res->ref_count = 1;
	return (usbyb_device *)res;
}

int libusby_init(libusby_context ** ctx)
{
	int r;
	libusby_context * res = malloc(usbyb_context_size);
	if (!res)
		return LIBUSBY_ERROR_NO_MEM;
	memset(res, 0, usbyb_context_size);

	r = usbyb_init((usbyb_context *)res);
	if (r < 0)
	{
		free(res);
		return r;
	}

	*ctx = res;
	return LIBUSBY_SUCCESS;
}

void libusby_exit(libusby_context * ctx)
{
	usbyb_exit((usbyb_context *)ctx);
	free(ctx);
}

libusby_transfer * libusby_alloc_transfer(libusby_context * ctx, int iso_packets)
{
	size_t alloc_size = usbyb_transfer_size + (sizeof(libusby_iso_packet_descriptor)*(iso_packets-1));
	usbyb_transfer * res = malloc(alloc_size);
	usbyi_transfer * resi = (usbyi_transfer *)res;
	if (!res)
		return NULL;
	memset(res, 0, alloc_size);

	if (usbyb_init_transfer(res) < 0)
	{
		free(res);
		return NULL;
	}

	resi->ctx = (usbyb_context *)ctx;
	resi->num_iso_packets = iso_packets;
	return usbyi_get_pub_tran(res);
}

void libusby_free_transfer(libusby_transfer * transfer)
{
	usbyb_transfer * trani = usbyi_get_tran(transfer);
	usbyb_clear_transfer(trani);
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
	usbyb_transfer * tranb = usbyi_get_tran(tran);
	int r = usbyb_perform_transfer(tranb);
	if (r == LIBUSBY_ERROR_NOT_SUPPORTED)
	{
		r = libusby_submit_transfer(tran);
		if (r >= 0)
			r = libusby_wait_for_transfer(tran);
	}
	return r;
}

int libusby_bulk_transfer(libusby_device_handle * dev_handle, libusby_endpoint_t endpoint, uint8_t * data, int length, int * transferred, libusby_timeout_t timeout)
{
	int r;

	libusby_device * dev = libusby_get_device(dev_handle);
	libusby_transfer * tran = libusby_alloc_transfer(dev->ctx, 0);
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
	usbyb_transfer * tranb = usbyi_get_tran(transfer);
	return usbyb_submit_transfer(tranb);
}

int libusby_cancel_transfer(libusby_transfer * transfer)
{
	usbyb_transfer * tranb = usbyi_get_tran(transfer);
	return usbyb_cancel_transfer(tranb);
}

int libusby_claim_interface(libusby_device_handle * dev_handle, int interface_number)
{
	return usbyb_claim_interface((usbyb_device_handle *)dev_handle, interface_number);
}

int libusby_release_interface(libusby_device_handle * dev_handle, int interface_number)
{
	return usbyb_release_interface((usbyb_device_handle *)dev_handle, interface_number);
}

libusby_device * libusby_get_device(libusby_device_handle * dev_handle)
{
	return (libusby_device *)dev_handle->dev;
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
	if (--dev->ref_count == 0)
	{
		usbyb_finalize_device((usbyb_device *)dev);
		free(dev);
	}
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
	int r;
	libusby_device_handle * res = malloc(usbyb_device_handle_size);
	if (!res)
		return LIBUSBY_ERROR_NO_MEM;
	memset(res, 0, usbyb_device_handle_size);

	res->dev = (usbyb_device *)dev;

	r = usbyb_open((usbyb_device_handle *)res);
	if (r < 0)
	{
		free(res);
		return r;
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
	usbyb_close((usbyb_device_handle *)dev_handle);
	libusby_unref_device(libusby_get_device(dev_handle));
	free(dev_handle);
}

int libusby_get_device_list(libusby_context * ctx, libusby_device *** list)
{
	return usbyb_get_device_list((usbyb_context *)ctx, list);
}

int libusby_get_device_descriptor(libusby_device_handle * dev_handle, libusby_device_descriptor * desc)
{
	libusby_device * dev = libusby_get_device(dev_handle);
	if (libusby_get_device_descriptor_cached(dev, desc) == LIBUSBY_SUCCESS)
		return LIBUSBY_SUCCESS;

	{
		uint8_t rawdesc[sizeof(libusby_device_descriptor)];
		int r = libusby_get_descriptor(dev_handle, 1, 0, rawdesc, sizeof rawdesc);
		if (r < 0)
			return r;

		r = usbyi_sanitize_device_desc(&dev->device_desc, rawdesc);
		if (r < 0)
			return r;
	}

	memcpy(desc, &dev->device_desc, sizeof dev->device_desc);
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
	libusby_device * dev = libusby_get_device(dev_handle);
	libusby_transfer * tran = libusby_alloc_transfer(dev->ctx, 0);
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
	int r = usbyb_get_descriptor((usbyb_device_handle *)dev_handle, desc_type, desc_index, 0, data, length);
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
	int r = usbyb_get_descriptor((usbyb_device_handle *)dev_handle, 3, desc_index, langid, data, length);
	if (r == LIBUSBY_ERROR_NOT_SUPPORTED)
		r = libusby_control_transfer(dev_handle, 0x80, 6/*GET_DESCRIPTOR*/, desc_index | 0x300, langid, data, length, 0);
	return r;
}

static int encode_utf8(char * data, int length, uint32_t cp)
{
	int res = 0;

	assert(length > 0);
	assert(cp < 0x110000);

	if (cp < 0x7f)
	{
		data[res++] = cp;
	}
	else if (cp < 0x7ff)
	{
		data[res++] = 0xC0 | (cp >> 6);
		if (res < length)
			data[res++] = 0x80 | (cp & 0x3f);
	}
	else if (cp < 0xffff)
	{
		data[res++] = 0xe0 | (cp >> 12);
		if (res < length)
			data[res++] = 0x80 | ((cp >> 6) & 0x3f);
		if (res < length)
			data[res++] = 0x80 | (cp & 0x3f);
	}
	else
	{
		data[res++] = 0xf0 | (cp >> 18);
		if (res < length)
			data[res++] = 0x80 | ((cp >> 12) & 0x3f);
		if (res < length)
			data[res++] = 0x80 | ((cp >> 6) & 0x3f);
		if (res < length)
			data[res++] = 0x80 | (cp & 0x3f);
	}

	return res;
}

int libusby_get_string_descriptor_utf8(libusby_device_handle * dev_handle, uint8_t desc_index, uint16_t langid, char * data, int length)
{
	int r, i;
	unsigned char utf16_buf[256];

	r = libusby_get_string_descriptor(dev_handle, desc_index, langid, utf16_buf, sizeof utf16_buf);
	if (r < 0)
		return r;

	if (r < 2 || utf16_buf[1] != 3)
		return LIBUSBY_ERROR_IO;

	if (r > utf16_buf[0])
		r = utf16_buf[0];

	if (r % 2 != 0)
		return LIBUSBY_ERROR_IO;

	// Translate UTF-16LE to UTF-8.
	{
		int data_len = 0;
		int encoded_len = 0;
		uint16_t high_surrogate = 0;
		for (i = 2; encoded_len < length && i < r; i += 2)
		{
			uint16_t cu = utf16_buf[i] | (utf16_buf[i+1] << 8);
			if (cu >= 0xDC00 && cu < 0xE000)
			{
				// low surrogate
				if (!high_surrogate)
					return LIBUSBY_ERROR_IO;

				encoded_len = encode_utf8(data + data_len, length - data_len, ((uint32_t)(high_surrogate - 0xD800) << 10) | (cu - 0xDC00));
				high_surrogate = 0;
			}
			else if (cu >= 0xD800 && cu < 0xDC00)
			{
				// high surrogate
				if (high_surrogate)
					return LIBUSBY_ERROR_IO;

				high_surrogate = cu;
			}
			else
			{
				if (high_surrogate)
					return LIBUSBY_ERROR_IO;

				encoded_len = encode_utf8(data + data_len, length - data_len, cu);
			}

			if (encoded_len < 0)
				return encoded_len;

			data_len += encoded_len;
		}

		if (i == r && high_surrogate)
			return LIBUSBY_ERROR_IO;

		r = data_len;
	}

	return r;
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

	wTotalLength = (header[3] << 8) | header[2];

	rawdesc = malloc(wTotalLength);
	if (!rawdesc)
		return LIBUSBY_ERROR_NO_MEM;

	r = libusby_get_descriptor(dev_handle, 2, config_index, rawdesc, wTotalLength);
	if (r >= 0)
		r = usbyi_sanitize_config_descriptor(config, rawdesc, r);

	free(rawdesc);
	return r;
}

int libusby_get_config_descriptor_cached(libusby_device * dev, uint8_t config_index, libusby_config_descriptor ** config)
{
	int r = LIBUSBY_ERROR_NOT_SUPPORTED;
	unsigned char header[6];
	uint16_t wTotalLength;
	unsigned char * rawdesc;

	r = usbyb_get_descriptor_cached((usbyb_device *)dev, 2, config_index, 0, header, sizeof header);
	if (r < 0)
		return r;

	wTotalLength = (header[3] << 8) | header[2];

	rawdesc = malloc(wTotalLength);
	if (!rawdesc)
		return LIBUSBY_ERROR_NO_MEM;

	r = usbyb_get_descriptor_cached((usbyb_device *)dev, 2, config_index, 0, rawdesc, wTotalLength);
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
	int r = usbyb_get_configuration((usbyb_device_handle *)dev_handle, config_value, /*cached_only=*/0);
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

int libusby_set_configuration(libusby_device_handle * dev_handle, int config_value)
{
	int r = usbyb_set_configuration((usbyb_device_handle *)dev_handle, config_value);
	if (r == LIBUSBY_ERROR_NOT_SUPPORTED)
	{
		r = libusby_control_transfer(dev_handle, 0x80, 9/*SET_CONFIGURATION*/, config_value, 0, 0, 0, 0);
		if (r == 0)
			r = LIBUSBY_SUCCESS;
		else
			r = LIBUSBY_ERROR_IO;
	}

	return r;
}

int libusby_get_configuration_cached(libusby_device_handle * dev_handle, int * config_value)
{
	return usbyb_get_configuration((usbyb_device_handle *)dev_handle, config_value, /*cached_only=*/1);
}

int libusby_wait_for_transfer(libusby_transfer * transfer)
{
	usbyb_transfer * tranb = usbyi_get_tran(transfer);
	return usbyb_wait_for_transfer(tranb);
}

int libusby_run_event_loop(libusby_context * ctx)
{
	return usbyb_run_event_loop((usbyb_context *)ctx);
}

void libusby_stop_event_loop(libusby_context * ctx)
{
	usbyb_stop_event_loop((usbyb_context *)ctx);
}

void libusby_reset_event_loop(libusby_context * ctx)
{
	usbyb_reset_event_loop((usbyb_context *)ctx);
}

libusby_transfer * usbyi_get_pub_tran(usbyb_transfer * tran)
{
	return (libusby_transfer *)((char *)tran + usbyb_transfer_pub_offset);
}

usbyb_transfer * usbyi_get_tran(libusby_transfer * tran)
{
	return (usbyb_transfer *)((char *)tran - usbyb_transfer_pub_offset);
}
