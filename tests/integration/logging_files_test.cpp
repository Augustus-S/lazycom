#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <lazycom/logging/session_writer.hpp>
#include <set>
#include <string>
#include <support/logging_builders.hpp>
#include <support/temporary_directory.hpp>
#include <sys/stat.h>

namespace {
namespace logging = lazycom::logging;
using lazycom::test::TemporaryDirectory;
using lazycom::test::UmaskGuard;
[[nodiscard]] std::string read_all(const std::filesystem::path &path) {
  std::ifstream input{path, std::ios::binary};
  REQUIRE(input);
  return {std::istreambuf_iterator<char>{input},
          std::istreambuf_iterator<char>{}};
}
[[nodiscard]] logging::SessionWriterOptions
options_for(const std::filesystem::path &directory) {
  logging::SessionWriterOptions options;
  options.directory = directory;
  options.flush_interval = std::chrono::milliseconds{10};
  options.flush_batch_bytes = 4096U;
  return options;
}
void write_controlled_file(const std::filesystem::path &path,
                           std::string_view contents) {
  std::ofstream output{path, std::ios::binary};
  REQUIRE(output);
  output << contents;
  output.close();
  REQUIRE(output);
  REQUIRE(::chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0);
}
} // namespace

TEST_CASE(
    "Linux logs use safe directory permissions and recoverable private files",
    "[integration][logging][file]") {
  for (const auto mode : {mode_t{0}, mode_t{0750}, mode_t{0720}}) {
    CAPTURE(mode);
    TemporaryDirectory temporary;
    const auto directory = temporary.path() / "state" / "lazycom" / "logs";
    if (mode != 0) {
      REQUIRE(std::filesystem::create_directories(directory));
      REQUIRE(::chmod(directory.c_str(), mode) == 0);
    }
    logging::SessionWriter writer{options_for(directory)};
    REQUIRE(writer.enable());
    logging::SessionCommandResult started;
    {
      UmaskGuard restrictive_umask{0777};
      started = writer.start_session(lazycom::test::log_header()).get();
    }
    if (mode == 0720) {
      CHECK(started.state == logging::SessionLogState::Error);
      continue;
    }
    REQUIRE(started.state == logging::SessionLogState::Recording);
    const auto path = writer.active_path();
    const auto record = lazycom::test::log_record(
        1U, logging::Direction::Rx,
        {std::byte{0x00}, std::byte{0xFF}, std::byte{0x7E}});
    REQUIRE(writer.try_enqueue(lazycom::test::session_record(record)) ==
            logging::EnqueueResult::Accepted);
    REQUIRE(writer.barrier(1U).get().state == logging::BarrierState::Confirmed);
    REQUIRE(writer.end_session().get().state ==
            logging::SessionLogState::Waiting);
    struct stat directory_status{}, file_status{};
    REQUIRE(::lstat(directory.c_str(), &directory_status) == 0);
    REQUIRE(::lstat(path.c_str(), &file_status) == 0);
    CHECK((directory_status.st_mode & 07777) == (mode == 0 ? S_IRWXU : mode));
    CHECK((file_status.st_mode & 07777) == (S_IRUSR | S_IWUSR));
    const auto decoded = logging::decode_ndjson(read_all(path));
    REQUIRE(decoded);
    REQUIRE(decoded->records.size() == 1U);
    CHECK(decoded->records[0].payload == record.payload);
  }
}

TEST_CASE("Linux session backend enforces its process directory lock",
          "[integration][logging][file]") {
  TemporaryDirectory temporary;
  const auto log_directory = temporary.path() / "logs";
  logging::SessionWriter first{options_for(log_directory)};
  logging::SessionWriter second{options_for(log_directory)};
  REQUIRE(first.enable());
  REQUIRE(second.enable());
  REQUIRE(first.start_session(lazycom::test::log_header()).get().state ==
          logging::SessionLogState::Recording);
  REQUIRE(second.start_session(lazycom::test::log_header()).get().state ==
          logging::SessionLogState::Error);
}

TEST_CASE(
    "Linux log quotas rotate valid logs and preserve damaged or empty files",
    "[integration][logging][quota]") {
  for (const auto scenario :
       {"rotation", "damaged bytes", "damaged and empty count"}) {
    CAPTURE(scenario);
    TemporaryDirectory temporary;
    const auto directory = temporary.path() / "logs";
    REQUIRE(std::filesystem::create_directory(directory));
    REQUIRE(::chmod(directory.c_str(), S_IRWXU) == 0);
    const auto header = lazycom::test::log_header();
    auto first = lazycom::test::log_record(1U, logging::Direction::Rx,
                                           lazycom::test::bytes("payload"));
    const auto header_line = logging::encode_header_line(header);
    const auto record_line = logging::encode_record_line(first);
    REQUIRE(header_line);
    REQUIRE(record_line);
    auto options = options_for(directory);
    options.quotas.max_files = 2U;
    options.quotas.max_file_bytes = header_line->size() + record_line->size();
    options.quotas.max_total_bytes = options.quotas.max_file_bytes * 2U;
    const auto damaged = directory / "lazycom-session-damaged.ndjson";
    const auto empty = directory / "lazycom-session-empty.ndjson";
    const bool rotation = std::string_view{scenario} == "rotation";
    const bool count_limit =
        std::string_view{scenario} == "damaged and empty count";
    if (!rotation) {
      write_controlled_file(damaged, std::string(4096U, 'x'));
      if (count_limit)
        write_controlled_file(empty, "");
      options.quotas.max_total_bytes =
          count_limit ? 1024U * 1024U : 4096U + header_line->size() - 1U;
    }
    logging::SessionWriter writer{options};
    REQUIRE(writer.enable());
    const auto started = writer.start_session(header).get();
    if (!rotation) {
      CHECK(started.state == logging::SessionLogState::Error);
      CHECK(std::filesystem::file_size(damaged) == 4096U);
      if (count_limit)
        CHECK(std::filesystem::file_size(empty) == 0U);
      continue;
    }
    REQUIRE(started.state == logging::SessionLogState::Recording);
    auto second = first, third = first;
    second.seq = 2U;
    third.seq = 3U;
    for (auto record : {first, second, third}) {
      REQUIRE(writer.try_enqueue(lazycom::test::session_record(
                  std::move(record))) == logging::EnqueueResult::Accepted);
    }
    REQUIRE(writer.barrier(3U).get().state == logging::BarrierState::Confirmed);
    REQUIRE(writer.end_session().get().state ==
            logging::SessionLogState::Waiting);
    std::set<std::uint64_t> retained_sequences;
    std::size_t retained_files{};
    for (const auto &entry : std::filesystem::directory_iterator{directory}) {
      if (entry.path().extension() != ".ndjson")
        continue;
      ++retained_files;
      const auto decoded = logging::decode_ndjson(read_all(entry.path()));
      REQUIRE(decoded);
      REQUIRE(decoded->records.size() == 1U);
      retained_sequences.insert(decoded->records[0].seq);
    }
    CHECK(retained_files == 2U);
    CHECK(retained_sequences.size() == 2U);
    CHECK(retained_sequences.contains(3U));
  }
}

TEST_CASE("quota cache rescans after an external directory change",
          "[integration][logging][quota]") {
  TemporaryDirectory temporary;
  const auto log_directory = temporary.path() / "logs";
  const auto header = lazycom::test::log_header();
  auto record = lazycom::test::log_record(1U, logging::Direction::Rx,
                                          lazycom::test::bytes("payload"));
  const auto header_line = logging::encode_header_line(header);
  const auto record_line = logging::encode_record_line(record);
  REQUIRE(header_line);
  REQUIRE(record_line);
  auto options = options_for(log_directory);
  options.quotas.max_file_bytes = header_line->size() + record_line->size();
  options.quotas.max_total_bytes = options.quotas.max_file_bytes + 4095U;

  logging::SessionWriter writer{options};
  REQUIRE(writer.enable());
  REQUIRE(writer.start_session(header).get().state ==
          logging::SessionLogState::Recording);
  const auto damaged = log_directory / "lazycom-session-external.ndjson";
  write_controlled_file(damaged, std::string(4096U, 'x'));

  REQUIRE(writer.try_enqueue(lazycom::test::session_record(record)) ==
          logging::EnqueueResult::Accepted);
  REQUIRE(writer.barrier(1U).get().state ==
          logging::BarrierState::WriterFailed);
  REQUIRE(std::filesystem::file_size(damaged) == 4096U);
}
