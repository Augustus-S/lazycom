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

/**
 * @brief Owner-confined state of one non-interleaved TX request.
 *
 * The object captures operation, connection, session, payload, input mode, and
 * an absolute steady-clock deadline at construction. It does not validate
 * those values and is not safe for concurrent mutation.
 */
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

  /**
   * @brief Advances the prefix accepted by one positive owner write.
   * @param bytes Positive count no greater than remaining().size().
   * @param observed_at Time assigned to the accepted prefix.
   * @return false without mutation for zero or out-of-range counts.
   */
  [[nodiscard]] bool accept(std::size_t bytes, RecordTime observed_at) noexcept;

  /**
   * @brief Builds the ordered terminal records for the operation.
   *
   * Failure, cancellation, and timeout emit the accepted TX prefix first, when
   * nonempty, followed by an ERR containing the operation ID. Success requires
   * the complete payload and emits one TX record for a nonempty payload.
   * Calling this function does not mark the object terminal.
   */
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
