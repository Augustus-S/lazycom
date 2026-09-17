#pragma once

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <sys/stat.h>

namespace lazycom::test {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    auto pattern =
        (std::filesystem::temp_directory_path() / "lazycom-test-XXXXXX")
            .string();
    if (::mkdtemp(pattern.data()) == nullptr) {
      throw std::runtime_error("cannot create private test directory");
    }
    path_ = pattern;
    if (::chmod(path_.c_str(), S_IRWXU) != 0) {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
      throw std::runtime_error("cannot set test directory permissions");
    }
  }
  ~TemporaryDirectory() {
    static_cast<void>(::chmod(path_.c_str(), S_IRWXU));
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
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

} // namespace lazycom::test
