#ifndef LIBUSBY_LIBUSBY_HPP
#define LIBUSBY_LIBUSBY_HPP

#include "libusby.h"
#include <stdexcept>
#include <vector>
#include <utility>
#include <cassert>

namespace libusby {

class error
	: std::runtime_error
{
public:
	explicit error(int error_code)
		: std::runtime_error("libusby error"), m_error_code(error_code)
	{
	}

	int error_code() const
	{
		return m_error_code;
	}

	static int check(int error_code)
	{
		if (error_code < 0)
			throw error(error_code);
		return error_code;
	}

private:
	int m_error_code;
};

class device
{
public:
	device()
		: m_dev(0)
	{
	}

	~device()
	{
		this->clear();
	}

	device(device const & other)
		: m_dev(other.m_dev)
	{
		if (m_dev)
			libusby_ref_device(m_dev);
	}

	device & operator=(device other)
	{
		this->swap(other);
		return *this;
	}

	void swap(device & other)
	{
		std::swap(m_dev, other.m_dev);
	}

	void clear()
	{
		if (m_dev)
		{
			libusby_unref_device(m_dev);
			m_dev = 0;
		}
	}

	libusby_device * get() const
	{
		return m_dev;
	}

	operator void const *() const
	{
		return m_dev? this: 0;
	}

	bool operator!() const
	{
		return !m_dev;
	}

	static device ref(libusby_device * dev)
	{
		assert(dev);
		libusby_ref_device(dev);
		return device(dev);
	}

	static device take_ownership(libusby_device * dev)
	{
		assert(dev);
		return device(dev);
	}

	friend bool operator<(device const & lhs, device const & rhs)
	{
		return std::less<libusby_device *>()(lhs.m_dev, rhs.m_dev);
	}

	friend bool operator==(device const & lhs, device const & rhs)
	{
		return lhs.m_dev == rhs.m_dev;
	}

	friend bool operator!=(device const & lhs, device const & rhs)
	{
		return lhs.m_dev != rhs.m_dev;
	}

private:
	explicit device(libusby_device * dev)
		: m_dev(dev)
	{
	}

	libusby_device * m_dev;
};

inline std::string get_string_desc_utf8(libusby_device_handle * h, uint8_t desc_index, uint16_t langid = 0)
{
	char buf[256];
	int r = libusby_get_string_descriptor_utf8(h, desc_index, langid, buf, sizeof buf);
	error::check(r);

	return std::string(buf, buf + r);
}

class device_handle
{
public:
	device_handle()
		: m_handle(0)
	{
	}

	explicit device_handle(device const & dev)
		: m_handle(0)
	{
		this->open(dev);
	}

	explicit device_handle(libusby_device * dev)
		: m_handle(0)
	{
		this->open(dev);
	}

	device_handle(device_handle && h)
		: m_handle(h.m_handle)
	{
		h.m_handle = 0;
	}

	~device_handle()
	{
		this->clear();
	}

	device_handle & operator=(device_handle && h)
	{
		this->clear();
		m_handle = h.m_handle;
		h.m_handle = 0;
		return *this;
	}

	void open(device const & dev)
	{
		this->open(dev.get());
	}

	void open(libusby_device * dev)
	{
		error::check(this->try_open(dev));
	}

	int try_open(device const & dev)
	{
		return this->try_open(dev.get());
	}

	int try_open(libusby_device * dev)
	{
		libusby_device_handle * handle;
		int r = libusby_open(dev, &handle);
		if (r < 0)
			return r;

		this->clear();
		m_handle = handle;
		return 0;
	}

	void clear()
	{
		if (m_handle)
		{
			libusby_close(m_handle);
			m_handle = 0;
		}
	}

	libusby_device_handle * get() const
	{
		return m_handle;
	}

	libusby_device_handle * release()
	{
		libusby_device_handle * res = m_handle;
		m_handle = 0;
		return res;
	}

	std::string get_string_desc_utf8(uint8_t desc_index, uint16_t langid = 0)
	{
		return ::libusby::get_string_desc_utf8(m_handle, desc_index, langid);
	}

private:
	libusby_device_handle * m_handle;

	device_handle(device_handle const &);
	device_handle & operator=(device_handle const &);
};

typedef std::vector<device> device_list;

class context
{
public:
	context()
		: m_ctx(0)
	{
	}

	~context()
	{
		this->clear();
	}

	void clear()
	{
		if (m_ctx)
			libusby_exit(m_ctx);
	}

	void create()
	{
		libusby_context * new_ctx = 0;
		error::check(libusby_init(&new_ctx));
		this->clear();
		m_ctx = new_ctx;
	}

	void create_with_polly(libpolly_context * polly)
	{
		libusby_context * new_ctx = 0;
		error::check(libusby_init_with_polly(&new_ctx, polly));
		this->clear();
		m_ctx = new_ctx;
	}

	libusby_context * get() const
	{
		return m_ctx;
	}

	device_list get_device_list()
	{
		device_list res;

		libusby_device ** list = 0;
		int r = libusby_get_device_list(m_ctx, &list);
		error::check(r);
		devlist_guard dg(list);

		res.reserve(r);
		for (int i = 0; i < r; ++i)
			res.push_back(device::ref(list[i]));

		return res;
	}

private:
	libusby_context * m_ctx;

	struct devlist_guard
	{
		explicit devlist_guard(libusby_device ** list) : m_list(list) {}
		~devlist_guard() { libusby_free_device_list(m_list, /*unref_devices=*/1); }
		libusby_device ** m_list;
	};

	context(context const &);
	context & operator=(context const &);
};

} // namespace libusby

#endif // LIBUSBY_LIBUSBY_HPP
