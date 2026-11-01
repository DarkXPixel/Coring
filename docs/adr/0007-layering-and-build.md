# ADR 0007 — One library per module, downward-only links, checked includes

## Context
Coring4 grew into spaghetti: one executable target compiling four generations of
code, duplicated concepts, and headers reaching in every direction.

## Decision
* `src/coring5/CMakeLists.txt` defines `coring5_<module>` static libraries via
  `coring5_module()`; links are `PUBLIC` only downward. Interface-only modules
  (`upstream`, `tls`, `handlers`) are `INTERFACE` libraries until implemented.
* `tools/check_layering.py` holds the allowed-include table and runs in CI; it
  catches header-only violations that link fine. (It was verified to fail on an
  injected `base → io` include.)
* `config` and `proto/http1` do not depend on `io`, so they are testable and
  fuzzable without a kernel ring. `ConfigManager` lives in `runtime/` for that
  reason (it needs `WorkerGroup`).
* Warnings-as-errors (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion
  -Wsign-conversion …`) on Coring5 targets only; legacy targets are untouched.
* Legacy code is quarantined behind `CORING_BUILD_LEGACY` and removed per
  MIGRATION.md.

## Consequences
* (+) Illegal dependencies fail the build or CI; modules can be extracted/tested alone.
* (−) The include table in the script must be kept in sync with CMake when a
  module or edge is added (the script fails on unknown modules to force this).
