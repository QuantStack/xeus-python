/***************************************************************************
* Copyright (c) 2018, Martin Renou, Johan Mabille, Sylvain Corlay and      *
* Wolf Vollprecht                                                          *
*                                                                          *
* Distributed under the terms of the BSD 3-Clause License.                 *
*                                                                          *
* The full license is in the file LICENSE, distributed with this software. *
****************************************************************************/

#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "xeus/xinterpreter.hpp"
#include "xeus/xcomm.hpp"

#include "pybind11/pybind11.h"
#include "pybind11/embed.h"

#include "xutils.hpp"
#include "xcomm.hpp"

namespace py = pybind11;
namespace nl = nlohmann;

namespace xpyt
{

    xcomm::xcomm(py::args /*args*/, py::kwargs kwargs)
        : m_comm(target(kwargs), id(kwargs))
    {
        m_comm.open(
            kwargs.attr("get")("metadata", py::dict()),
            kwargs.attr("get")("data", py::dict()),
            pylist_to_zmq_buffers(kwargs.attr("get")("buffers", py::list()))
        );
    }

    xcomm::xcomm(xeus::xcomm&& comm)
        : m_comm(std::move(comm))
    {
    }

    xcomm::~xcomm()
    {
    }

    std::string xcomm::comm_id() const
    {
        return m_comm.id();
    }

    bool xcomm::kernel() const
    {
        return true;
    }

    void xcomm::close(py::args /*args*/, py::kwargs kwargs)
    {
        m_comm.close(
            kwargs.attr("get")("metadata", py::dict()),
            kwargs.attr("get")("data", py::dict()),
            pylist_to_zmq_buffers(kwargs.attr("get")("buffers", py::list()))
        );
    }

    void xcomm::send(py::args /*args*/, py::kwargs kwargs)
    {
        m_comm.send(
            kwargs.attr("get")("metadata", py::dict()),
            kwargs.attr("get")("data", py::dict()),
            pylist_to_zmq_buffers(kwargs.attr("get")("buffers", py::list()))
        );
    }

    void xcomm::on_msg(python_callback_type callback)
    {
        m_comm.on_message(cpp_callback(callback));
    }

    void xcomm::on_close(python_callback_type callback)
    {
        m_comm.on_close(cpp_callback(callback));
    }

    xeus::xtarget* xcomm::target(py::kwargs kwargs) const
    {
        std::string target_name = kwargs["target_name"].cast<std::string>();
        return xeus::get_interpreter().comm_manager().target(target_name);
    }

    xeus::xguid xcomm::id(py::kwargs kwargs) const
    {
        if (py::hasattr(kwargs, "comm_id"))
        {
            return kwargs["comm_id"].cast<std::string>();
        }
        else
        {
            return xeus::new_xguid();
        }
    }

    auto xcomm::cpp_callback(python_callback_type py_callback) const -> cpp_callback_type
    {
        return [this, py_callback] (const xeus::xmessage& msg) {
            py_callback(cppmessage_to_pymessage(msg));
        };
    }

    void register_target(py::str target_name, py::object callback)
    {
        auto target_callback = [target_name, callback] (xeus::xcomm&& comm, const xeus::xmessage& msg) {
            callback(xcomm(std::move(comm)), cppmessage_to_pymessage(msg));
        };

        xeus::get_interpreter().comm_manager().register_comm_target(
            static_cast<std::string>(target_name), target_callback
        );
    }

    py::object get_kernel()
    {
        return py::none();
    }

    PYBIND11_EMBEDDED_MODULE(xeus_python_kernel, m) {
        py::class_<xcomm>(m, "XPythonComm")
        .def(py::init<py::args, py::kwargs>())
        .def("close", &xcomm::close)
        .def("send", &xcomm::send)
        .def("on_msg", &xcomm::on_msg)
        .def("on_close", &xcomm::on_close)
        .def_property_readonly("comm_id", &xcomm::comm_id)
        .def_property_readonly("kernel", &xcomm::kernel);

        m.def("register_target", &register_target);
        m.def("get_kernel", &get_kernel);
    }
}
