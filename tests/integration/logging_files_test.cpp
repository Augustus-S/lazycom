#include <lazycom/config/persistence.hpp>
#include <lazycom/logging/session_writer.hpp>

#include <support/logging_builders.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    static std::uint64_t sequence{};
    path_ = std::filesystem::temp_directory_path() /
            ("lazycom-logging-files-" + std::to_string(::getpid()) + "-" +
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

[[nodiscard]] std::string read_all(const std::filesystem::path &path) {
  std::ifstream input{path, std::ios::binary};
  REQUIRE(input);
  return {std::istreambuf_iterator<char>{input},
          std::istreambuf_iterator<char>{}};
}

[[nodiscard]] lazycom::logging::SessionWriterOptions
options_for(const std::filesystem::path &directory) {
  lazycom::logging::SessionWriterOptions options;
  options.directory = directory;
  options.flush_interval = std::chrono::milliseconds{10};
  options.flush_batch_bytes = 4096U;
  return options;
}

} // namespace

TEST_CASE("Linux session backend creates private recoverable NDJSON",
          "[integration][logging][file]") {
  TemporaryDirectory temporary;
  const auto log_directory = temporary.path() / "logs";
  lazycom::logging::SessionWriter writer{options_for(log_directory)};
  REQUIRE(writer.enable());
  lazycom::logging::SessionCommandResult started;
  {
    UmaskGuard restrictive_umask{0777};
    started =
        writer.start_session(lazycom::test::LogHeaderBuilder{}.build()).get();
  }
  REQUIRE(started.state == lazycom::logging::SessionLogState::Recording);
  const auto log_path = writer.active_path();

  auto record = lazycom::test::LogRecordBuilder{
      1U,
      lazycom::logging::Direction::Rx,
      {std::byte{0x00}, std::byte{0xFF},
       std::byte{
           0x7E}}}.build();
  REQUIRE(writer.try_enqueue({record}) ==
          lazycom::logging::EnqueueResult::Accepted);
  REQUIRE(writer.barrier(1U).get().state ==
          lazycom::logging::BarrierState::Confirmed);
  REQUIRE(writer.end_session().get().state ==
          lazycom::logging::SessionLogState::Waiting);

  struct stat directory_status{};
  struct stat file_status{};
  REQUIRE(::lstat(log_directory.c_str(), &directory_status) == 0);
  REQUIRE(::lstat(log_path.c_str(), &file_status) == 0);
  REQUIRE((directory_status.st_mode & 07777) == S_IRWXU);
  REQUIRE((file_status.st_mode & 07777) == (S_IRUSR | S_IWUSR));

  const auto decoded = lazycom::logging::decode_ndjson(read_all(log_path));
  REQUIRE(decoded);
  REQUIRE(decoded->records.size() == 1U);
  REQUIRE(decoded->records[0].payload == record.payload);
}

TEST_CASE("Linux session backend enforces its process directory lock",
          "[integration][logging][file]") {
  TemporaryDirectory temporary;
  const auto log_directory = temporary.path() / "logs";
  lazycom::logging::SessionWriter first{options_for(log_directory)};
  lazycom::logging::SessionWriter second{options_for(log_directory)};
  REQUIRE(first.enable());
  REQUIRE(second.enable());
  REQUIRE(first.start_session(lazycom::test::LogHeaderBuilder{}.build())
              .get()
              .state == lazycom::logging::SessionLogState::Recording);
  REQUIRE(second.start_session(lazycom::test::LogHeaderBuilder{}.build())
              .get()
              .state == lazycom::logging::SessionLogState::Error);
}

TEST_CASE("Linux session backend creates nested default directories",
          "[integration][logging][file]") {
  TemporaryDirectory temporary;
  const auto log_directory = temporary.path() / "state" / "lazycom" / "logs";
  lazycom::logging::SessionWriter writer{options_for(log_directory)};
  REQUIRE(writer.enable());
  REQUIRE(writer.start_session(lazycom::test::LogHeaderBuilder{}.build())
              .get()
              .state == lazycom::logging::SessionLogState::Recording);
  struct stat status{};
  REQUIRE(::lstat(log_directory.c_str(), &status) == 0);
  CHECK((status.st_mode & 07777) == S_IRWXU);
}

TEST_CASE("Linux session backend accepts writable user-owned directories",
          "[integration][logging][file]") {
  TemporaryDirectory temporary;
  const auto log_directory = temporary.path() / "logs";
  REQUIRE(std::filesystem::create_directory(log_directory));
  REQUIRE(::chmod(log_directory.c_str(), S_IRWXU | S_IRGRP | S_IXGRP) == 0);
  lazycom::logging::SessionWriter writer{options_for(log_directory)};
  REQUIRE(writer.enable());
  CHECK(writer.start_session(lazycom::test::LogHeaderBuilder{}.build())
            .get()
            .state == lazycom::logging::SessionLogState::Recording);
}

TEST_CASE("Linux session backend rotates and deletes only verified logs",
          "[integration][logging][quota]") {
  TemporaryDirectory temporary;
  const auto log_directory = temporary.path() / "logs";
  auto options = options_for(log_directory);
  const auto header = lazycom::test::LogHeaderBuilder{}.build();
  auto first =
      lazycom::test::LogRecordBuilder{1U, lazycom::logging::Direction::Rx,
                                      lazycom::test::bytes("payload")}
          .build();
  auto second = first;
  auto third = first;
  second.seq = 2U;
  third.seq = 3U;
  const auto header_line = lazycom::logging::encode_header_line(header);
  const auto record_line = lazycom::logging::encode_record_line(first);
  REQUIRE(header_line);
  REQUIRE(record_line);
  const auto one_file_size = header_line->size() + record_line->size();
  options.quotas.max_files = 2U;
  options.quotas.max_file_bytes = one_file_size;
  options.quotas.max_total_bytes = one_file_size * 2U;

  lazycom::logging::SessionWriter writer{options};
  REQUIRE(writer.enable());
  REQUIRE(writer.start_session(header).get().state ==
          lazycom::logging::SessionLogState::Recording);
  REQUIRE(writer.try_enqueue({first, second, third}) ==
          lazycom::logging::EnqueueResult::Accepted);
  REQUIRE(writer.barrier(3U).get().state ==
          lazycom::logging::BarrierState::Confirmed);
  REQUIRE(writer.end_session().get().state ==
          lazycom::logging::SessionLogState::Waiting);

  std::set<std::uint64_t> retained_sequences;
  std::size_t retained_files = 0U;
  for (const auto &entry : std::filesystem::directory_iterator{log_directory}) {
    if (entry.path().extension() != ".ndjson") {
      continue;
    }
    ++retained_files;
    const auto decoded =
        lazycom::logging::decode_ndjson(read_all(entry.path()));
    REQUIRE(decoded);
    REQUIRE(decoded->records.size() == 1U);
    retained_sequences.insert(decoded->records[0].seq);
  }
  REQUIRE(retained_files == 2U);
  REQUIRE(retained_sequences.size() == 2U);
  REQUIRE(retained_sequences.contains(3U));
}

TEST_CASE("damaged controlled logs count toward quota and are not deleted",
          "[integration][logging][quota]") {
  TemporaryDirectory temporary;
  const auto log_directory = temporary.path() / "logs";
  REQUIRE(std::filesystem::create_directory(log_directory));
  REQUIRE(::chmod(log_directory.c_str(), S_IRWXU) == 0);
  const auto damaged = log_directory / "lazycom-session-damaged.ndjson";
  {
    std::ofstream output{damaged, std::ios::binary};
    REQUIRE(output);
    output << std::string(4096U, 'x');
  }
  REQUIRE(::chmod(damaged.c_str(), S_IRUSR | S_IWUSR) == 0);

  const auto header = lazycom::test::LogHeaderBuilder{}.build();
  const auto header_line = lazycom::logging::encode_header_line(header);
  REQUIRE(header_line);
  auto options = options_for(log_directory);
  options.quotas.max_files = 10U;
  options.quotas.max_file_bytes = header_line->size() + 1024U;
  options.quotas.max_total_bytes = 4096U + header_line->size() - 1U;
  lazycom::logging::SessionWriter writer{options};
  REQUIRE(writer.enable());
  REQUIRE(writer.start_session(header).get().state ==
          lazycom::logging::SessionLogState::Error);
  REQUIRE(std::filesystem::exists(damaged));
  REQUIRE(std::filesystem::file_size(damaged) == 4096U);
}

TEST_CASE("persistence worker writes a private TOML through the real backend",
          "[integration][config][file]") {
  TemporaryDirectory temporary;
  const lazycom::config::PersistencePaths paths{
      temporary.path() / "config.toml", temporary.path() / "quick_send.toml",
      temporary.path() / "state.toml"};
  lazycom::config::PersistenceWorker worker{paths};
  lazycom::config::StateSnapshot state;
  state.last_quick_send_slot = 7U;

  auto submitted = worker.save_state(state, {});
  REQUIRE(submitted.accepted());
  const auto completion = submitted.completion.get();
  REQUIRE(completion.outcome.state == lazycom::config::CommitState::Committed);
  const auto loaded = lazycom::config::load_state_toml(paths.state);
  REQUIRE(loaded.accepted);
  REQUIRE(loaded.snapshot == state);
}
