#pragma once

// Asio helpers shared by the client sessions and replication.

#include <asio.hpp>
#include <asio/experimental/awaitable_operators.hpp>

#include <chrono>
#include <exception>
#include <limits>
#include <memory>
#include <string>

#include "storage/rocks_storage.h"

namespace datadam {

inline constexpr auto kUseAwaitable = asio::as_tuple(asio::use_awaitable);
inline constexpr size_t kReadChunk = 16 * 1024;

// Counts nothing; read_more and flush take a counter of bytes, called with
// what they moved.
struct NoByteCount {
  void operator()(size_t) const {}
};

template <class Stream, class Count = NoByteCount>
asio::awaitable<bool> read_more(Stream& stream, std::string& in, Count count = {}) {
  size_t old = in.size();
  in.resize(old + kReadChunk);
  auto [ec, n] = co_await stream.async_read_some(asio::buffer(in.data() + old, kReadChunk), kUseAwaitable);
  in.resize(old + n);
  count(n);
  co_return !ec;
}

template <class Stream, class Count = NoByteCount>
asio::awaitable<bool> flush(Stream& stream, std::string& out, Count count = {}) {
  if (out.empty()) co_return true;
  auto [ec, n] = co_await asio::async_write(stream, asio::buffer(out), kUseAwaitable);
  count(n);
  out.clear();
  co_return !ec;
}

// Waits, without holding the thread, until a write takes the sequence past
// after_seq or the steady-clock deadline (ms) passes.
inline asio::awaitable<void> wait_for_write(RocksStorage& storage, std::shared_ptr<asio::steady_timer>& timer,
                                            uint64_t after_seq, int64_t deadline_ms) {
  auto ex = co_await asio::this_coro::executor;
  if (!timer) timer = std::make_shared<asio::steady_timer>(ex);
  if (deadline_ms == std::numeric_limits<int64_t>::max()) {
    timer->expires_at(asio::steady_timer::time_point::max());
  } else {
    timer->expires_at(std::chrono::steady_clock::time_point(std::chrono::milliseconds(deadline_ms)));
  }
  // The wake-up is posted to this connection's strand, so it cannot run before the wait starts.
  uint64_t id = storage.on_next_write(after_seq, [t = timer, ex] { asio::post(ex, [t] { t->cancel(); }); });
  if (id == 0) co_return;  // already written
  co_await timer->async_wait(kUseAwaitable);
  storage.cancel_write_wait(id);
}

inline asio::awaitable<void> catching(asio::awaitable<void> a, std::exception_ptr& failed) {
  try {
    co_await std::move(a);
  } catch (...) {
    failed = std::current_exception();
  }
}

// Runs a and b until either ends, ended or failed, and rethrows its failure.
// Not a || b: operator|| waits for the first to end *without* an exception,
// so one that throws leaves the other running, for good if it is waiting on
// a connection that stays quiet.
inline asio::awaitable<void> first_to_end(asio::awaitable<void> a, asio::awaitable<void> b) {
  using namespace asio::experimental::awaitable_operators;
  std::exception_ptr failed_a, failed_b;
  auto first = co_await (catching(std::move(a), failed_a) || catching(std::move(b), failed_b));
  // The other one was cancelled; whatever that threw does not matter.
  if (std::exception_ptr& failed = first.index() == 0 ? failed_a : failed_b) std::rethrow_exception(failed);
}

inline void consume(std::string& in, size_t pos) {
  if (pos >= in.size()) in.clear();
  else if (pos > 0) in.erase(0, pos);
}

}  // namespace datadam
