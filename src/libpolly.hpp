#ifndef LIBUSBY_LIBPOLLY_HPP
#define LIBUSBY_LIBPOLLY_HPP

#include "libpolly.h"
#include <utility>
#include <stdexcept>
#include <boost/function.hpp>

namespace libpolly {

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

	context(context const & other)
		: m_ctx(other.m_ctx)
	{
		if (m_ctx)
			libpolly_ref_context(m_ctx);
	}

	context & operator=(context rhs)
	{
		swap(*this, rhs);
		return *this;
	}

	void clear()
	{
		if (m_ctx)
		{
			libpolly_unref_context(m_ctx);
			m_ctx = 0;
		}
	}

	libpolly_context * get() const
	{
		return m_ctx;
	}

	friend void swap(context & lhs, context & rhs)
	{
		std::swap(lhs.m_ctx, rhs.m_ctx);
	}

	void post(boost::function<void()> const & handler)
	{
		boost::function<void()> * phandler = new boost::function<void()>(handler);
		int r = libpolly_submit_task_direct(m_ctx, &context::post_handler, phandler);
		if (r < 0)
		{
			delete phandler;
			throw std::runtime_error("can't submit a task");
		}
	}

	static context create()
	{
		libpolly_context * ctx;
		libpolly_init(&ctx); // XXX
		return context(ctx);
	}

	static context create_with_worker()
	{
		libpolly_context * ctx;
		libpolly_init_with_worker(&ctx); // XXX
		return context(ctx);
	}

	static context take(libpolly_context * ctx)
	{
		return context(ctx);
	}

	static context ref(libpolly_context * ctx)
	{
		libpolly_ref_context(ctx);
		return context::take(ctx);
	}

private:
	libpolly_context * m_ctx;

	static void post_handler(void * user_data)
	{
		boost::function<void()> * phandler = static_cast<boost::function<void()> *>(user_data);
		if (*phandler)
			(*phandler)();
		delete phandler;
	}

	explicit context(libpolly_context * ctx)
		: m_ctx(ctx)
	{
	}
};

class event
{
public:
	event(context & ctx)
		: m_event(0)
	{
		libpolly_create_event(ctx.get(), &m_event);
		// XXX: error checking
	}

	event(libpolly_context * ctx)
		: m_event(0)
	{
		libpolly_create_event(ctx, &m_event);
		// XXX: error checking
	}

	~event()
	{
		libpolly_destroy_event(m_event);
	}

	void set()
	{
		libpolly_set_event(m_event);
	}

	void reset()
	{
		libpolly_reset_event(m_event);
	}

	void wait()
	{
		libpolly_wait_for_event(m_event);
	}

private:
	libpolly_event * m_event;

	event(event const &);
	event & operator=(event const &);
};

}

#endif
