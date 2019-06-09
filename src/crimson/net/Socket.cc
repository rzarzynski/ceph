// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "Socket.h"

#include "Errors.h"

namespace ceph::net {

namespace {

inline seastar::temporary_buffer<char> sharing_split(
  seastar::temporary_buffer<char>& buf,
  const size_t bytes)
{
  auto shared_part = buf.share(0, bytes);
  buf.trim_front(bytes);
  return shared_part;
}

} // anonymous namespace

seastar::future<bufferlist> Socket::read(const size_t bytes)
{
  r.remaining = bytes;
  r.sgl.clear();
  return seastar::do_until(
    [this] { return r.remaining == 0; },
    [this] {
      if (rbuf.empty()) {
        return in.read().then([this] (read_buffer_t&& new_rbuf) {
          if (new_rbuf.empty()) {
            throw std::system_error(make_error_code(error::read_eof));
          }
          rbuf = std::move(new_rbuf);
          return seastar::now();
        });
      }

      const size_t round_size = std::min(r.remaining, rbuf.size());
      r.sgl.push_back(buffer::create(sharing_split(rbuf, round_size)));
      r.remaining -= round_size;
      return seastar::now();
    }
  ).then([this] {
    return seastar::make_ready_future<ceph::bufferlist>(std::move(r.sgl));
  });
}

seastar::future<Socket::read_buffer_t> Socket::read_exactly(size_t bytes) {
  if (bytes <= rbuf.size()) {
    // oh, the cheap and straightforward case ::read_exactly() is really
    // intended for.
    return seastar::make_ready_future<seastar::temporary_buffer<char>>(
        sharing_split(rbuf, bytes));
  }

  r.remaining = bytes;
  r.contiguous_buffer = seastar::temporary_buffer<char>(bytes);
  return seastar::do_until(
    [this] { return r.remaining == 0; },
    [this] {
      if (rbuf.empty()) {
        return in.read().then([this] (read_buffer_t&& new_rbuf) {
          if (new_rbuf.empty()) {
            throw std::system_error(make_error_code(error::read_eof));
          }
          rbuf = std::move(new_rbuf);
          return seastar::now();
        });
      }

      const size_t round_size = std::min(r.remaining, rbuf.size());
      const size_t completed = r.contiguous_buffer.size() - r.remaining;
      std::copy(rbuf.get(), rbuf.get() + round_size,
                r.contiguous_buffer.get_write() + completed);
      r.remaining -= round_size;
      rbuf.trim_front(round_size);
      return seastar::now();
    }
  ).then([this] {
    return seastar::make_ready_future<seastar::temporary_buffer<char>>(
        std::move(r.contiguous_buffer));
  });
}

} // namespace ceph::net
