#pragma once

#include <lazycom/base/error.hpp>
#include <lazycom/logging/schema.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lazycom::logging {

enum class SessionLogState { Off, Waiting, Recording, Error };

struct SessionLogQuotas {
  std::size_t max_files{100U};
  std::uint64_t max_total_bytes{1024ULL * 1024ULL * 1024ULL};
  std::uint64_t max_file_bytes{64ULL * 1024ULL * 1024ULL};
  auto operator<=>(const SessionLogQuotas &) const = default;
};

struct SessionWriterOptions {
  std::filesystem::path directory;
  SessionLogQuotas quotas;
  std::size_t queue_max_records{4096U};
  std::size_t queue_max_bytes{8U * 1024U * 1024U};
  std::size_t flush_batch_bytes{256U * 1024U};
  std::chrono::milliseconds flush_interval{1000};
  bool include_system{true};
  bool include_error{true};
};

// The worker owns one implementation for its complete lifetime. Tests can
// inject failures without weakening the Linux directory-fd implementation.
class SessionLogFileSystem {
public:
  virtual ~SessionLogFileSystem() = default;

  virtual Status begin_session(const std::filesystem::path &directory,
                               std::string_view started_at,
                               std::string_view encoded_header,
                               const SessionLogQuotas &quotas) = 0;
  virtual Status append_line(std::string_view encoded_record) = 0;
  virtual Status flush() = 0;
  virtual Status close() noexcept = 0;
  [[nodiscard]] virtual std::filesystem::path active_path() const = 0;
};

[[nodiscard]] std::unique_ptr<SessionLogFileSystem>
make_linux_session_log_file_system();

enum class EnqueueResult {
  Accepted,
  NotRecording,
  EmptyBatch,
  InvalidBatch,
  QueueFull,
  Stopping,
};

struct SessionCommandResult {
  SessionLogState state{SessionLogState::Off};
  std::uint64_t processed_through_seq{};
  std::optional<Error> error;
};

class SessionWriter {
public:
  explicit SessionWriter(SessionWriterOptions options,
                         std::unique_ptr<SessionLogFileSystem> file_system =
                             make_linux_session_log_file_system());
  ~SessionWriter();

  SessionWriter(const SessionWriter &) = delete;
  SessionWriter &operator=(const SessionWriter &) = delete;
  SessionWriter(SessionWriter &&) = delete;
  SessionWriter &operator=(SessionWriter &&) = delete;

  // OFF -> WAITING. ERROR must first be reset with disable().
  [[nodiscard]] bool enable() noexcept;
  // Immediately stops producers; completion follows final flush and close.
  [[nodiscard]] std::future<SessionCommandResult> disable();

  // Valid only while WAITING. Completion is Recording only after the header was
  // safely created, or Error after a create/quota failure.
  [[nodiscard]] std::future<SessionCommandResult> start_session(Header header);
  // Immediately changes Recording -> Waiting and drains records ordered before
  // this command before flushing and closing.
  [[nodiscard]] std::future<SessionCommandResult> end_session();

  // Never waits for filesystem I/O or queue space. Queue exhaustion moves the
  // writer to ERROR and prevents any later record from being written.
  [[nodiscard]] EnqueueResult try_enqueue(std::vector<Record> batch) noexcept;

  // Every returned future completes, including writer error and shutdown.
  [[nodiscard]] std::future<BarrierResult> barrier(std::uint64_t target_seq);
  [[nodiscard]] std::future<SessionCommandResult> shutdown();

  [[nodiscard]] SessionLogState state() const noexcept;
  [[nodiscard]] std::uint64_t processed_through_seq() const noexcept;
  [[nodiscard]] std::size_t queued_records() const noexcept;
  [[nodiscard]] std::size_t queued_bytes() const noexcept;
  [[nodiscard]] std::filesystem::path active_path() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lazycom::logging
