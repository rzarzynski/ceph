#include <atomic>
#include <errno.h>
#include <limits.h>

#include <sys/uio.h>

#include "include/types.h"
#include "include/buffer_raw.h"
#include "include/compat.h"
#include "include/mempool.h"
#include "armor.h"
#include "common/environment.h"
#include "common/errno.h"
#include "common/safe_io.h"
#include "common/strtol.h"
#include "common/likely.h"
#include "common/valgrind.h"
#include "common/deleter.h"
#include "common/RWLock.h"
#include "include/spinlock.h"
#include "include/scope_guard.h"

#define CEPH_BUFFER_ALLOC_UNIT  (std::min(CEPH_PAGE_SIZE, 4096u))
#define CEPH_BUFFER_APPEND_SIZE (CEPH_BUFFER_ALLOC_UNIT - sizeof(raw_combined))

#define FORCE_INLINE __attribute__((used,always_inline,optimize("O3"),optimize("ipa-pta"),optimize("ipa-cp-clone")))

namespace ceph {

  /*
   * raw_combined is always placed within a single allocation along
   * with the data buffer.  the data goes at the beginning, and
   * raw_combined at the end.
   */
  class buffer::raw_combined : public buffer::raw {
    size_t alignment;
  public:
    raw_combined(char *dataptr, unsigned l, unsigned align,
		 int mempool) noexcept
      : raw(dataptr, l, mempool),
	alignment(align) {
      //inc_total_alloc(len);
      //inc_history_alloc(len);
    }
    ~raw_combined() override {
      //dec_total_alloc(len);
    }
    raw* clone_empty() override {
      return create(len, alignment);
    }

    static raw_combined* FORCE_INLINE __attribute__((malloc)) create(unsigned len,
				unsigned align,
				int mempool = mempool::mempool_buffer_anon) noexcept {
      if (!align)
	align = sizeof(size_t);
      size_t rawlen = round_up_to(sizeof(buffer::raw_combined),
				  alignof(buffer::raw_combined));
      size_t datalen = round_up_to(len, alignof(buffer::raw_combined));

      char *ptr = 0;
      int r = ::posix_memalign((void**)(void*)&ptr, align, rawlen + datalen);

      // actual data first, since it has presumably larger alignment restriction
      // then put the raw_combined at the end
      return new (ptr + datalen) raw_combined(ptr, len, align, mempool);
    }

    static void operator delete(void *ptr) {
      raw_combined *raw = (raw_combined *)ptr;
      ::free((void *)raw->data);
    }
  };




__attribute__((used)) buffer::ptr::ptr(buffer::raw* __restrict r)
  : _raw(r),
    _off(0),
    _len(r->len)   // no lock needed; this is an unref raw.
{
  r->nref++;
//  bdout << "ptr " << this << " get " << _raw << bendl;
}

inline FORCE_INLINE int buffer::ptr::get_mempool() const {
  buffer::raw* const __restrict__ _raw = this->_raw;
    if (_raw) {
      return _raw->mempool;
    }
    return mempool::mempool_buffer_anon;
  }

inline FORCE_INLINE int buffer::ptr::raw_nref() const
{
  buffer::raw* const __restrict__ _raw = this->_raw;
  assert(_raw);
  return _raw->nref;
}

inline FORCE_INLINE unsigned buffer::ptr::unused_tail_length() __restrict__ const noexcept
{
  return _raw->len - (_off+_len);
}

inline FORCE_INLINE unsigned buffer::ptr::raw_length() const
{
  buffer::raw* const __restrict__ _raw = this->_raw;
  assert(_raw);
  return _raw->len;
}

inline FORCE_INLINE unsigned buffer::ptr::append(const char* __restrict p, unsigned l) noexcept
{
  buffer::raw* const __restrict__ __raw = this->_raw;
  auto* __restrict _this = this;
  assert(__raw);
  assert(l <= unused_tail_length());
  char* __restrict__ c = __raw->data + _this->_off + _this->_len;
  maybe_inline_memcpy(c, p, l, 32);
  _this->_len += l;
  return _this->_len + _this->_off;
}


inline FORCE_INLINE int buffer::list::get_mempool() const 
  {
    if (_buffers.empty()) {
      return mempool::mempool_buffer_anon;
    }
    return _buffers.back().get_mempool();
  }

inline FORCE_INLINE size_t buffer::list::get_append_buffer_unused_tail_length() __restrict__ const noexcept
{
      if (unlikely(last_writeable == nullptr)) {
	return 0;
      }

#if 0
      if (buf.raw_nref() != 1) {
	return 0;
      }
#endif

      //return last_writeable->unused_tail_length();
      return last_writeable->_raw->len - (last_writeable->_off+last_writeable->_len);
    }

inline FORCE_INLINE buffer::ptr& buffer::list::refill_append_space(const unsigned len) noexcept
{
  // make a new buffer.  fill out a complete page, factoring in the
  // raw_combined overhead.
  size_t need = round_up_to(len, sizeof(size_t)) + sizeof(raw_combined);
  size_t alen = round_up_to(need, 4096) -
    sizeof(raw_combined);
  buffer::ptr& new_back = \
    _buffers.emplace_back(raw_combined::create(alen, 0));
  new_back.set_length(0);   // unused, so far.
  last_writeable = &new_back;
  return new_back;
}

inline FORCE_INLINE void buffer::list::microreserve(size_t len) noexcept
{
  try {
  if (get_append_buffer_unused_tail_length() < len) {
    auto& new_back = refill_append_space(len);
    if (new_back.offset() != 0)
      __builtin_unreachable();
    if (new_back.length() != 0)
      __builtin_unreachable();
  if (last_writeable != std::addressof(new_back))
    __builtin_unreachable();
  }
  } catch (...) { __builtin_unreachable(); }

  assert(get_append_buffer_unused_tail_length() >= len);
  if (last_writeable == nullptr)
    __builtin_unreachable();
  if (last_writeable->unused_tail_length() < len)
    __builtin_unreachable();
  if (last_writeable->raw_length() < len)
    __builtin_unreachable();

  auto& last = _buffers.back();
  _last_data = last._raw->data + last._off + last._len;
  _free_in_last = len;
  return;
  assert(!_buffers.empty());
  assert(_buffers.back().raw_length() >= len);
  assert(_buffers.back().raw_length() >= (len/2));
  assert(_buffers.back().unused_tail_length() >= len);
}


inline void FORCE_INLINE buffer::list::append(const char* __restrict data, unsigned len) noexcept
{
  _len += len;

#undef WANT_RAW_RELOAD

#ifdef WANT_RAW_RELOAD
  const unsigned free_in_last = get_append_buffer_unused_tail_length();
#else
  // this is exactly what get_append_buffer_unused_tail_length() does.
  // The only difference I see is lack of inlining (we did it manually)
  // and NOT ignoring __restrict__ on "this" pointer (so "_len += len"
  // doesn't force reloads). Well, some time ago there was a bug report
  // in GCC's tracker [1]. The comments say the issue has been fixed
  // but who knows...
  // [1] https://gcc.gnu.org/bugzilla/show_bug.cgi?id=60712

  if (likely(_free_in_last >= len)) {
    //char* const c = last_writeable->_raw->data + last_writeable->_off+last_writeable->_len;
    //char* __restrict__ const c = last_writeable->_raw->data; // + last_writeable->_off+last_writeable->_len;
    __builtin_memcpy(_last_data + (128 - _free_in_last), data, len);
    last_writeable->_len += len;
    _free_in_last -= len;
    return;
  }

  unsigned free_in_last = 0;
  if (likely(last_writeable != nullptr)) {
    auto* raw = last_writeable->_raw;
    //free_in_last = last.get_raw()->len - (last.offset()+last.length());
    free_in_last = raw->len - (last_writeable->_off + last_writeable->_len);
  }
#endif // WANT_RAW_RELOAD

  if (likely(free_in_last >= len)) {
    assert(last_writeable != nullptr);
    assert(last_writeable != nullptr);
    assert(last_writeable != nullptr);
    assert(last_writeable != nullptr);

    //char* const __restrict__ c = last_writeable->get_raw()->data + last_writeable->offset()+last_writeable->length();
    //memcpy(c, data, len);
    last_writeable->_len += len;
    free_in_last -= len;

    // Easier to (not :-) - thankfully to DCE) spot in `perf annotate` and `objdump -dCS`
    assert(last_writeable != nullptr);
    assert(last_writeable != nullptr);
    assert(last_writeable != nullptr);
    assert(last_writeable != nullptr);
    assert(last_writeable != nullptr);
    assert(last_writeable != nullptr);
    assert(last_writeable != nullptr);
    assert(last_writeable != nullptr);
    return;
  }

  const unsigned first_round = len;
  //_len += len;
  const unsigned second_round = len; // - first_round;
  if (unlikely(second_round)) {
    auto& new_back = refill_append_space(second_round);
    new_back.append(data + first_round, second_round);
  }
}

} // namespace ceph
