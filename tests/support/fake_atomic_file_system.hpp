#pragma once

#include <lazycom/config/safe_file.hpp>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace lazycom::test {

enum class AtomicFailurePoint {
  None,
  Begin,
  Stage,
  Commit,
  DirectorySync,
  DirectorySyncException,
};

[[nodiscard]] inline Status atomic_test_failure(std::string detail) {
  return tl::make_unexpected(make_error(
      ErrorCode::ConfigIoFailed, Operation::SaveConfig, std::move(detail)));
}

class FakeAtomicTransaction final : public config::AtomicWriteTransaction {
public:
  explicit FakeAtomicTransaction(const AtomicFailurePoint point)
      : point_{point} {}

  Status stage(const std::string_view bytes) override {
    staged = std::string{bytes};
    return point_ == AtomicFailurePoint::Stage ? atomic_test_failure("stage")
                                               : Status{};
  }

  Result<config::SafeFileIdentity> commit() override {
    if (point_ == AtomicFailurePoint::Commit) {
      return tl::make_unexpected(atomic_test_failure("commit").error());
    }
    config::SafeFileIdentity identity;
    identity.exists = true;
    identity.device = 17U;
    identity.inode = 23U;
    identity.size = staged.size();
    return identity;
  }

  Status sync_parent_directory() override {
    if (point_ == AtomicFailurePoint::DirectorySyncException) {
      throw std::runtime_error{"directory sync"};
    }
    return point_ == AtomicFailurePoint::DirectorySync
               ? atomic_test_failure("directory sync")
               : Status{};
  }

  std::string staged;

private:
  AtomicFailurePoint point_;
};

class FakeAtomicFileSystem final : public config::AtomicFileSystem {
public:
  explicit FakeAtomicFileSystem(const AtomicFailurePoint point)
      : point_{point} {}

  Result<std::unique_ptr<config::AtomicWriteTransaction>>
  begin_atomic_write(const std::filesystem::path &, std::size_t,
                     const config::SafeFileIdentity &) override {
    if (point_ == AtomicFailurePoint::Begin) {
      return tl::make_unexpected(make_error(ErrorCode::ConfigIoFailed,
                                            Operation::SaveConfig, "begin"));
    }
    return std::make_unique<FakeAtomicTransaction>(point_);
  }

private:
  AtomicFailurePoint point_;
};

} // namespace lazycom::test
