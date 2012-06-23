#ifndef LIBUSBY_LIBUSBYI_H
#define LIBUSBY_LIBUSBYI_H

#include "libusby.h"
#include "libusbyi_fwd.h"
#include <stddef.h>
#include "os/os.h"

struct usbyi_device_list
{
	int count;
	int capacity;
	libusby_device ** list;
};

struct libusby_context
{
	// XXX: devicelist lock
	usbyi_device_list devices;
};

struct libusby_device
{
	libusby_context * ctx;
	int ref_count;
	libusby_device_descriptor device_desc;
};

struct libusby_device_handle
{
	usbyb_device * dev;
};

struct usbyi_transfer
{
	usbyb_context * ctx;
	int num_iso_packets;
	void * priv;
	usbyb_transfer * next;
	usbyb_transfer * prev;
};

usbyb_device * usbyi_alloc_device(libusby_context * ctx);
int usbyi_append_device_list(usbyi_device_list * devices, libusby_device * dev);
int usbyi_sanitize_device_desc(libusby_device_descriptor * desc, uint8_t * rawdesc);

libusby_transfer * usbyi_get_pub_tran(usbyb_transfer * tran);
usbyb_transfer * usbyi_get_tran(libusby_transfer * tran);

#endif // LIBUSBY_LIBUSBYI_H
