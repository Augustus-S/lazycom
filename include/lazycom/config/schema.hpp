#pragma once

/**
 * @file
 * @brief Bounded TOML configuration models, validation, loading, and writing.
 *
 * Snapshot structs are plain values and are not self-validating. Call the
 * matching validator before applying a programmatically constructed snapshot.
 * Parsers reject an entire invalid candidate rather than merging defaults into
 * it, and serializers validate before producing output.
 */

#include <lazycom/base/error.hpp>
#include <lazycom/config/safe_file.hpp>
#include <lazycom/config/types.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace lazycom::config {

/**
 * @brief Result of parsing or safely loading one complete snapshot.
 *
 * accepted is true only when snapshot represents the complete accepted
 * candidate. A rejected document returns the built-in default snapshot with
 * read_only set, preserving the current configuration from partial updates.
 * document and all diagnostic values own their storage.
 */
template <class Snapshot> struct SnapshotLoadResult {
  Snapshot snapshot{};
  bool accepted{};
  bool file_exists{true};
  bool read_only{};
  SafeFileIdentity file_identity;
  std::string document;
  std::vector<SchemaMessage> errors;
  std::vector<SchemaMessage> warnings;
};

using ConfigLoadResult = SnapshotLoadResult<ConfigSnapshot>;
using QuickSendLoadResult = SnapshotLoadResult<QuickSendSnapshot>;
using StateLoadResult = SnapshotLoadResult<StateSnapshot>;

/**
 * @brief Parses a complete config.toml candidate from owned-or-borrowed text.
 * @return An accepted complete snapshot, or a read-only default snapshot with
 * errors. Syntax, schema, range, version, and resource failures reject the
 * whole candidate. Unknown keys are warnings and can be preserved on write by
 * passing result.document to serialize_config_toml().
 */
[[nodiscard]] ConfigLoadResult parse_config_toml(std::string_view document);
/** @brief Parses a complete, bounded quick_send.toml candidate. */
[[nodiscard]] QuickSendLoadResult
parse_quick_send_toml(std::string_view document);
/** @brief Parses a complete, bounded state.toml candidate. */
[[nodiscard]] StateLoadResult parse_state_toml(std::string_view document);

/**
 * @brief Safely reads and parses config.toml.
 * @return A missing file as an accepted default with file_exists false; unsafe,
 * unreadable, or invalid files produce a rejected read-only result.
 */
[[nodiscard]] ConfigLoadResult
load_config_toml(const std::filesystem::path &path);
/** @brief Safely reads and parses quick_send.toml. */
[[nodiscard]] QuickSendLoadResult
load_quick_send_toml(const std::filesystem::path &path);
/** @brief Safely reads and parses state.toml. */
[[nodiscard]] StateLoadResult
load_state_toml(const std::filesystem::path &path);

/**
 * @brief Validates and serializes a config snapshot as normalized TOML.
 * @param snapshot Complete candidate to serialize.
 * @param preserved_document Previously parsed document whose unknown values
 * should be retained. Comments and original layout are not preserved.
 * @return Canonical TOML ending in one LF, or an Error.
 */
[[nodiscard]] Result<std::string>
serialize_config_toml(const ConfigSnapshot &snapshot,
                      std::string_view preserved_document = {});
/** @brief Validates and serializes a quick-send snapshot as normalized TOML. */
[[nodiscard]] Result<std::string>
serialize_quick_send_toml(const QuickSendSnapshot &snapshot,
                          std::string_view preserved_document = {});
/** @brief Validates and serializes a UI-state snapshot as normalized TOML. */
[[nodiscard]] Result<std::string>
serialize_state_toml(const StateSnapshot &snapshot,
                     std::string_view preserved_document = {});

} // namespace lazycom::config
