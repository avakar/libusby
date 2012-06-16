#include "linux_usbfs.h"
#include "../libusbyi.h"

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

struct watched_fd
{
    int fd;
    int refcount;
};

typedef struct usbfs_ctx_private
{
    pthread_mutex_t ctx_mutex;
    pthread_cond_t ctx_cond;
    int loop_enabled;
    int loop_locked;

    struct watched_fd * watched_fds;
    int watched_fd_count;
    int watched_fd_capacity;

    int pipe[2];
} usbfs_ctx_private;

typedef struct usbfs_device_private
{
    int busno;
    int devno;
    int fd;
} usbfs_device_private;

typedef struct usbfs_devhandle_private
{
    int wrfd;
    int active_config_value;
} usbfs_devhandle_private;

int usbyi_init_os_ctx(libusby_context * ctx)
{
    return LIBUSBY_SUCCESS;
}

void usbyi_clear_os_ctx(libusby_context * ctx)
{
}

int usbyi_init_os_transfer(usbyi_transfer * tran)
{
    if (pthread_cond_init(&tran->os_priv.cond, 0) != 0)
        return LIBUSBY_ERROR_NO_MEM;

    tran->os_priv.active = 0;
    return LIBUSBY_SUCCESS;
}

void usbyi_clear_os_transfer(usbyi_transfer * tran)
{
    pthread_cond_destroy(&tran->os_priv.cond);
}

static void usbfs_unwatch_fd(usbfs_ctx_private * ctxpriv, int fdindex)
{
    if (--ctxpriv->watched_fds[fdindex].refcount == 0)
    {
        if (--ctxpriv->watched_fd_count)
            ctxpriv->watched_fds[fdindex] = ctxpriv->watched_fds[ctxpriv->watched_fd_count];
    }
}

int libusby_run_event_loop_impl(libusby_context * ctx, libusby_transfer * tran)
{
    usbfs_ctx_private * ctxpriv = usbyi_ctx_to_priv(ctx);
    struct pollfd * pollfds = 0;
    int pollfds_cap = 0;
    int i;
    int r = LIBUSBY_SUCCESS;

    while (ctxpriv->loop_enabled && ctxpriv->loop_locked)
        pthread_cond_wait(&ctxpriv->ctx_cond, &ctxpriv->ctx_mutex);
    ctxpriv->loop_locked = 1;

    while (r >= 0 && ctxpriv->loop_enabled)
    {
        int fdcount = ctxpriv->watched_fd_count;

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

        pollfds[fdcount].fd = ctxpriv->pipe[0];
        pollfds[fdcount].events = POLLIN;
        pollfds[fdcount].revents = 0;

        for (i = 0; i < fdcount; ++i)
        {
            pollfds[i].fd = ctxpriv->watched_fds[i].fd;
            pollfds[i].events = POLLOUT;
            pollfds[i].revents = 0;
        }

        pthread_mutex_unlock(&ctxpriv->ctx_mutex);

        if (poll(pollfds, fdcount+1, -1) < 0)
        {
            r = LIBUSBY_ERROR_IO;
        }
        else
        {
            for (i = 0; i < fdcount; ++i)
            {
                struct usbdevfs_urb * urb;
                libusby_transfer * tran;
                usbyi_transfer * trani;

                if ((pollfds[i].revents & POLLOUT) == 0)
                    continue;

                if (ioctl(pollfds[i].fd, USBDEVFS_REAPURBNDELAY, &urb) < 0)
                    continue;

                tran = urb->usercontext;
                trani = usbyi_tran_to_trani(tran);

                tran->actual_length = trani->os_priv.req.actual_length;
                if (tran->type == LIBUSBY_TRANSFER_TYPE_CONTROL)
                    tran->actual_length += 8;

                if (trani->os_priv.req.status != 0)
                    tran->status = LIBUSBY_ERROR_IO;

                pthread_mutex_lock(&ctxpriv->ctx_mutex);
                trani->os_priv.active = 2;

                if (tran->callback)
                {
                    pthread_mutex_unlock(&ctxpriv->ctx_mutex);
                    tran->callback(tran);
                    pthread_mutex_lock(&ctxpriv->ctx_mutex);
                }

                if (trani->os_priv.active == 2)
                {
                    trani->os_priv.active = 0;
                    pthread_cond_broadcast(&trani->os_priv.cond);
                }

                usbfs_unwatch_fd(ctxpriv, i);
                pthread_mutex_unlock(&ctxpriv->ctx_mutex);
            }

            if (pollfds[fdcount].revents & POLLIN)
            {
                char dummy;
                if (read(pollfds[fdcount].fd, &dummy, 1) < 0)
                    r = LIBUSBY_ERROR_IO;
            }
        }

        pthread_mutex_lock(&ctxpriv->ctx_mutex);
    }

    free(pollfds);
    ctxpriv->loop_locked = 0;
    return r;
}

int libusby_run_event_loop(libusby_context * ctx)
{
    usbfs_ctx_private * ctxpriv = usbyi_ctx_to_priv(ctx);
    int r;

    pthread_mutex_lock(&ctxpriv->ctx_mutex);
    r = libusby_run_event_loop_impl(ctx, 0);
    pthread_mutex_unlock(&ctxpriv->ctx_mutex);
    return r;
}

int libusby_wait_for_transfer(libusby_transfer * tran)
{
    usbyi_transfer * trani = usbyi_tran_to_trani(tran);
    usbfs_ctx_private * ctxpriv = usbyi_ctx_to_priv(trani->ctx);
    int r = LIBUSBY_SUCCESS;

    pthread_mutex_lock(&ctxpriv->ctx_mutex);
    if (!ctxpriv->loop_locked)
        r = libusby_run_event_loop_impl(trani->ctx, tran);

    while (trani->os_priv.active)
        pthread_cond_wait(&trani->os_priv.cond, &ctxpriv->ctx_mutex);

    pthread_mutex_unlock(&ctxpriv->ctx_mutex);
    return r;
}

void libusby_stop_event_loop(libusby_context * ctx)
{
    usbfs_ctx_private * ctxpriv = usbyi_ctx_to_priv(ctx);
    pthread_mutex_lock(&ctxpriv->ctx_mutex);
    ctxpriv->loop_enabled = 0;
    if (ctxpriv->loop_locked)
    {
        char dummy = 's';
        write(ctxpriv->pipe[1], &dummy, 1);
    }
    pthread_cond_broadcast(&ctxpriv->ctx_cond);
    pthread_mutex_unlock(&ctxpriv->ctx_mutex);
}

void libusby_reset_event_loop(libusby_context * ctx)
{
    usbfs_ctx_private * ctxpriv = usbyi_ctx_to_priv(ctx);
    pthread_mutex_lock(&ctxpriv->ctx_mutex);
    ctxpriv->loop_enabled = 1;
    pthread_mutex_unlock(&ctxpriv->ctx_mutex);
}

static int usbfs_init(libusby_context * ctx)
{
    usbfs_ctx_private * ctxpriv = usbyi_ctx_to_priv(ctx);
    if (pthread_mutex_init(&ctxpriv->ctx_mutex, NULL) < 0)
        return LIBUSBY_ERROR_NO_MEM;

    if (pthread_cond_init(&ctxpriv->ctx_cond, NULL) < 0)
    {
        pthread_mutex_destroy(&ctxpriv->ctx_mutex);
        return LIBUSBY_ERROR_NO_MEM;
    }

    if (pipe(ctxpriv->pipe) < 0)
    {
        pthread_cond_destroy(&ctxpriv->ctx_cond);
        pthread_mutex_destroy(&ctxpriv->ctx_mutex);
        return LIBUSBY_ERROR_NO_MEM;
    }

    ctxpriv->loop_enabled = 1;
    ctxpriv->loop_locked = 0;
    return LIBUSBY_SUCCESS;
}

static void usbfs_exit(libusby_context * ctx)
{
    usbfs_ctx_private * ctxpriv = usbyi_ctx_to_priv(ctx);
    free(ctxpriv->watched_fds);
    close(ctxpriv->pipe[0]);
    close(ctxpriv->pipe[1]);
    pthread_cond_destroy(&ctxpriv->ctx_cond);
    pthread_mutex_destroy(&ctxpriv->ctx_mutex);
}

static int usbfs_get_device_list(libusby_context * ctx, libusby_device *** list)
{
    usbyi_device_list devlist = {0};
    DIR * dir = 0;
    DIR * busdir = 0;
    struct dirent * ent;
    int fd = -1;
    int r;

    dir = opendir("/dev/bus/usb");
    if (!dir)
    {
        *list = NULL;
        return LIBUSBY_SUCCESS;
    }

    while (ent = readdir(dir))
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

        while (busent = readdir(busdir))
        {
            int i;
            int devno = strtol(busent->d_name, &end, 10);

            if (*end != 0)
                continue;

            strcpy(fname, "/dev/bus/usb/");
            strcat(fname, ent->d_name);
            strcat(fname, "/");
            strcat(fname, busent->d_name);

            fd = open(fname, O_RDONLY);
            if (fd == -1)
                continue;

            for (i = 0; i < ctx->devices.count; ++i)
            {
                usbfs_device_private * devpriv = usbyi_dev_to_devpriv(ctx->devices.list[i]);
                if (devpriv->busno == busno && devpriv->devno == devno)
                {
                    r = usbyi_append_device_list(&devlist, ctx->devices.list[i]);
                    if (r < 0)
                        goto error;
                    libusby_ref_device(ctx->devices.list[i]);
                    break;
                }
            }

            if (i == ctx->devices.count)
            {
                libusby_device * dev = usbyi_alloc_device(ctx);
                usbfs_device_private * devpriv;
                if (!dev)
                {
                    r = LIBUSBY_ERROR_NO_MEM;
                    goto error;
                }

                r = usbyi_append_device_list(&devlist, dev);
                if (r < 0)
                {
                    libusby_unref_device(dev);
                    continue;
                }

                devpriv = usbyi_dev_to_devpriv(dev);
                devpriv->busno = busno;
                devpriv->devno = devno;
                devpriv->fd = fd;

                {
                    char rawdesc[0x12]; // XXX
                    if (read(fd, rawdesc, sizeof rawdesc) == sizeof rawdesc)
                        usbyi_sanitize_device_desc(&dev->device_desc, rawdesc);
                }
            }
        }

        closedir(busdir);
    }

    closedir(dir);

    *list = devlist.list;
    return devlist.count;

error:
    if (fd != -1)
        close(fd);
    closedir(busdir);
    closedir(dir);
    if (devlist.list)
        libusby_free_device_list(devlist.list, 1);
    return r;
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

static int usbfs_submit_transfer(libusby_transfer * tran)
{
    usbfs_devhandle_private * handlepriv = usbyi_handle_to_handlepriv(tran->dev_handle);
    usbyi_transfer * trani = usbyi_tran_to_trani(tran);
    usbyi_os_transfer * ostran = &trani->os_priv;
    usbfs_ctx_private * ctxpriv = usbyi_ctx_to_priv(tran->dev_handle->dev->ctx);
    int i;
    int r = LIBUSBY_SUCCESS;

    memset(&ostran->req, 0, sizeof ostran->req);

    switch (tran->type)
    {
    case LIBUSBY_TRANSFER_TYPE_CONTROL:
        ostran->req.type = USBDEVFS_URB_TYPE_CONTROL;
        break;
    case LIBUSBY_TRANSFER_TYPE_BULK:
        ostran->req.type = USBDEVFS_URB_TYPE_BULK;
        break;
    case LIBUSBY_TRANSFER_TYPE_INTERRUPT:
        ostran->req.type = USBDEVFS_URB_TYPE_INTERRUPT;
        break;
    default:
        return LIBUSBY_ERROR_NOT_SUPPORTED;
    }

    ostran->req.endpoint = tran->endpoint;
    ostran->req.buffer = tran->buffer;
    ostran->req.buffer_length = tran->length;
    ostran->req.usercontext = tran;

    pthread_mutex_lock(&ctxpriv->ctx_mutex);

    for (i = 0; i != ctxpriv->watched_fd_count; ++i)
    {
        if (ctxpriv->watched_fds[i].fd == handlepriv->wrfd)
            break;
    }

    if (ctxpriv->watched_fd_count == i)
    {
        if (ctxpriv->watched_fd_count == ctxpriv->watched_fd_capacity)
        {
            int newcap = ctxpriv->watched_fd_capacity == 0? 4: ctxpriv->watched_fd_capacity * 3 / 2;
            struct watched_fd * newfds = realloc(ctxpriv->watched_fds, sizeof(struct watched_fd) * newcap);
            if (!newfds)
                r = LIBUSBY_ERROR_NO_MEM;
            ctxpriv->watched_fds = newfds;
            ctxpriv->watched_fd_capacity = newcap;
        }

        ++ctxpriv->watched_fd_count;
        ctxpriv->watched_fds[i].fd = handlepriv->wrfd;
        ctxpriv->watched_fds[i].refcount = 0;
    }

    if (r >= 0 && ioctl(handlepriv->wrfd, USBDEVFS_SUBMITURB, &ostran->req) < 0)
    {
        usbfs_unwatch_fd(ctxpriv, i);
        r = usbfs_error();
    }

    if (r >= 0 && ctxpriv->loop_locked)
    {
        int dummy = 'u';
        write(ctxpriv->pipe[1], &dummy, 1);
        trani->os_priv.active = 1;
    }

    pthread_mutex_unlock(&ctxpriv->ctx_mutex);
    return r;
}

static int usbfs_cancel_transfer(libusby_transfer * tran)
{
    usbfs_devhandle_private * handlepriv = usbyi_handle_to_handlepriv(tran->dev_handle);
    usbyi_transfer * trani = usbyi_tran_to_trani(tran);
    usbyi_os_transfer * ostran = &trani->os_priv;

    if (ioctl(handlepriv->wrfd, USBDEVFS_DISCARDURB, &ostran->req) < 0)
        return usbfs_error();

    return LIBUSBY_SUCCESS;
}

static int usbfs_open(libusby_device_handle *dev_handle)
{
    usbfs_device_private * devpriv = usbyi_dev_to_devpriv(dev_handle->dev);
    usbfs_devhandle_private * handlepriv = usbyi_handle_to_handlepriv(dev_handle);
    char fdpath[32];
    int wrfd;

    sprintf(fdpath, "/proc/self/fd/%d", devpriv->fd);
    wrfd = open(fdpath, O_RDWR);
    if (wrfd == -1)
        return LIBUSBY_ERROR_ACCESS;

    handlepriv->wrfd = wrfd;
    handlepriv->active_config_value = -1;
    return LIBUSBY_SUCCESS;
}

static void usbfs_close(libusby_device_handle *dev_handle)
{
    usbfs_devhandle_private * handlepriv = usbyi_handle_to_handlepriv(dev_handle);
    close(handlepriv->wrfd);
}

static int usbfs_get_descriptor(libusby_device_handle * dev_handle, uint8_t desc_type, uint8_t desc_index, unsigned char * data, int length)
{
    return LIBUSBY_ERROR_NOT_SUPPORTED;
}

static int usbfs_perform_transfer(libusby_transfer * tran)
{
    int r;
    usbfs_devhandle_private * handlepriv = usbyi_handle_to_handlepriv(tran->dev_handle);
    if (tran->type == LIBUSBY_TRANSFER_TYPE_CONTROL)
    {
        struct usbdevfs_ctrltransfer req;
        req.bRequestType = tran->buffer[0];
        req.bRequest = tran->buffer[1];
        req.wValue = tran->buffer[2] | (tran->buffer[3] << 8);
        req.wIndex = tran->buffer[4] | (tran->buffer[5] << 8);
        req.wLength = tran->buffer[6] | (tran->buffer[7] << 8);
        req.timeout = 0;
        req.data = tran->buffer + 8;

        r = ioctl(handlepriv->wrfd, USBDEVFS_CONTROL, &req);
        if (r >= 0)
        {
            tran->actual_length = r + 8;
            tran->status = LIBUSBY_TRANSFER_COMPLETED;
        }
        else
        {
            switch (errno)
            {
            case ENODEV:
                tran->status = LIBUSBY_TRANSFER_NO_DEVICE;
                break;
            case EPIPE:
                tran->status = LIBUSBY_TRANSFER_STALL;
                break;
            default:
                tran->status = LIBUSBY_TRANSFER_ERROR;
            }
        }

        return LIBUSBY_SUCCESS;
    }

    return LIBUSBY_ERROR_NOT_SUPPORTED;
}

static int usbfs_claim_interface(libusby_device_handle * dev_handle, int interface_number)
{
    usbfs_devhandle_private * handlepriv = usbyi_handle_to_handlepriv(dev_handle);
    if (ioctl(handlepriv->wrfd, USBDEVFS_CLAIMINTERFACE, &interface_number) < 0)
        return usbfs_error();
    return LIBUSBY_SUCCESS;
}

static int usbfs_release_interface(libusby_device_handle * dev_handle, int interface_number)
{
    usbfs_devhandle_private * handlepriv = usbyi_handle_to_handlepriv(dev_handle);
    if (ioctl(handlepriv->wrfd, USBDEVFS_RELEASEINTERFACE, &interface_number) < 0)
        return usbfs_error();
    return LIBUSBY_SUCCESS;
}

int usbfs_get_configuration(libusby_device_handle * dev_handle, int * config_value, int cached_only)
{
    usbfs_devhandle_private * handlepriv = usbyi_handle_to_handlepriv(dev_handle);
    if (handlepriv->active_config_value < 0)
        return LIBUSBY_ERROR_NOT_SUPPORTED;
    *config_value = handlepriv->active_config_value;
    return LIBUSBY_SUCCESS;
}

int usbfs_set_configuration(libusby_device_handle * dev_handle, int config_value)
{
    usbfs_devhandle_private * handlepriv = usbyi_handle_to_handlepriv(dev_handle);

    if (ioctl(handlepriv->wrfd, USBDEVFS_SETCONFIGURATION, &config_value) < 0)
        return usbfs_error();

    handlepriv->active_config_value = config_value;
    return LIBUSBY_SUCCESS;
}

static usbyi_backend const linux_usbfs_backend = {
    sizeof(usbfs_ctx_private),
    sizeof(usbfs_device_private),
    sizeof(usbfs_devhandle_private),

    &usbfs_init,
    &usbfs_exit,

    &usbfs_get_device_list,

    &usbfs_open,
    &usbfs_close,

    &usbfs_get_descriptor,
    &usbfs_get_configuration,
    &usbfs_set_configuration,

    &usbfs_claim_interface,
    &usbfs_release_interface,

    &usbfs_perform_transfer,
    &usbfs_submit_transfer,
    &usbfs_cancel_transfer,
    0, //&usbfs_reap_transfer,
};

int libusby_init(libusby_context **ctx)
{
    return usbyi_init(ctx, &linux_usbfs_backend);
}
