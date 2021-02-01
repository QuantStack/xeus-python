/***************************************************************************
* Copyright (c) 2018, Martin Renou, Johan Mabille, Sylvain Corlay, and     *
* Wolf Vollprecht                                                          *
* Copyright (c) 2018, QuantStack                                           *
*                                                                          *
* Distributed under the terms of the BSD 3-Clause License.                 *
*                                                                          *
* The full license is in the file LICENSE, distributed with this software. *
****************************************************************************/

#include <algorithm>
#include <iostream>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

#include "xeus/xinterpreter.hpp"
#include "xeus/xsystem.hpp"

#include "pybind11/functional.h"

#include "xeus-python/xinterpreter.hpp"
#include "xeus-python/xeus_python_config.hpp"
#include "xeus-python/xtraceback.hpp"
#include "xeus-python/xutils.hpp"

#include "xpython_kernel.hpp"
#include "xdisplay.hpp"
#include "xinput.hpp"
#include "xinspect.hpp"
#include "xinteractiveshell.hpp"
#include "xinternal_utils.hpp"
#include "xis_complete.hpp"
#include "xlinecache.hpp"
#include "xstream.hpp"

namespace py = pybind11;
namespace nl = nlohmann;
using namespace pybind11::literals;

namespace xpyt
{

    interpreter::interpreter(bool redirect_output_enabled/*=true*/, bool redirect_display_enabled/*=true*/)
    {
        xeus::register_interpreter(this);
        if (redirect_output_enabled)
        {
            redirect_output();
        }
        redirect_display(redirect_display_enabled);

        // Monkey patch the IPython modules later in the execution, in the configure_impl
        // This is needed because the kernel needs to initialize the history_manager before
        // we can expose it to Python.
    }

    interpreter::~interpreter()
    {
    }

    void interpreter::configure_impl()
    {
        if (m_release_gil_at_startup)
        {
            // The GIL is not held by default by the interpreter, so every time we need to execute Python code we
            // will need to acquire the GIL
            //
            m_release_gil = gil_scoped_release_ptr(new py::gil_scoped_release());
        }

        py::gil_scoped_acquire acquire;

        py::module sys = py::module::import("sys");

        // Monkey patching "import linecache". This monkey patch does not work with Python2.
        sys.attr("modules")["linecache"] = get_linecache_module();

        py::module jedi = py::module::import("jedi");
        jedi.attr("api").attr("environment").attr("get_default_environment") = py::cpp_function([jedi] () {
            jedi.attr("api").attr("environment").attr("SameEnvironment")();
        });

        // Monkey patching "from ipykernel.comm import Comm"
        sys.attr("modules")["ipykernel.comm"] = get_kernel_module();

        // Monkey patching "import IPython.core.display"
        sys.attr("modules")["IPython.core.display"] = get_display_module();

        // Monkey patching "from IPython import get_ipython"
        sys.attr("modules")["IPython.core.getipython"] = get_kernel_module();

        // Add get_ipython to global namespace
        py::globals()["get_ipython"] = get_kernel_module().attr("get_ipython");

        // Initializes get_ipython result
        get_kernel_module().attr("get_ipython")();

        m_has_ipython = get_kernel_module().attr("has_ipython").cast<bool>();

        // Initialize cached inputs
        py::globals()["_i"] = "";
        py::globals()["_ii"] = "";
        py::globals()["_iii"] = "";

        load_extensions();
    }

    nl::json interpreter::execute_request_impl(int execution_count,
                                               const std::string& code,
                                               bool silent,
                                               bool /*store_history*/,
                                               nl::json /*user_expressions*/,
                                               bool allow_stdin)
    {
        py::gil_scoped_acquire acquire;
        nl::json kernel_res;

        py::str code_copy;
        if(m_has_ipython)
        {
            py::module input_transformers = py::module::import("IPython.core.inputtransformer2");
            py::object transformer_manager = input_transformers.attr("TransformerManager")();
            code_copy = transformer_manager.attr("transform_cell")(code);
        }
        else
        {
            // Special handling of question mark whe IPython is not installed. Otherwise, this
            // is already implemented in the IPython.core.inputtransformer2 module that we
            // import. This is a temporary implementation until:
            // - either we reimplement the parsing logic in xeus-python
            // - or this logic is extracted from IPython into a dedicated package, that becomes
            // a dependency of both xeus-python and IPython.
            if (code.size() >= 2 && code[0] == '?')
            {
                std::string result = formatted_docstring(code);
                if (result.empty())
                {
                    result = "Object " + code.substr(1) + " not found.";
                }

                kernel_res["status"] = "ok";
                kernel_res["payload"] = nl::json::array();
                kernel_res["payload"][0] = nl::json::object({
                    {"data", {
                        {"text/plain", result}
                    }},
                    {"source", "page"},
                    {"start", 0}
                });
                kernel_res["user_expressions"] = nl::json::object();

                return kernel_res;
            }
            code_copy = code;
        }

        // Scope guard performing the temporary monkey patching of input and
        // getpass with a function sending input_request messages.
        auto input_guard = input_redirection(allow_stdin);

        try
        {
            // Import modules
            py::module ast = py::module::import("ast");
            py::module builtins = py::module::import("builtins");

            // Parse code to AST
            py::object code_ast = ast.attr("parse")(code_copy, "<string>", "exec");
            py::list expressions = code_ast.attr("body");

            std::string filename = get_cell_tmp_file(code);
            register_filename_mapping(filename, execution_count);

            // Caching the input code
            py::module linecache = py::module::import("linecache");
            linecache.attr("xupdatecache")(code, filename);

            // If the last statement is an expression, we compile it separately
            // in an interactive mode (This will trigger the display hook)
            py::object last_stmt = expressions[py::len(expressions) - 1];
            if (py::isinstance(last_stmt, ast.attr("Expr")))
            {
                code_ast.attr("body").attr("pop")();

                py::list interactive_nodes;
                interactive_nodes.append(last_stmt);

                py::object interactive_ast = ast.attr("Interactive")(interactive_nodes);

                py::object compiled_code = builtins.attr("compile")(code_ast, filename, "exec");

                py::object compiled_interactive_code = builtins.attr("compile")(interactive_ast, filename, "single");

                if (m_displayhook.ptr() != nullptr)
                {
                    m_displayhook.attr("set_execution_count")(execution_count);
                }

                exec(compiled_code);
                exec(compiled_interactive_code);
            }
            else
            {
                py::object compiled_code = builtins.attr("compile")(code_ast, filename, "exec");
                exec(compiled_code);
            }

            kernel_res["status"] = "ok";
            kernel_res["user_expressions"] = nl::json::object();
            if (m_has_ipython)
            {
                py::object pyshell = get_kernel_module().attr("get_ipython")();

                pyshell.attr("events").attr("trigger")("post_execute");

                xinteractive_shell* xshell = pyshell.cast<xinteractive_shell*>();
                auto payload = xshell->get_payloads();
                kernel_res["payload"] = payload;
                xshell->clear_payloads();
            }
            else
            {
                kernel_res["payload"] = nl::json::array();
            }
        }
        catch (py::error_already_set& e)
        {
            xerror error = extract_error(e);

            if (!silent)
            {
                publish_execution_error(error.m_ename, error.m_evalue, error.m_traceback);
            }

            kernel_res["status"] = "error";
            kernel_res["ename"] = error.m_ename;
            kernel_res["evalue"] = error.m_evalue;
            kernel_res["traceback"] = error.m_traceback;
        }

        // Cache inputs
        py::globals()["_iii"] = py::globals()["_ii"];
        py::globals()["_ii"] = py::globals()["_i"];
        py::globals()["_i"] = code;

        return kernel_res;
    }

    nl::json interpreter::complete_request_impl(
        const std::string& code,
        int cursor_pos)
    {
        py::gil_scoped_acquire acquire;
        nl::json kernel_res;
        std::vector<std::string> matches;
        int cursor_start = cursor_pos;

        py::list completions = get_completions(code, cursor_pos);

        if (py::len(completions) != 0)
        {
            cursor_start -= py::len(completions[0].attr("name_with_symbols")) - py::len(completions[0].attr("complete"));
            for (py::handle completion : completions)
            {
                matches.push_back(completion.attr("name_with_symbols").cast<std::string>());
            }
        }

        kernel_res["cursor_start"] = cursor_start;
        kernel_res["cursor_end"] = cursor_pos;
        kernel_res["matches"] = matches;
        kernel_res["status"] = "ok";
        return kernel_res;
    }

    nl::json interpreter::inspect_request_impl(const std::string& code,
                                               int cursor_pos,
                                               int /*detail_level*/)
    {
        py::gil_scoped_acquire acquire;
        nl::json kernel_res;
        nl::json pub_data;

        std::string docstring = formatted_docstring(code, cursor_pos);

        bool found = false;
        if (!docstring.empty())
        {
            found = true;
            pub_data["text/plain"] = docstring;
        }

        kernel_res["data"] = pub_data;
        kernel_res["metadata"] = nl::json::object();
        kernel_res["found"] = found;
        kernel_res["status"] = "ok";
        return kernel_res;
    }

    nl::json interpreter::is_complete_request_impl(const std::string& code)
    {
        py::gil_scoped_acquire acquire;
        nl::json kernel_res;

        py::module completion_module = get_completion_module();
        py::list result = completion_module.attr("check_complete")(code);

        auto status = result[0].cast<std::string>();

        kernel_res["status"] = status;
        if (status.compare("incomplete") == 0)
        {
            kernel_res["indent"] = std::string(result[1].cast<std::size_t>(), ' ');
        }
        return kernel_res;
    }

    nl::json interpreter::kernel_info_request_impl()
    {
        nl::json result;
        result["implementation"] = "xeus-python";
        result["implementation_version"] = XPYT_VERSION;

        /* The jupyter-console banner for xeus-python is the following:
          __  _____ _   _ ___
          \ \/ / _ \ | | / __|
           >  <  __/ |_| \__ \
          /_/\_\___|\__,_|___/

          xeus-python: a Jupyter lernel for Python
        */

        std::string banner = ""
              "  __  _____ _   _ ___\n"
              "  \\ \\/ / _ \\ | | / __|\n"
              "   >  <  __/ |_| \\__ \\\n"
              "  /_/\\_\\___|\\__,_|___/\n"
              "\n"
              "  xeus-python: a Jupyter kernel for Python\n"
              "  Python ";
        banner.append(PY_VERSION);
#ifdef XEUS_PYTHON_PYPI_WARNING
        banner.append("\n"
              "\n"
              "WARNING: this instance of xeus-python has been installed from a PyPI wheel.\n"
              "We recommend using a general-purpose package manager instead, such as Conda/Mamba."
              "\n");
#endif
        result["banner"] = banner;
        result["debugger"] = true;

        result["language_info"]["name"] = "python";
        result["language_info"]["version"] = PY_VERSION;
        result["language_info"]["mimetype"] = "text/x-python";
        result["language_info"]["file_extension"] = ".py";

        result["help_links"] = nl::json::array();
        result["help_links"][0] = nl::json::object({
            {"text", "Xeus-Python Reference"},
            {"url", "https://xeus-python.readthedocs.io"}
        });

        result["status"] = "ok";
        return result;
    }

    void interpreter::shutdown_request_impl()
    {
    }

    nl::json interpreter::internal_request_impl(const nl::json& content)
    {
        py::gil_scoped_acquire acquire;
        std::string code = content.value("code", "");
        nl::json reply;
        try
        {
            // Import modules
            py::module ast = py::module::import("ast");
            py::module builtins = py::module::import("builtins");

            // Parse code to AST
            py::object code_ast = ast.attr("parse")(code, "<string>", "exec");

            std::string filename = "debug_this_thread";
            py::object compiled_code = builtins.attr("compile")(code_ast, filename, "exec");
            exec(compiled_code);

            reply["status"] = "ok";
        }
        catch (py::error_already_set& e)
        {
            xerror error = extract_error(e);

            publish_execution_error(error.m_ename, error.m_evalue, error.m_traceback);
            error.m_traceback.resize(1);
            error.m_traceback[0] = code;

            reply["status"] = "error";
            reply["ename"] = error.m_ename;
            reply["evalue"] = error.m_evalue;
            reply["traceback"] = error.m_traceback;
        }
        return reply;
    }

    void interpreter::redirect_output()
    {
        py::module sys = py::module::import("sys");
        py::module stream_module = get_stream_module();

        sys.attr("stdout") = stream_module.attr("Stream")("stdout");
        sys.attr("stderr") = stream_module.attr("Stream")("stderr");
    }

    void interpreter::redirect_display(bool install_hook/*=true*/)
    {
        py::module display_module = get_display_module();
        m_displayhook = display_module.attr("DisplayHook")();
        if (install_hook)
        {
            py::module sys = py::module::import("sys");
            sys.attr("displayhook") = m_displayhook;
        }

        // Expose display functions to Python
        py::globals()["display"] = display_module.attr("display");
        py::globals()["update_display"] = display_module.attr("update_display");
    }

    void interpreter::load_extensions()
    {
        if (m_has_ipython)
        {
            py::module os = py::module::import("os");
            py::module path = py::module::import("os.path");
            py::module sys = py::module::import("sys");
            py::module fnmatch = py::module::import("fnmatch");

            py::str extensions_path = path.attr("join")(sys.attr("exec_prefix"), "etc", "xeus-python", "extensions");

            if (!is_pyobject_true(path.attr("exists")(extensions_path)))
            {
                return;
            }

            py::list list_files = os.attr("listdir")(extensions_path);

            xinteractive_shell* xshell = get_kernel_module()
                .attr("get_ipython")()
                .cast<xinteractive_shell*>();
            py::object extension_manager = xshell->get_extension_manager();

            for (const py::handle& file : list_files)
            {
                if (!is_pyobject_true(fnmatch.attr("fnmatch")(file, "*.json")))
                {
                    continue;
                }

                try
                {
                    std::ifstream config_file(py::str(path.attr("join")(extensions_path, file)).cast<std::string>());
                    nl::json config;
                    config_file >> config;

                    if (config["enabled"].get<bool>())
                    {
                        extension_manager.attr("load_extension")(config["module"].get<std::string>());
                    }
                }
                catch (py::error_already_set& e)
                {
                    xerror error = extract_error(e);

                    std::cerr << "Warning: Failed loading extension with: " << error.m_ename << ": " << error.m_evalue << std::endl;
                }
            }
        }
    }
}
