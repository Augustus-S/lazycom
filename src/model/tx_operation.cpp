#include <lazycom/model/tx_operation.hpp>

#include <string>
#include <utility>

namespace lazycom::model {

TxOperation::TxOperation(const OperationId operation_id,
                         const ConnectionGeneration generation,
                         const SessionId session_id, SharedPayloadPtr bytes,
                         const logging::InputMode input_mode,
                         const Deadline deadline)
    : operation_id_(operation_id), generation_(generation),
      session_id_(session_id), bytes_(std::move(bytes)),
      input_mode_(input_mode), deadline_(deadline) {}

std::span<const std::byte> TxOperation::bytes() const noexcept {
  return bytes_ ? bytes_->bytes() : std::span<const std::byte>{};
}

std::span<const std::byte> TxOperation::remaining() const noexcept {
  return bytes().subspan(offset_);
}

bool TxOperation::accept(const std::size_t accepted,
                         RecordTime observed_at) noexcept {
  if (accepted == 0U || accepted > remaining().size()) {
    return false;
  }
  offset_ += accepted;
  last_accept_time_ = std::move(observed_at);
  return true;
}

tl::expected<std::vector<RecordDraft>, TxOperationError>
TxOperation::terminal_records(const TxTermination termination,
                              RecordTime terminal_time,
                              std::string error_code,
                              std::string error_message) const {
  if (termination == TxTermination::Succeeded && offset_ != bytes().size()) {
    return tl::unexpected(TxOperationError::InvalidTermination);
  }
  if (offset_ != 0U && !last_accept_time_) {
    return tl::unexpected(TxOperationError::InvalidAcceptance);
  }

  std::vector<RecordDraft> records;
  records.reserve(termination == TxTermination::Succeeded ? 1U : 2U);
  if (offset_ != 0U) {
    RecordDraft tx;
    tx.direction = logging::Direction::Tx;
    tx.time = *last_accept_time_;
    tx.payload = ByteSlice{bytes_, 0U, offset_};
    tx.input_mode = input_mode_;
    tx.operation_id = operation_id_;
    records.push_back(std::move(tx));
  }
  if (termination != TxTermination::Succeeded) {
    RecordDraft error;
    error.direction = logging::Direction::Err;
    error.time = std::move(terminal_time);
    error.code = std::move(error_code);
    error.operation_id = operation_id_;
    error.message = "operation_id=" + std::to_string(operation_id_.value) +
                    ": " + std::move(error_message);
    records.push_back(std::move(error));
  }
  return records;
}

} // namespace lazycom::model
