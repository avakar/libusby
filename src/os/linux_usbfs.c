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

struct watched_fd
{
    int fd;
    int refcount;
};

struct libusby_context
{
    usbyi_device_list_node devlist_head;

    pthread_mutex_t ctx_mutex;
    pthread_cond_t ctx_cond;
    int loop_enabled;
    int loop_locked;

    struct watched_fd * watched_fds;
    int watched_fd_count;
    int watched_fd_capacity;

    int pipe[2];
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

static void usbfs_unwatch_fd(usbyb_context * ctx, int fdindex)
{
    if (--ctx->watched_fds[fdindex].refcount == 0)
    {
        if (--ctx->watched_fd_count)
            ctx->watched_fds[fdindex] = ctx->watched_fds[ctx->watched_fd_count];
    }
}

static int usbfs_run_event_loop_impl(usbyb_context * ctx, usbyb_transfer * watch_tran)
{
    struct pollfd * pollfds = 0;
    int pollfds_cap = 0;
    int i;
    int r = LIBUSBY_SUCCESS;

    while (ctx->loop_enabled && ctx->loop_locked)
        pthread_cond_wait(&ctx->ctx_cond, &ctx->ctx_mutex);
    ctx->loop_locked = 1;

    while ((!watch_tran || watch_tran->active) && r >= 0 && ctx->loop_enabled)
    {
        int fdcount = ctx->watched_fd_count;

        if (pollfds_cap < fdcount + 1)
        {
            struct pollfd * newpollfds;
            newpollfds = realloc(pollfds, sizeof(struct pollfd) * (fdcount + 1));
            if (!newpollfds)
            {
                r = LIBUSBY_ERROR_NO_MEM;
                break;
            }

            pollfds = newpollfds;
            pollfds_cap = fdcount + 1;
        }

        pollfds[fdcount].fd = ctx->pipe[0];
        pollfds[fdcount].events = POLLIN;
        pollfds[fdcount].revents = 0;

        for (i = 0; i < fdcount; ++i)
        {
            pollfds[i].fd = ctx->watched_fds[i].fd;
            pollfds[i].events = POLLOUT;
            pollfds[i].revents = 0;
        }

        pthread_mutex_unlock(&ctx->ctx_mutex);

        if (poll(pollfds, fdcount+1, -1) < 0)
        {
            r = LIBUSBY_ERROR_IO;
        }
        else
        {
            for (i = 0; i < fdcount; ++i)
            {
                struct usbdevfs_urb * urb;
                usbyb_transfer * tran;

                if ((pollfds[i].revents & POLLOUT) == 0)
                    continue;

                if (ioctl(pollfds[i].fd, USBDEVFS_REAPURBNDELAY, &urb) < 0)
                    continue;

                tran = urb->usercontext;

                tran->pub.actual_length = tran->req.actual_length;
                if (tran->pub.type == LIBUSBY_TRANSFER_TYPE_CONTROL)
                    tran->pub.actual_length += 8;

                if (tran->req.status != 0)
                    tran->pub.status = LIBUSBY_ERROR_IO;

                pthread_mutex_lock(&ctx->ctx_mutex);
                tran->active = 2;

                if (tran->pub.callback)
                {
                    pthread_mutex_unlock(&ctx->ctx_mutex);
                    tran->pub.callback(&tran->pub);
                    pthread_mutex_lock(&ctx->ctx_mutex);
                }

                if (tran->active == 2)
                {
                    tran->active = 0;
                    pthread_cond_broadcast(&tran->cond);
                }

                usbfs_unwatch_fd(ctx, i);
                pthread_mutex_unlock(&ctx->ctx_mutex);
            }

            if (pollfds[fdcount].revents & POLLIN)
            {
                char dummy;
                if (read(pollfds[fdcount].fd, &dummy, 1) < 0)
                    r = LIBUSBY_ERROR_IO;
            }
        }

        pthread_mutex_lock(&ctx->ctx_mutex);
    }

    free(pollfds);
    ctx->loop_locked = 0;
    return r;
}

int usbyb_run_event_loop(usbyb_context * ctx)
{
    int r;
    pthread_mutex_lock(&ctx->ctx_mutex);
    r = usbfs_run_event_loop_impl(ctx, 0);
    pthread_mutex_unlock(&ctx->ctx_mutex);
    return r;
}

int usbyb_wait_for_transfer(usbyb_transfer * tran)
{
    usbyb_context * ctx = tran->intrn.ctx;
    int r = LIBUSBY_SUCCESS;

    pthread_mutex_lock(&ctx->ctx_mutex);
    if (!ctx->loop_locked)
        r = usbfs_run_event_loop_impl(ctx, tran);

    while (tran->active)
        pthread_cond_wait(&tran->cond, &ctx->ctx_mutex);

    pthread_mutex_unlock(&ctx->ctx_mutex);
    return r;
}

void usbyb_stop_event_loop(usbyb_context * ctx)
{
    pthread_mutex_lock(&ctx->ctx_mutex);
    ctx->loop_enabled = 0;
    if (ctx->loop_locked)
    {
        char dummy = 's';
        write(ctx->pipe[1], &dummy, 1);
    }
    pthread_cond_broadcast(&ctx->ctx_cond);
    pthread_mutex_unlock(&ctx->ctx_mutex);
}

void usbyb_reset_event_loop(usbyb_context * ctx)
{
    pthread_mutex_lock(&ctx->ctx_mutex);
    ctx->loop_enabled = 1;
    pthread_mutex_unlock(&ctx->ctx_mutex);
}

int usbyb_init(usbyb_context * ctx)
{
    usbyi_init_devlist_head(&ctx->devlist_head);

    if (pthread_mutex_init(&ctx->ctx_mutex, NULL) < 0)
        return LIBUSBY_ERROR_NO_MEM;

    if (pthread_cond_init(&ctx->ctx_cond, NULL) < 0)
    {
        pthread_mutex_destroy(&ctx->ctx_mutex);
        return LIBUSBY_ERROR_NO_MEM;
    }

    if (pipe(ctx->pipe) < 0)
    {
        pthread_cond_destroy(&ctx->ctx_cond);
        pthread_mutex_destroy(&ctx->ctx_mutex);
        return LIBUSBY_ERROR_NO_MEM;
    }

    ctx->loop_enabled = 1;
    ctx->loop_locked = 0;
    return LIBUSBY_SUCCESS;
}

void usbyb_exit(usbyb_context * ctx)
{
    assert(ctx->devlist_head.next == &ctx->devlist_head);
    free(ctx->watched_fds);
    close(ctx->pipe[0]);
    close(ctx->pipe[1]);
    pthread_cond_destroy(&ctx->ctx_cond);
    pthread_mutex_destroy(&ctx->ctx_mutex);
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

            pthread_mutex_lock(&ctx->ctx_mutex);
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

            pthread_mutex_unlock(&ctx->ctx_mutex);
        }

        closedir(busdir);
    }

    closedir(dir);

    *list = devlist.list;
    return devlist.count;

error:
    pthread_mutex_unlock(&ctx->ctx_mutex);
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

int usbyb_submit_transfer(usbyb_transfer * tran)
{
    usbyb_device_handle * handle = (usbyb_device_handle *)tran->pub.dev_handle;
    usbyb_context * ctx = (usbyb_context *)handle->pub.dev->pub.ctx;
    int i;
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

    pthread_mutex_lock(&ctx->ctx_mutex);

    for (i = 0; i != ctx->watched_fd_count; ++i)
    {
        if (ctx->watched_fds[i].fd == handle->wrfd)
            break;
    }

    if (ctx->watched_fd_count == i)
    {
        if (ctx->watched_fd_count == ctx->watched_fd_capacity)
        {
            int newcap = ctx->watched_fd_capacity == 0? 4: ctx->watched_fd_capacity * 3 / 2;
            struct watched_fd * newfds = realloc(ctx->watched_fds, sizeof(struct watched_fd) * newcap);
            if (!newfds)
                r = LIBUSBY_ERROR_NO_MEM;
            ctx->watched_fds = newfds;
            ctx->watched_fd_capacity = newcap;
        }

        ++ctx->watched_fd_count;
        ctx->watched_fds[i].fd = handle->wrfd;
        ctx->watched_fds[i].refcount = 0;
    }

    if (r >= 0 && ioctl(handle->wrfd, USBDEVFS_SUBMITURB, &tran->req) < 0)
    {
        usbfs_unwatch_fd(ctx, i);
        r = usbfs_error();
    }

    if (r >= 0 && ctx->loop_locked)
    {
        int dummy = 'u';
        write(ctx->pipe[1], &dummy, 1);
        tran->active = 1;
    }

    pthread_mutex_unlock(&ctx->ctx_mutex);
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

int usbyb_get_descriptor(usbyb_device_handle * handle, uint8_t desc_type, uint8_t desc_index, unsigned char * data, int length)
{
    return usbyb_get_descriptor_cached(handle->pub.dev, desc_type, desc_index, data, length);
}

int usbyb_get_descriptor_cached(usbyb_device * dev, uint8_t desc_type, uint8_t desc_index, unsigned char * data, int length)
{
    int i;
    uint8_t * cache_ptr = dev->desc_cache;

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
