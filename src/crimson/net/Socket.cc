// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "Socket.h"
#include <seastar/core/polymorphic_temporary_buffer.hh>

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

void Socket::return_unused(buffer_t&& buf) {
  returned_rxbuf = std::move(buf);
}

seastar::temporary_buffer<char>
Socket::create(seastar::compat::polymorphic_allocator<char>* const allocator)
{
#if 0
  // space for segments_n, epilogue_n and preable_n+1
  return seastar::make_temporary_buffer<char>(allocator, 8192);
#else

#if 1
  if (!returned_rxbuf.empty()) {
    return std::move(returned_rxbuf);
  }

  ceph_assert(read_hint.bytes > 0);
  logger().debug("{}: read_hint: .bytes={}, alignment: .base={}, .at={}",
                 __func__,
                 read_hint.bytes,
                 read_hint.alignment.base,
                 read_hint.alignment.at);

  auto ret = seastar::make_temporary_buffer<char>(allocator,
    read_hint.alignment.base + read_hint.bytes);
  if (read_hint.alignment.base != 0) {
    const auto offset_in_buf = read_hint.alignment.base
      - p2phase<uintptr_t>(reinterpret_cast<uintptr_t>(ret.get()), read_hint.alignment.base)
      - read_hint.alignment.at;
    ret.trim_front(offset_in_buf);
    ret.trim(read_hint.bytes);
  }

  hinted_rxbuf = ceph::buffer::create(ret.share());
  return ret;

  // TODO: implement prefetching for very small (under 4K) chunk sizes to not
  // hurt RADOS' reads while the POSIX stack is being used (and till it lacks
  // io_uring support).
#endif
#endif
}


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

      if (hinted_rxbuf.c_str() <= rbuf.get() &&
          hinted_rxbuf.end_c_str() >= rbuf.get() + rbuf.size()) {
        // yay, S* gave us back (a part of) buffer the ibf had produced.
        size_t offset = rbuf.get() - hinted_rxbuf.c_str();
        r.sgl.append(hinted_rxbuf, offset, round_size);
        rbuf.trim_front(round_size);
      } else {
        r.sgl.push_back(buffer::create(sharing_split(rbuf, round_size)));
      }
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
