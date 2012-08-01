#ifndef LIBUSBY_LIBPOLLY_HPP
#define LIBUSBY_LIBPOLLY_HPP

#include "libpolly.h"
#include <utility>

namespace libpolly {

class context
{
public:
	context()
		: m_ctx(0)
	{
	}

	explicit context(libpolly_context * ctx)
		: m_ctx(ctx)
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

	friend void swap(context & lhs, context & rhs)
	{
		std::swap(lhs.m_ctx, rhs.m_ctx);
	}

private:
	libpolly_context * m_ctx;
};

}

#endif
