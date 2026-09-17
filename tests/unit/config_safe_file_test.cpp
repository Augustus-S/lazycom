#include <lazycom/config/safe_file.hpp>
#include <lazycom/config/schema.hpp>

#include <support/fake_atomic_file_system.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    static std::uint64_t sequence{};
    path_ = std::filesystem::temp_directory_path() /
            ("lazycom-config-test-" + std::to_string(::getpid()) + "-" +
             std::to_string(sequence++));
    std::filesystem::create_directory(path_);
    REQUIRE(::chmod(path_.c_str(), S_IRWXU) == 0);
  }

  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

class UmaskGuard {
public:
  explicit UmaskGuard(mode_t value) noexcept : previous_{::umask(value)} {}
  ~UmaskGuard() { static_cast<void>(::umask(previous_)); }

  UmaskGuard(const UmaskGuard &) = delete;
  UmaskGuard &operator=(const UmaskGuard &) = delete;

private:
  mode_t previous_;
};

void write_test_file(const std::filesystem::path &path, std::string_view bytes,
                     mode_t mode = S_IRUSR | S_IWUSR) {
  const int descriptor =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
  REQUIRE(descriptor >= 0);
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count =
        ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
    REQUIRE(count > 0);
    offset += static_cast<std::size_t>(count);
  }
  REQUIRE(::close(descriptor) == 0);
  REQUIRE(::chmod(path.c_str(), mode) == 0);
}

void overwrite_test_file(const std::filesystem::path &path,
                         std::string_view bytes) {
  const int descriptor = ::open(path.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
  REQUIRE(descriptor >= 0);
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count =
        ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
    REQUIRE(count > 0);
    offset += static_cast<std::size_t>(count);
  }
  REQUIRE(::close(descriptor) == 0);
}

} // namespace

TEST_CASE("missing safe file returns a non-error empty result",
          "[config][file]") {
  TemporaryDirectory directory;
  const auto result =
      lazycom::config::read_safe_file(directory.path() / "state.toml", 1024);

  REQUIRE(result);
  REQUIRE_FALSE(result->identity.exists);
  REQUIRE(result->bytes.empty());
}

TEST_CASE("safe file reads private regular files", "[config][file]") {
  TemporaryDirectory directory;
  const auto path = directory.path() / "config.toml";
  write_test_file(path, "version = 1\n");

  const auto result = lazycom::config::read_safe_file(path, 1024);
  REQUIRE(result);
  REQUIRE(result->identity.exists);
  REQUIRE(result->bytes == "version = 1\n");
  REQUIRE(result->identity.size == result->bytes.size());
}

TEST_CASE("schema load results retain the safe file identity",
          "[config][file]") {
  TemporaryDirectory directory;
  const auto path = directory.path() / "state.toml";
  write_test_file(
      path, "version = 1\nlast_quick_send_slot = 1\nlast_interval_ms = 0\n");

  const auto safe = lazycom::config::read_safe_file(
      path, lazycom::config::kStateMaximumBytes);
  const auto loaded = lazycom::config::load_state_toml(path);
  REQUIRE(safe);
  REQUIRE(loaded.accepted);
  REQUIRE(loaded.file_identity == safe->identity);
}

TEST_CASE("safe file identity uses SHA-256 content digest", "[config][file]") {
  TemporaryDirectory directory;
  const auto path = directory.path() / "digest.toml";
  write_test_file(path, "abc");
  const auto loaded = lazycom::config::read_safe_file(path, 1024);
  REQUIRE(loaded);

  const std::array<std::uint8_t, 32> expected{
      0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
      0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
      0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
  REQUIRE(loaded->identity.content_digest == expected);
}

TEST_CASE("safe file rejects symlinks sizes and public permissions",
          "[config][file]") {
  TemporaryDirectory directory;
  const auto private_file = directory.path() / "private.toml";
  write_test_file(private_file, "12345");

  const auto link = directory.path() / "link.toml";
  REQUIRE(::symlink(private_file.c_str(), link.c_str()) == 0);
  REQUIRE_FALSE(lazycom::config::read_safe_file(link, 1024));
  REQUIRE_FALSE(lazycom::config::read_safe_file(private_file, 4));

  const auto public_file = directory.path() / "public.toml";
  write_test_file(public_file, "x", S_IRUSR | S_IWUSR | S_IROTH);
  REQUIRE_FALSE(lazycom::config::read_safe_file(public_file, 1024));
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
        write ? lazycom::config::write_file_atomically(path, "version = 1\n",
                                                       1024U, {})
                        .state == lazycom::config::CommitState::NotCommitted
              : !lazycom::config::read_safe_file(path, 1024U);
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

TEST_CASE("safe file requires exact modes and rejects NUL paths",
          "[config][file]") {
  TemporaryDirectory directory;
  const auto executable_file = directory.path() / "executable.toml";
  write_test_file(executable_file, "x", S_IRWXU);
  REQUIRE_FALSE(lazycom::config::read_safe_file(executable_file, 1024));

  const auto nested = directory.path() / "nested";
  REQUIRE(std::filesystem::create_directory(nested));
  REQUIRE(::chmod(nested.c_str(), S_IRUSR | S_IXUSR) == 0);
  REQUIRE_FALSE(lazycom::config::read_safe_file(nested / "state.toml", 1024));
  REQUIRE(::chmod(nested.c_str(), S_IRWXU) == 0);

  auto native = (directory.path() / "state.toml").string();
  native.insert(native.size() - 5U, 1U, '\0');
  REQUIRE_FALSE(
      lazycom::config::read_safe_file(std::filesystem::path{native}, 1024));
}

TEST_CASE("Linux atomic writer creates private durable target",
          "[config][file]") {
  TemporaryDirectory directory;
  const auto path = directory.path() / "state.toml";

  lazycom::config::AtomicWriteOutcome outcome;
  {
    UmaskGuard restrictive_umask{0777};
    outcome =
        lazycom::config::write_file_atomically(path, "version = 1\n", 1024, {});
  }
  REQUIRE(outcome.state == lazycom::config::CommitState::Committed);
  REQUIRE_FALSE(outcome.error);

  struct stat status{};
  REQUIRE(::lstat(path.c_str(), &status) == 0);
  REQUIRE(S_ISREG(status.st_mode));
  REQUIRE((status.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) ==
          (S_IRUSR | S_IWUSR));
  const auto loaded = lazycom::config::read_safe_file(path, 1024);
  REQUIRE(loaded);
  REQUIRE(loaded->bytes == "version = 1\n");
  REQUIRE(outcome.committed_identity);
  CHECK(*outcome.committed_identity == loaded->identity);
}

TEST_CASE("committed file identity rejects a subsequent external replacement",
          "[config][file]") {
  TemporaryDirectory directory;
  const auto path = directory.path() / "config.toml";
  const auto saved =
      lazycom::config::write_file_atomically(path, "version = 1\n", 1024U, {});
  REQUIRE(saved.state == lazycom::config::CommitState::Committed);
  REQUIRE(saved.committed_identity);

  const auto replacement = directory.path() / "external.toml";
  write_test_file(replacement, "external document");
  REQUIRE(::rename(replacement.c_str(), path.c_str()) == 0);
  const auto overwritten = lazycom::config::write_file_atomically(
      path, "version = 1\nx = 2\n", 1024U, *saved.committed_identity);
  CHECK(overwritten.state == lazycom::config::CommitState::NotCommitted);
  CHECK_FALSE(overwritten.committed_identity);
  const auto current = lazycom::config::read_safe_file(path, 1024U);
  REQUIRE(current);
  CHECK(current->bytes == "external document");
}

TEST_CASE("Linux atomic writer creates a missing private application directory",
          "[config][file]") {
  TemporaryDirectory parent;
  const auto base = parent.path() / "missing-config-home";
  const auto directory = base / "nested" / "lazycom";
  const auto path = directory / "state.toml";

  lazycom::config::AtomicWriteOutcome outcome;
  {
    UmaskGuard restrictive{0777};
    outcome =
        lazycom::config::write_file_atomically(path, "version = 1\n", 1024, {});
  }
  REQUIRE(outcome.state == lazycom::config::CommitState::Committed);

  struct stat directory_status{};
  struct stat file_status{};
  REQUIRE(::lstat(directory.c_str(), &directory_status) == 0);
  REQUIRE(::lstat(path.c_str(), &file_status) == 0);
  REQUIRE(S_ISDIR(directory_status.st_mode));
  REQUIRE((directory_status.st_mode & 07777) == S_IRWXU);
  REQUIRE(S_ISREG(file_status.st_mode));
  REQUIRE((file_status.st_mode & 07777) == (S_IRUSR | S_IWUSR));
  REQUIRE(std::filesystem::is_directory(base / "nested"));
}

TEST_CASE("Linux atomic writer rejects a symlink target", "[config][file]") {
  TemporaryDirectory directory;
  const auto real_path = directory.path() / "real.toml";
  const auto link_path = directory.path() / "state.toml";
  write_test_file(real_path, "old");
  REQUIRE(::symlink(real_path.c_str(), link_path.c_str()) == 0);

  const auto outcome = lazycom::config::write_file_atomically(
      link_path, "replacement", 1024, {});
  REQUIRE(outcome.state == lazycom::config::CommitState::NotCommitted);
  REQUIRE(outcome.error);
  const auto original = lazycom::config::read_safe_file(real_path, 1024);
  REQUIRE(original);
  REQUIRE(original->bytes == "old");
}

TEST_CASE("Linux atomic writer keeps exactly one private previous snapshot",
          "[config][file]") {
  TemporaryDirectory directory;
  const auto path = directory.path() / "config.toml";
  REQUIRE(
      lazycom::config::write_file_atomically(path, "version = 1\n", 1024, {})
          .state == lazycom::config::CommitState::Committed);
  const auto first = lazycom::config::read_safe_file(path, 1024);
  REQUIRE(first);
  REQUIRE(lazycom::config::write_file_atomically(path, "version = 1\nx = 2\n",
                                                 1024, first->identity)
              .state == lazycom::config::CommitState::Committed);

  const auto current = lazycom::config::read_safe_file(path, 1024);
  const auto backup =
      lazycom::config::read_safe_file(path.string() + ".bak", 1024);
  REQUIRE(current);
  REQUIRE(backup);
  REQUIRE(current->bytes == "version = 1\nx = 2\n");
  REQUIRE(backup->bytes == "version = 1\n");
}

TEST_CASE("unsafe backup prevents committing the main file", "[config][file]") {
  TemporaryDirectory directory;
  const auto path = directory.path() / "state.toml";
  write_test_file(path, "old");
  const auto loaded = lazycom::config::read_safe_file(path, 1024);
  REQUIRE(loaded);
  REQUIRE(::symlink(path.c_str(), (path.string() + ".bak").c_str()) == 0);

  const auto outcome = lazycom::config::write_file_atomically(path, "new", 1024,
                                                              loaded->identity);
  REQUIRE(outcome.state == lazycom::config::CommitState::NotCommitted);
  const auto current = lazycom::config::read_safe_file(path, 1024);
  REQUIRE(current);
  REQUIRE(current->bytes == "old");
}

TEST_CASE("atomic writer rejects external atomic replacement",
          "[config][file]") {
  TemporaryDirectory directory;
  const auto path = directory.path() / "config.toml";
  const auto replacement = directory.path() / "replacement.toml";
  write_test_file(path, "old value");
  const auto loaded = lazycom::config::read_safe_file(path, 1024);
  REQUIRE(loaded);

  write_test_file(replacement, "external!");
  REQUIRE(::rename(replacement.c_str(), path.c_str()) == 0);
  const auto outcome = lazycom::config::write_file_atomically(
      path, "new value", 1024, loaded->identity);
  REQUIRE(outcome.state == lazycom::config::CommitState::NotCommitted);

  const auto current = lazycom::config::read_safe_file(path, 1024);
  REQUIRE(current);
  REQUIRE(current->bytes == "external!");
}

TEST_CASE("atomic writer detects in-place changes after staging",
          "[config][file]") {
  TemporaryDirectory directory;
  const auto path = directory.path() / "state.toml";
  write_test_file(path, "original");
  const auto loaded = lazycom::config::read_safe_file(path, 1024);
  REQUIRE(loaded);

  auto transaction =
      lazycom::config::linux_atomic_file_system().begin_atomic_write(
          path, 1024, loaded->identity);
  REQUIRE(transaction);
  REQUIRE((*transaction)->stage("new data"));
  overwrite_test_file(path, "modified");
  const timespec times[2]{
      {0, UTIME_OMIT},
      {static_cast<time_t>(loaded->identity.mtime_seconds),
       static_cast<long>(loaded->identity.mtime_nanoseconds)}};
  REQUIRE(::utimensat(AT_FDCWD, path.c_str(), times, 0) == 0);

  REQUIRE_FALSE((*transaction)->commit());
  const auto current = lazycom::config::read_safe_file(path, 1024);
  REQUIRE(current);
  REQUIRE(current->bytes == "modified");
}

TEST_CASE("atomic writer never replaces an unexpected newly-created target",
          "[config][file]") {
  TemporaryDirectory directory;
  const auto path = directory.path() / "state.toml";
  auto transaction =
      lazycom::config::linux_atomic_file_system().begin_atomic_write(path, 1024,
                                                                     {});
  REQUIRE(transaction);
  REQUIRE((*transaction)->stage("ours"));
  write_test_file(path, "external");

  REQUIRE_FALSE((*transaction)->commit());
  const auto current = lazycom::config::read_safe_file(path, 1024);
  REQUIRE(current);
  REQUIRE(current->bytes == "external");
}

TEST_CASE("atomic writer directory lock is nonblocking and released with owner",
          "[config][file]") {
  TemporaryDirectory directory;
  const auto path = directory.path() / "state.toml";
  auto first = lazycom::config::linux_atomic_file_system().begin_atomic_write(
      path, 1024U, {});
  REQUIRE(first);

  const auto contended =
      lazycom::config::linux_atomic_file_system().begin_atomic_write(path,
                                                                     1024U, {});
  REQUIRE_FALSE(contended);
  CHECK(contended.error().detail == "configuration directory is busy");

  first->reset();
  REQUIRE(lazycom::config::linux_atomic_file_system().begin_atomic_write(
      path, 1024U, {}));
}

TEST_CASE("atomic writer enforces the caller file limit", "[config][file]") {
  const auto target = std::filesystem::path{"/unused/state.toml"};
  lazycom::test::FakeAtomicFileSystem file_system{
      lazycom::test::AtomicFailurePoint::None};
  const auto outcome = lazycom::config::write_file_atomically(
      target, "12345", 4, {}, file_system);
  REQUIRE(outcome.state == lazycom::config::CommitState::NotCommitted);
  REQUIRE(outcome.error);
}

TEST_CASE("atomic writer applies the caller limit to the backup source",
          "[config][file]") {
  TemporaryDirectory directory;
  const auto path = directory.path() / "state.toml";
  write_test_file(path, "12345");
  const auto loaded = lazycom::config::read_safe_file(path, 8);
  REQUIRE(loaded);

  const auto outcome =
      lazycom::config::write_file_atomically(path, "new", 4, loaded->identity);
  REQUIRE(outcome.state == lazycom::config::CommitState::NotCommitted);
  const auto current = lazycom::config::read_safe_file(path, 8);
  REQUIRE(current);
  REQUIRE(current->bytes == "12345");
}

TEST_CASE("atomic writer exposes all three commit states", "[config][file]") {
  const auto target = std::filesystem::path{"/unused/state.toml"};

  lazycom::test::FakeAtomicFileSystem stage_failure{
      lazycom::test::AtomicFailurePoint::Stage};
  REQUIRE(lazycom::config::write_file_atomically(target, "data", 1024, {},
                                                 stage_failure)
              .state == lazycom::config::CommitState::NotCommitted);

  lazycom::test::FakeAtomicFileSystem commit_failure{
      lazycom::test::AtomicFailurePoint::Commit};
  REQUIRE(lazycom::config::write_file_atomically(target, "data", 1024, {},
                                                 commit_failure)
              .state == lazycom::config::CommitState::NotCommitted);

  lazycom::test::FakeAtomicFileSystem sync_failure{
      lazycom::test::AtomicFailurePoint::DirectorySync};
  REQUIRE(lazycom::config::write_file_atomically(target, "data", 1024, {},
                                                 sync_failure)
              .state ==
          lazycom::config::CommitState::CommittedDurabilityUnknown);

  lazycom::test::FakeAtomicFileSystem success{
      lazycom::test::AtomicFailurePoint::None};
  REQUIRE(
      lazycom::config::write_file_atomically(target, "data", 1024, {}, success)
          .state == lazycom::config::CommitState::Committed);
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
    const auto outcome = lazycom::config::write_file_atomically(
        target, "data", 1024U, {}, file_system);
    CHECK(outcome.state ==
          (failure == lazycom::test::AtomicFailurePoint::None
               ? lazycom::config::CommitState::Committed
               : lazycom::config::CommitState::CommittedDurabilityUnknown));
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
    const auto outcome = lazycom::config::write_file_atomically(
        target, "data", 1024U, {}, file_system);
    CHECK(outcome.state == lazycom::config::CommitState::NotCommitted);
    CHECK_FALSE(outcome.committed_identity);
  }
}
