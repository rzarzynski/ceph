// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <exception>
#include <system_error>

namespace ceph::osd {
class error : private std::system_error {
public:
  error(const std::errc ec)
    : system_error(std::make_error_code(ec)) {
  }

  using system_error::code;
  using system_error::what;

  friend error make_error(int ret);

private:
  error(const int ret) noexcept
    : system_error(ret, std::system_category()) {
  }
};

inline error make_error(const int ret) {
  return error{-ret};
}

struct object_not_found : public error {
  object_not_found() : error(std::errc::no_such_file_or_directory) {}
};

struct object_corrupted : public error {
  object_corrupted() : error(std::errc::illegal_byte_sequence) {}
};

struct invalid_argument : public error {
  invalid_argument() : error(std::errc::invalid_argument) {}
};

struct no_message_available : public error {
  no_message_available() : error(std::errc::no_message_available) {}
};

// FIXME: error handling
struct operation_not_supported : public error {
  operation_not_supported()
    : error(std::errc::operation_not_supported) {
  }
};

struct permission_denied : public error {
  permission_denied() : error(std::errc::operation_not_permitted) {}
};

struct input_output_error : public error {
  input_output_error() : error(std::errc::io_error) {}
};

} // namespace ceph::osd


namespace ceph {

namespace _impl {
  enum class ct_error {
    enoent,
    invarg,
    enodata
  };
}

// unthrowable_wrapper ensures compilation failure when somebody
// would like to `throw make_error<...>)()` instead of returning.
// returning allows for the compile-time verification of future's
// AllowedErrorsV and also avoid the burden of throwing.
template <_impl::ct_error ErrorV> struct unthrowable_wrapper {
  unthrowable_wrapper(const unthrowable_wrapper&) = delete;
  static constexpr unthrowable_wrapper instance{};
  template <class T> friend const T& make_error();

  // comparison operator for the static_assert in future's ctor.
  template <_impl::ct_error OtherErrorV>
  constexpr bool operator==(const unthrowable_wrapper<OtherErrorV>&) const {
    return OtherErrorV == ErrorV;
  }

private:
  // can be used only for initialize the `instance` member
  explicit unthrowable_wrapper() = default;
};

template <class T> [[nodiscard]] const T& make_error() {
  return T::instance;
}

template <class... WrappedAllowedErrorsT>
struct errorator {
  template <class... ValuesT>
  class future : private seastar::future<ValuesT...> {
    using base_t = seastar::future<ValuesT...>;

    // TODO: let `exception` use other type than `ct_error`.
    template <_impl::ct_error V>
    class exception {
      exception() = default;
    public:
      exception(const exception&) = default;
    };

    template <class ErrorVisitorT, class FuturatorT>
    class maybe_handle_error_t {
      const std::type_info& type_info;
      typename FuturatorT::type result;
      ErrorVisitorT errfunc;

    public:
      maybe_handle_error_t(ErrorVisitorT&& errfunc, std::exception_ptr ep)
        : type_info(*ep.__cxa_exception_type()),
          result(FuturatorT::make_exception_future(std::move(ep))),
          errfunc(std::forward<ErrorVisitorT>(errfunc)) {
      }

      template <_impl::ct_error ErrorV>
      void operator()(const unthrowable_wrapper<ErrorV>& e) {
        // Throwing an exception isn't the sole way to signalize an error
        // with it. This approach nicely fits cold, infrequent issues but
        // when applied to a hot one (like ENOENT on write path), it will
        // likely hurt performance.
        // Alternative approach for hot errors is to create exception_ptr
        // on our own and place it in the future via make_exception_future.
        // When ::handle_exception is called, handler would inspect stored
        // exception whether it's hot-or-cold before rethrowing it.
        // The main advantage is both types flow through very similar path
        // based on future::handle_exception.
        //
        // NOTE: this is extension of language. It should be available both
        // in GCC and Clang but a fallback (basing on throw-catch) could be
        // added as well.
        if (type_info == typeid(exception<ErrorV>)) {
          result.ignore_ready_future();
          result = std::forward<ErrorVisitorT>(errfunc)(e);
        } else {
          //std::cout << "not this ErrorV" << std::endl;
        }
      }

      auto get_result() && {
        return std::move(result);
      }
    };

    using base_t::base_t;

    future(base_t&& base)
      : base_t(std::move(base)) {
    }

  public:
    // initialize seastar::future as failed without any throwing (for
    // details see seastar::make_exception_future()).
    template <_impl::ct_error ErrorV>
    future(const unthrowable_wrapper<ErrorV>& e)
      : base_t(seastar::make_exception_future<ValuesT...>(exception<ErrorV>{})) {
      // this is `fold expression` of C++17
      static_assert((... || (e == WrappedAllowedErrorsT::instance)),
                    "disallowed ct_error");
    }

    template <class ValueFuncT, class ErrorVisitorT>
    auto safe_then(ValueFuncT&& valfunc, ErrorVisitorT&& errfunc) {
      return this->then_wrapped(
        [ valfunc = std::forward<ValueFuncT>(valfunc),
          errfunc = std::forward<ErrorVisitorT>(errfunc)
        ] (auto future) mutable {
          using futurator_t = \
            seastar::futurize<std::result_of_t<ValueFuncT(ValuesT&&...)>>;
          if (__builtin_expect(future.failed(), false)) {
            maybe_handle_error_t<ErrorVisitorT, futurator_t> maybe_handle_error(
              std::forward<ErrorVisitorT>(errfunc),
              std::move(future).get_exception()
            );
            (maybe_handle_error(WrappedAllowedErrorsT::instance) , ...);
            return std::move(maybe_handle_error).get_result();
          } else {
            return futurator_t::apply(std::forward<ValueFuncT>(valfunc),
                                      std::move(future).get());
          }
        });
    }

    // the visitor that ignores any errors
    struct ignore_all {
      template <_impl::ct_error ErrorV>
      void operator()(const unthrowable_wrapper<ErrorV>& e) {
        static_assert((... || (e == WrappedAllowedErrorsT::instance)),
                      "ignoring disallowed ct_error");
        // NOP
      }
    };

    friend errorator<WrappedAllowedErrorsT...>;
  };

  template <class... ValuesT>
  static future<ValuesT...> its_error_free(seastar::future<ValuesT...>&& plain_future) {
    return future<ValuesT...>(std::move(plain_future));
  }
}; // class errorator

#ifdef REUSE_EP
template <class... WrappedAllowedErrorsT>
template <_impl::ct_error V>
std::exception_ptr future<WrappedAllowedErrorsT>::exception<V>::ep = std::make_exception_ptr(exception<V>{});
#endif // REUSE_EP

namespace ct_error {
  using enoent = unthrowable_wrapper<_impl::ct_error::enoent>;
  using enodata = unthrowable_wrapper<_impl::ct_error::enodata>;
  using invarg = unthrowable_wrapper<_impl::ct_error::invarg>;
}

} // namespace ceph
