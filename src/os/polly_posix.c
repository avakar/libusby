#include "libpolly_posix.h"
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

struct pollyi_entry
{
	int fd;
	short events;
	libpolly_posix_callback callback;
	void * user_data;
};

struct libpolly_context
{
	int refcount;
	pthread_mutex_t entry_list_mutex; // and pollfd cache

	struct pollyi_entry * entry_list;
	int entry_list_size;
	int entry_list_reserve;
	int entry_list_capacity;

	// The cache might be grabbed by a currently executing event loop,
	// in which case `pollfd_cache` is 0. When `libpolly_posix_prepare_add` goes on
	// to resize the cache, it may actually resize the 0 pointer. That's fine --
	// the event loop will grab the new cache and dispose of the old one when
	// it updates the fd list.
	struct pollfd * pollfd_cache;
	int pollfd_cache_capacity;

	int control_pipe[2];

	pthread_mutex_t loop_mutex;
	int loop_token_acquired;

	libpolly_posix_loop_release_registration * release_registration_list;
};

struct libpolly_event_loop
{
	libpolly_context * ctx;
	pthread_cond_t cond;
	pthread_t thread;
};

int libpolly_posix_prepare_add(libpolly_context * ctx)
{
	pthread_mutex_lock(&ctx->entry_list_mutex);

	if (ctx->entry_list_capacity == ctx->entry_list_size + ctx->entry_list_reserve)
	{
		int new_capacity = ctx->entry_list_capacity? ctx->entry_list_capacity * 2: 4;
		struct pollyi_entry * new_entry_list = realloc(ctx->entry_list, new_capacity * sizeof(struct pollyi_entry));
		if (!new_entry_list)
			goto no_mem;

		ctx->entry_list = new_entry_list;
		ctx->entry_list_capacity = new_capacity;
	}

	if (ctx->pollfd_cache_capacity < ctx->entry_list_capacity + 1)
	{
		struct pollfd * new_pollfd_cache = realloc(ctx->pollfd_cache, sizeof((ctx->entry_list_capacity + 1) * sizeof(struct pollfd)));
		if (!new_pollfd_cache)
			goto no_mem;
		ctx->pollfd_cache = new_pollfd_cache;
		ctx->pollfd_cache_capacity = ctx->entry_list_capacity + 1;
	}

	assert(ctx->entry_list_capacity > ctx->entry_list_size + ctx->entry_list_reserve);
	assert(ctx->pollfd_cache_capacity > ctx->entry_list_size + ctx->entry_list_reserve + 1);
	++ctx->entry_list_reserve;

	pthread_mutex_unlock(&ctx->entry_list_mutex);
	return LIBUSBY_SUCCESS;

no_mem:
	pthread_mutex_unlock(&ctx->entry_list_mutex);
	return LIBUSBY_ERROR_NO_MEM;
}

void libpolly_posix_cancel_add(libpolly_context * ctx)
{
	pthread_mutex_lock(&ctx->entry_list_mutex);
	--ctx->entry_list_reserve;
	pthread_mutex_unlock(&ctx->entry_list_mutex);
}

void libpolly_posix_add(libpolly_context * ctx, int fd, short events, libpolly_posix_callback callback, void * user_data)
{
	pthread_mutex_lock(&ctx->entry_list_mutex);

	assert(ctx->entry_list_capacity > ctx->entry_list_size + ctx->entry_list_reserve);
	assert(ctx->entry_list_reserve > 0);

	ctx->entry_list[ctx->entry_list_size].fd = fd;
	ctx->entry_list[ctx->entry_list_size].events = events;
	ctx->entry_list[ctx->entry_list_size].callback = callback;
	ctx->entry_list[ctx->entry_list_size].user_data = user_data;
	++ctx->entry_list_size;
	--ctx->entry_list_reserve;

	{
		char cmd = 'u';
		write(ctx->control_pipe[1], &cmd, 1); // XXX: assumes nothrow
	}

	pthread_mutex_unlock(&ctx->entry_list_mutex);
}

pthread_mutex_t * libpolly_posix_get_loop_mutex(libpolly_context * ctx)
{
	return &ctx->loop_mutex;
}

int libpolly_posix_acquire_loop(libpolly_context * ctx)
{
	// assert(pthread_is_locked(&ctx->loop_mutex));
	if (ctx->loop_token_acquired)
		return LIBUSBY_ERROR_BUSY;

	ctx->loop_token_acquired = 1;
	return LIBUSBY_SUCCESS;
}

void libpolly_posix_release_loop(libpolly_context * ctx)
{
	libpolly_posix_loop_release_registration * reg = ctx->release_registration_list;

	// assert(pthread_is_locked(&ctx->loop_mutex));
	assert(ctx->loop_token_acquired);
	ctx->loop_token_acquired = 0;

	for (; reg; reg = reg->next)
		pthread_cond_broadcast(reg->cond);
}

void libpolly_posix_register_loop_release_notification(libpolly_context * ctx, libpolly_posix_loop_release_registration * registration)
{
	// assert(pthread_is_locked(&ctx->loop_mutex));
	assert(registration->cond);
	registration->ctx = ctx;
	registration->prev = 0;
	registration->next = ctx->release_registration_list;
	ctx->release_registration_list = registration;
}

void libpolly_posix_unregister_loop_release_notification(libpolly_posix_loop_release_registration * registration)
{
	// assert(pthread_is_locked(&ctx->loop_mutex));
	assert(registration->ctx);
	assert(registration->prev || registration->ctx->release_registration_list == registration);

	if (registration->prev)
		registration->prev->next = registration->next;
	else
		registration->ctx->release_registration_list = registration->next;

	if (registration->next)
		registration->next->prev = registration->prev;
}

int libpolly_init(libpolly_context ** ctx)
{
	libpolly_context * res = malloc(sizeof(libpolly_context));
	if (!res)
		return LIBUSBY_ERROR_NO_MEM;
	memset(res, 0, sizeof(libpolly_context));
	res->refcount = 1;

	res->pollfd_cache = malloc(sizeof(struct pollfd));
	if (!res->pollfd_cache)
		goto error_mem;
	res->pollfd_cache_capacity = 1;

	if (pthread_mutex_init(&res->entry_list_mutex, 0) != 0)
		goto error_mem;

	if (pthread_mutex_init(&res->loop_mutex, 0) != 0)
		goto error_entry_list_mutex;

	if (pipe(res->control_pipe) != 0)
		goto error_loop_mutex;

	*ctx = res;
	return LIBUSBY_SUCCESS;
	
error_loop_mutex:
	pthread_mutex_destroy(&res->loop_mutex);
error_entry_list_mutex:
	pthread_mutex_destroy(&res->entry_list_mutex);
error_mem:
	free(res->pollfd_cache);
	free(res);
	return LIBUSBY_ERROR_NO_MEM;
}

void libpolly_ref_context(libpolly_context * ctx)
{
	__sync_fetch_and_add(&ctx->refcount, 1);
}

void libpolly_unref_context(libpolly_context * ctx)
{
	if (__sync_sub_and_fetch(&ctx->refcount, 1) == 0)
	{
		assert(ctx->entry_list_size == 0);
		assert(ctx->entry_list_reserve == 0);
		free(ctx->entry_list);
		free(ctx->pollfd_cache);
		pthread_mutex_destroy(&ctx->loop_mutex);
		pthread_mutex_destroy(&ctx->entry_list_mutex);
		close(ctx->control_pipe[0]);
		close(ctx->control_pipe[1]);
		free(ctx);
	}
}

static int pollyb_loop_dispatch(int fd, short revents, libpolly_posix_callback callback, void * callback_data, void * run_data)
{
	(void)run_data;
	callback(fd, revents, callback_data);
	return LIBUSBY_SUCCESS;
}

static void pollyb_unlock_mutex_trampoline(void * arg)
{
	pthread_mutex_unlock(arg);
}

static void pollyb_unregister_loop_release_notification_trampoline(void * arg)
{
	libpolly_posix_unregister_loop_release_notification(arg);
}

static void pollyb_release_loop_trampoline(void * arg)
{
	libpolly_context * ctx = arg;
	pthread_mutex_lock(&ctx->loop_mutex);
	libpolly_posix_release_loop(ctx);
	pthread_mutex_unlock(&ctx->loop_mutex);
}

struct pollyb_run_context
{
	libpolly_context * ctx;
	struct pollfd * pollfd_cache;
};

static void pollyb_run_cleanup(void * arg)
{
	struct pollyb_run_context * run_ctx = arg;
	libpolly_context * ctx = run_ctx->ctx;
	struct pollfd * pollfd_cache = run_ctx->pollfd_cache;

	pthread_mutex_lock(&ctx->entry_list_mutex);
	if (ctx->pollfd_cache)
		free(pollfd_cache);
	else
		ctx->pollfd_cache = pollfd_cache;
	pthread_mutex_unlock(&ctx->entry_list_mutex);
}

int libpolly_posix_run(libpolly_context * ctx, libpolly_posix_run_callback run_callback, void * run_data)
{
	int r = LIBUSBY_SUCCESS;
	struct pollyb_run_context run_ctx = { ctx, 0 };

	int old_cancelstate;
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancelstate);

	assert(ctx->loop_token_acquired);

	pthread_cleanup_push(&pollyb_run_cleanup, &run_ctx);

	while (r == 0)
	{
		int i;
		int entry_count;
		int rcount;

		pthread_mutex_lock(&ctx->entry_list_mutex);
		if (ctx->pollfd_cache)
		{
			// Grab the new cache (it will have higher capacity than our old cache)
			// and dispose of the old one.
			free(run_ctx.pollfd_cache);
			run_ctx.pollfd_cache = ctx->pollfd_cache;
			ctx->pollfd_cache = 0;
		}

		assert(ctx->pollfd_cache_capacity >= ctx->entry_list_size + 1);
		entry_count = ctx->entry_list_size;
		for (i = 0; i < entry_count; ++i)
		{
			run_ctx.pollfd_cache[i].fd = ctx->entry_list[i].fd;
			run_ctx.pollfd_cache[i].events = ctx->entry_list[i].events;
			run_ctx.pollfd_cache[i].revents = 0;
		}
		pthread_mutex_unlock(&ctx->entry_list_mutex);

		run_ctx.pollfd_cache[entry_count].fd = ctx->control_pipe[0];
		run_ctx.pollfd_cache[entry_count].events = POLLIN;
		run_ctx.pollfd_cache[entry_count].revents = 0;

		pthread_setcancelstate(old_cancelstate, 0);
		rcount = poll(run_ctx.pollfd_cache, entry_count+1, -1); // XXX: this is a possible point of failure, handle it somehow
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancelstate);

		if (rcount < 0)
		{
			r = LIBUSBY_ERROR_NO_MEM;
			break;
		}
		
		if (run_ctx.pollfd_cache[entry_count].revents)
		{
			char cmd;
			// XXX: handle errors and hup in revents

			assert(run_ctx.pollfd_cache[entry_count].revents & POLLIN);
			assert(rcount > 0);
			--rcount;

			read(ctx->control_pipe[0], &cmd, 1); // XXX: assumes no-throw guarantees
			assert(cmd == 'u');
		}

		if (rcount > 0)
		{
			struct pollyi_entry selected_entry;

			for (i = 0; i < entry_count; ++i)
			{
				if (run_ctx.pollfd_cache[i].revents)
					break;
			}

			pthread_mutex_lock(&ctx->entry_list_mutex);
			selected_entry = ctx->entry_list[i];
			ctx->entry_list[i] = ctx->entry_list[ctx->entry_list_size-1];
			--ctx->entry_list_size;
			pthread_mutex_unlock(&ctx->entry_list_mutex);

			assert(selected_entry.fd == run_ctx.pollfd_cache[i].fd);
			r = run_callback(selected_entry.fd, run_ctx.pollfd_cache[i].revents, selected_entry.callback, selected_entry.user_data, run_data);
		}
	}

	pthread_cleanup_pop(1);
	pthread_setcancelstate(old_cancelstate, 0);
	return r;
}

static void * pollyb_event_loop(void * param)
{
	libpolly_event_loop * loop = param;
	libpolly_context * ctx = loop->ctx;

	pthread_mutex_lock(&ctx->loop_mutex);
	pthread_cleanup_push(&pollyb_unlock_mutex_trampoline, &ctx->loop_mutex);
	while (libpolly_posix_acquire_loop(ctx) < 0)
	{
		libpolly_posix_loop_release_registration reg;
		reg.cond = &loop->cond;

		libpolly_posix_register_loop_release_notification(ctx, &reg);
		pthread_cleanup_push(&pollyb_unregister_loop_release_notification_trampoline, &reg);
		pthread_cond_wait(&loop->cond, &ctx->loop_mutex);
		pthread_cleanup_pop(1);
	}
	pthread_cleanup_pop(1);

	pthread_cleanup_push(&pollyb_release_loop_trampoline, ctx);
	libpolly_posix_run(ctx, &pollyb_loop_dispatch, 0);
	pthread_cleanup_pop(1);

	return 0;
}

int libpolly_start_event_loop(libpolly_context * ctx, libpolly_event_loop ** loop)
{
	libpolly_event_loop * res = malloc(sizeof(libpolly_event_loop));
	if (!res)
		return LIBUSBY_ERROR_NO_MEM;

	if (pthread_cond_init(&res->cond, 0) != 0)
	{
		free(res);
		return LIBUSBY_ERROR_NO_MEM;
	}

	res->ctx = ctx;
	libpolly_ref_context(ctx);

	if (pthread_create(&res->thread, 0, &pollyb_event_loop, res) != 0)
	{
		pthread_cond_destroy(&res->cond);
		libpolly_unref_context(ctx);
		free(res);
		return LIBUSBY_ERROR_NO_MEM;
	}

	*loop = res;
	return LIBUSBY_SUCCESS;
}

void libpolly_join_event_loop(libpolly_event_loop * loop)
{
	pthread_cancel(loop->thread);
	pthread_join(loop->thread, 0);
	libpolly_unref_context(loop->ctx);
	pthread_cond_destroy(&loop->cond);
	free(loop);
}
