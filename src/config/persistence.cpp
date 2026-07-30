#include <lazycom/config/persistence.hpp>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace lazycom::config {
namespace {

[[nodiscard]] constexpr std::size_t file_index(PersistenceFile file) noexcept {
  switch (file) {
  case PersistenceFile::Config:
    return 0U;
  case PersistenceFile::QuickSend:
    return 1U;
  case PersistenceFile::State:
    return 2U;
  }
  return 0U;
}

[[nodiscard]] Error persistence_error(std::string_view detail) {
  return make_error(ErrorCode::ConfigIoFailed, Operation::SaveConfig, detail);
}

[[nodiscard]] bool expected_filename(PersistenceFile file,
                                     const std::filesystem::path &path) {
  if (!path.is_absolute() || path.filename().empty() ||
      path.native().find('\0') != std::string::npos) {
    return false;
  }
  switch (file) {
  case PersistenceFile::Config:
    return path.filename() == "config.toml";
  case PersistenceFile::QuickSend:
    return path.filename() == "quick_send.toml";
  case PersistenceFile::State:
    return path.filename() == "state.toml";
  }
  return false;
}

} // namespace

struct PersistenceWorker::Impl {
  struct Request {
    PersistenceFile file{PersistenceFile::Config};
    std::filesystem::path path;
    std::string document;
    std::size_t maximum_bytes{};
    SafeFileIdentity expected_identity;
    std::promise<SaveCompletion> promise;
  };

  Impl(PersistencePaths worker_paths, AtomicFileSystem &worker_file_system)
      : paths{std::move(worker_paths)}, file_system{worker_file_system},
        worker{[this] { run(); }} {}

  [[nodiscard]] const std::filesystem::path &path(PersistenceFile file) const {
    switch (file) {
    case PersistenceFile::Config:
      return paths.config;
    case PersistenceFile::QuickSend:
      return paths.quick_send;
    case PersistenceFile::State:
      return paths.state;
    }
    return paths.config;
  }

  [[nodiscard]] SaveSubmission submit(PersistenceFile file,
                                      Result<std::string> serialized,
                                      std::size_t maximum_bytes,
                                      SafeFileIdentity expected_identity,
                                      bool read_only) {
    if (read_only) {
      return {SaveSubmitState::ReadOnly, {},
              persistence_error(
                  "configuration is read-only after an unsafe or invalid load")};
    }
    if (!serialized) {
      return {SaveSubmitState::Invalid, {}, serialized.error()};
    }
    if (!expected_filename(file, path(file))) {
      return {SaveSubmitState::Invalid, {},
              persistence_error("persistence target has an unexpected path")};
    }

    Request request{file, path(file), std::move(*serialized), maximum_bytes,
                    std::move(expected_identity), {}};
    auto future = request.promise.get_future();
    {
      std::scoped_lock lock{mutex};
      if (stopping) {
        return {SaveSubmitState::Stopping, {},
                persistence_error("persistence worker is stopping")};
      }
      const auto index = file_index(file);
      if (active[index]) {
        return {SaveSubmitState::Busy, {},
                persistence_error(
                    "a save for this configuration file is already active")};
      }
      active[index] = true;
      requests.push_back(std::move(request));
    }
    condition.notify_one();
    return {SaveSubmitState::Accepted, std::move(future), std::nullopt};
  }

  void run() {
    while (true) {
      std::optional<Request> request;
      {
        std::unique_lock lock{mutex};
        condition.wait(lock, [this] { return stopping || !requests.empty(); });
        if (requests.empty()) {
          if (stopping) {
            return;
          }
          continue;
        }
        request.emplace(std::move(requests.front()));
        requests.pop_front();
      }

      AtomicWriteOutcome outcome;
      try {
        outcome = write_file_atomically(
            request->path, request->document, request->maximum_bytes,
            request->expected_identity, file_system);
      } catch (const std::exception &exception) {
        outcome = {CommitState::NotCommitted,
                   make_error(ErrorCode::ConfigIoFailed,
                              Operation::SaveConfig, exception.what())};
      } catch (...) {
        outcome = {CommitState::NotCommitted,
                   persistence_error("persistence worker save failed")};
      }
      {
        std::scoped_lock lock{mutex};
        active[file_index(request->file)] = false;
      }
      request->promise.set_value(
          {request->file, std::move(outcome), std::move(request->document)});
    }
  }

  PersistencePaths paths;
  AtomicFileSystem &file_system;
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::deque<Request> requests;
  std::array<bool, 3> active{};
  bool stopping{};
  std::jthread worker;
};

PersistenceWorker::PersistenceWorker(PersistencePaths paths,
                                     AtomicFileSystem &file_system)
    : impl_{std::make_unique<Impl>(std::move(paths), file_system)} {}

PersistenceWorker::~PersistenceWorker() { shutdown(); }

SaveSubmission PersistenceWorker::save_config(
    const ConfigSnapshot &snapshot, const SafeFileIdentity &expected_identity,
    std::string_view preserved_document, bool read_only) {
  return impl_->submit(PersistenceFile::Config,
                       serialize_config_toml(snapshot, preserved_document),
                       kConfigMaximumBytes, expected_identity, read_only);
}

SaveSubmission PersistenceWorker::save_quick_send(
    const QuickSendSnapshot &snapshot,
    const SafeFileIdentity &expected_identity,
    std::string_view preserved_document, bool read_only) {
  return impl_->submit(
      PersistenceFile::QuickSend,
      serialize_quick_send_toml(snapshot, preserved_document),
      kQuickSendMaximumBytes, expected_identity, read_only);
}

SaveSubmission PersistenceWorker::save_state(
    const StateSnapshot &snapshot, const SafeFileIdentity &expected_identity,
    std::string_view preserved_document, bool read_only) {
  return impl_->submit(PersistenceFile::State,
                       serialize_state_toml(snapshot, preserved_document),
                       kStateMaximumBytes, expected_identity, read_only);
}

void PersistenceWorker::shutdown() {
  if (!impl_) {
    return;
  }
  {
    std::scoped_lock lock{impl_->mutex};
    impl_->stopping = true;
  }
  impl_->condition.notify_one();
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
}

bool PersistenceWorker::active(PersistenceFile file) const noexcept {
  std::scoped_lock lock{impl_->mutex};
  return impl_->active[file_index(file)];
}

} // namespace lazycom::config
