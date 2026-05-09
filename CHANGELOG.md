## [Unreleased]

## [0.1.3] - 2026-05-09

- Fix `sub!` and `gsub!` to expand the backing file when the replacement grows the string
- Improve test coverage and fix incorrect test assertions
- Fix `squeeze!` to support Ruby string range notation by delegating to `String#squeeze!`
- Remove dead code and fix `mmap_lock` type safety
- Fix memory management, initialisation, and mutation bugs in C extension

## [0.1.2] - 2025-11-18

- Standardise directory structure to fix native extension loading

## [0.1.1] - 2025-11-18

- Fix precompiled binaries being included in the gem files

## [0.1.0] - 2025-07-19

- Rewrite tenderlove/mmap with modern Ruby support
