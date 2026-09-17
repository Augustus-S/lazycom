#include <catch2/catch_test_macros.hpp>
#include <lazycom/diagnostics/diagnostics.hpp>
#include <lazycom/serial/libserialport_probe.hpp>

TEST_CASE("compiled dependency versions are available", "[dependencies]") {
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
