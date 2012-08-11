#include "../libspy.h"
#include <assert.h>
#include <stdlib.h>
#include <libudev.h>

static char * spyi_strdup(char const * str)
{
	int len = strlen(str);
	char * res = malloc(len + 1);
	strcpy(res, str);
	return res;
}

int libspy_get_device_list(libspy_context * ctx, libspy_device const ** device_list)
{
	int r = LIBUSBY_ERROR_NO_MEM;
	struct udev * udev_ctx = 0;
	struct udev_enumerate * en = 0;
	struct udev_list_entry *devices, *dev_list_entry;
	struct udev_device *dev = 0;
	libspy_device spy_dev = {0};
	
	libspy_device * res = 0;
	int res_capacity = 0;
	int res_size = 0;
	
	(void *)ctx;

	udev_ctx = udev_new();
	if (!udev_ctx)
		goto error;
	
	en = udev_enumerate_new(udev_ctx);
	if (!en)
		goto error;

	udev_enumerate_add_match_subsystem(en, "tty");
	udev_enumerate_scan_devices(en);
	devices = udev_enumerate_get_list_entry(en);

	udev_list_entry_foreach(dev_list_entry, devices)
	{
		char const * path = udev_list_entry_get_name(dev_list_entry);
		dev = udev_device_new_from_syspath(udev_ctx, path);
		if (!dev)
			goto error;

		spy_dev.name = spyi_strdup(udev_device_get_sysname(dev));
		if (!spy_dev.name)
			goto error;
		spy_dev.path = spyi_strdup(udev_device_get_devnode(dev));
		if (!spy_dev.path)
			goto error;

		if (res_size + 1 >= res_capacity)
		{
			int new_capacity = res_capacity? 2 * res_capacity: 4;
			libspy_device * new_res = realloc(res, new_capacity*sizeof(libspy_device));
			if (!new_res)
				goto error;
			res = new_res;
			res_capacity = new_capacity;
		}

		res[res_size++] = spy_dev;
		spy_dev.name = 0;
		spy_dev.path = 0;

		udev_device_unref(dev);
		dev = 0;
	}

	r = res_size;
	if (res)
	{
		assert(res_capacity > res_size);
		res[res_size].name = 0;
		res[res_size].path = 0;
		res[res_size].friendly_name = 0;
	}
	*device_list = res;
	res = 0;

error:
	if (res)
		libspy_free_device_list(res);
	if (dev)
		udev_device_unref(dev);
	free(spy_dev.path);
	free(spy_dev.name);
	if (en)
		udev_enumerate_unref(en);
	if (udev_ctx)
		udev_unref(udev_ctx);
	return r;
}
