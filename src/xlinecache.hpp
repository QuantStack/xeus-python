/***************************************************************************
* Copyright (c) 2018, Martin Renou, Johan Mabille, Sylvain Corlay, and     *
* Wolf Vollprecht                                                          *
* Copyright (c) 2018, QuantStack                                           *
*                                                                          *
* Distributed under the terms of the BSD 3-Clause License.                 *
*                                                                          *
* The full license is in the file LICENSE, distributed with this software. *
****************************************************************************/

#ifndef XPYT_LINECACHE_HPP
#define XPYT_LINECACHE_HPP

#include "xeus-python/xpybind11_include.hpp"

namespace py = pybind11;

namespace xpyt
{
    py::module get_linecache_module();
}

#endif
