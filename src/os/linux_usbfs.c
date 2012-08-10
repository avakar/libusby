#include "os.h"
#include "../libusbyi.h"
#include <assert.h>
#include <string.h>
#include <stdint.h>

#define _GNU_SOURCE
#include <poll.h>
#include <stddef.h>
#include <dirent.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdio.h>
#include <linux/usbdevice_fs.h>
#include <pthread.h>

#include "libpolly_posix.h"

struct usbyb_context
{
	libusby_context pub;
	usbyi_device_list_node devlist_head;
	pthread_mutex_t * ctx_mutex;
};

struct usbyb_device
{
	libusby_device pub;
	usbyi_device_list_node devnode;

	int busno;
	int devno;
	int fd;

	uint8_t * desc_cache;
};

struct usbyb_device_handle
{
	libusby_device_handle pub;

	int wrfd;
	int active_config_value;
};

struct usbyb_transfer
{
	usbyi_transfer intrn;

	pthread_cond_t cond;
	int active;
	struct usbdevfs_urb req;

	libusby_transfer pub;
};

int const usbyb_context_size = sizeof(usbyb_context);
int const usbyb_device_size = sizeof(usbyb_device);
int const usbyb_device_handle_size = sizeof(usbyb_device_handle);
int const usbyb_transfer_size = sizeof(usbyb_transfer);
int const usbyb_transfer_pub_offset = offsetof(usbyb_transfer, pub);

int usbyb_init_transfer(usbyb_transfer * tran)
{
	if (pthread_cond_init(&tran->cond, 0) != 0)
		return LIBUSBY_ERROR_NO_MEM;

	tran->active = 0;
	return LIBUSBY_SUCCESS;
}

void usbyb_clear_transfer(usbyb_transfer * tran)
{
	pthread_cond_destroy(&tran->cond);
}

static int usbyb_loop_callback(int fd, short revents, libpolly_posix_callback callback, void * callback_data, void * run_data)
{
	usbyb_transfer * tran = run_data;
	usbyb_context * ctx = tran->intrn.ctx;
	int r;

	callback(fd, revents, callback_data);

	// TODO: maybe a load_acquire would be enough?
	pthread_mutex_lock(ctx->ctx_mutex);
	r = (tran->active? 0: 1);
	pthread_mutex_unlock(ctx->ctx_mutex);

	return r;
}

int usbyb_wait_for_transfer(usbyb_transfer * tran)
{
	usbyb_context * ctx = tran->intrn.ctx;
	int r = LIBUSBY_SUCCESS;

	pthread_mutex_lock(ctx->ctx_mutex);

	while (tran->active && libpolly_posix_acquire_loop(ctx->pub.polly_ctx) < 0)
	{
		libpolly_posix_loop_release_registration reg;
		reg.cond = &tran->cond;

		libpolly_posix_register_loop_release_notification(ctx->pub.polly_ctx, &reg);
		pthread_cond_wait(&tran->cond, ctx->ctx_mutex);
		libpolly_posix_unregister_loop_release_notification(&reg);
	}

	if (tran->active)
	{
		pthread_mutex_unlock(ctx->ctx_mutex);
		r = libpolly_posix_run(ctx->pub.polly_ctx, usbyb_loop_callback, tran);
		pthread_mutex_lock(ctx->ctx_mutex);
		libpolly_posix_release_loop(ctx->pub.polly_ctx);
	}

	assert(!tran->active);
	pthread_mutex_unlock(ctx->ctx_mutex);

	return r == 1? 0: r;
}

int usbyb_init(usbyb_context * ctx)
{
	usbyi_init_devlist_head(&ctx->devlist_head);
	ctx->ctx_mutex = libpolly_posix_get_loop_mutex(ctx->pub.polly_ctx);
	return LIBUSBY_SUCCESS;
}

void usbyb_exit(usbyb_context * ctx)
{
	assert(ctx->devlist_head.next == &ctx->devlist_head);
}

int usbyb_get_device_list(usbyb_context * ctx, libusby_device *** list)
{
	usbyi_device_list devlist;
	DIR * dir = 0;
	DIR * busdir = 0;
	struct dirent * ent;
	int fd = -1;
	int r = LIBUSBY_SUCCESS;

	memset(&devlist, 0, sizeof devlist);
	dir = opendir("/dev/bus/usb");
	if (!dir)
	{
		*list = NULL;
		return LIBUSBY_SUCCESS;
	}

	while (r >= 0 && (ent = readdir(dir)))
	{
		char fname[2*NAME_MAX+32];
		struct dirent * busent;
		char * end;
		int busno = strtol(ent->d_name, &end, 10);

		if (*end != 0)
			continue;

		strcpy(fname, "/dev/bus/usb/");
		strcat(fname, ent->d_name);

		busdir = opendir(fname);
		if (!busdir)
			continue;

		while (r >= 0 && (busent = readdir(busdir)))
		{
			int devno = strtol(busent->d_name, &end, 10);
			usbyi_device_list_node * devnode;

			if (*end != 0)
				continue;

			strcpy(fname, "/dev/bus/usb/");
			strcat(fname, ent->d_name);
			strcat(fname, "/");
			strcat(fname, busent->d_name);

			fd = open(fname, O_RDONLY);
			if (fd == -1)
				continue;

			pthread_mutex_lock(ctx->ctx_mutex);
			for (devnode = ctx->devlist_head.next; devnode != &ctx->devlist_head; devnode = devnode->next)
			{
				usbyb_device * dev = container_of(devnode, usbyb_device, devnode);
				if (dev->busno == busno && dev->devno == devno)
				{
					r = usbyi_append_device_list(&devlist, &dev->pub);
					if (r < 0)
						goto error;
					libusby_ref_device(&dev->pub);
					break;
				}
			}

			if (devnode == &ctx->devlist_head)
			{
				usbyb_device * dev;
				int i;
				size_t cache_len = 0;

				dev = usbyi_alloc_device(ctx);
				if (!dev)
				{
					r = LIBUSBY_ERROR_NO_MEM;
					goto error;
				}

				dev->busno = busno;
				dev->devno = devno;
				dev->fd = fd;
				usbyi_insert_before_devlist_node(&dev->devnode, &ctx->devlist_head);

				/* Note that the device descriptor is read in host-endian. */
				if (read(fd, &dev->pub.device_desc, sizeof dev->pub.device_desc) != sizeof dev->pub.device_desc)
					goto error_unref_dev;

				for (i = 0; i < dev->pub.device_desc.bNumConfigurations; ++i)
				{
					uint8_t config_header[4];
					uint16_t wTotalLength;
					uint8_t * cache;

					if (read(fd, config_header, sizeof config_header) != sizeof config_header)
						goto error_unref_dev;
					wTotalLength = config_header[2] | (config_header[3] << 8);
					if (wTotalLength < sizeof config_header)
						goto error_unref_dev;

					cache = realloc(dev->desc_cache, cache_len + wTotalLength);
					if (!cache)
					{
						r = LIBUSBY_ERROR_NO_MEM;
						goto error_unref_dev;
					}
					dev->desc_cache = cache;

					memcpy(dev->desc_cache + cache_len, config_header, sizeof config_header);
					if (read(fd, dev->desc_cache + cache_len + sizeof config_header, wTotalLength - sizeof config_header)
							!= (int)(wTotalLength - sizeof config_header))
					{
						goto error_unref_dev;
					}

					cache_len += wTotalLength;
				}

				r = usbyi_append_device_list(&devlist, &dev->pub);
				if (r < 0)
				{
error_unref_dev:
					libusby_unref_device(&dev->pub);
				}
			}
			else
			{
				close(fd);
			}

			pthread_mutex_unlock(ctx->ctx_mutex);
		}

		closedir(busdir);
	}

	closedir(dir);

	*list = devlist.list;
	return devlist.count;

error:
	pthread_mutex_unlock(ctx->ctx_mutex);
	if (fd != -1)
		close(fd);
	closedir(busdir);
	closedir(dir);
	if (devlist.list)
		libusby_free_device_list(devlist.list, /*unref_devices=*/1);
	return r;
}

void usbyb_finalize_device(usbyb_device * dev)
{
	usbyi_remove_devlist_node(&dev->devnode);
	free(dev->desc_cache);
	close(dev->fd);
}

static int usbfs_error()
{
	switch (errno)
	{
	case ENODEV:
		return LIBUSBY_ERROR_NO_DEVICE;
	case EBUSY:
		return LIBUSBY_ERROR_BUSY;
	default:
		return LIBUSBY_ERROR_IO;
	}
}

static void usbyb_reap_event(int fd, short revents, void * user_data)
{
	struct usbdevfs_urb * urb;
	usbyb_context * ctx = user_data;
	usbyb_transfer * tran;

	assert(revents & POLLOUT);
	if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &urb) < 0)
		return;

	tran = urb->usercontext;
	tran->pub.actual_length = tran->req.actual_length;
	if (tran->pub.type == LIBUSBY_TRANSFER_TYPE_CONTROL)
		tran->pub.actual_length += 8;

	if (tran->req.status != 0)
		tran->pub.status = LIBUSBY_ERROR_IO;

	pthread_mutex_lock(ctx->ctx_mutex);
	tran->active = 2;

	if (tran->pub.callback)
	{
		pthread_mutex_unlock(ctx->ctx_mutex);
		tran->pub.callback(&tran->pub);
		pthread_mutex_lock(ctx->ctx_mutex);
	}

	if (tran->active == 2)
	{
		tran->active = 0;
		pthread_cond_broadcast(&tran->cond);
	}

	pthread_mutex_unlock(ctx->ctx_mutex);
}

int usbyb_submit_transfer(usbyb_transfer * tran)
{
	usbyb_device_handle * handle = (usbyb_device_handle *)tran->pub.dev_handle;
	usbyb_context * ctx = (usbyb_context *)handle->pub.dev->pub.ctx;
	int r = LIBUSBY_SUCCESS;

	assert(ctx == tran->intrn.ctx);

	memset(&tran->req, 0, sizeof tran->req);

	switch (tran->pub.type)
	{
	case LIBUSBY_TRANSFER_TYPE_CONTROL:
		tran->req.type = USBDEVFS_URB_TYPE_CONTROL;
		break;
	case LIBUSBY_TRANSFER_TYPE_BULK:
		tran->req.type = USBDEVFS_URB_TYPE_BULK;
		break;
	case LIBUSBY_TRANSFER_TYPE_INTERRUPT:
		tran->req.type = USBDEVFS_URB_TYPE_INTERRUPT;
		break;
	default:
		return LIBUSBY_ERROR_NOT_SUPPORTED;
	}

	tran->req.endpoint = tran->pub.endpoint;
	tran->req.buffer = tran->pub.buffer;
	tran->req.buffer_length = tran->pub.length;
	tran->req.usercontext = tran;

	pthread_mutex_lock(ctx->ctx_mutex);

	r = libpolly_posix_prepare_add(ctx->pub.polly_ctx);
	if (r >= 0)
	{
		if (ioctl(handle->wrfd, USBDEVFS_SUBMITURB, &tran->req) < 0)
			r = usbfs_error();

		if (r >= 0)
		{
			libpolly_posix_add(ctx->pub.polly_ctx, handle->wrfd, POLLOUT, &usbyb_reap_event, ctx);
			tran->active = 1;
		}
		else
		{
			libpolly_posix_cancel_add(ctx->pub.polly_ctx);
		}
	}

	pthread_mutex_unlock(ctx->ctx_mutex);
	return r;
}

int usbyb_cancel_transfer(usbyb_transfer * tran)
{
	usbyb_device_handle * handle = (usbyb_device_handle *)tran->pub.dev_handle;

	if (ioctl(handle->wrfd, USBDEVFS_DISCARDURB, &tran->req) < 0)
		return usbfs_error();

	return LIBUSBY_SUCCESS;
}

int usbyb_open(usbyb_device_handle * handle)
{
	usbyb_device * dev = handle->pub.dev;
	char fdpath[32];
	int wrfd;

	sprintf(fdpath, "/proc/self/fd/%d", dev->fd);
	wrfd = open(fdpath, O_RDWR);
	if (wrfd == -1)
		return LIBUSBY_ERROR_ACCESS;

	handle->wrfd = wrfd;
	handle->active_config_value = -1;
	return LIBUSBY_SUCCESS;
}

void usbyb_close(usbyb_device_handle * handle)
{
	close(handle->wrfd);
	handle->wrfd = -1;
}

int usbyb_get_descriptor(usbyb_device_handle * handle, uint8_t desc_type, uint8_t desc_index, uint16_t langid, unsigned char * data, int length)
{
	return usbyb_get_descriptor_cached(handle->pub.dev, desc_type, desc_index, langid, data, length);
}

int usbyb_get_descriptor_cached(usbyb_device * dev, uint8_t desc_type, uint8_t desc_index, uint16_t langid, unsigned char * data, int length)
{
	int i;
	uint8_t * cache_ptr = dev->desc_cache;

	(void)langid;

	if (desc_type != 2/*CONFIGURATION*/)
		return LIBUSBY_ERROR_NOT_SUPPORTED;


	for (i = 0; i < dev->pub.device_desc.bNumConfigurations; ++i)
	{
		uint16_t wTotalLength = cache_ptr[2] | (cache_ptr[3] << 8);

		if (i == desc_index)
		{
			if (length > wTotalLength)
				length = wTotalLength;
			memcpy(data, cache_ptr, length);
			return length;
		}

		cache_ptr += wTotalLength;
	}

	return LIBUSBY_ERROR_INVALID_PARAM;
}

int usbyb_perform_transfer(usbyb_transfer * tran)
{
	usbyb_device_handle * handle = (usbyb_device_handle *)tran->pub.dev_handle;

	if (tran->pub.type == LIBUSBY_TRANSFER_TYPE_CONTROL)
	{
		int r;
		struct usbdevfs_ctrltransfer req;
		req.bRequestType = tran->pub.buffer[0];
		req.bRequest = tran->pub.buffer[1];
		req.wValue = tran->pub.buffer[2] | (tran->pub.buffer[3] << 8);
		req.wIndex = tran->pub.buffer[4] | (tran->pub.buffer[5] << 8);
		req.wLength = tran->pub.buffer[6] | (tran->pub.buffer[7] << 8);
		req.timeout = 0;
		req.data = tran->pub.buffer + 8;

		r = ioctl(handle->wrfd, USBDEVFS_CONTROL, &req);
		if (r >= 0)
		{
			tran->pub.actual_length = r + 8;
			tran->pub.status = LIBUSBY_TRANSFER_COMPLETED;
		}
		else
		{
			switch (errno)
			{
			case ENODEV:
				tran->pub.status = LIBUSBY_TRANSFER_NO_DEVICE;
				break;
			case EPIPE:
				tran->pub.status = LIBUSBY_TRANSFER_STALL;
				break;
			default:
				tran->pub.status = LIBUSBY_TRANSFER_ERROR;
			}
		}

		return LIBUSBY_SUCCESS;
	}

	return LIBUSBY_ERROR_NOT_SUPPORTED;
}

int usbyb_claim_interface(usbyb_device_handle * handle, int interface_number)
{
	if (ioctl(handle->wrfd, USBDEVFS_CLAIMINTERFACE, &interface_number) < 0)
		return usbfs_error();
	return LIBUSBY_SUCCESS;
}

int usbyb_release_interface(usbyb_device_handle * handle, int interface_number)
{
	if (ioctl(handle->wrfd, USBDEVFS_RELEASEINTERFACE, &interface_number) < 0)
		return usbfs_error();
	return LIBUSBY_SUCCESS;
}

int usbyb_get_configuration(usbyb_device_handle * handle, int * config_value, int cached_only)
{
	(void)cached_only;
	if (handle->active_config_value < 0)
		return LIBUSBY_ERROR_NOT_SUPPORTED;
	*config_value = handle->active_config_value;
	return LIBUSBY_SUCCESS;
}

int usbyb_set_configuration(usbyb_device_handle * handle, int config_value)
{
	if (ioctl(handle->wrfd, USBDEVFS_SETCONFIGURATION, &config_value) < 0)
		return usbfs_error();

	handle->active_config_value = config_value;
	return LIBUSBY_SUCCESS;
}
