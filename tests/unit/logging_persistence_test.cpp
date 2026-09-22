#include <lazycom/app/session_records.hpp>
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
      release_close{}, block_append{}, append_entered{}, release_append{};
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
    std::unique_lock lock{state_->mutex};
    state_->append_entered = true;
    state_->condition.notify_all();
    state_->condition.wait(lock, [this] {
      return !state_->block_append || state_->release_append;
    });
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
  explicit LogFixture(
      SessionWriterOptions options = writer_options(),
      model::GlobalMemoryBudget budget = model::GlobalMemoryBudget{})
      : writer{options, std::make_unique<FakeLogFileSystem>(fake),
               std::move(budget)} {}
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
    fake->release_append = true;
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
  for (auto value : {record(1U, "one"), record(2U, "two")}) {
    REQUIRE(test.writer.try_enqueue(lazycom::test::session_record(
                std::move(value))) == EnqueueResult::Accepted);
  }
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

TEST_CASE(
    "production records share storage through UI eviction and log completion",
    "[application][logging][budget]") {
  model::GlobalMemoryBudget budget;
  app::SessionRecords records{budget};
  config::ReceiveSettings limits;
  limits.visible_max_records = 2U;
  const auto steady = std::chrono::steady_clock::now();
  const auto utc = std::chrono::system_clock::now();
  records.start(limits, steady, utc);
  const auto payload = test::bytes("payload");
  const auto append = [&](SessionEventOrigin origin =
                              SessionEventOrigin::Normal) {
    return records.append(limits, origin, app::RecordDirection::Rx, payload, {},
                          {}, {}, steady, utc, {});
  };
  auto first = append();
  REQUIRE(first.status == model::SequenceStatus::Accepted);
  REQUIRE(records.visible().front().owner == first.record);
  CHECK(records.visible().front().payload.data() ==
        first.record->payload().data());
  std::weak_ptr<const model::SessionRecord> retained = first.record;
  LogFixture log{writer_options(), budget};
  log.start();
  {
    std::scoped_lock lock{log.fake->mutex};
    log.fake->block_append = true;
  }
  REQUIRE(log.writer.try_enqueue(first.record) == EnqueueResult::Accepted);
  {
    std::unique_lock lock{log.fake->mutex};
    REQUIRE(log.fake->condition.wait_for(
        lock, 1s, [&] { return log.fake->append_entered; }));
  }
  first = {};
  records.clear();
  CHECK_FALSE(retained.expired());
  CHECK(budget.used(model::BudgetCategory::UiRecords) > 0U);
  log.release_close();
  REQUIRE(ready(log.writer.barrier(1U)).state == BarrierState::Confirmed);
  CHECK(retained.expired());
  CHECK(budget.total_used() == 0U);
  for (int index = 0; index < 3; ++index) {
    REQUIRE(append().status == model::SequenceStatus::Accepted);
  }
  REQUIRE(records.visible().size() == 2U);
  CHECK(records.visible().front().record_id == 3U);
  CHECK(records.gap_records() == 1U);
  records.finish();
  CHECK(append().status == model::SequenceStatus::Closed);
  records.start(limits, steady, utc);
  REQUIRE(append().status == model::SequenceStatus::Accepted);
  CHECK(records.visible().back().record_id == 5U);
  CHECK(records.visible().back().sequence == 1U);
  REQUIRE(append(SessionEventOrigin::Cleanup).status ==
          model::SequenceStatus::Accepted);
  CHECK(append().status == model::SequenceStatus::InvalidOrigin);
  records.finish();
  records.clear();
  CHECK(budget.total_used() == 0U);

  auto small_limits = model::BudgetLimits::defaults();
  small_limits
      .category[static_cast<std::size_t>(model::BudgetCategory::UiRecords)] =
      8U * 1024U;
  model::GlobalMemoryBudget small{small_limits};
  app::SessionRecords pressured{small};
  pressured.start(limits, steady, utc);
  const std::vector<std::byte> large_payload(4096U, std::byte{'x'});
  const auto large = [&] {
    return pressured.append(limits, SessionEventOrigin::Normal,
                            app::RecordDirection::Rx, large_payload, {}, {}, {},
                            steady, utc, {});
  };
  REQUIRE(large().status == model::SequenceStatus::Accepted);
  auto held = large();
  REQUIRE(held.status == model::SequenceStatus::Accepted);
  CHECK(pressured.gap_records() == 1U);
  CHECK(pressured.visible().front().record_id == 2U);
  // A log owner can retain the physical allocation after display eviction.
  CHECK(large().status == model::SequenceStatus::BudgetExhausted);
  CHECK(pressured.last_sequence() == 2U);
  CHECK(pressured.visible().empty());
  CHECK(small.total_used() <= 8U * 1024U);
  held = {};
  CHECK(small.total_used() == 0U);
}

TEST_CASE("session queue overload is nonblocking and terminal",
          "[logging][worker]") {
  auto options = writer_options();
  options.queue_max_bytes = 1U;
  LogFixture test{options};
  test.start();
  CHECK(test.writer.try_enqueue(lazycom::test::session_record(
            record(1U, "too large"))) == EnqueueResult::QueueFull);
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
  REQUIRE(test.writer.try_enqueue(lazycom::test::session_record(
              record(1U, "one"))) == EnqueueResult::Accepted);
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
  CHECK(test.writer.try_enqueue(lazycom::test::session_record(
            record(1U, "after disable"))) == EnqueueResult::NotRecording);
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
