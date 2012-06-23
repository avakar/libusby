#ifndef LIBUSBY_LIBUSBYI_H
#define LIBUSBY_LIBUSBYI_H

#include "libusby.h"
#include "libusbyi_fwd.h"
#include <stddef.h>
#include "os/os.h"

#define container_of(ptr, type, member) ((type *)((char *)ptr - offsetof(type, member)))

struct usbyi_device_list
{
	int count;
	int capacity;
	libusby_device ** list;
};

typedef struct usbyi_device_list_node
{
	struct usbyi_device_list_node * prev;
	struct usbyi_device_list_node * next;
} usbyi_device_list_node;

void usbyi_init_devlist_head(usbyi_device_list_node * head);
void usbyi_insert_before_devlist_node(usbyi_device_list_node * node, usbyi_device_list_node * next);
usbyi_device_list_node * usbyi_remove_devlist_node(usbyi_device_list_node * dev_node);

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
