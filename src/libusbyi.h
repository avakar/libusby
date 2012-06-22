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
#include "os/os.h"

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
	libusby_context * ctx;
	int num_iso_packets;
	usbyi_os_transfer os_priv;
	void * priv;
	usbyi_transfer * next;
	usbyi_transfer * prev;
};

int libusby_init(libusby_context ** ctx);
usbyb_device * usbyi_alloc_device(libusby_context * ctx);
usbyi_transfer * usbyi_tran_to_trani(libusby_transfer * tran);
libusby_transfer * usbyi_trani_to_tran(usbyi_transfer * trani);
int usbyi_append_device_list(usbyi_device_list * devices, libusby_device * dev);
int usbyi_sanitize_device_desc(libusby_device_descriptor * desc, uint8_t * rawdesc);

#endif // LIBUSBY_LIBUSBYI_H
