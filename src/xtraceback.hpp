/***************************************************************************
* Copyright (c) 2018, Martin Renou, Johan Mabille, Sylvain Corlay, and     *
* Wolf Vollprecht                                                          *
* Copyright (c) 2018, QuantStack                                           *
*                                                                          *
* Distributed under the terms of the BSD 3-Clause License.                 *
*                                                                          *
* The full license is in the file LICENSE, distributed with this software. *
****************************************************************************/

#ifndef XPYT_TRACEBACK_HPP
#define XPYT_TRACEBACK_HPP

#include <vector>
#include <string>

#include "xeus-python/xpybind11_include.hpp"

namespace py = pybind11;

namespace xpyt
{
    struct xerror
    {
        std::string m_ename;
        std::string m_evalue;
        std::vector<std::string> m_traceback;
    };

    void register_filename_mapping(const std::string& filename, int execution_count);
    xerror extract_error(py::error_already_set& error);
}

#endif
