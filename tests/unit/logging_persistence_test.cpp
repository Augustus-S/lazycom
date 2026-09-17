#include <lazycom/config/persistence.hpp>
#include <lazycom/logging/session_writer.hpp>

#include <catch2/catch_test_macros.hpp>
#include <support/fake_atomic_file_system.hpp>
#include <support/logging_builders.hpp>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;
using namespace lazycom;
using namespace lazycom::config;
using namespace lazycom::logging;

struct FakeLogState {
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<std::string> records;
  bool fail_flush{}, throw_append{}, block_close{}, close_entered{},
      release_close{};
  std::size_t flushes{}, closes{};
};
class FakeLogFileSystem final : public SessionLogFileSystem {
public:
  explicit FakeLogFileSystem(std::shared_ptr<FakeLogState> state)
      : state_{std::move(state)} {}
  Status begin_session(const std::filesystem::path &, std::string_view,
                       std::string_view, const SessionLogQuotas &) override {
    active_ = true;
    return {};
  }
  Status append_line(std::string_view record) override {
    std::scoped_lock lock{state_->mutex};
    if (state_->throw_append)
      throw std::runtime_error{"append exception"};
    state_->records.emplace_back(record);
    return {};
  }
  Status flush() override {
    std::scoped_lock lock{state_->mutex};
    ++state_->flushes;
    if (state_->fail_flush)
      return tl::unexpected(make_error(ErrorCode::LoggingDiskFull,
                                       Operation::WriteSessionLog, "flush"));
    return {};
  }
  Status close() noexcept override {
    std::unique_lock lock{state_->mutex};
    if (active_) {
      state_->close_entered = true;
      state_->condition.notify_all();
      state_->condition.wait(lock, [this] {
        return !state_->block_close || state_->release_close;
      });
      ++state_->closes;
    }
    active_ = false;
    return {};
  }
  std::filesystem::path active_path() const override {
    return active_ ? std::filesystem::path{"/fake/log.ndjson"}
                   : std::filesystem::path{};
  }

private:
  std::shared_ptr<FakeLogState> state_;
  bool active_{};
};
SessionWriterOptions writer_options() {
  SessionWriterOptions options;
  options.directory = "/fake";
  options.flush_batch_bytes = 1024U * 1024U;
  options.flush_interval = 1h;
  return options;
}
Record record(std::uint64_t seq, std::string_view payload) {
  return test::log_record(seq, Direction::Rx, test::bytes(payload));
}
template <typename T> T ready(std::future<T> future) {
  REQUIRE(future.wait_for(1s) == std::future_status::ready);
  return future.get();
}
struct LogFixture {
  explicit LogFixture(SessionWriterOptions options = writer_options())
      : writer{options, std::make_unique<FakeLogFileSystem>(fake)} {}
  ~LogFixture() {
    release_close();
    writer.request_stop();
    if (!writer.wait_until_stopped(std::chrono::steady_clock::now() + 2s))
      std::terminate();
  }
  void start() {
    REQUIRE(writer.enable());
    REQUIRE(ready(writer.start_session(test::log_header())).state ==
            SessionLogState::Recording);
  }
  void release_close() {
    std::scoped_lock lock{fake->mutex};
    fake->release_close = true;
    fake->condition.notify_all();
  }
  std::shared_ptr<FakeLogState> fake{std::make_shared<FakeLogState>()};
  SessionWriter writer;
};

struct BlockingAtomicState {
  std::mutex mutex;
  std::condition_variable condition;
  bool entered{}, released{};
};
class BlockingAtomicTransaction final : public AtomicWriteTransaction {
public:
  explicit BlockingAtomicTransaction(BlockingAtomicState &state)
      : state_{state} {}
  Status stage(std::string_view) override { return {}; }
  Result<SafeFileIdentity> commit() override {
    std::unique_lock lock{state_.mutex};
    state_.entered = true;
    state_.condition.notify_all();
    state_.condition.wait(lock, [this] { return state_.released; });
    SafeFileIdentity identity;
    identity.exists = true;
    return identity;
  }
  Status sync_parent_directory() override { return {}; }

private:
  BlockingAtomicState &state_;
};
class BlockingAtomicFileSystem final : public AtomicFileSystem {
public:
  Result<std::unique_ptr<AtomicWriteTransaction>>
  begin_atomic_write(const std::filesystem::path &, std::size_t,
                     const SafeFileIdentity &) override {
    return std::make_unique<BlockingAtomicTransaction>(state);
  }
  BlockingAtomicState state;
};
const PersistencePaths paths{"/unused/config.toml", "/unused/quick_send.toml",
                             "/unused/state.toml"};
struct PersistenceFixture {
  ~PersistenceFixture() {
    release();
    worker.request_stop();
    if (!worker.wait_until_stopped(std::chrono::steady_clock::now() + 2s))
      std::terminate();
  }
  void release() {
    std::scoped_lock lock{file_system.state.mutex};
    file_system.state.released = true;
    file_system.state.condition.notify_all();
  }
  BlockingAtomicFileSystem file_system;
  PersistenceWorker worker{paths, file_system};
};
} // namespace

TEST_CASE("session writer orders records and completes a flushed barrier",
          "[logging][worker]") {
  LogFixture test;
  test.start();
  REQUIRE(test.writer.try_enqueue({record(1U, "one"), record(2U, "two")}) ==
          EnqueueResult::Accepted);
  const auto barrier = ready(test.writer.barrier(2U));
  CHECK(barrier.state == BarrierState::Confirmed);
  CHECK(barrier.processed_through_seq == 2U);
  CHECK_FALSE(barrier.fsync_guaranteed);
  std::scoped_lock lock{test.fake->mutex};
  REQUIRE(test.fake->records.size() == 2U);
  CHECK(decode_record_line(test.fake->records[0])->seq == 1U);
  CHECK(decode_record_line(test.fake->records[1])->seq == 2U);
  CHECK(test.fake->flushes == 1U);
}

TEST_CASE("session queue overload is nonblocking and terminal",
          "[logging][worker]") {
  auto options = writer_options();
  options.queue_max_bytes = 1U;
  LogFixture test{options};
  test.start();
  CHECK(test.writer.try_enqueue({record(1U, "too large")}) ==
        EnqueueResult::QueueFull);
  CHECK(test.writer.state() == SessionLogState::Error);
  CHECK(ready(test.writer.barrier(1U)).state == BarrierState::WriterFailed);
}

TEST_CASE("session IO failures and exceptions complete queued futures",
          "[logging][worker]") {
  LogFixture test;
  SECTION("flush returns an error") { test.fake->fail_flush = true; }
  SECTION("append throws at the worker boundary") {
    test.fake->throw_append = true;
  }
  test.start();
  REQUIRE(test.writer.try_enqueue({record(1U, "one")}) ==
          EnqueueResult::Accepted);
  CHECK(ready(test.writer.barrier(1U)).state == BarrierState::WriterFailed);
  CHECK(test.writer.state() == SessionLogState::Error);
  CHECK(ready(test.writer.shutdown()).state == SessionLogState::Error);
}

TEST_CASE("disable shares one close while enforcing its waiter capacity",
          "[logging][worker]") {
  LogFixture test;
  test.start();
  {
    std::scoped_lock lock{test.fake->mutex};
    test.fake->block_close = true;
  }
  std::vector<std::future<SessionCommandResult>> waiters;
  waiters.reserve(256U);
  waiters.push_back(test.writer.disable());
  {
    std::unique_lock lock{test.fake->mutex};
    REQUIRE(test.fake->condition.wait_for(
        lock, 1s, [&] { return test.fake->close_entered; }));
  }
  CHECK(test.writer.try_enqueue({record(1U, "after disable")}) ==
        EnqueueResult::NotRecording);
  for (std::size_t i = 1U; i < 256U; ++i)
    waiters.push_back(test.writer.disable());
  const auto overflow = ready(test.writer.disable());
  CHECK(overflow.state == SessionLogState::Off);
  REQUIRE(overflow.error);
  CHECK(overflow.error->detail == "session log control capacity is full");
  test.release_close();
  for (auto &waiter : waiters) {
    const auto result = ready(std::move(waiter));
    CHECK(result.state == SessionLogState::Off);
    CHECK_FALSE(result.error);
  }
  {
    std::scoped_lock lock{test.fake->mutex};
    CHECK(test.fake->closes == 1U);
  }
  CHECK_FALSE(test.writer.wait_until_stopped(std::chrono::steady_clock::now()));
  ready(test.writer.shutdown());
  REQUIRE(
      test.writer.wait_until_stopped(std::chrono::steady_clock::now() + 1s));
}

TEST_CASE("barrier and control hard limits never strand futures",
          "[logging][worker]") {
  LogFixture test;
  test.start();
  std::vector<std::future<BarrierResult>> barriers;
  for (std::size_t i = 0U; i < 400U; ++i)
    barriers.push_back(test.writer.barrier(1U));
  auto state = SessionLogState::Recording;
  for (std::size_t attempt = 0U;
       attempt < 100U && state != SessionLogState::Off; ++attempt) {
    state = ready(test.writer.disable()).state;
    std::this_thread::yield();
  }
  REQUIRE(state == SessionLogState::Off);
  for (auto &barrier : barriers)
    CHECK(ready(std::move(barrier)).state == BarrierState::WriterFailed);
}

TEST_CASE(
    "persistence rejects read-only saves and preserves committed identity",
    "[config][persistence]") {
  test::FakeAtomicFileSystem file_system{
      test::AtomicFailurePoint::DirectorySync};
  PersistenceWorker worker{paths, file_system};
  const auto rejected = worker.save_config(ConfigSnapshot{}, {}, {}, true);
  CHECK(rejected.state == SaveSubmitState::ReadOnly);
  REQUIRE(rejected.error);
  auto submitted = worker.save_state(StateSnapshot{}, {});
  REQUIRE(submitted.accepted());
  const auto completion = ready(std::move(submitted.completion));
  CHECK(completion.file == PersistenceFile::State);
  CHECK(completion.outcome.state == CommitState::CommittedDurabilityUnknown);
  REQUIRE(completion.outcome.committed_identity);
  CHECK(completion.outcome.committed_identity->exists);
  CHECK(completion.outcome.committed_identity->size ==
        completion.serialized_document.size());
  CHECK(completion.serialized_document.find("version = 1") !=
        std::string::npos);
}

TEST_CASE(
    "persistence rejects overlapping saves and drains accepted files on stop",
    "[config][persistence]") {
  PersistenceFixture test;
  auto first = test.worker.save_config(ConfigSnapshot{}, {});
  REQUIRE(first.accepted());
  {
    auto &state = test.file_system.state;
    std::unique_lock lock{state.mutex};
    REQUIRE(state.condition.wait_for(lock, 1s, [&] { return state.entered; }));
  }
  CHECK(test.worker.save_config(ConfigSnapshot{}, {}).state ==
        SaveSubmitState::Busy);
  auto second = test.worker.save_state(StateSnapshot{}, {});
  REQUIRE(second.accepted());
  test.worker.request_stop();
  CHECK_FALSE(
      test.worker.wait_until_stopped(std::chrono::steady_clock::now() + 20ms));
  CHECK(test.worker.save_state(StateSnapshot{}, {}).state ==
        SaveSubmitState::Stopping);
  test.release();
  REQUIRE(
      test.worker.wait_until_stopped(std::chrono::steady_clock::now() + 1s));
  CHECK(ready(std::move(first.completion)).outcome.state ==
        CommitState::Committed);
  CHECK(ready(std::move(second.completion)).outcome.state ==
        CommitState::Committed);
  test.worker.shutdown();
}
