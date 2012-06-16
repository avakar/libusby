#ifndef LIBUSBY_OS_LINUX_USBFS_H
#define LIBUSBY_OS_LINUX_USBFS_H

#include "../libusby.h"
#include "../libusbyi_fwd.h"
#include <pthread.h>
#include <linux/usbdevice_fs.h>

struct usbyi_os_ctx
{
	int nn;
};

struct usbyi_os_transfer
{
    pthread_cond_t cond;
    int active;
    struct usbdevfs_urb req;
};

int usbyi_init_os_ctx(libusby_context * ctx);
void usbyi_clear_os_ctx(libusby_context * ctx);

int usbyi_init_os_transfer(usbyi_transfer * trani);
void usbyi_clear_os_transfer(usbyi_transfer * trani);

#endif // LIBUSBY_OS_LINUX_USBFS_H
