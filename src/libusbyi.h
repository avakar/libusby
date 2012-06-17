#ifndef LIBUSBY_LIBUSBYI_H
#define LIBUSBY_LIBUSBYI_H

#ifdef _WIN32
#include "os/win32.h"
#elif __linux
#include "os/linux_usbfs.h"
#else
#error Unsupported architecture.
#endif

#include "libusby.h"
#include "libusbyi_fwd.h"
#include <stddef.h>

#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

struct usbyi_device_list
{
	int count;
	int capacity;
	libusby_device ** list;
};

struct libusby_context
{
	usbyi_backend const * backend;
	// XXX: devicelist lock
	usbyi_device_list devices;
	usbyi_os_ctx os_priv;
};

struct libusby_device
{
	libusby_context * ctx;
	int ref_count;
	libusby_device_descriptor device_desc;
};

struct libusby_device_handle
{
	libusby_device * dev;
};

struct usbyi_transfer
{
	libusby_context * ctx;
	int num_iso_packets;
	usbyi_os_transfer os_priv;
	void * priv;
	usbyi_transfer * next;
	usbyi_transfer * prev;
};

struct usbyi_backend
{
	int context_private_size;
	int device_private_size;
	int device_handle_private_size;

	int (*init)(libusby_context *ctx);
	void (*exit)(libusby_context *ctx);

	int (*get_device_list)(libusby_context * ctx, libusby_device *** list);

	int (*open)(libusby_device_handle *dev_handle); // opt
	void (*close)(libusby_device_handle *dev_handle); // opt

	int (*get_descriptor)(libusby_device_handle * dev_handle, uint8_t desc_type, uint8_t desc_index, unsigned char * data, int length); // opt
	int (*get_descriptor_cached)(libusby_device * dev, uint8_t desc_type, uint8_t desc_index, unsigned char * data, int length); // opt

	int (*get_configuration)(libusby_device_handle * dev_handle, int * config_value, int cached_only); // opt
	int (*set_configuration)(libusby_device_handle * dev_handle, int config_value); // opt

	int (*claim_interface)(libusby_device_handle * dev_handle, int interface_number); // opt
	int (*release_interface)(libusby_device_handle * dev_handle, int interface_number); // opt

	int (*perform_transfer)(libusby_transfer * tran); // opt
	int (*submit_transfer)(libusby_transfer * tran);
	int (*cancel_transfer)(libusby_transfer * tran);
	void (*reap_transfer)(usbyi_transfer * tran);
};

int usbyi_init(libusby_context ** ctx, usbyi_backend const * backend);
libusby_device * usbyi_alloc_device(libusby_context * ctx);
usbyi_transfer * usbyi_tran_to_trani(libusby_transfer * tran);
libusby_transfer * usbyi_trani_to_tran(usbyi_transfer * trani);
void * usbyi_dev_to_devpriv(libusby_device * dev);
void * usbyi_handle_to_handlepriv(libusby_device_handle * handle);
void * usbyi_ctx_to_priv(libusby_context * ctx);
int usbyi_append_device_list(usbyi_device_list * devices, libusby_device * dev);
int usbyi_sanitize_device_desc(libusby_device_descriptor * desc, uint8_t * rawdesc);

#endif // LIBUSBY_LIBUSBYI_H
