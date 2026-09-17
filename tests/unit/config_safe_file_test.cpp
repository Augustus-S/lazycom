#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <lazycom/config/safe_file.hpp>
#include <lazycom/config/schema.hpp>
#include <string>
#include <support/fake_atomic_file_system.hpp>
#include <support/temporary_directory.hpp>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
namespace config = lazycom::config;
using lazycom::test::TemporaryDirectory;
using lazycom::test::UmaskGuard;
void write_test_file(const std::filesystem::path &path, std::string_view bytes,
                     mode_t mode = S_IRUSR | S_IWUSR) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  REQUIRE(output);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  REQUIRE(output);
  REQUIRE(::chmod(path.c_str(), mode) == 0);
}
} // namespace

TEST_CASE("safe reads retain file identity and a known SHA-256 digest",
          "[config][file]") {
  TemporaryDirectory directory;
  const auto path = directory.path() / "state.toml";
  const auto missing = config::read_safe_file(path, 1024);
  REQUIRE(missing);
  CHECK_FALSE(missing->identity.exists);
  CHECK(missing->bytes.empty());
  const auto document =
      "version = 1\nlast_quick_send_slot = 1\nlast_interval_ms = 0\n";
  write_test_file(path, document);
  const auto safe = config::read_safe_file(path, config::kStateMaximumBytes);
  const auto loaded = config::load_state_toml(path);
  REQUIRE(safe);
  REQUIRE(loaded.accepted);
  CHECK(safe->bytes == document);
  CHECK(safe->identity.exists);
  CHECK(safe->identity.size == safe->bytes.size());
  CHECK(loaded.file_identity == safe->identity);
  write_test_file(path, "abc");
  const auto digest = config::read_safe_file(path, 1024);
  REQUIRE(digest);
  const std::array<std::uint8_t, 32> expected{
      0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
      0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
      0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
  CHECK(digest->identity.content_digest == expected);
}

TEST_CASE("safe files reject symlinks sizes modes and NUL paths",
          "[config][file]") {
  TemporaryDirectory directory;
  const auto path = directory.path() / "config.toml";
  write_test_file(path, "12345");
  const auto link = directory.path() / "link.toml";
  REQUIRE(::symlink(path.c_str(), link.c_str()) == 0);
  CHECK_FALSE(config::read_safe_file(link, 1024));
  CHECK_FALSE(config::read_safe_file(path, 4));
  const auto outcome =
      config::write_file_atomically(link, "replacement", 1024, {});
  CHECK(outcome.state == config::CommitState::NotCommitted);
  CHECK(outcome.error);
  const auto original = config::read_safe_file(path, 1024);
  REQUIRE(original);
  CHECK(original->bytes == "12345");
  for (const auto mode : {mode_t{0700}, mode_t{0604}}) {
    CAPTURE(mode);
    REQUIRE(::chmod(path.c_str(), mode) == 0);
    CHECK_FALSE(config::read_safe_file(path, 1024));
  }
  auto native = path.string();
  native.insert(native.size() - 5U, 1U, '\0');
  CHECK_FALSE(config::read_safe_file(std::filesystem::path{native}, 1024));
  REQUIRE(::chmod(directory.path().c_str(), 0500) == 0);
  CHECK_FALSE(config::read_safe_file(directory.path() / "missing.toml", 1024));
  REQUIRE(::chmod(directory.path().c_str(), 0700) == 0);
}

TEST_CASE("atomic writes create private nested directories and exact committed "
          "identity",
          "[config][file]") {
  TemporaryDirectory parent;
  const auto directory =
      parent.path() / "missing-config-home" / "nested" / "lazycom";
  const auto path = directory / "state.toml";
  config::AtomicWriteOutcome outcome;
  {
    UmaskGuard restrictive_umask{0777};
    outcome = config::write_file_atomically(path, "version = 1\n", 1024, {});
  }
  REQUIRE(outcome.state == config::CommitState::Committed);
  CHECK_FALSE(outcome.error);
  struct stat directory_status{}, file_status{};
  REQUIRE(::lstat(directory.c_str(), &directory_status) == 0);
  REQUIRE(::lstat(path.c_str(), &file_status) == 0);
  CHECK(S_ISDIR(directory_status.st_mode));
  CHECK((directory_status.st_mode & 07777) == S_IRWXU);
  CHECK(S_ISREG(file_status.st_mode));
  CHECK((file_status.st_mode & 07777) == (S_IRUSR | S_IWUSR));
  const auto loaded = config::read_safe_file(path, 1024);
  REQUIRE(loaded);
  CHECK(loaded->bytes == "version = 1\n");
  REQUIRE(outcome.committed_identity);
  CHECK(*outcome.committed_identity == loaded->identity);
}

TEST_CASE(
    "atomic backup preserves the previous snapshot and refuses unsafe sources",
    "[config][file]") {
  for (const auto scenario : {"replace", "symlink", "oversized"}) {
    CAPTURE(scenario);
    TemporaryDirectory directory;
    const auto path = directory.path() / "state.toml";
    const auto backup = path.string() + ".bak";
    write_test_file(path, "12345");
    const auto loaded = config::read_safe_file(path, 1024);
    REQUIRE(loaded);
    if (std::string_view{scenario} == "symlink")
      REQUIRE(::symlink(path.c_str(), backup.c_str()) == 0);
    const auto outcome = config::write_file_atomically(
        path, "new", std::string_view{scenario} == "oversized" ? 4U : 1024U,
        loaded->identity);
    const auto current = config::read_safe_file(path, 1024);
    REQUIRE(current);
    if (std::string_view{scenario} != "replace") {
      CHECK(outcome.state == config::CommitState::NotCommitted);
      CHECK(current->bytes == "12345");
      continue;
    }
    REQUIRE(outcome.state == config::CommitState::Committed);
    CHECK(current->bytes == "new");
    const auto previous = config::read_safe_file(backup, 1024);
    REQUIRE(previous);
    CHECK(previous->bytes == "12345");
    REQUIRE(
        config::write_file_atomically(path, "third", 1024, current->identity)
            .state == config::CommitState::Committed);
    const auto latest_backup = config::read_safe_file(backup, 1024);
    REQUIRE(latest_backup);
    CHECK(latest_backup->bytes == "new");
  }
}

TEST_CASE("configuration FIFO paths are rejected without waiting for a peer",
          "[config][file]") {
  TemporaryDirectory directory;
  const auto path = directory.path() / "config.toml";
  REQUIRE(::mkfifo(path.c_str(), S_IRUSR | S_IWUSR) == 0);
  bool write = false;
  SECTION("read") {}
  SECTION("atomic save") { write = true; }

  const auto child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    ::alarm(2U);
    const bool rejected =
        write ? config::write_file_atomically(path, "version = 1\n", 1024U, {})
                        .state == config::CommitState::NotCommitted
              : !config::read_safe_file(path, 1024U);
    ::_exit(rejected ? 0 : 1);
  }
  int status{};
  pid_t waited{};
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  REQUIRE(waited == child);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);
}

TEST_CASE("committed file identity rejects a subsequent external replacement",
          "[config][file]") {
  TemporaryDirectory directory;
  const auto path = directory.path() / "config.toml";
  const auto saved =
      config::write_file_atomically(path, "version = 1\n", 1024U, {});
  REQUIRE(saved.state == config::CommitState::Committed);
  REQUIRE(saved.committed_identity);

  const auto replacement = directory.path() / "external.toml";
  write_test_file(replacement, "external document");
  REQUIRE(::rename(replacement.c_str(), path.c_str()) == 0);
  const auto overwritten = config::write_file_atomically(
      path, "version = 1\nx = 2\n", 1024U, *saved.committed_identity);
  CHECK(overwritten.state == config::CommitState::NotCommitted);
  CHECK_FALSE(overwritten.committed_identity);
  const auto current = config::read_safe_file(path, 1024U);
  REQUIRE(current);
  CHECK(current->bytes == "external document");
}

TEST_CASE("atomic writer detects in-place changes after staging",
          "[config][file]") {
  TemporaryDirectory directory;
  const auto path = directory.path() / "state.toml";
  write_test_file(path, "original");
  const auto loaded = config::read_safe_file(path, 1024);
  REQUIRE(loaded);

  auto transaction = config::linux_atomic_file_system().begin_atomic_write(
      path, 1024, loaded->identity);
  REQUIRE(transaction);
  REQUIRE((*transaction)->stage("new data"));
  write_test_file(path, "modified");
  const timespec times[2]{
      {0, UTIME_OMIT},
      {static_cast<time_t>(loaded->identity.mtime_seconds),
       static_cast<long>(loaded->identity.mtime_nanoseconds)}};
  REQUIRE(::utimensat(AT_FDCWD, path.c_str(), times, 0) == 0);

  REQUIRE_FALSE((*transaction)->commit());
  const auto current = config::read_safe_file(path, 1024);
  REQUIRE(current);
  REQUIRE(current->bytes == "modified");
}

TEST_CASE("atomic writer never replaces an unexpected newly-created target",
          "[config][file]") {
  TemporaryDirectory directory;
  const auto path = directory.path() / "state.toml";
  auto transaction =
      config::linux_atomic_file_system().begin_atomic_write(path, 1024, {});
  REQUIRE(transaction);
  REQUIRE((*transaction)->stage("ours"));
  write_test_file(path, "external");

  REQUIRE_FALSE((*transaction)->commit());
  const auto current = config::read_safe_file(path, 1024);
  REQUIRE(current);
  REQUIRE(current->bytes == "external");
}

TEST_CASE("atomic writer directory lock is nonblocking and released with owner",
          "[config][file]") {
  TemporaryDirectory directory;
  const auto path = directory.path() / "state.toml";
  auto first =
      config::linux_atomic_file_system().begin_atomic_write(path, 1024U, {});
  REQUIRE(first);

  const auto contended =
      config::linux_atomic_file_system().begin_atomic_write(path, 1024U, {});
  REQUIRE_FALSE(contended);
  CHECK(contended.error().detail == "configuration directory is busy");

  first->reset();
  REQUIRE(
      config::linux_atomic_file_system().begin_atomic_write(path, 1024U, {}));
}

TEST_CASE("atomic writer enforces the caller file limit", "[config][file]") {
  const auto target = std::filesystem::path{"/unused/state.toml"};
  lazycom::test::FakeAtomicFileSystem file_system{
      lazycom::test::AtomicFailurePoint::None};
  const auto outcome =
      config::write_file_atomically(target, "12345", 4, {}, file_system);
  REQUIRE(outcome.state == config::CommitState::NotCommitted);
  REQUIRE(outcome.error);
}

TEST_CASE("committed identity survives directory synchronization failures",
          "[config][file]") {
  const auto target = std::filesystem::path{"/unused/state.toml"};
  for (const auto failure :
       {lazycom::test::AtomicFailurePoint::None,
        lazycom::test::AtomicFailurePoint::DirectorySync,
        lazycom::test::AtomicFailurePoint::DirectorySyncException}) {
    CAPTURE(failure);
    lazycom::test::FakeAtomicFileSystem file_system{failure};
    const auto outcome =
        config::write_file_atomically(target, "data", 1024U, {}, file_system);
    CHECK(outcome.state ==
          (failure == lazycom::test::AtomicFailurePoint::None
               ? config::CommitState::Committed
               : config::CommitState::CommittedDurabilityUnknown));
    REQUIRE(outcome.committed_identity);
    CHECK(outcome.committed_identity->exists);
    CHECK(outcome.committed_identity->device == 17U);
    CHECK(outcome.committed_identity->inode == 23U);
    CHECK(outcome.committed_identity->size == 4U);
  }
  for (const auto failure : {lazycom::test::AtomicFailurePoint::Begin,
                             lazycom::test::AtomicFailurePoint::Stage,
                             lazycom::test::AtomicFailurePoint::Commit}) {
    CAPTURE(failure);
    lazycom::test::FakeAtomicFileSystem file_system{failure};
    const auto outcome =
        config::write_file_atomically(target, "data", 1024U, {}, file_system);
    CHECK(outcome.state == config::CommitState::NotCommitted);
    CHECK_FALSE(outcome.committed_identity);
  }
}
