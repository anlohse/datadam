#pragma once

// Asio helpers shared by the client sessions and replication.

#include <asio.hpp>

#include <chrono>
#include <limits>
#include <memory>
#include <string>

#include "storage/rocks_storage.h"

namespace datadam {

inline constexpr auto kUseAwaitable = asio::as_tuple(asio::use_awaitable);
inline constexpr size_t kReadChunk = 16 * 1024;

template <class Stream>
asio::awaitable<bool> read_more(Stream& stream, std::string& in) {
  size_t old = in.size();
  in.resize(old + kReadChunk);
  auto [ec, n] = co_await stream.async_read_some(asio::buffer(in.data() + old, kReadChunk), kUseAwaitable);
  in.resize(old + n);
  co_return !ec;
}

template <class Stream>
asio::awaitable<bool> flush(Stream& stream, std::string& out) {
  if (out.empty()) co_return true;
  auto [ec, n] = co_await asio::async_write(stream, asio::buffer(out), kUseAwaitable);
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

inline void consume(std::string& in, size_t pos) {
  if (pos >= in.size()) in.clear();
  else if (pos > 0) in.erase(0, pos);
}

}  // namespace datadam
