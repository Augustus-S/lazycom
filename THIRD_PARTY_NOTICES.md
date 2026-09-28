# Third-Party Notices

LazyCom currently vendors the following build inputs. Exact source URLs and
SHA-256 values are recorded in `include/dependencies/DEPENDENCIES.lock`.

| Dependency | Version | License | License file |
| --- | --- | --- | --- |
| FTXUI | 6.1.9 | MIT | `third_party/ftxui/LICENSE` |
| libserialport | 0.1.2 | LGPL-3.0-or-later | `third_party/libserialport/COPYING` |
| toml++ | 3.4.0 | MIT | `include/dependencies/licenses/tomlplusplus-LICENSE` |
| nlohmann/json | 3.12.0 | MIT | `include/dependencies/licenses/nlohmann-json-LICENSE.MIT` |
| tl::expected | 1.2.0 | CC0-1.0 | `include/dependencies/licenses/tl-expected-COPYING` |
| spdlog | 1.15.3 | MIT | `third_party/spdlog/LICENSE` |

Catch2 and the test sources are maintained in the separate `../lazycom-test`
repository. It preserves their licenses, dependency lock and migration hashes.

libserialport is built as a shared library. Distribution must preserve the
LGPL license, corresponding source, and the user's ability to replace the
library with a compatible build.

Vendored trees are trimmed to the Linux build inputs; non-building upstream
content (examples, tests, docs, CI/ecosystem metadata, non-Linux sources) is
removed. `third_party/libserialport/LAZYCOM_PATCHES.md` records the details for
libserialport; `third_party/ftxui/LAZYCOM_PATCHES.md` records the FTXUI test-support
removal. Upstream source URLs and SHA-256 values in
`include/dependencies/DEPENDENCIES.lock` still identify the original archives.
