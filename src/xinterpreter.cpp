/***************************************************************************
* Copyright (c) 2018, Martin Renou, Johan Mabille, Sylvain Corlay and      *
* Wolf Vollprecht                                                          *
*                                                                          *
* Distributed under the terms of the BSD 3-Clause License.                 *
*                                                                          *
* The full license is in the file LICENSE, distributed with this software. *
****************************************************************************/

#include <string>
#include <utility>
#include <vector>
#include <algorithm>

#include "nlohmann/json.hpp"

#include "xeus/xinterpreter.hpp"

#include "pybind11/functional.h"

#include "xpyt_config.hpp"
#include "xinterpreter.hpp"
#include "xstream.hpp"
#include "xdisplay.hpp"

namespace py = pybind11;
namespace nl = nlohmann;
using namespace pybind11::literals;

namespace xpyt
{
    void interpreter::configure_impl()
    {
        py::module jedi = py::module::import("jedi");
        jedi.attr("api").attr("environment").attr("get_default_environment") = py::cpp_function([jedi] () {
            jedi.attr("api").attr("environment").attr("SameEnvironment")();
        });
    }

    interpreter::interpreter(int /*argc*/, const char* const* /*argv*/)
    {
        xeus::register_interpreter(this);
        redirect_output();
        redirect_display();

        // Monkey patching ipywidgets
        py::module sys = py::module::import("sys");
        py::module types = py::module::import("types");
        py::module xeus_python_kernel = py::module::import("xeus_python_kernel");
        py::module xeus_python_display = py::module::import("xeus_python_display");

        py::object xpython_comm = xeus_python_kernel.attr("XPythonComm");

        py::module kernel = types.attr("ModuleType")("kernel");
        py::setattr(kernel, "Comm", xpython_comm);
        py::setattr(kernel, "display", m_displayhook);
        py::setattr(kernel, "clear_output", xeus_python_display.attr("clear_output"));
        py::setattr(kernel, "register_target", xeus_python_kernel.attr("register_target"));
        py::setattr(kernel, "get_ipython", xeus_python_kernel.attr("get_kernel"));

        py::module display = types.attr("ModuleType")("display");
        py::setattr(display, "display", m_displayhook);
        py::setattr(display, "clear_output", xeus_python_display.attr("clear_output"));

        sys.attr("modules")["ipywidgets.widgets.kernel"] = kernel;
        sys.attr("modules")["IPython.display"] = display; // For support of a variety of notebooks
    }

    interpreter::~interpreter() {}

    nl::json interpreter::execute_request_impl(
        int execution_counter,
        const std::string& code,
        bool silent,
        bool /*store_history*/,
        const nl::json::object_t* /*user_expressions*/,
        bool /*allow_stdin*/)
    {
        nl::json kernel_res;

        // TODO: Check for magics
        if (code.compare("?") == 0)
        {
            std::string html_content = R"(<style>
            #pager-container {
                padding: 0;
                margin: 0;
                width: 100%;
                height: 100%;
            }
            .xpyt-iframe-pager {
                padding: 0;
                margin: 0;
                width: 100%;
                height: 100%;
                border: none;
            }
            </style>
            <iframe class="xpyt-iframe-pager" src="https://docs.python.org/"></iframe>)";

            kernel_res["status"] = "ok";
            kernel_res["payload"] = nl::json::array();
            kernel_res["payload"][0] = nl::json::object({
                {"data", {
                    {"text/plain", "https://docs.python.org/"},
                    {"text/html", html_content}}
                },
                {"source", "page"},
                {"start", 0}
            });
            kernel_res["user_expressions"] = nl::json::object();

            return kernel_res;
        }

        try
        {
            // Import AST ans builtins modules
            py::module ast = py::module::import("ast");
            py::module builtins = py::module::import("builtins");

            // Parse code to AST
            py::object code_ast = ast.attr("parse")(code, "<string>", "exec");
            py::list expressions = code_ast.attr("body");

            // If the last statement is an expression, we compile it seperately
            // in an interactive mode (This will trigger the display hook)
            py::object last_stmt = expressions[py::len(expressions) - 1];
            if (py::isinstance(last_stmt, ast.attr("Expr")))
            {
                code_ast.attr("body").attr("pop")();

                py::list interactive_nodes;
                interactive_nodes.append(last_stmt);

                py::object interactive_ast = ast.attr("Interactive")(interactive_nodes);

                py::object compiled_code = builtins.attr("compile")(code_ast, "<ast>", "exec");
                py::object compiled_interactive_code = builtins.attr("compile")(interactive_ast, "<ast>", "single");

                m_displayhook.attr("set_execution_count")(execution_counter);

                builtins.attr("exec")(compiled_code, py::globals());
                builtins.attr("exec")(compiled_interactive_code, py::globals());
            }
            else
            {
                py::object compiled_code = builtins.attr("compile")(code_ast, "<ast>", "exec");
                builtins.attr("exec")(compiled_code, py::globals());
            }

            kernel_res["status"] = "ok";
            kernel_res["payload"] = nl::json::array();
            kernel_res["user_expressions"] = nl::json::object();

        } catch(const std::exception& e) {

            std::string ename = "Execution error";
            std::string evalue = e.what();
            std::vector<std::string> traceback({ename + ": " + evalue});

            if (!silent)
            {
                publish_execution_error(ename, evalue, traceback);
            }

            kernel_res["status"] = "error";
            kernel_res["ename"] = ename;
            kernel_res["evalue"] = evalue;
            kernel_res["traceback"] = traceback;
        }

        return kernel_res;
    }

    nl::json interpreter::complete_request_impl(
        const std::string& code,
        int cursor_pos)
    {
        nl::json kernel_res;

        py::list completions = jedi_interpret(code, cursor_pos).attr("completions")();

        int cursor_start = cursor_pos - (py::len(completions[0].attr("name_with_symbols")) - py::len(completions[0].attr("complete")));

        std::vector<std::string> matches;
        for (py::handle completion: completions)
        {
            matches.push_back(completion.attr("name_with_symbols").cast<std::string>());
        }

        kernel_res["cursor_start"] = cursor_start;
        kernel_res["cursor_end"] = cursor_pos;
        kernel_res["matches"] = matches;
        kernel_res["status"] = "ok";
        return kernel_res;
    }

    nl::json interpreter::inspect_request_impl(
        const std::string& code,
        int cursor_pos,
        int /*detail_level*/)
    {
        nl::json kernel_res;
        nl::json pub_data;

        py::list definitions = jedi_interpret(code, cursor_pos).attr("goto_definitions")();

        bool found = false;
        if (py::len(definitions) != 0)
        {
            found = true;
            pub_data["text/plain"] = definitions[0].attr("docstring")().cast<std::string>();
        }

        kernel_res["data"] = pub_data;
        kernel_res["metadata"] = nl::json::object();;
        kernel_res["found"] = found;
        kernel_res["status"] = "ok";
        return kernel_res;
    }

    nl::json interpreter::is_complete_request_impl(const std::string& /*code*/)
    {
        return nl::json::object();
    }

    nl::json interpreter::kernel_info_request_impl()
    {
        nl::json result;
        result["implementation"] = "xeus-python";
        result["implementation_version"] = XPYT_VERSION;

        /* The jupyter-console banner for xeus-python is the following:
            __   ________ _    _  _____       _______     _________ _    _  ____  _   _
            \ \ / /  ____| |  | |/ ____|     |  __ \ \   / /__   __| |  | |/ __ \| \ | |
             \ V /| |__  | |  | | (___ ______| |__) \ \_/ /   | |  | |__| | |  | |  \| |
              > < |  __| | |  | |\___ \______|  ___/ \   /    | |  |  __  | |  | | . ` |
             / . \| |____| |__| |____) |     | |      | |     | |  | |  | | |__| | |\  |
            /_/ \_\______|\____/|_____/      |_|      |_|     |_|  |_|  |_|\____/|_| \_|

          C++ Jupyter Kernel for Python
        */

        result["banner"] = " __   ________ _    _  _____       _______     _________ _    _  ____  _   _ \n"
                           " \\ \\ / /  ____| |  | |/ ____|     |  __ \\ \\   / /__   __| |  | |/ __ \\| \\ | |\n"
                           "  \\ V /| |__  | |  | | (___ ______| |__) \\ \\_/ /   | |  | |__| | |  | |  \\| |\n"
                           "   > < |  __| | |  | |\\___ \\______|  ___/ \\   /    | |  |  __  | |  | | . ` |\n"
                           "  / . \\| |____| |__| |____) |     | |      | |     | |  | |  | | |__| | |\\  |\n"
                           " /_/ \\_\\______|\\____/|_____/      |_|      |_|     |_|  |_|  |_|\\____/|_| \\_|\n"
                           "\n"
                           "  C++ Jupyter Kernel for Python  ";

        result["language_info"]["name"] = "python";
        result["language_info"]["version"] = PY_VERSION;
        result["language_info"]["mimetype"] = "text/x-python";
        result["language_info"]["file_extension"] = ".py";
        return result;
    }

    void interpreter::shutdown_request_impl()
    {
    }

    void interpreter::input_reply_impl(const std::string& /*value*/)
    {
    }

    void interpreter::redirect_output()
    {
        py::module sys = py::module::import("sys");
        py::module xeus_python_stream = py::module::import("xeus_python_stream");

        sys.attr("stdout") = xeus_python_stream.attr("XPythonStream")("stdout");
        sys.attr("stderr") = xeus_python_stream.attr("XPythonStream")("stderr");
    }

    void interpreter::redirect_display()
    {
        py::module sys = py::module::import("sys");
        py::module xeus_python_display = py::module::import("xeus_python_display");

        m_displayhook = xeus_python_display.attr("XPythonDisplay")();

        sys.attr("displayhook") = m_displayhook;
        py::globals()["display"] = m_displayhook;
    }

    py::object interpreter::jedi_interpret(const std::string& code, int cursor_pos)
    {
        py::module jedi = py::module::import("jedi");

        py::str py_code = code.substr(0, cursor_pos + 1);

        py::int_ line = 1;
        py::int_ column = 0;
        if (py::len(py_code) != 0)
        {
            py::list lines = py_code.attr("splitlines")();
            line = py::len(lines);
            column = py::len(lines[py::len(lines) - 1]);
        }

        return jedi.attr("Interpreter")(py_code, py::make_tuple(py::globals()), "line"_a=line, "column"_a=column);
    }
}
