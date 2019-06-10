// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <seastar/core/reactor.hh>
#include <seastar/core/sharded.hh>
#include <seastar/net/packet.hh>

#include "include/buffer.h"
#include "msg/msg_types.h"

namespace ceph::net {

class Socket;
using SocketFRef = seastar::foreign_ptr<std::unique_ptr<Socket>>;

class Socket : private seastar::net::input_buffer_factory
{
public:
  using read_buffer_t = seastar::temporary_buffer<char>;

private:
  const seastar::shard_id sid;
  seastar::connected_socket socket;
  seastar::net::input_buffer_factory* ibf = nullptr;
  seastar::input_stream<char> in;
  seastar::output_stream<char> out;

  read_buffer_t rbuf;

  // reading state for read() and read_exactly()
  struct {
    ceph::bufferlist sgl;
    read_buffer_t contiguous_buffer;
    size_t remaining;
  } r;

  struct construct_tag {};

  // input_buffer_factory
  buffer_t
  create(seastar::compat::polymorphic_allocator<char>* const allocator) override {
    ceph_assert(ibf);
    // XXX: we'll have deep, multi-stage delegation. CPU's front-end might not
    // be supper happy. Profile and refactor if necessary.
    return ibf->create(allocator);
  }

  void return_unused(buffer_t&& buf) {
    return ibf->return_unused(std::move(buf));
  }

 public:
  Socket(seastar::connected_socket&& _socket, construct_tag)
    : sid{seastar::engine().cpu_id()},
      socket(std::move(_socket)),
      in(socket.input(this)),
      // the default buffer size 8192 is too small that may impact our write
      // performance. see seastar::net::connected_socket::output()
      out(socket.output(65536)) {}

  Socket(Socket&& o) = delete;

  static seastar::future<SocketFRef>
  connect(const entity_addr_t& peer_addr) {
    return seastar::connect(peer_addr.in4_addr())
      .then([] (seastar::connected_socket socket) {
        return seastar::make_foreign(std::make_unique<Socket>(std::move(socket),
							      construct_tag{}));
      });
  }

  static seastar::future<SocketFRef, entity_addr_t>
  accept(seastar::server_socket& listener) {
    return listener.accept().then([] (seastar::connected_socket socket,
				      seastar::socket_address paddr) {
        entity_addr_t peer_addr;
        peer_addr.set_sockaddr(&paddr.as_posix_sockaddr());
        return seastar::make_ready_future<SocketFRef, entity_addr_t>(
          seastar::make_foreign(std::make_unique<Socket>(std::move(socket),
							 construct_tag{})),
	  peer_addr);
      });
  }

  // HACK: this is a makeshift solution for evaluating the input buffer
  // factory concept. We need this because a Socket can (theoretically)
  // live on different core than SocketConnection that makes use of it.
  // See: the accept loop in SocketMessenger::do_start();
  void set_input_buffer_factory(seastar::net::input_buffer_factory* const ibf) {
    this->ibf = ibf;
  }

  /// read the requested number of bytes into a bufferlist
  // TODO: rename to read_scattered() or read_sgl()
  seastar::future<ceph::bufferlist> read(size_t bytes);
  // TODO: rename to read_contiguous()
  seastar::future<read_buffer_t> read_exactly(size_t bytes);

  using packet = seastar::net::packet;
  seastar::future<> write(packet&& buf) {
    return out.write(std::move(buf));
  }
  seastar::future<> flush() {
    return out.flush();
  }
  seastar::future<> write_flush(packet&& buf) {
    return out.write(std::move(buf)).then([this] { return out.flush(); });
  }

  /// Socket can only be closed once.
  seastar::future<> close() {
    return seastar::smp::submit_to(sid, [this] {
        return seastar::when_all(
          in.close(), out.close()).discard_result();
      });
  }
};

} // namespace ceph::net
