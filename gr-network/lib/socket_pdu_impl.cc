/* -*- c++ -*- */
/*
 * Copyright 2013,2019 Free Software Foundation, Inc.
 *
 * This file is part of GNU Radio
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include <sstream>
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "socket_pdu_impl.h"
#include "tcp_connection.h"
#include <gnuradio/io_signature.h>
#include <gnuradio/pdu.h>

namespace gr {
namespace network {

socket_pdu::sptr socket_pdu::make(std::string type,
                                  std::string addr,
                                  std::string port,
                                  int MTU /*= 10000*/,
                                  bool tcp_no_delay /*= false*/)
{
    return gnuradio::make_block_sptr<socket_pdu_impl>(
        type, addr, port, MTU, tcp_no_delay);
}

socket_pdu_impl::socket_pdu_impl(std::string type,
                                 std::string addr,
                                 std::string port,
                                 int MTU /*= 10000*/,
                                 bool tcp_no_delay /*= false*/)
    : block("socket_pdu", io_signature::make(0, 0, 0), io_signature::make(0, 0, 0)),
      d_tcp_no_delay(tcp_no_delay)
{
    d_rxbuf.resize(MTU);

    message_port_register_in(msgport_names::pdus());
    message_port_register_out(msgport_names::pdus());

    if ((type == "TCP_SERVER") &&
        ((addr.empty()) || (addr == "0.0.0.0"))) { // Bind on all interfaces
        int port_num = atoi(port.c_str());
        if (port_num == 0)
            throw std::invalid_argument(
                "gr::pdu:socket_pdu: invalid port for TCP_SERVER");
        d_tcp_endpoint =
            boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port_num);
    } else if ((type == "TCP_SERVER") || (type == "TCP_CLIENT")) {
        boost::asio::ip::tcp::resolver resolver(d_io_service);
        boost::asio::ip::tcp::resolver::query query(
            boost::asio::ip::tcp::v4(),
            addr,
            port,
            boost::asio::ip::resolver_query_base::passive);
        d_tcp_endpoint = *resolver.resolve(query);
    } else if ((type == "UDP_SERVER") &&
               ((addr.empty()) || (addr == "0.0.0.0"))) { // Bind on all interfaces
        int port_num = atoi(port.c_str());
        if (port_num == 0)
            throw std::invalid_argument(
                "gr::pdu:socket_pdu: invalid port for UDP_SERVER");
        d_udp_endpoint =
            boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), port_num);
    } else if ((type == "UDP_SERVER") || (type == "UDP_CLIENT")) {
        boost::asio::ip::udp::resolver resolver(d_io_service);
        boost::asio::ip::udp::resolver::query query(
            boost::asio::ip::udp::v4(),
            addr,
            port,
            boost::asio::ip::resolver_query_base::passive);

        if (type == "UDP_SERVER")
            d_udp_endpoint = *resolver.resolve(query);
        else
            d_udp_endpoint_other = *resolver.resolve(query);
    }

    if (type == "TCP_SERVER") {
        d_acceptor_tcp = std::make_shared<boost::asio::ip::tcp::acceptor>(d_io_service,
                                                                          d_tcp_endpoint);
        d_acceptor_tcp->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));

        start_tcp_accept();

        set_msg_handler(msgport_names::pdus(),
                        [this](pmt::pmt_t msg) { this->tcp_server_send(msg); });
    } else if (type == "TCP_CLIENT") {
        boost::system::error_code error = boost::asio::error::host_not_found;
        d_tcp_socket = std::make_shared<boost::asio::ip::tcp::socket>(d_io_service);
        d_tcp_socket->connect(d_tcp_endpoint, error);
        if (error)
            throw boost::system::system_error(error);
        d_tcp_socket->set_option(boost::asio::ip::tcp::no_delay(d_tcp_no_delay));

        set_msg_handler(msgport_names::pdus(),
                        [this](pmt::pmt_t msg) { this->tcp_client_send(msg); });

        d_tcp_socket->async_read_some(
            boost::asio::buffer(d_rxbuf),
            [this](const boost::system::error_code& error, size_t bytes_transferred) {
                handle_tcp_read(error, bytes_transferred);
            });
    } else if (type == "UDP_SERVER") {
        d_udp_socket =
            std::make_shared<boost::asio::ip::udp::socket>(d_io_service, d_udp_endpoint);
        d_udp_socket->async_receive_from(
            boost::asio::buffer(d_rxbuf),
            d_udp_endpoint_other,
            [this](const boost::system::error_code& error, size_t bytes_transferred) {
                handle_udp_read(error, bytes_transferred);
            });

        set_msg_handler(msgport_names::pdus(),
                        [this](pmt::pmt_t msg) { this->udp_send(msg); });
    } else if (type == "UDP_CLIENT") {
        d_udp_socket =
            std::make_shared<boost::asio::ip::udp::socket>(d_io_service, d_udp_endpoint);
        d_udp_socket->async_receive_from(
            boost::asio::buffer(d_rxbuf),
            d_udp_endpoint_other,
            [this](const boost::system::error_code& error, size_t bytes_transferred) {
                handle_udp_read(error, bytes_transferred);
            });

        set_msg_handler(msgport_names::pdus(),
                        [this](pmt::pmt_t msg) { this->udp_send(msg); });
    } else
        throw std::runtime_error("gr::pdu:socket_pdu: unknown socket type");

    d_thread = gr::thread::thread([this] { run_io_service(); });
    d_started = true;
}

socket_pdu_impl::~socket_pdu_impl() { stop(); }

bool socket_pdu_impl::stop()
{
    if (d_started) {
        d_io_service.stop();
        d_thread.interrupt();
        d_thread.join();
    }
    d_started = false;
    return true;
}

void socket_pdu_impl::handle_tcp_read(const boost::system::error_code& error,
                                      size_t bytes_transferred)
{
    if (!error) {
        pmt::pmt_t vector =
            pmt::init_u8vector(bytes_transferred, (const uint8_t*)&d_rxbuf[0]);
        pmt::pmt_t pdu = pmt::cons(pmt::PMT_NIL, vector);
        message_port_pub(msgport_names::pdus(), pdu);

        d_tcp_socket->async_read_some(
            boost::asio::buffer(d_rxbuf),
            [this](const boost::system::error_code& error, size_t bytes_transferred) {
                handle_tcp_read(error, bytes_transferred);
            });
    } else
        throw boost::system::system_error(error);
}

void socket_pdu_impl::start_tcp_accept()
{
#if (BOOST_VERSION >= 107000)
    tcp_connection::sptr new_connection =
        tcp_connection::make(d_io_service, d_rxbuf.size(), d_tcp_no_delay);
#else
    tcp_connection::sptr new_connection = tcp_connection::make(
        d_acceptor_tcp->get_io_service(), d_rxbuf.size(), d_tcp_no_delay);
#endif

    d_acceptor_tcp->async_accept(
        new_connection->socket(),
        [this, new_connection](const boost::system::error_code& error) {
            handle_tcp_accept(new_connection, error);
        });
}

void socket_pdu_impl::tcp_server_send(pmt::pmt_t msg)
{
    pmt::pmt_t vector = pmt::cdr(msg);
    for (size_t i = 0; i < d_tcp_connections.size(); i++)
        d_tcp_connections[i]->send(vector);
}

void socket_pdu_impl::handle_tcp_accept(tcp_connection::sptr new_connection,
                                        const boost::system::error_code& error)
{
    if (!error) {
        // Garbage collect closed sockets
        std::vector<tcp_connection::sptr>::iterator it = d_tcp_connections.begin();
        while (it != d_tcp_connections.end()) {
            if (!(**it).socket().is_open())
                it = d_tcp_connections.erase(it);
            else
                ++it;
        }

        new_connection->start(this);
        d_tcp_connections.push_back(new_connection);
        start_tcp_accept();
    } else {
        d_logger->error("{:s}", error.message());
    }
}

void socket_pdu_impl::tcp_client_send(pmt::pmt_t msg)
{
    pmt::pmt_t vector = pmt::cdr(msg);
    size_t len = pmt::blob_length(vector);
    size_t offset = 0;
    std::vector<char> txbuf(std::min(len, d_rxbuf.size()));
    while (offset < len) {
        size_t send_len = std::min((len - offset), txbuf.size());
        memcpy(&txbuf[0], pmt::uniform_vector_elements(vector, offset), send_len);
        offset += send_len;
        d_tcp_socket->send(boost::asio::buffer(txbuf, send_len));
    }
}

void socket_pdu_impl::udp_send(pmt::pmt_t msg)
{
    if (d_udp_endpoint_other.address().to_string() == "0.0.0.0")
        return;

    pmt::pmt_t vector = pmt::cdr(msg);
    size_t len = pmt::blob_length(vector);
    size_t offset = 0;
    std::vector<char> txbuf(std::min(len, d_rxbuf.size()));
    while (offset < len) {
        size_t send_len = std::min((len - offset), txbuf.size());
        memcpy(&txbuf[0], pmt::uniform_vector_elements(vector, offset), send_len);
        offset += send_len;
        d_udp_socket->send_to(boost::asio::buffer(txbuf, send_len), d_udp_endpoint_other);
    }
}

void socket_pdu_impl::handle_udp_read(const boost::system::error_code& error,
                                      size_t bytes_transferred)
{
    if (!error) {
        pmt::pmt_t vector =
            pmt::init_u8vector(bytes_transferred, (const uint8_t*)&d_rxbuf[0]);
        pmt::pmt_t pdu = pmt::cons(pmt::PMT_NIL, vector);

        message_port_pub(msgport_names::pdus(), pdu);

        d_udp_socket->async_receive_from(
            boost::asio::buffer(d_rxbuf),
            d_udp_endpoint_other,
            [this](const boost::system::error_code& error, size_t bytes_transferred) {
                handle_udp_read(error, bytes_transferred);
            });
    }
}

} /* namespace network */
} /* namespace gr */
