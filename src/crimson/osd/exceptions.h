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
    enodata,
    input_output_error,
    object_corrupted
  };
}

// unthrowable_wrapper ensures compilation failure when somebody
// would like to `throw make_error<...>)()` instead of returning.
// returning allows for the compile-time verification of future's
// AllowedErrorsV and also avoid the burden of throwing.
template <_impl::ct_error ErrorV>
struct unthrowable_wrapper {
  using wrapped_type = decltype(ErrorV);

  unthrowable_wrapper(const unthrowable_wrapper&) = delete;
  static constexpr unthrowable_wrapper instance{};
  template <class T> friend const T& make_error();

  // comparison operator for the static_assert in future's ctor.
  template <_impl::ct_error OtherErrorV>
  constexpr bool operator==(const unthrowable_wrapper<OtherErrorV>&) const {
    return OtherErrorV == ErrorV;
  }

private:
  // can be used only to initialize the `instance` member
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
          constexpr bool explicitly_discarded = std::is_invocable_r<
            struct ignore_marker_t&&, ErrorVisitorT, decltype(e)>::value;
          if constexpr (!explicitly_discarded) {
            result = std::forward<ErrorVisitorT>(errfunc)(e);
          }
        }
      }

      auto get_result() && {
        return std::move(result);
      }
    };

    template <class, class = std::void_t<>>
    struct is_error {
      static constexpr bool value = false;
    };
    template <class T>
    struct is_error<T, std::void_t<typename T::wrapped_type>> {
      // specialization for _impl::ct_error. it could be written in much
      // simpler form – without void_t and is_same_v.
      static constexpr bool value = \
        std::is_same_v<typename T::wrapped_type, _impl::ct_error>;
    };
    template <class T>
    static inline constexpr bool is_error_v = is_error<T>::value;

    template <class, class = std::void_t<>>
    struct get_errorator {
      // generic template for non-errorated things (plain types and
      // vanilla seastar::future as well).
      using type = errorator<>;
    };
    template <class FutureT>
    struct get_errorator<FutureT,
                         std::void_t<typename FutureT::errorator_type>> {
      using type = typename FutureT::errorator_type;
    };
    template <class T>
    using get_errorator_t = typename get_errorator<T>::type;

    template <class ValueFuncErroratorT, class... ErrorVisitorRetsT>
    struct make_errorator {
      // NOP. The generic template.
    };
    template <class... ValueFuncWrappedAllowedErrorsT,
              class    ErrorVisitorRetsHeadT,
              class... ErrorVisitorRetsTailT>
    struct make_errorator<errorator<ValueFuncWrappedAllowedErrorsT...>,
                          ErrorVisitorRetsHeadT,
                          ErrorVisitorRetsTailT...> {
      using type = std::conditional_t<
        is_error_v<ErrorVisitorRetsHeadT>,
        typename make_errorator<errorator<ValueFuncWrappedAllowedErrorsT...,
                                          ErrorVisitorRetsHeadT>,
                                ErrorVisitorRetsTailT...>::type,
        typename make_errorator<errorator<ValueFuncWrappedAllowedErrorsT...>,
                                ErrorVisitorRetsTailT...>::type>;
    };
    // finish the recursion
    template <class... ValueFuncWrappedAllowedErrorsT>
    struct make_errorator<errorator<ValueFuncWrappedAllowedErrorsT...>> {
      using type = ::ceph::errorator<ValueFuncWrappedAllowedErrorsT...>;
    };
    template <class... Args>
    using make_errorator_t = typename make_errorator<Args...>::type;

    using base_t::base_t;

  public:
    using errorator_type = ceph::errorator<WrappedAllowedErrorsT...>;

    future(base_t&& base)
      : base_t(std::move(base)) {
    }

    template <template <class...> class ErroratedFuture,
              class = std::void_t<
                typename ErroratedFuture<ValuesT...>::errorator_type>>
    operator ErroratedFuture<ValuesT...> () && {
      using DestErrorator = \
        typename ErroratedFuture<ValuesT...>::errorator_type;
      using ThisErrorator = errorator<WrappedAllowedErrorsT...>;

      static_assert(!DestErrorator::template is_less_errorated_v<ThisErrorator>,
                    "conversion is possible to more-or-eq errorated future!");
      return ErroratedFuture<ValuesT...>(std::move(*this).plainifyxxx());
    }

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
      using ValueFuncResult = std::result_of_t<ValueFuncT(ValuesT&&...)>;
      // recognize whether there can be any error coming from the Value
      // Function.
      using ValueFuncErrorator = get_errorator_t<ValueFuncResult>;
      // mutate the Value Function's errorator to harvest errors coming
      // from the Error Visitor. Yes, it's perfectly fine to fail error
      // handling at one step and delegate even broader set of issues
      // to next continuation.
      using ReturnErrorator = make_errorator_t<
        ValueFuncErrorator,
        std::decay_t<std::result_of_t<
          ErrorVisitorT(decltype(WrappedAllowedErrorsT::instance))>>...>;
      // OK, now we know about all errors next continuation must take
      // care about. If Visitor handled everything and the Value Func
      // doesn't return any, we'll finish with errorator<>::future
      // which is just vanilla seastar::future – that's it, next cont
      // finally could use `.then()`!
      using Futurator = \
        typename ReturnErrorator::template futurize<ValueFuncResult>;
      return typename Futurator::type{ this->then_wrapped(
        [ valfunc = std::forward<ValueFuncT>(valfunc),
          errfunc = std::forward<ErrorVisitorT>(errfunc)
        ] (auto future) mutable {
          if (__builtin_expect(future.failed(), false)) {
            maybe_handle_error_t<ErrorVisitorT, Futurator> maybe_handle_error(
              std::forward<ErrorVisitorT>(errfunc),
              std::move(future).get_exception()
            );
            (maybe_handle_error(WrappedAllowedErrorsT::instance) , ...);
            return plainify(std::move(maybe_handle_error).get_result());
          } else {
            return plainify(Futurator::apply(std::forward<ValueFuncT>(valfunc),
                                             std::move(future).get()));
          }
        })};
    }

    template <class Func>
    void then(Func&&) = delete;

    friend errorator<WrappedAllowedErrorsT...>;
    template<class>
    friend auto plainify(future<ValuesT...>&&);

    base_t&& plainifyxxx(void) && {
      return std::move(*this);
    }
  };

  // the visitor that forwards handling of all errors to next continuation
  struct pass_further {
    template <_impl::ct_error ErrorV>
    auto operator()(const unthrowable_wrapper<ErrorV>& e) {
      static_assert((... || (e == WrappedAllowedErrorsT::instance)),
                    "passing further disallowed ct_error");
      return ceph::make_error<std::decay_t<decltype(e)>>();
    }
  };

  struct discard_all {
    template <_impl::ct_error ErrorV>
    auto operator()(const unthrowable_wrapper<ErrorV>& e) {
      static_assert((... || (e == WrappedAllowedErrorsT::instance)),
                    "discarding disallowed ct_error");
      return ignore_marker_t{};
    }
  };

  template <class... ValuesT>
  static future<ValuesT...> its_error_free(seastar::future<ValuesT...>&& plain_future) {
    return future<ValuesT...>(std::move(plain_future));
  }

  // derived errorators
  template <class... NewWrappedAllowedErrorsT>
  using extend = errorator<WrappedAllowedErrorsT...,
                           NewWrappedAllowedErrorsT...>;

  // comparing errorators
  template <class ErrorT>
  struct is_carried {
    static constexpr bool value = \
      ((WrappedAllowedErrorsT::instance == ErrorT::instance) || ...);
  };
  template <class ErrorT>
  static constexpr bool is_carried_v = is_carried<ErrorT>::value;

  template <class>
  struct is_less_errorated {
    // NOP.
  };
  template <class... OtherWrappedAllowedErrorsT>
  struct is_less_errorated<errorator<OtherWrappedAllowedErrorsT...>> {
    static constexpr bool value = \
      ((!is_carried_v<OtherWrappedAllowedErrorsT>) || ...);
  };
  template <class OtherErrorator>
  static constexpr bool is_less_errorated_v = \
    is_less_errorated<OtherErrorator>::value;

private:
  struct ignore_marker_t{};
  template <class... ValueT>
  friend class future;

  template <class... Args>
  static auto plainify(seastar::future<Args...>&& fut) {
    return std::forward<seastar::future<Args...>>(fut);
  }
  template <class Arg>
  static auto plainify(Arg&& arg) {
    return std::forward<Arg>(arg).plainifyxxx();
  }

  template <class...>
  struct tuple2future {
  };
  template <class... Args>
  struct tuple2future <std::tuple<Args...>> {
    using type = future<Args...>;
  };

  template <class T, class = std::void_t<T>>
  class futurize {
    using vanilla_futurize = seastar::futurize<T>;
  public:
    using type = typename tuple2future<typename vanilla_futurize::value_type>::type;
    //using type = typename tuple2future<typename vanilla_futurize::typename type::value_type>::type;

    template <class Func, class... Args>
    static type apply(Func&& func, std::tuple<Args...>&& args) {
      return vanilla_futurize::apply(std::forward<Func>(func),
                                     std::forward<std::tuple<Args...>>(args));
    }

    template <typename Arg>
    static type make_exception_future(Arg&& arg) {
      return vanilla_futurize::make_exception_future(std::forward<Arg>(arg));
    }
  };
  template <template <class...> class ErroratedFutureT,
            class... ValuesT>
  class futurize<ErroratedFutureT<ValuesT...>,
                 std::void_t<typename ErroratedFutureT<ValuesT...>::errorator_type>> {
  public:
    using type = ::ceph::errorator<WrappedAllowedErrorsT...>::future<ValuesT...>;

    template <class Func, class... Args>
    static type apply(Func&& func, std::tuple<Args...>&& args) {
      try {
        return ::seastar::apply(std::forward<Func>(func), std::move(args));
      } catch (...) {
        return make_exception_future(std::current_exception());
      }
    }

    template <typename Arg>
    static type make_exception_future(Arg&& arg) {
      return ::seastar::make_exception_future<ValuesT...>(std::forward<Arg>(arg));
    }
  };
}; // class errorator, generic template

// no errors? errorator<>::future is plain seastar::future then!
template <>
class errorator<> {
public:
  template <class... ValuesT>
  using future = ::seastar::future<ValuesT...>;

  template <class T>
  using futurize = ::seastar::futurize<T>;
}; // class errorator, <> specialization

#ifdef REUSE_EP
template <class... WrappedAllowedErrorsT>
template <_impl::ct_error V>
std::exception_ptr future<WrappedAllowedErrorsT>::exception<V>::ep = std::make_exception_ptr(exception<V>{});
#endif // REUSE_EP

namespace ct_error {
  using enoent = unthrowable_wrapper<_impl::ct_error::enoent>;
  using enodata = unthrowable_wrapper<_impl::ct_error::enodata>;
  using invarg = unthrowable_wrapper<_impl::ct_error::invarg>;
  using input_output_error = unthrowable_wrapper<_impl::ct_error::input_output_error>;
  using object_corrupted = unthrowable_wrapper<_impl::ct_error::object_corrupted>;
}

} // namespace ceph
