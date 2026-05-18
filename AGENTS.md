# Project Overview

cpulimit limits CPU usage of a target process by monitoring CPU consumption
and sending POSIX signals (`SIGSTOP` / `SIGCONT`).

If `-i` / `--include-children` is enabled, the limit MUST apply to all
descendants.

- Implementation language MUST be C.
- The baseline C standard MUST be C89.
- Supported platforms MUST be Linux, macOS, and FreeBSD.

---

# Rule Interpretation

- Requirements MUST use RFC 2119 keywords (`MUST`, `SHOULD`, `MAY`).
- If requirements conflict, resolution MUST prioritize in this order:
  1. Correctness and safety
  2. Portability and behavioral consistency
  3. Performance
  4. Maintainability
- Requirements in this file MUST apply to source code, tests, build scripts,
  and CI-related changes unless explicitly scoped.

---

# Product Requirements

## Functional Requirements

- The program MUST accept a target process by:
  - PID, or
  - executable name, or
  - command line pattern.
- Input validation MUST reject invalid, inconsistent, or out-of-range values.
- CPU usage calculation MUST be correct for the selected target set.
- CPU usage MUST be sampled periodically.
- Limiting MUST be enforced by suspending and resuming with `SIGSTOP` /
  `SIGCONT`.
- The program MUST handle graceful shutdown signals (`SIGINT`, `SIGQUIT`,
  `SIGTERM`, `SIGHUP`, `SIGPIPE`).
- On exit (including error paths), the target process set MUST be restored to a
  runnable state when possible.
- Target exit during monitoring MUST be handled safely.
- Rapid child creation/exit MUST be handled safely.
- With `-i` / `--include-children`:
  - descendants MUST be discovered and tracked,
  - newly spawned descendants MUST be included,
  - short-lived descendants SHOULD be missed as rarely as reasonably possible.
- Exit codes MUST be documented and meaningful:
  - `0` on success,
  - non-zero on error.
- Any CLI behavior change (flags, defaults, exit codes, help text) MUST update
  user-facing documentation.

## Non-Functional Requirements

- Runtime overhead MUST remain low and MUST avoid unnecessary busy-waiting.
- The implementation MUST avoid introducing scheduler-visible latency beyond
  what is required by limiting.
- Behavior SHOULD be deterministic under equivalent conditions.
- The program MUST fail gracefully with actionable error messages.
- The program MUST NOT crash on invalid input or recoverable runtime errors.
- The implementation MUST avoid race conditions in monitoring/signaling logic.
- Zombie processes MUST be avoided.
- Re-parented and terminated descendants MUST be handled correctly.
- Memory, file descriptors, and other resources MUST be released on all paths.
- The implementation MUST remain robust under high system CPU load.

---

# Engineering Standards

## Language and Portability

- Code MUST be written in C and conform to C89.
- Implementations MUST use C89 and POSIX.1-2001 APIs where possible.
- GNU extensions MAY be used only when required functionality cannot be
  achieved with C89 + POSIX.1-2001.
- Code MUST compile as C89 and all later C standards, and as C++98 and all
  later C++ standards, without warnings/errors.
- Undefined behavior MUST be avoided.
- Implementation-defined behavior SHOULD be minimized and documented when relied
  upon.
- The code MUST NOT depend on a specific libc implementation.
- Portable solutions MUST be preferred when feasible.
- The program MUST build and run correctly on Linux, macOS, and FreeBSD.
- The code MUST NOT assume `/proc` exists without platform guards.

## Platform-Specific Code

- Platform-specific code MUST be enclosed by clear conditional compilation.
- Platform-specific blocks MUST document:
  - why specialization is required,
  - supported platform(s),
  - any unavoidable behavioral differences.
- Observable cross-platform behavior MUST remain functionally equivalent.
- If exact equivalence is impossible due to OS constraints, differences MUST be
  minimized and documented.

## Safety and Correctness

- Integer narrowing MUST be explicit where it can occur.
- Signed/unsigned interactions MUST be handled carefully.
- Integer overflow and unsafe pointer arithmetic MUST be prevented.
- All allocations MUST be checked for failure.
- Return values MUST be checked whenever they affect correctness, behavior, or
  guarantees.
- Errors MUST be reported to `stderr` with relevant context.
- `errno` SHOULD be preserved when required for accurate diagnostics.
- Fatal errors MUST produce non-zero exit status.
- Recoverable errors SHOULD be handled gracefully.
- Permission-denied and similar OS errors MUST be handled explicitly.

## Signals and Concurrency

- Signal handlers MUST use only async-signal-safe operations.
- Shared state accessed from handlers MUST use `volatile sig_atomic_t` or
  equivalent safe synchronization.
- Termination signaling paths MUST avoid leaving target processes stopped.
- Timing logic MUST use monotonic sources and remain robust to wall-clock
  changes.

## Documentation and Style

- Public structs, struct members, macros, and functions MUST be documented.
- Complex algorithms and non-obvious logic SHOULD include explanatory comments.
- Public API declarations and definitions MUST stay semantically synchronized.
- Comments MUST use consistent C-style block formatting (`/* ... */`).
- Formatting MUST follow `/.clang-format`.
- Standalone `{ ... }` blocks used only for scoping MUST NOT be used.
- To limit scope in C89, variables MUST be declared at the start of the
  nearest existing control-flow block; if none exists, they MUST be declared at
  function scope.
- Variable scope MUST be minimized without introducing standalone scope blocks.
- Unnecessary headers MUST NOT be included.
- Source lines SHOULD stay within 80 columns when practical.
- Long string literals MUST NOT be split only to satisfy line length.
- `assert` arguments MUST be simple expressions (no function or macro calls).

---

# Build, Test, and Analysis Requirements

## Required Tooling

- Required tools MUST include: `gcc`, `clang`, `make`, `cmake`,
  `clang-format`, `cppcheck`, `clang-tidy`, `valgrind`.
- On Ubuntu, required tools SHOULD be installed with:
  - `sudo apt-get update && sudo apt-get -qqy install build-essential clang-format cppcheck clang-tidy valgrind`

## Build Requirements

- Before submission, builds MUST succeed with both compilers:
  - `rm -rf build && cmake -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Release -B build && cmake --build build --target all`
  - `rm -rf build && cmake -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Release -B build && cmake --build build --target all`
- New warnings introduced by a change MUST be resolved before submission.

## Test and Analysis Requirements

For each compiler (`gcc`, then `clang`), checks MUST run in this order:

1. The workflow MUST clean and build the default target.
2. The workflow MUST run static checks:
   - `cmake --build build --target check`
   - The workflow MUST review:
      - `src/cppcheck-report.txt`
      - `src/clang-tidy-report.txt`
      - `tests/cppcheck-report.txt`
      - `tests/clang-tidy-report.txt`
3. The workflow MUST run dynamic checks:
   - `cmake --build build --target valgrind`

Additional requirements MUST be enforced:

- New features MUST include tests.
- Bug fixes MUST include regression tests.
- Tests for changed behavior MUST be deterministic and non-flaky.
- Unit/integration tests MUST live in `tests/cpulimit_test.c` unless there is a
  documented reason to create another file.
- Changes MUST NOT reduce coverage of the modified behavior.

---

# Change Management

## Review and Refactoring Expectations

- Correctness and safety MUST be prioritized over cosmetic improvements.
- Clearly related issues discovered in touched code SHOULD be addressed in the
  same change; if deferred, the reason SHOULD be documented.
- Behavior changes, refactors, formatting, and test additions SHOULD be
  separated into logically independent commits where feasible.
- Each commit SHOULD remain buildable and testable.
- Refactoring MUST NOT change observable behavior unless explicitly intended and
  documented.

## Continuous Integration

- Every pull request MUST pass build, tests, static analysis, and dynamic
  analysis checks.
- Pull requests with failing required checks MUST NOT be merged.

## Commit Policy

- Commit messages MUST:
  - be in English,
  - use ASCII only,
  - follow Conventional Commits,
  - keep subject length within 50 characters,
  - wrap body lines at 72 characters.
- One commit SHOULD represent one logical change.
- Breaking changes MUST be clearly identified.

---

# Repository Structure (Reference)

## Top-Level

- `/src`: production source code
- `/tests`: tests and test helpers
- `/cmake`: CMake helper scripts
- `/CMakeLists.txt`: top-level CMake configuration
- `/Makefile`: legacy Make build
- `/build_with_cmake.sh`: release build helper
- `/.clang-format`: formatting configuration
- `/.clang-tidy`: static analysis configuration
- `/README.md`: user documentation
- `/AGENTS.md`: agent/development policy

## Source Modules (`/src`)

- `main.c`: entry point and top-level control flow
- `cli.[ch]`: CLI parsing and config creation
- `limiter.[ch]`: high-level limiting orchestration
- `limit_process.[ch]`: enforcement loop (`SIGSTOP` / `SIGCONT`)
- `process_group.[ch]`: tracked-process set and descendant handling
- `process_finder.[ch]`: PID/executable target resolution
- `process_iterator.h`: process iteration API
  - `process_iterator_linux.c`: Linux `/proc`
  - `process_iterator_freebsd.c`: FreeBSD `libkvm`
  - `process_iterator_apple.c`: macOS `libproc` + `sysctl`
- `process_table.[ch]`: PID lookup table
- `list.[ch]`: generic doubly linked list
- `signal_handler.[ch]`: termination/TTY signal handling
- `time_util.[ch]`: monotonic time and sleep helpers
- `util.[ch]`: general utility helpers

## Tests (`/tests`)

- `cpulimit_test.c`: main test suite
- `busy.c`: pthread-based CPU load helper
- `multi_process_busy.c`: fork-based multi-process load helper

## CMake Helpers (`/cmake`)

- `CpulimitOptions.cmake`: compiler flags and platform libs
- `CheckedFlags.cmake`: validated flag application
- `RunCheck.cmake`: cppcheck + clang-tidy runner
- `RunFormat.cmake`: clang-format runner
- `RunValgrind.cmake`: valgrind runner
- `Uninstall.cmake`: uninstall target logic
