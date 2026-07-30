# Phase 1 Completion Report

## Scope

Phase 1 implements the core models and versioned schemas described by
`Plan.md` section 16 and `DevelopPlan.md` section 14. It intentionally does not
implement the phase 2 serial owner or the interactive FTXUI application.

## Delivered

- Stable `ErrorCode` registry, bounded error details, `Result<T>`, strong IDs,
  generation overflow handling, typed commands and typed events.
- Fixed fatal signal slots, five worker lifecycle signals, worker exception
  boundaries and main-thread-only `FatalStopping` entry.
- Connection, interaction, overlay and session logging state machines.
- Permanently partitioned normal, TX and control completion reservations.
- Complete version 1 snapshots for `config.toml`, `quick_send.toml` and
  `state.toml`, including field scopes, ranges and conservative 128 MiB budget
  validation.
- Safe TOML reads and writes with exact 0700/0600 permissions, no-follow file
  access, per-file size limits, SHA-256 load identity conflict detection, one
  `.bak`, and the three commit outcomes.
- NDJSON v1.0 header and record codecs, strict UTF-8/base64 round trips,
  untrusted-input limits, incomplete-tail handling and flush barrier semantics.
- Reusable fake clock, fake atomic filesystem and schema data builders.

## Verification

All 74 tests pass in these configurations:

- GCC Debug with strict warnings as errors
- Clang Debug with strict warnings as errors
- GCC Release
- GCC Debug with diagnostics compiled out
- GCC Debug with ASan and UBSan

A separate Release build with `BUILD_TESTING=OFF` and
`LAZYCOM_BUILD_TESTS=OFF` confirms that Catch2 is not configured or linked into
the production executable.

## Remaining Work

- The stage 0 three-family USB-UART hardware matrix and repeatable performance
  report still require physical devices and benchmark work.
- The current executable remains a bootstrap display. Serial ownership belongs
  to phase 2; framing, the complete data path and UI belong to later phases.
- POSIX does not provide a conditional rename by prior content identity. The
  writer validates the target at transaction start and immediately before
  rename, but an uncooperative process with the same uid has a final narrow race
  window.
- The bounded whole-document NDJSON reader is a schema validator. Large log
  consumption will require a streaming reader in the logging implementation
  phase.
