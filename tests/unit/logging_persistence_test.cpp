#include <lazycom/config/persistence.hpp>
#include <lazycom/logging/session_writer.hpp>

#include <support/fake_atomic_file_system.hpp>
#include <support/logging_builders.hpp>

#include <catch2/catch_test_macros.hpp>

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

struct FakeLogState {
  std::mutex mutex;
  std::condition_variable condition;
  std::string header;
  std::vector<std::string> records;
  bool fail_begin{};
  bool fail_append{};
  bool fail_flush{};
  bool throw_append{};
  bool block_close{};
  bool close_entered{};
  bool release_close{};
  std::size_t flushes{};
  std::size_t closes{};
};

class FakeLogFileSystem final : public lazycom::logging::SessionLogFileSystem {
public:
  explicit FakeLogFileSystem(std::shared_ptr<FakeLogState> state)
      : state_{std::move(state)} {}

  lazycom::Status
  begin_session(const std::filesystem::path &, std::string_view,
                std::string_view encoded_header,
                const lazycom::logging::SessionLogQuotas &) override {
    std::scoped_lock lock{state_->mutex};
    if (state_->fail_begin) {
      return failure("begin");
    }
    state_->header = encoded_header;
    active_ = true;
    return {};
  }

  lazycom::Status append_line(std::string_view encoded_record) override {
    std::scoped_lock lock{state_->mutex};
    if (state_->throw_append) {
      throw std::runtime_error{"append exception"};
    }
    if (state_->fail_append) {
      return failure("append");
    }
    state_->records.emplace_back(encoded_record);
    return {};
  }

  lazycom::Status flush() override {
    std::scoped_lock lock{state_->mutex};
    ++state_->flushes;
    return state_->fail_flush ? failure("flush") : lazycom::Status{};
  }

  lazycom::Status close() noexcept override {
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

  [[nodiscard]] std::filesystem::path active_path() const override {
    return active_ ? std::filesystem::path{"/fake/log.ndjson"}
                   : std::filesystem::path{};
  }

private:
  [[nodiscard]] static lazycom::Status failure(std::string_view detail) {
    return tl::make_unexpected(
        lazycom::make_error(lazycom::ErrorCode::LoggingDiskFull,
                            lazycom::Operation::WriteSessionLog, detail));
  }

  std::shared_ptr<FakeLogState> state_;
  bool active_{};
};

struct BlockingAtomicState {
  std::mutex mutex;
  std::condition_variable condition;
  bool entered_commit{};
  bool release{};
};

class BlockingAtomicTransaction final
    : public lazycom::config::AtomicWriteTransaction {
public:
  explicit BlockingAtomicTransaction(std::shared_ptr<BlockingAtomicState> state)
      : state_{std::move(state)} {}

  lazycom::Status stage(std::string_view) override { return {}; }
  lazycom::Result<lazycom::config::SafeFileIdentity> commit() override {
    std::unique_lock lock{state_->mutex};
    state_->entered_commit = true;
    state_->condition.notify_all();
    state_->condition.wait(lock, [this] { return state_->release; });
    lazycom::config::SafeFileIdentity identity;
    identity.exists = true;
    return identity;
  }
  lazycom::Status sync_parent_directory() override { return {}; }

private:
  std::shared_ptr<BlockingAtomicState> state_;
};

class BlockingAtomicFileSystem final
    : public lazycom::config::AtomicFileSystem {
public:
  explicit BlockingAtomicFileSystem(std::shared_ptr<BlockingAtomicState> state)
      : state_{std::move(state)} {}

  lazycom::Result<std::unique_ptr<lazycom::config::AtomicWriteTransaction>>
  begin_atomic_write(const std::filesystem::path &, std::size_t,
                     const lazycom::config::SafeFileIdentity &) override {
    return std::make_unique<BlockingAtomicTransaction>(state_);
  }

private:
  std::shared_ptr<BlockingAtomicState> state_;
};

[[nodiscard]] lazycom::logging::SessionWriterOptions writer_options() {
  lazycom::logging::SessionWriterOptions options;
  options.directory = "/fake";
  options.flush_batch_bytes = 1024U * 1024U;
  options.flush_interval = std::chrono::hours{1};
  return options;
}

[[nodiscard]] lazycom::logging::Record record(std::uint64_t seq,
                                              std::string_view payload) {
  return lazycom::test::LogRecordBuilder{seq, lazycom::logging::Direction::Rx,
                                         lazycom::test::bytes(payload)}
      .build();
}

} // namespace

TEST_CASE("session writer orders records and completes a flushed barrier",
          "[logging][worker]") {
  auto fake = std::make_shared<FakeLogState>();
  lazycom::logging::SessionWriter writer{
      writer_options(), std::make_unique<FakeLogFileSystem>(fake)};

  REQUIRE(writer.enable());
  REQUIRE(writer.start_session(lazycom::test::LogHeaderBuilder{}.build())
              .get()
              .state == lazycom::logging::SessionLogState::Recording);
  REQUIRE(writer.try_enqueue({record(1U, "one"), record(2U, "two")}) ==
          lazycom::logging::EnqueueResult::Accepted);

  const auto barrier = writer.barrier(2U).get();
  REQUIRE(barrier.state == lazycom::logging::BarrierState::Confirmed);
  REQUIRE(barrier.processed_through_seq == 2U);
  REQUIRE_FALSE(barrier.fsync_guaranteed);

  std::scoped_lock lock{fake->mutex};
  REQUIRE(fake->records.size() == 2U);
  REQUIRE(lazycom::logging::decode_record_line(fake->records[0])->seq == 1U);
  REQUIRE(lazycom::logging::decode_record_line(fake->records[1])->seq == 2U);
  REQUIRE(fake->flushes == 1U);
}

TEST_CASE("session queue overload is nonblocking and terminal",
          "[logging][worker]") {
  auto fake = std::make_shared<FakeLogState>();
  auto options = writer_options();
  options.queue_max_bytes = 1U;
  lazycom::logging::SessionWriter writer{
      options, std::make_unique<FakeLogFileSystem>(fake)};
  REQUIRE(writer.enable());
  REQUIRE(writer.start_session(lazycom::test::LogHeaderBuilder{}.build())
              .get()
              .state == lazycom::logging::SessionLogState::Recording);

  REQUIRE(writer.try_enqueue({record(1U, "too large")}) ==
          lazycom::logging::EnqueueResult::QueueFull);
  REQUIRE(writer.state() == lazycom::logging::SessionLogState::Error);
  REQUIRE(writer.barrier(1U).get().state ==
          lazycom::logging::BarrierState::WriterFailed);
}

TEST_CASE("session flush failures complete barriers as writer failures",
          "[logging][worker]") {
  auto fake = std::make_shared<FakeLogState>();
  fake->fail_flush = true;
  lazycom::logging::SessionWriter writer{
      writer_options(), std::make_unique<FakeLogFileSystem>(fake)};
  REQUIRE(writer.enable());
  REQUIRE(writer.start_session(lazycom::test::LogHeaderBuilder{}.build())
              .get()
              .state == lazycom::logging::SessionLogState::Recording);
  REQUIRE(writer.try_enqueue({record(1U, "one")}) ==
          lazycom::logging::EnqueueResult::Accepted);
  REQUIRE(writer.barrier(1U).get().state ==
          lazycom::logging::BarrierState::WriterFailed);
  REQUIRE(writer.state() == lazycom::logging::SessionLogState::Error);
}

TEST_CASE("session worker exception boundary completes queued futures",
          "[logging][worker]") {
  auto fake = std::make_shared<FakeLogState>();
  fake->throw_append = true;
  lazycom::logging::SessionWriter writer{
      writer_options(), std::make_unique<FakeLogFileSystem>(fake)};
  REQUIRE(writer.enable());
  REQUIRE(writer.start_session(lazycom::test::LogHeaderBuilder{}.build())
              .get()
              .state == lazycom::logging::SessionLogState::Recording);
  REQUIRE(writer.try_enqueue({record(1U, "throws")}) ==
          lazycom::logging::EnqueueResult::Accepted);

  auto barrier = writer.barrier(1U);
  REQUIRE(barrier.wait_for(std::chrono::seconds{1}) ==
          std::future_status::ready);
  REQUIRE(barrier.get().state == lazycom::logging::BarrierState::WriterFailed);
  auto shutdown = writer.shutdown();
  REQUIRE(shutdown.wait_for(std::chrono::seconds{1}) ==
          std::future_status::ready);
  REQUIRE(shutdown.get().state == lazycom::logging::SessionLogState::Error);
}

TEST_CASE("repeated disable shares one close and completes every caller",
          "[logging][worker]") {
  auto fake = std::make_shared<FakeLogState>();
  lazycom::logging::SessionWriter writer{
      writer_options(), std::make_unique<FakeLogFileSystem>(fake)};
  REQUIRE(writer.enable());
  REQUIRE(writer.start_session(lazycom::test::LogHeaderBuilder{}.build())
              .get()
              .state == lazycom::logging::SessionLogState::Recording);

  auto first = writer.disable();
  auto second = writer.disable();
  REQUIRE(first.get().state == lazycom::logging::SessionLogState::Off);
  REQUIRE(second.get().state == lazycom::logging::SessionLogState::Off);
  std::scoped_lock lock{fake->mutex};
  REQUIRE(fake->closes == 1U);
}

TEST_CASE("disable capacity reports an error and closes the producer window",
          "[logging][worker]") {
  auto fake = std::make_shared<FakeLogState>();
  lazycom::logging::SessionWriter writer{
      writer_options(), std::make_unique<FakeLogFileSystem>(fake)};
  REQUIRE(writer.enable());
  REQUIRE(writer.start_session(lazycom::test::LogHeaderBuilder{}.build())
              .get()
              .state == lazycom::logging::SessionLogState::Recording);
  {
    std::scoped_lock lock{fake->mutex};
    fake->block_close = true;
  }

  std::vector<std::future<lazycom::logging::SessionCommandResult>> waiters;
  waiters.reserve(256U);
  waiters.push_back(writer.disable());
  {
    std::unique_lock lock{fake->mutex};
    REQUIRE(fake->condition.wait_for(lock, std::chrono::seconds{1},
                                     [&fake] { return fake->close_entered; }));
  }
  REQUIRE(writer.try_enqueue({record(1U, "after disable")}) ==
          lazycom::logging::EnqueueResult::NotRecording);
  for (std::size_t index = 1U; index < 256U; ++index) {
    waiters.push_back(writer.disable());
  }
  auto overflow = writer.disable();
  REQUIRE(overflow.wait_for(std::chrono::seconds{1}) ==
          std::future_status::ready);
  const auto overflow_result = overflow.get();
  REQUIRE(overflow_result.state == lazycom::logging::SessionLogState::Off);
  REQUIRE(overflow_result.error);
  CHECK(overflow_result.error->detail ==
        "session log control capacity is full");

  {
    std::scoped_lock lock{fake->mutex};
    fake->release_close = true;
  }
  fake->condition.notify_all();
  for (auto &waiter : waiters) {
    REQUIRE(waiter.wait_for(std::chrono::seconds{1}) ==
            std::future_status::ready);
    REQUIRE_FALSE(waiter.get().error);
  }
  CHECK_FALSE(writer.wait_until_stopped(std::chrono::steady_clock::now()));
  auto stopped = writer.shutdown();
  REQUIRE(stopped.wait_for(std::chrono::seconds{1}) ==
          std::future_status::ready);
  REQUIRE(writer.wait_until_stopped(std::chrono::steady_clock::now() +
                                    std::chrono::seconds{1}));
}

TEST_CASE("barrier and control hard limits never strand futures",
          "[logging][worker]") {
  auto fake = std::make_shared<FakeLogState>();
  lazycom::logging::SessionWriter writer{
      writer_options(), std::make_unique<FakeLogFileSystem>(fake)};
  REQUIRE(writer.enable());
  REQUIRE(writer.start_session(lazycom::test::LogHeaderBuilder{}.build())
              .get()
              .state == lazycom::logging::SessionLogState::Recording);

  std::vector<std::future<lazycom::logging::BarrierResult>> barriers;
  barriers.reserve(400U);
  for (std::size_t index = 0U; index < 400U; ++index) {
    barriers.push_back(writer.barrier(1U));
  }

  auto disabled_state = lazycom::logging::SessionLogState::Recording;
  for (std::size_t attempt = 0U; attempt < 100U; ++attempt) {
    auto disabled = writer.disable();
    REQUIRE(disabled.wait_for(std::chrono::seconds{1}) ==
            std::future_status::ready);
    disabled_state = disabled.get().state;
    if (disabled_state == lazycom::logging::SessionLogState::Off) {
      break;
    }
    std::this_thread::yield();
  }
  REQUIRE(disabled_state == lazycom::logging::SessionLogState::Off);
  for (auto &barrier : barriers) {
    REQUIRE(barrier.wait_for(std::chrono::seconds{1}) ==
            std::future_status::ready);
    REQUIRE(barrier.get().state ==
            lazycom::logging::BarrierState::WriterFailed);
  }
}

TEST_CASE("persistence worker preserves atomic commit tri-state",
          "[config][persistence]") {
  const lazycom::config::PersistencePaths paths{
      "/unused/config.toml", "/unused/quick_send.toml", "/unused/state.toml"};
  lazycom::test::FakeAtomicFileSystem file_system{
      lazycom::test::AtomicFailurePoint::DirectorySync};
  lazycom::config::PersistenceWorker worker{paths, file_system};

  auto submitted = worker.save_state(lazycom::config::StateSnapshot{}, {});
  REQUIRE(submitted.accepted());
  const auto completion = submitted.completion.get();
  REQUIRE(completion.file == lazycom::config::PersistenceFile::State);
  REQUIRE(completion.outcome.state ==
          lazycom::config::CommitState::CommittedDurabilityUnknown);
  REQUIRE(completion.outcome.committed_identity);
  CHECK(completion.outcome.committed_identity->exists);
  CHECK(completion.outcome.committed_identity->size ==
        completion.serialized_document.size());
  REQUIRE(completion.serialized_document.find("version = 1") !=
          std::string::npos);
}

TEST_CASE("persistence rejects read-only snapshots before filesystem access",
          "[config][persistence]") {
  const lazycom::config::PersistencePaths paths{
      "/unused/config.toml", "/unused/quick_send.toml", "/unused/state.toml"};
  lazycom::test::FakeAtomicFileSystem file_system{
      lazycom::test::AtomicFailurePoint::None};
  lazycom::config::PersistenceWorker worker{paths, file_system};

  const auto submitted =
      worker.save_config(lazycom::config::ConfigSnapshot{}, {}, {}, true);
  REQUIRE(submitted.state == lazycom::config::SaveSubmitState::ReadOnly);
  REQUIRE(submitted.error);
}

TEST_CASE("persistence serializes three files and rejects same-file overlap",
          "[config][persistence]") {
  const lazycom::config::PersistencePaths paths{
      "/unused/config.toml", "/unused/quick_send.toml", "/unused/state.toml"};
  auto state = std::make_shared<BlockingAtomicState>();
  BlockingAtomicFileSystem file_system{state};
  lazycom::config::PersistenceWorker worker{paths, file_system};

  auto first = worker.save_config(lazycom::config::ConfigSnapshot{}, {});
  REQUIRE(first.accepted());
  {
    std::unique_lock lock{state->mutex};
    state->condition.wait(lock, [&state] { return state->entered_commit; });
  }
  const auto duplicate =
      worker.save_config(lazycom::config::ConfigSnapshot{}, {});
  REQUIRE(duplicate.state == lazycom::config::SaveSubmitState::Busy);
  auto other_file = worker.save_state(lazycom::config::StateSnapshot{}, {});
  REQUIRE(other_file.accepted());

  {
    std::scoped_lock lock{state->mutex};
    state->release = true;
  }
  state->condition.notify_all();
  REQUIRE(first.completion.get().outcome.state ==
          lazycom::config::CommitState::Committed);
  REQUIRE(other_file.completion.get().outcome.state ==
          lazycom::config::CommitState::Committed);
}

TEST_CASE("persistence stop wait is bounded until accepted saves drain",
          "[config][persistence]") {
  const lazycom::config::PersistencePaths paths{
      "/unused/config.toml", "/unused/quick_send.toml", "/unused/state.toml"};
  auto state = std::make_shared<BlockingAtomicState>();
  BlockingAtomicFileSystem file_system{state};
  lazycom::config::PersistenceWorker worker{paths, file_system};
  auto save = worker.save_state(lazycom::config::StateSnapshot{}, {});
  REQUIRE(save.accepted());
  {
    std::unique_lock lock{state->mutex};
    REQUIRE(state->condition.wait_for(lock, std::chrono::seconds{1}, [&state] {
      return state->entered_commit;
    }));
  }

  worker.request_stop();
  CHECK_FALSE(worker.wait_until_stopped(std::chrono::steady_clock::now() +
                                        std::chrono::milliseconds{20}));
  CHECK(worker.save_state(lazycom::config::StateSnapshot{}, {}).state ==
        lazycom::config::SaveSubmitState::Stopping);
  {
    std::scoped_lock lock{state->mutex};
    state->release = true;
  }
  state->condition.notify_all();
  REQUIRE(worker.wait_until_stopped(std::chrono::steady_clock::now() +
                                    std::chrono::seconds{1}));
  REQUIRE(save.completion.get().outcome.state ==
          lazycom::config::CommitState::Committed);
  worker.shutdown();
}
