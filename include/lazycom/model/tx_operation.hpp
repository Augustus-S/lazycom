#pragma once

#include <lazycom/base/ids.hpp>
#include <lazycom/logging/schema.hpp>
#include <lazycom/model/session_record.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <tl/expected.hpp>

namespace lazycom::model {

enum class TxTermination { Succeeded, Failed, Cancelled, TimedOut };
enum class TxOperationError { InvalidAcceptance, InvalidTermination };

class TxOperation {
public:
  using Deadline = std::chrono::steady_clock::time_point;

  TxOperation(OperationId operation_id, ConnectionGeneration generation,
              SessionId session_id, SharedPayloadPtr bytes,
              logging::InputMode input_mode, Deadline deadline);

  [[nodiscard]] OperationId operation_id() const noexcept {
    return operation_id_;
  }
  [[nodiscard]] ConnectionGeneration generation() const noexcept {
    return generation_;
  }
  [[nodiscard]] SessionId session_id() const noexcept { return session_id_; }
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] std::span<const std::byte> remaining() const noexcept;
  [[nodiscard]] Deadline deadline() const noexcept { return deadline_; }

  // A positive owner write advances only the prefix known to be OS-accepted.
  [[nodiscard]] bool accept(std::size_t bytes, RecordTime observed_at) noexcept;

  // Failure/cancellation/timeout emits accepted TX first (if any), followed by
  // an ERR carrying the operation ID. Success requires the whole request.
  [[nodiscard]] tl::expected<std::vector<RecordDraft>, TxOperationError>
  terminal_records(TxTermination termination, RecordTime terminal_time,
                   std::string error_code = "LC-SER-2004",
                   std::string error_message = "TX operation terminated") const;

private:
  OperationId operation_id_{};
  ConnectionGeneration generation_{};
  SessionId session_id_{};
  SharedPayloadPtr bytes_;
  logging::InputMode input_mode_{logging::InputMode::Text};
  Deadline deadline_{};
  std::size_t offset_{};
  std::optional<RecordTime> last_accept_time_;
};

} // namespace lazycom::model
