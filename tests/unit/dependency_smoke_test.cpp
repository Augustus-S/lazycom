#include <lazycom/core/build_info.hpp>
#include <lazycom/diagnostics/diagnostics.hpp>
#include <lazycom/serial/libserialport_probe.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <tl/expected.hpp>
#include <toml++/toml.hpp>

#include <string>

TEST_CASE("header dependencies are usable", "[dependencies]") {
  const auto document = nlohmann::json{{"type", "header"}, {"version", 1}};
  const auto config = toml::parse("enabled = true\n");
  const tl::expected<int, std::string> value = 42;

  REQUIRE(document.at("type") == "header");
  REQUIRE(config["enabled"].value_or(false));
  REQUIRE(value.value() == 42);
}

TEST_CASE("compiled dependency versions are available", "[dependencies]") {
  REQUIRE(lazycom::version() == "0.1.0");
  REQUIRE(lazycom::serial::libserialport_version_supported());
  REQUIRE(lazycom::serial::libserialport_version().text == "0.1.2");
#if defined(LAZYCOM_EXPECT_DIAGNOSTICS)
  REQUIRE(lazycom::diagnostics::compiled_in());
  REQUIRE(lazycom::diagnostics::backend_version() > 0);
#else
  REQUIRE_FALSE(lazycom::diagnostics::compiled_in());
  REQUIRE(lazycom::diagnostics::backend_version() == 0);
#endif
}
