#ifndef CPPUDDLE_COMMON_WRAPPER_HPP
#define CPPUDDLE_COMMON_WRAPPER_HPP

#include <hpx/futures/future.hpp>
#include <hpx/hpx_init.hpp>
#include <hpx/include/async.hpp>
#include <hpx/include/iostreams.hpp>
#include <hpx/include/lcos.hpp>
#include <hpx/lcos/promise.hpp>
#include <hpx/mutex.hpp>

template <typename Executor, typename F, typename... Ts>
void exec_post_wrapper(Executor & exec, F &&f, Ts &&...ts) {
  hpx::functional::tag_invoke(hpx::parallel::execution::post, exec,
                              std::forward<F>(f), std::forward<Ts>(ts)...);
}

template <typename Executor, typename F, typename... Ts>
hpx::lcos::future<void> exec_async_wrapper(Executor & exec, F &&f, Ts &&...ts) {
  return hpx::functional::tag_invoke(hpx::parallel::execution::async_execute,
                                     exec, std::forward<F>(f),
                                     std::forward<Ts>(ts)...);
}

#endif // CPPUDDLE_COMMON_WRAPPER_HPP
