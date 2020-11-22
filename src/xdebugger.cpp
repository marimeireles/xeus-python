/***************************************************************************
* Copyright (c) 2018, Martin Renou, Johan Mabille, Sylvain Corlay, and     *
* Wolf Vollprecht                                                          *
* Copyright (c) 2018, QuantStack                                           *
*                                                                          *
* Distributed under the terms of the BSD 3-Clause License.                 *
*                                                                          *
* The full license is in the file LICENSE, distributed with this software. *
****************************************************************************/

#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

// This must be included BEFORE pybind
// otherwise it fails to build on Windows
// because of the redefinition of snprintf
#include "nlohmann/json.hpp"

#include "pybind11_json/pybind11_json.hpp"

#include "pybind11/pybind11.h"
#include "pybind11/stl.h"

#include "xeus/xinterpreter.hpp"
#include "xeus/xmiddleware.hpp"
#include "xeus/xsystem.hpp"

#include "xeus-python/xdebugger.hpp"
#include "xptvsd_client.hpp"
#include "xutils.hpp"

namespace nl = nlohmann;
namespace py = pybind11;

using namespace pybind11::literals;
using namespace std::placeholders;

namespace xpyt
{
    debugger::debugger(zmq::context_t& context,
                       const xeus::xconfiguration& kernel_config,
                       const std::string& user_name,
                       const std::string& session_id)
        : p_ptvsd_client(new xptvsd_client(context, kernel_config, xeus::get_socket_linger(), user_name, session_id,
                                           std::bind(&debugger::handle_event, this, _1)))
        , m_ptvsd_socket(context, zmq::socket_type::req)
        , m_ptvsd_header(context, zmq::socket_type::req)
        , m_ptvsd_port("")
        , m_is_started(false)
    {
        m_ptvsd_socket.set(zmq::sockopt::linger, xeus::get_socket_linger());
        m_ptvsd_header.set(zmq::sockopt::linger, xeus::get_socket_linger());
        m_ptvsd_port = xeus::find_free_port(100, 5678, 5900);
    }

    debugger::~debugger()
    {
        delete p_ptvsd_client;
        p_ptvsd_client = nullptr;
    }

    nl::json debugger::process_request_impl(const nl::json& header,
                                            const nl::json& message)
    {
        nl::json reply = nl::json::object();

        if(message["command"] == "initialize")
        {
            if(m_is_started)
            {
                std::clog << "XEUS-PYTHON: the debugger has already started" << std::endl;
            }
            else
            {
                start();
                std::clog << "XEUS-PYTHON: the debugger has started" << std::endl;
            }
        }

        if(m_is_started)
        {
            std::string header_buffer = header.dump();
            zmq::message_t raw_header(header_buffer.c_str(), header_buffer.length());
            m_ptvsd_header.send(raw_header, zmq::send_flags::none);
            // client responds with ACK message
            (void)m_ptvsd_header.recv(raw_header);

            if(message["command"] == "dumpCell")
            {
                reply = dump_cell_request(message);
            }
            else if(message["command"] == "setBreakpoints")
            {
                reply = set_breakpoints_request(message);
            }
            else if(message["command"] == "source")
            {
                reply = source_request(message);
            }
            else if(message["command"] == "stackTrace")
            {
                reply = stack_trace_request(message);
            }
            else if(message["command"] == "variables")
            {
                reply = variables_request(message);
            }
            else
            {
                reply = forward_message(message);
            }
        }

        if(message["command"] == "debugInfo")
        {
            reply = debug_info_request(message);
        }
        else if(message["command"] == "inspectVariables")
        {
            reply = inspect_variables_request(message);
        }
        else if(message["command"] == "disconnect")
        {
            stop();
            std::clog << "XEUS-PYTHON: the debugger has stopped" << std::endl;
        }

        return reply;
    }

    nl::json debugger::forward_message(const nl::json& message)
    {
        std::string content = message.dump();
        size_t content_length = content.length();
        std::string buffer = xptvsd_client::HEADER
                           + std::to_string(content_length)
                           + xptvsd_client::SEPARATOR
                           + content;
        zmq::message_t raw_message(buffer.c_str(), buffer.length());
        m_ptvsd_socket.send(raw_message, zmq::send_flags::none);

        zmq::message_t raw_reply;
        (void)m_ptvsd_socket.recv(raw_reply);

        return nl::json::parse(std::string(raw_reply.data<const char>(), raw_reply.size()));
    }

    nl::json debugger::dump_cell_request(const nl::json& message)
    {
        std::string code;
        try
        {
            code = message["arguments"]["code"];
        }
        catch(nl::json::type_error& e)
        {
            std::clog << e.what() << std::endl;
        }
        catch(...)
        {
            std::clog << "XEUS-PYTHON: Unknown issue" << std::endl;
        }

        std::string next_file_name = get_cell_tmp_file(code);
        std::clog << "XEUS-PYTHON: dumped " << next_file_name << std::endl;

        std::fstream fs(next_file_name, std::ios::in);
        if(!fs.is_open())
        {
            fs.clear();
            fs.open(next_file_name, std::ios::out);
            fs << code;
        }

        nl::json reply = {
            {"type", "response"},
            {"request_seq", message["seq"]},
            {"success", true},
            {"command", message["command"]},
            {"body", {
                {"sourcePath", next_file_name}
            }}
        };
        return reply;
    }

    nl::json debugger::set_breakpoints_request(const nl::json& message)
    {
        std::string source = message["arguments"]["source"]["path"];
        m_breakpoint_list.erase(source);
        nl::json bp_json = message["arguments"]["breakpoints"];
        std::vector<nl::json> bp_list(bp_json.begin(), bp_json.end());
        m_breakpoint_list.insert(std::make_pair(std::move(source), std::move(bp_list)));
        nl::json breakpoint_reply = forward_message(message);
        return breakpoint_reply;
    }

    nl::json debugger::source_request(const nl::json& message)
    {
        std::string sourcePath;
        try
        {
            sourcePath = message["arguments"]["source"]["path"];
        }
        catch(nl::json::type_error& e)
        {
            std::clog << e.what() << std::endl;
        }
        catch(...)
        {
            std::clog << "XEUS-PYTHON: Unknown issue" << std::endl;
        }

        std::ifstream ifs(sourcePath, std::ios::in);
        if(!ifs.is_open())
        {
            nl::json reply = {
                {"type", "response"},
                {"request_seq", message["seq"]},
                {"success", false},
                {"command", message["command"]},
                {"message", "source unavailable"},
                {"body", {{}}}
            };
            return reply;
        }

        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

        nl::json reply = {
            {"type", "response"},
            {"request_seq", message["seq"]},
            {"success", true},
            {"command", message["command"]},
            {"body", {
                {"content", content}
            }}
        };
        return reply;
    }

    nl::json debugger::stack_trace_request(const nl::json& message)
    {
        nl::json reply = forward_message(message);
        size_t size = reply["body"]["stackFrames"].size();
        for(size_t i = 0; i < size; ++i)
        {
            if(reply["body"]["stackFrames"][i]["source"]["path"] == "<string>")
            {
                reply["body"]["stackFrames"].erase(i);
                break;
            }
        }
#ifdef WIN32
        size = reply["body"]["stackFrames"].size();
        for(size_t i = 0; i < size; ++i)
        {
            std::string path = reply["body"]["stackFrames"][i]["source"]["path"];
            std::replace(path.begin(), path.end(), '\\', '/');
            reply["body"]["stackFrames"][i]["source"]["path"] = path;
        }
#endif
        return reply;
    }

    nl::json debugger::variables_request(const nl::json& message)
    {
        nl::json reply = forward_message(message);
        auto start_it =  message["arguments"].find("start");
        auto count_it = message["arguments"].find("count");
        auto end_it = message["arguments"].end();
        if(start_it != end_it || count_it != end_it)
        {
            int start = start_it != end_it ? start_it->get<int>() : 0;
            int count = count_it != end_it ? count_it->get<int>() : 0;
            if(start != 0 || count != 0)
            {
                int end = count == 0 ? reply["body"]["variables"].size() : start + count;
                nl::json old_variables_list = reply["body"]["variables"];
                reply["body"].erase("variables");
                nl::json variables_list;
                for(int i = start; i < end; ++i)
                {
                    variables_list.push_back(old_variables_list.at(i));
                }
                reply["body"]["variables"] = variables_list;
            }
        }
        return reply;
    }

    nl::json debugger::debug_info_request(const nl::json& message)
    {
        nl::json breakpoint_list = nl::json::array();

        if(m_is_started)
        {
            for(auto it = m_breakpoint_list.cbegin(); it != m_breakpoint_list.cend(); ++it)
            {
                breakpoint_list.push_back({{"source", it->first},
                                           {"breakpoints", it->second}});
            }
        }

        std::lock_guard<std::mutex> lock(m_stopped_mutex);
        nl::json reply = {
            {"type", "response"},
            {"request_seq", message["seq"]},
            {"success", true},
            {"command", message["command"]},
            {"body", {
                {"isStarted", m_is_started},
                {"hashMethod", "Murmur2"},
                {"hashSeed", get_hash_seed()},
                {"tmpFilePrefix", get_tmp_prefix()},
                {"tmpFileSuffix", get_tmp_suffix()},
                {"breakpoints", breakpoint_list},
                {"stoppedThreads", m_stopped_threads}
            }}
        };
        return reply;
    }

    nl::json debugger::inspect_variables_request(const nl::json& message)
    {
        py::gil_scoped_acquire acquire;
        py::object variables = py::globals();

        nl::json json_vars = nl::json::array();
        for (const py::handle& key : variables)
        {
            nl::json json_var = nl::json::object();
            json_var["name"] = py::str(key);
            json_var["variablesReference"] = 0;
            try
            {
                json_var["value"] = variables[key];
            }
            catch(std::exception&)
            {
                json_var["value"] = py::repr(variables[key]);
            }
            json_vars.push_back(json_var);
        }

        nl::json reply = {
            {"type", "response"},
            {"request_seq", message["seq"]},
            {"success", true},
            {"command", message["command"]},
            {"body", {
                {"variables", json_vars}
            }}
        };

        return reply;
    }

    void debugger::start()
    {
        std::string host = "127.0.0.1";
        std::string temp_dir = xeus::get_temp_directory_path();
        std::string log_dir = temp_dir + "/" + "xpython_debug_logs_" + std::to_string(xeus::get_current_pid());

        xeus::create_directory(log_dir);

        // PTVSD has to be started in the main thread
        std::string code = "import ptvsd\nptvsd.enable_attach((\'" + host + "\'," + m_ptvsd_port
                         + "), log_dir=\'" + log_dir + "\')";
        nl::json json_code;
        json_code["code"] = code;
        nl::json rep = xdebugger::get_control_messenger().send_to_shell(json_code);
        std::string status = rep["status"].get<std::string>();
        if(status != "ok")
        {
            std::string ename = rep["ename"].get<std::string>();
            std::string evalue = rep["evalue"].get<std::string>();
            std::vector<std::string> traceback = rep["traceback"].get<std::vector<std::string>>();
            std::clog << "Exception raised when trying to import ptvsd" << std::endl;
            for(std::size_t i = 0; i < traceback.size(); ++i)
            {
                std::clog << traceback[i] << std::endl;
            }
            std::clog << ename << " - " << evalue << std::endl;
        }

        std::string controller_end_point = xeus::get_controller_end_point("debugger");
        std::string controller_header_end_point = xeus::get_controller_end_point("debugger_header");
        std::string publisher_end_point = xeus::get_publisher_end_point();

        m_ptvsd_socket.bind(controller_end_point);
        m_ptvsd_header.bind(controller_header_end_point);

        std::string ptvsd_end_point = "tcp://" + host + ':' + m_ptvsd_port;
        std::thread client(&xptvsd_client::start_debugger,
                           p_ptvsd_client,
                           ptvsd_end_point,
                           publisher_end_point,
                           controller_end_point,
                           controller_header_end_point);
        client.detach();

        m_ptvsd_socket.send(zmq::message_t("REQ", 3), zmq::send_flags::none);
        zmq::message_t ack;
        (void)m_ptvsd_socket.recv(ack);

        m_is_started = true;

        std::string tmp_folder =  get_tmp_prefix();
        xeus::create_directory(tmp_folder);
    }

    void debugger::stop()
    {
        std::string controller_end_point = xeus::get_controller_end_point("debugger");
        std::string controller_header_end_point = xeus::get_controller_end_point("debugger_header");
        m_ptvsd_socket.unbind(controller_end_point);
        m_ptvsd_header.unbind(controller_header_end_point);
        m_breakpoint_list.clear();
        m_stopped_threads.clear();
        m_is_started = false;
    }

    void debugger::handle_event(const nl::json& message)
    {
        std::string event = message["event"];
        if(event == "stopped")
        {
            std::lock_guard<std::mutex> lock(m_stopped_mutex);
            int id = message["body"]["threadId"];
            m_stopped_threads.insert(id);
        }
        else if(event == "continued")
        {
            std::lock_guard<std::mutex> lock(m_stopped_mutex);
            int id = message["body"]["threadId"];
            m_stopped_threads.erase(id);
        }
    }

    std::unique_ptr<xeus::xdebugger> make_python_debugger(zmq::context_t& context,
                                                          const xeus::xconfiguration& kernel_config,
                                                          const std::string& user_name,
                                                          const std::string& session_id,
                                                          const nl::json& debugger_config)
    {
        return std::unique_ptr<xeus::xdebugger>(new debugger(context, kernel_config, user_name, session_id));
    }
}

