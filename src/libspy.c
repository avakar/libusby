#include "libspy.h"
#include <stdlib.h>
#include <string.h>

int libspy_init(libspy_context ** ctx)
{
	libpolly_context * polly;
	int r = libpolly_init(&polly);
	if (r < 0)
		return r;

	r = libspy_init_with_polly(ctx, polly);
	libpolly_unref_context(polly);
	return r;
}

void libspy_free_device_list(libspy_device const * device_list)
{
	libspy_device const * cur = device_list;
	for (; cur->path; ++cur)
	{
		free((void *)cur->friendly_name);
		free((void *)cur->name);
		free((void *)cur->path);
	}

	free((void *)device_list);
}
