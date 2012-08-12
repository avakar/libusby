#include "../libspy.h"
#include "libpolly_posix.h"
#include <assert.h>
#include <libudev.h>
#include <stdio.h>
#include <memory.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <termios.h>

#include <errno.h>
#include <pthread.h>

#include <stdlib.h>

struct libspy_context
{
	libpolly_context * polly;
	pthread_mutex_t * loop_mutex;
};

struct libspy_device_handle
{
	libspy_context * ctx;
	int fd;
};

typedef enum spyi_transfer_flags
{
	spyi_tf_resubmit = 1
} spyi_transfer_flags;

struct libspy_transfer
{
	libspy_context * ctx;
	void * user_data;

	uint8_t * buf;
	int length;
	int actual_length;

	int flags;

	libspy_device_handle * handle;
	void (* callback)(libspy_transfer * transfer, libspy_transfer_status status);

	pthread_cond_t cond;
	int active;
	libpolly_task * cancel_task;
};

void libspy_set_user_data(libspy_transfer * transfer, void * user_data)
{
	transfer->user_data = user_data;
}

void * libspy_get_user_data(libspy_transfer * transfer)
{
	return transfer->user_data;
}

int libspy_get_transfer_length(libspy_transfer * transfer)
{
	return transfer->actual_length;
}

int libspy_init_with_polly(libspy_context ** ctx, libpolly_context * polly)
{
	libspy_context * res = malloc(sizeof(libspy_context));
	if (!res)
		return LIBSPY_ERROR_NO_MEM;

	res->polly = polly;
	res->loop_mutex = libpolly_posix_get_loop_mutex(polly);

	libpolly_ref_context(polly);
	*ctx = res;
	return LIBSPY_SUCCESS;
}

void libspy_exit(libspy_context * ctx)
{
	libpolly_unref_context(ctx->polly);
	free(ctx);
}

void libspy_close(libspy_device_handle *handle)
{
	close(handle->fd);
	free(handle);
}

int libspy_write(libspy_device_handle *handle, const uint8_t *buf, int len)
{
	int r;

	struct pollfd pfd;
	pfd.fd = handle->fd;
	pfd.events = POLLOUT;
	pfd.revents = 0;

	if (poll(&pfd, 1, -1) < 0)
		return LIBUSBY_ERROR_IO;
	assert(!(pfd.revents & POLLNVAL));

	if (pfd.revents & POLLHUP)
		return 0;

	if (pfd.revents & POLLERR)
		return LIBUSBY_ERROR_IO;

	assert(pfd.revents & POLLOUT);
	r = write(handle->fd, buf, len);
	if (r < 0)
	{
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		return LIBUSBY_ERROR_IO;
	}
	return r;
}

int libspy_open(libspy_context * ctx, char const * path, libspy_device_settings const * settings, libspy_device_handle ** handle)
{
	struct termios tios;
	libspy_device_handle * res;
	
	if (settings && settings->parity != libspy_parity_none && settings->parity != libspy_parity_odd && settings->parity != libspy_parity_even)
		return LIBUSBY_ERROR_INVALID_PARAM;

	res = malloc(sizeof(libspy_device_handle));
	if (!res)
		return LIBUSBY_ERROR_NO_MEM;

	res->ctx = (libspy_context *)ctx;
	res->fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (res->fd < 0)
	{
		free(res);
		return LIBUSBY_ERROR_NOT_FOUND; // XXX: return correct error code
	}

	if (settings && tcgetattr(res->fd, &tios) == 0)
	{
		speed_t speed = B0;
		switch (settings->baud_rate)
		{
		case 50:     speed = B50; break;
		case 75:     speed = B75; break;
		case 110:    speed = B110; break;
		case 134:    speed = B134; break;
		case 150:    speed = B150; break;
		case 200:    speed = B200; break;
		case 300:    speed = B300; break;
		case 600:    speed = B600; break;
		case 1200:   speed = B1200; break;
		case 1800:   speed = B1800; break;
		case 2400:   speed = B2400; break;
		case 4800:   speed = B4800; break;
		case 9600:   speed = B9600; break;
		case 19200:  speed = B19200; break;
		case 38400:  speed = B38400; break;
		case 57600:  speed = B57600; break;
		case 115200: speed = B115200; break;
		case 230400: speed = B230400; break;
		}

		if (speed != B0)
		{
			cfsetispeed(&tios, speed);
			cfsetospeed(&tios, speed);

			tios.c_iflag = 0;
			tios.c_oflag = 0;
			tios.c_cflag = CREAD;
			tios.c_lflag = 0;

			switch (settings->bits)
			{
			default:
				tios.c_cflag |= CS8;
				break;
			case 7:
				tios.c_cflag |= CS7;
				break;
			case 6:
				tios.c_cflag |= CS6;
				break;
			case 5:
				tios.c_cflag |= CS5;
				break;
			}

			if (settings->stopbits == libspy_stopbits_2)
				tios.c_cflag |= CSTOPB;

			switch (settings->parity)
			{
			default:
				break;
			case libspy_parity_odd:
				tios.c_cflag = PARENB | PARODD;
				tios.c_iflag = INPCK;
				break;
			case libspy_parity_even:
				tios.c_cflag = PARENB;
				tios.c_iflag = INPCK;
				break;
			}

			tcsetattr(res->fd, TCSANOW, &tios);
		}
	}

	*handle = res;
	return LIBUSBY_SUCCESS;
}

int libspy_begin_open(libspy_context *ctx, const char *path, const libspy_device_settings *settings, libspy_open_callback callback, void *user_data, libspy_open_future **future)
{
	libspy_device_handle * handle;
	int r = libspy_open(ctx, path, settings, &handle);
	if (r < 0)
		return r;

	*future = (libspy_open_future *)ctx;
	callback(*future, user_data, handle);
	return LIBUSBY_SUCCESS;
}

void libspy_wait_for_open(libspy_open_future *future)
{
	(void)future;
}

void libspy_cancel_open(libspy_open_future *future)
{
	(void)future;
}

void libspy_free_open_future(libspy_open_future *future)
{
	(void)future;
}

libspy_transfer * libspy_alloc_transfer(libspy_context * ctx)
{
	libspy_transfer * res = malloc(sizeof(libspy_transfer));
	if (!res)
		return 0;
	memset(res, 0, sizeof *res);

	if (pthread_cond_init(&res->cond, 0) != 0)
		return 0;

	res->cancel_task = 0;
	res->active = 0;

	res->ctx = ctx;
	return res;
}

void libspy_free_transfer(libspy_transfer * transfer)
{
	assert(transfer->active == 0);
	assert(transfer->cancel_task == 0);
	pthread_cond_destroy(&transfer->cond);
	free(transfer);
}

static void spyb_read_ready_callback(int fd, short revents, void * user_data)
{
	libspy_transfer * transfer = user_data;
	libspy_context * ctx = transfer->ctx;
	int r;
	
	if (revents == 0)
	{
		if (transfer->callback)
			transfer->callback(transfer, libspy_transfer_canceled);

		pthread_mutex_lock(ctx->loop_mutex);
		transfer->active = 0;
		if (transfer->cancel_task)
		{
			libpolly_cancel_task(transfer->cancel_task);
			transfer->cancel_task = 0;
		}
	}
	else
	{
		r = read(transfer->handle->fd, transfer->buf, transfer->length);
		if (r < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				transfer->actual_length = 0;
				if (transfer->callback)
					transfer->callback(transfer, libspy_transfer_completed);
			}
			else
			{
				if (transfer->callback)
					transfer->callback(transfer, libspy_transfer_error);
			}
		}
		else
		{
			transfer->actual_length = r;
			if (transfer->callback)
				transfer->callback(transfer, libspy_transfer_completed);
		}
	
		pthread_mutex_lock(ctx->loop_mutex);
		if (transfer->flags & spyi_tf_resubmit)
		{
			if (libpolly_posix_add_direct(ctx->polly, fd, POLLIN, &spyb_read_ready_callback, user_data) < 0)
			{
				pthread_mutex_unlock(ctx->loop_mutex);
				if (transfer->callback)
					transfer->callback(transfer, libspy_transfer_error);
	
				pthread_mutex_lock(ctx->loop_mutex);
				transfer->active = 0;
				if (transfer->cancel_task)
				{
					libpolly_cancel_task(transfer->cancel_task);
					transfer->cancel_task = 0;
				}
			}
		}
		else
		{
			transfer->active = 0;
			if (transfer->cancel_task)
			{
				libpolly_cancel_task(transfer->cancel_task);
				transfer->cancel_task = 0;
			}
		}
	}

	pthread_cond_broadcast(&transfer->cond);
	pthread_mutex_unlock(ctx->loop_mutex);
}

int libspy_submit_continuous_read(libspy_transfer * transfer,
	libspy_device_handle * handle, uint8_t * buf, int len,
	void (* callback)(libspy_transfer * transfer, libspy_transfer_status status))
{
	libspy_context * ctx = transfer->ctx;
	int r;

	assert(transfer->active == 0);
	assert(transfer->cancel_task == 0);

	transfer->callback = callback;
	transfer->handle = handle;
	transfer->flags = spyi_tf_resubmit;
	transfer->buf = buf;
	transfer->length = len;

	r = libpolly_prepare_task(ctx->polly, &transfer->cancel_task);
	if (r < 0)
		return r;
	r = libpolly_posix_add_direct(ctx->polly, handle->fd, POLLIN, &spyb_read_ready_callback, transfer);
	if (r < 0)
	{
		libpolly_cancel_task(transfer->cancel_task);
		transfer->cancel_task = 0;
	}
	else
	{
		transfer->active = 1;
	}
	return r;
}

static void spyb_cancel_callback(void * user_data)
{
	libspy_transfer * transfer = user_data;
	libpolly_posix_remove(transfer->ctx->polly, user_data);
}

void libspy_cancel_transfer(libspy_transfer * transfer)
{
	libpolly_task * task = 0;
	libspy_context * ctx = transfer->ctx;

	pthread_mutex_lock(ctx->loop_mutex);
	if (transfer->cancel_task)
	{
		task = transfer->cancel_task;
		transfer->cancel_task = 0;
	}
	pthread_mutex_unlock(ctx->loop_mutex);

	if (task)
		libpolly_submit_task(task, &spyb_cancel_callback, transfer);
}

void libspy_wait_for_transfer(libspy_transfer * transfer)
{
	libspy_context * ctx = transfer->ctx;
	libpolly_posix_loop_release_registration reg;
	reg.cond = &transfer->cond;

	pthread_mutex_lock(ctx->loop_mutex);
	libpolly_posix_register_loop_release_notification(ctx->polly, &reg);

	while (transfer->active)
		pthread_cond_wait(&transfer->cond, ctx->loop_mutex);

	libpolly_posix_unregister_loop_release_notification(&reg);
	pthread_mutex_unlock(ctx->loop_mutex);
}
