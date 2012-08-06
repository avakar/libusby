#ifndef LIBSPY_LIBSPY_HPP
#define LIBSPY_LIBSPY_HPP

#include "libspy.h"

namespace libspy {

class device_list
{
public:
	device_list(libspy_device const * devlist)
		: m_devlist(devlist)
	{
	}

	~device_list()
	{
		libspy_free_device_list(m_devlist);
	}

private:
	libspy_device const * m_devlist;

	device_list(device_list const &);
	device_list & operator=(device_list const &);
};

} // namespace libspy

#endif // LIBSPY_LIBSPY_HPP
