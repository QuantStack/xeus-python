/***************************************************************************
* Copyright (c) 2018, Martin Renou, Johan Mabille, Sylvain Corlay, and     *
* Wolf Vollprecht                                                          *
* Copyright (c) 2018, QuantStack                                           *
*                                                                          *
* Distributed under the terms of the BSD 3-Clause License.                 *
*                                                                          *
* The full license is in the file LICENSE, distributed with this software. *
****************************************************************************/

#include "zmq_addon.hpp"
#include "nlohmann/json.hpp"
#include "xeus/xmessage.hpp"
#include "xdebugpy_client.hpp"

#include <iostream>
#include <thread>
#include <chrono>

namespace nl = nlohmann;

namespace xpyt
{
    xdebugpy_client::xdebugpy_client(zmq::context_t& context,
                                 const xeus::xconfiguration& config,
                                 int socket_linger,
                                 const std::string& user_name,
                                 const std::string& session_id,
                                 const event_callback& cb)
        : m_debugpy_socket(context, zmq::socket_type::stream)
        , m_id_size(256)
        , m_publisher(context, zmq::socket_type::pub)
        , m_controller(context, zmq::socket_type::rep)
        , m_controller_header(context, zmq::socket_type::rep)
        , m_user_name(user_name)
        , m_session_id(session_id)
        , m_event_callback(cb)
        , p_auth(xeus::make_xauthentication(config.m_signature_scheme, config.m_key))
        , m_parent_header("")
        , m_request_stop(false)
    {
        m_debugpy_socket.setsockopt(ZMQ_LINGER, socket_linger);
        m_publisher.setsockopt(ZMQ_LINGER, socket_linger);
        m_controller.setsockopt(ZMQ_LINGER, socket_linger);
        m_controller_header.setsockopt(ZMQ_LINGER, socket_linger);
    }

    void xdebugpy_client::start_debugger(std::string debugpy_end_point,
                                       std::string publisher_end_point,
                                       std::string controller_end_point,
                                       std::string controller_header_end_point)
    {
        m_publisher.connect(publisher_end_point);
        m_controller.connect(controller_end_point);
        m_controller_header.connect(controller_header_end_point);
        
        m_debugpy_socket.connect(debugpy_end_point);
        m_debugpy_socket.getsockopt(ZMQ_IDENTITY, m_socket_id, &m_id_size);

        // Tells the controller that the connection with
        // debugpy has been established
        zmq::message_t req;
        (void)m_controller.recv(req);
        m_controller.send(zmq::message_t("ACK", 3), zmq::send_flags::none);
        
        zmq::pollitem_t items[] = {
            { m_controller_header, 0, ZMQ_POLLIN, 0 },
            { m_controller, 0, ZMQ_POLLIN, 0 },
            { m_debugpy_socket, 0, ZMQ_POLLIN, 0 }
        };
        
        m_request_stop = false;
        while(!m_request_stop)
        {
            zmq::poll(&items[0], 3, -1);

            if(items[0].revents & ZMQ_POLLIN)
            {
                handle_header_socket();
            }

            if(items[1].revents & ZMQ_POLLIN)
            {
                std::cout << "DEBUGPY - Received message on control socket - BEGIN" << std::endl;
                handle_control_socket();
                std::cout << "DEBUGPY - Received message on control socket - END" << std::endl;
            }

            if(items[2].revents & ZMQ_POLLIN)
            {
                handle_debugpy_socket(m_message_queue);
            }

            process_message_queue();
        }

        m_debugpy_socket.disconnect(debugpy_end_point);
        m_controller.disconnect(controller_end_point);
        m_controller_header.disconnect(controller_header_end_point);
        m_publisher.disconnect(publisher_end_point);
        // Reset m_request_stop for the next debugging session
        m_request_stop = false;
    }

    void xdebugpy_client::process_message_queue()
    {
        while(!m_message_queue.empty())
        {
            const std::string& raw_message = m_message_queue.front();
            nl::json message = nl::json::parse(raw_message);
            // message is either an event or a response
            if(message["type"] == "event")
            {
                handle_event(std::move(message));
            }
            else
            {
                if(message["command"] == "disconnect")
                {
                    m_request_stop = true;
                }
                zmq::message_t reply(raw_message.c_str(), raw_message.size());
                m_controller.send(reply, zmq::send_flags::none);
            }
            m_message_queue.pop();
        }
    }

    void xdebugpy_client::handle_header_socket()
    {
        zmq::message_t message;
        (void)m_controller_header.recv(message);
        m_parent_header = std::string(message.data<const char>(), message.size());
        m_controller_header.send(zmq::message_t("ACK", 3), zmq::send_flags::none);
    }

    void xdebugpy_client::handle_debugpy_socket(queue_type& message_queue)
    {
        using size_type = std::string::size_type;
        
        std::string buffer = "";
        bool messages_received = false;
        size_type header_pos = std::string::npos;
        size_type separator_pos = std::string::npos;
        size_type msg_size = 0;
        size_type msg_pos = std::string::npos;
        size_type hint = 0;

        while(!messages_received)
        {
            while(header_pos == std::string::npos)
            {
                append_tcp_message(buffer);
                header_pos = buffer.find(HEADER, hint);
            }

            separator_pos = buffer.find(SEPARATOR, header_pos + HEADER_LENGTH);
            while(separator_pos == std::string::npos)
            {
                hint = buffer.size();
                append_tcp_message(buffer);
                separator_pos = buffer.find(SEPARATOR, hint);
            }

            msg_size = std::stoull(buffer.substr(header_pos + HEADER_LENGTH, separator_pos));
            msg_pos = separator_pos + SEPARATOR_LENGTH;

            // The end of the buffer does not contain a full message
            while(buffer.size() - msg_pos < msg_size)
            {
                append_tcp_message(buffer);
            }

            // The end of the buffer contains a full message
            if(buffer.size() - msg_pos == msg_size)
            {
                message_queue.push(buffer.substr(msg_pos));
                messages_received = true;
            }
            else
            {
                // The end of the buffer contains a full message
                // and the beginning of a new one. We push the first
                // one in the queue, and loop again to get the next
                // one.
                message_queue.push(buffer.substr(msg_pos, msg_size));
                hint = msg_pos + msg_size;
                header_pos = buffer.find(HEADER, hint);
                separator_pos = std::string::npos;
            }
        }
    }

    void xdebugpy_client::handle_control_socket()
    {
        std::cout << "DEBUGPY_CLIENT::handle_control_socket() BEGIN" << std::endl;
        zmq::message_t message;
        (void)m_controller.recv(message);

        std::string raw_message = std::string(message.data<const char>(), message.size());
        std::cout << "raw_message = " << raw_message << std::endl;
        if (raw_message == "WAIT_ATTACH")
        {
            std::cout << "received WAIT_ATTACH msg" << std::endl;
            std::cout << "DEBUPY_CLIENT::handle_control_socket() END" << std::endl;
            return;
        }

        auto pos = raw_message.find(SEPARATOR);
        std::string to_parse = raw_message.substr(pos+SEPARATOR_LENGTH);
        nl::json json_message = nl::json::parse(to_parse);
        // Sends a ZMQ header (required for stream socket) and forwards
        // the message
        m_debugpy_socket.send(zmq::message_t(m_socket_id, m_id_size), zmq::send_flags::sndmore);
        m_debugpy_socket.send(message, zmq::send_flags::none);

        std::cout << "json message = " << json_message << std::endl;
        if (json_message["command"] == "attach")
        {
            m_controller.send(zmq::message_t("ACK", 3), zmq::send_flags::none);
        }
        std::cout << "DEBUGPY_CLIENT::handle_control_socket() END" << std::endl;
    }

    void xdebugpy_client::append_tcp_message(std::string& buffer)
    {
        // First message is a ZMQ header that we discard
        zmq::message_t header;
        (void)m_debugpy_socket.recv(header);

        zmq::message_t content;
        (void)m_debugpy_socket.recv(content);

        buffer += std::string(content.data<const char>(), content.size());
    }

    void xdebugpy_client::handle_event(nl::json message)
    {
        if(message["event"] == "stopped" && message["body"]["reason"] == "step")
        {
            int thread_id = message["body"]["threadId"];
            int seq = message["seq"];
            nl::json frames = get_stack_frames(thread_id, seq);
            if(frames.size() == 1 && frames[0]["source"]["path"]=="<string>")
            {
                wait_next(thread_id, seq);
            }
            else
            {
                forward_event(std::move(message));
            }
        }
        else
        {
            forward_event(std::move(message));
        }
    }

    void xdebugpy_client::forward_event(nl::json message)
    {
        m_event_callback(message);
        zmq::multipart_t wire_msg;
        nl::json header = xeus::make_header("debug_event", m_user_name, m_session_id);
        nl::json parent_header = m_parent_header.empty() ? nl::json::object() : nl::json::parse(m_parent_header);
        xeus::xpub_message msg("debug_event",
                                std::move(header),
                                std::move(parent_header),
                                nl::json::object(),
                                std::move(message),
                                xeus::buffer_sequence());
        std::move(msg).serialize(wire_msg, *p_auth);
        wire_msg.send(m_publisher);
    }

    nl::json xdebugpy_client::get_stack_frames(int thread_id, int seq)
    {
        nl::json request = {
            {"type", "request"},
            {"seq", seq},
            {"command", "stackTrace"},
            {"arguments", {
                {"threadId", thread_id}
            }}
        };

        send_debugpy_request(std::move(request));

        bool wait_for_stack_frame = true;
        nl::json reply;
        while(wait_for_stack_frame)
        {
            handle_debugpy_socket(m_stopped_queue);
            while(!m_stopped_queue.empty())
            {
                const std::string& raw_message = m_stopped_queue.front();
                nl::json message = nl::json::parse(raw_message);
                if(message["type"] == "response" && message["command"] == "stackTrace")
                {
                    reply = std::move(message);
                    wait_for_stack_frame = false;
                }
                else
                {
                    m_message_queue.push(raw_message);
                }
                m_stopped_queue.pop();
            }
        }
        return reply["body"]["stackFrames"];
    }

    void xdebugpy_client::wait_next(int thread_id, int seq)
    {
        nl::json request = {
            {"type", "request"},
            {"seq", seq},
            {"command", "next"},
            {"arguments", {
                {"threadId", thread_id}
            }}
        };

        send_debugpy_request(std::move(request));
        
        bool wait_reply = true;
        bool wait_event = true;
        while(wait_reply && wait_event)
        {
            handle_debugpy_socket(m_stopped_queue);

            while(!m_stopped_queue.empty())
            {
                const std::string& raw_message = m_stopped_queue.front();
                nl::json message = nl::json::parse(raw_message);
                std::string msg_type = message["type"];
                if(msg_type == "event" && message["event"] == "continued" && message["body"]["threadId"] == thread_id)
                {
                    wait_event = false;
                }
                else if(msg_type == "response" && message["command"] == "next")
                {
                    wait_reply = false;
                }
                else
                {
                    m_message_queue.push(raw_message);
                }
                m_stopped_queue.pop();
            }
        }
    }

    void xdebugpy_client::send_debugpy_request(nl::json message)
    {
        std::string content = message.dump();
        size_t content_length = content.length();
        std::string buffer = xdebugpy_client::HEADER
                           + std::to_string(content_length)
                           + xdebugpy_client::SEPARATOR
                           + content;
        zmq::message_t raw_message(buffer.c_str(), buffer.length());

        m_debugpy_socket.send(zmq::message_t(m_socket_id, m_id_size), zmq::send_flags::sndmore);
        m_debugpy_socket.send(raw_message, zmq::send_flags::none);
    }
}

