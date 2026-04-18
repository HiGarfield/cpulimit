# Project Overview

cpulimit is a utility that limits the CPU usage of a process to a specified
percentage. It enforces the limit by monitoring CPU consumption and sending
the POSIX signals SIGSTOP and SIGCONT.

If `-i` or `--include-children` is specified, the limit MUST also apply to
all descendant processes.

Language: C
C standard: C89
Supported platforms: Linux, macOS, FreeBSD

---

# Design Goals

- The program MUST be lightweight and have minimal runtime overhead.
- The program MUST NOT busy-wait unnecessarily.
- The program MUST NOT introduce noticeable latency to the system scheduler.
- The program MUST behave deterministically under identical conditions.
- The program MUST fail gracefully with meaningful error messages.
- The program MUST NOT crash under invalid input or runtime errors.
- The program MUST avoid race conditions when monitoring or signaling processes.
- The program MUST avoid zombie processes.
- The program MUST properly handle terminated or re-parented child processes.
- The program MUST clean up all resources before exit.

---

# Functional Requirements

- The program MUST accept a target process specified by:
  - PID, or
  - Executable name, or
  - Command line pattern.
- The program MUST validate all user inputs.
- The program MUST reject any invalid inputs.
- The program MUST correctly calculate CPU usage.
- The program MUST periodically sample CPU usage.
- The program MUST suspend and resume the process using SIGSTOP and SIGCONT.
- The program MUST handle SIGINT and SIGTERM gracefully.
- The program MUST restore the target process to a running state before
  exiting.
- The program MUST handle the case where the target process exits during
  monitoring.
- The program MUST handle the case of processes that rapidly spawn and
  terminate children.
- If `-i` or `--include-children` is specified:
  - All descendant processes MUST be detected.
  - Newly spawned children MUST be detected and limited.
  - The implementation MUST avoid missing short-lived children where
    reasonably possible.
- The program MUST return meaningful exit codes:
  - 0 on success.
  - Non-zero on error.
- Exit codes MUST be documented.

---

# Non-Functional Requirements

- The program MUST NOT leak memory.
- The program MUST NOT leak file descriptors or any other resources.
- The program MUST NOT rely on undefined behavior.
- The program MUST NOT assume a specific scheduler implementation.
- The program MUST be thread-safe where threads are used.
- The program SHOULD avoid global mutable state.
- The program MUST handle extremely high CPU load scenarios robustly.

---

# Detailed Usage

See `/README.md`.

---

# Repository Structure

## Top-Level Files and Directories (key entries)

- `/src`: main source code.
- `/tests`: test code and test helper programs.
- `/cmake`: CMake helper scripts used by the build system.
- `/CMakeLists.txt`: top-level CMake build configuration.
- `/Makefile`: legacy Make-based build configuration.
- `/build_with_cmake.sh`: convenience script to configure and build
  with CMake in release mode.
- `/.clang-format`: formatting configuration for `clang-format`.
- `/.clang-tidy`: static analysis configuration for `clang-tidy`.
- `/.gitignore`: specifies files excluded from version control.
- `/.github`: GitHub Actions CI workflow and Copilot instructions.
- `/README.md`: user documentation.
- `/AGENTS.md`: development rules and guidelines for AI agents.
- `/LICENSE`: license file.

## Source Modules (`/src`)

Each module is exposed via a `.h` header and implemented in a `.c`
file (or platform-specific `.c` files). The header comment is the
authoritative description.

- `main.c`: program entry point; parses arguments, sets up signal
  handlers, and dispatches to the appropriate execution mode.
- `cli.h / cli.c`: command-line argument parsing; produces the
  `config` struct consumed by `limiter`.
- `limiter.h / limiter.c`: top-level limiting logic; supports two
  modes: attach-to-existing-PID/exe and fork-and-exec-command.
- `limit_process.h / limit_process.c`: core CPU enforcement loop;
  implements the SIGSTOP/SIGCONT algorithm and manages per-cycle
  sampling and sleep intervals.
- `process_group.h / process_group.c`: manages the set of monitored
  processes (root process plus optional descendants), tracks CPU
  usage with exponential moving average, and detects newly spawned
  children.
- `process_finder.h / process_finder.c`: locates a target process by
  PID or executable name; prefers ancestor processes when multiple
  matches exist.
- `process_iterator.h`: cross-platform process enumeration interface.
  Platform-specific implementations:
  - `process_iterator_linux.c`: Linux implementation using `/proc`.
  - `process_iterator_freebsd.c`: FreeBSD implementation using
    `libkvm` (`kvm(3)` APIs: `kvm_getprocs`, `kvm_getargv`).
  - `process_iterator_apple.c`: macOS implementation using
    `libproc` and `sysctl`.
  Each file is always compiled; it is guarded by a platform `#ifdef`
  and provides a placeholder `typedef` on other platforms.
- `process_table.h / process_table.c`: hash table (separate chaining)
  for O(1) average-case process lookup by PID; supports stale-entry
  removal.
- `list.h / list.c`: generic doubly linked list with O(1) append,
  head/tail access, and field-value search.
- `signal_handler.h / signal_handler.c`: installs handlers for
  SIGINT, SIGQUIT, SIGTERM, SIGHUP, and SIGPIPE; exposes a quit flag
  and TTY-termination detection used by the main loop.
- `time_util.h / time_util.c`: monotonic-clock access, nanosleep
  wrapper, millisecond time-difference calculation, and
  nanosecond-to-`timespec` conversion.
- `util.h / util.c`: utility macros (`MAX`, `MIN`, `CLAMP`), CPU
  core count, process priority adjustment, and basename extraction.

## Test Files (`/tests`)

- `cpulimit_test.c`: main unit and integration test suite; all tests
  MUST be placed here.
- `busy.c`: CPU load generator using pthreads; used by the test suite
  to drive the process under test.
- `multi_process_busy.c`: CPU load generator using `fork`; spawns
  multiple busy-loop child processes to test the `-i` option.

## CMake Scripts (`/cmake`)

- `CpulimitOptions.cmake`: centralises compiler warning flags,
  platform-specific library detection (`librt`, `libkvm`, `libproc`),
  and the shared source-file list.
- `CheckedFlags.cmake`: provides `add_checked_flags()`, which probes
  each flag for compiler acceptance before applying it.
- `RunCheck.cmake`: runs `cppcheck` and `clang-tidy` on `src/` and
  `tests/`, writing four report files used by the `check` target.
- `RunFormat.cmake`: runs `clang-format` in-place on all C source and
  header files; invoked by the `format` target.
- `RunValgrind.cmake`: runs the test binary under `valgrind` and
  fails the build if memory errors or leaks are detected.
- `Uninstall.cmake`: removes installed files listed in CMake's
  install manifest; invoked by the `uninstall` target.

---

# Development Environment

- `gcc` MUST be installed to build and test the project.
- `clang` MUST be installed to build and test the project.
- `make` MUST be installed to build and test the project.
- `cmake` MUST be installed to build and test the project.
- `clang-format` MUST be installed to format the codebase.
- `cppcheck` MUST be installed to perform static analysis on the codebase.
- `clang-tidy` MUST be installed to perform static analysis on the codebase.
- `valgrind` MUST be installed to perform dynamic analysis on the codebase.
- In Ubuntu, the following command MUST be used to install all required tools:
  `sudo apt-get update && sudo apt-get -qqy install build-essential clang-format cppcheck clang-tidy valgrind`
- All required tools MUST be installed before running any build or test step.
- The absence of any required tool is considered a critical error and MUST
  result in an immediate halt.

---

# Coding Standards

## Language

- Code MUST be written in C.
- Code MUST conform to the C89 standard.
- Code MUST be implemented using functions defined in:
  - The C89 standard.
  - The POSIX.1-2001 standard.
- Implementations MAY supplement with GNU extensions only when the required
  functionality cannot be achieved using only the above standards.
- Code MUST NOT use any other non-standard libraries or compiler-specific
  features.
- Code MUST compile under C89 and all later C standards.
- Code MUST compile under C++98 and all later C++ standards.
- Undefined behavior and implementation-defined behavior MUST be avoided.
- Code MUST NOT depend on any specific C library implementation.
- All integer conversions MUST be explicit where narrowing may occur.
- Signed/unsigned comparisons MUST be handled carefully.
- Integer overflow MUST be avoided.
- Pointer arithmetic MUST be safe and validated.
- Double and multi-level pointers passed to the `free` function MUST be
  explicitly cast to `(void *)` to avoid compiler warnings.
- All memory allocations MUST be checked for failure.
- Function return values SHOULD be validated whenever they affect the
  program's correctness, behavior, or guarantees, and MAY be ignored only
  if any resulting error is external and does not affect those properties.
- `(void)` casts MUST NOT be used to silence unused return value warnings
  for functions whose return values do not need to be checked.
- Errors or warnings MUST NOT occur across all compiler optimization levels.

## Extensions

- POSIX.1-2001 extensions MAY be used when required.
- GNU extensions MAY be used only when the required functionality cannot
  be achieved using C89 and POSIX.1-2001 standards.
- Fallback portable implementations SHOULD be provided when feasible.

## Portability

- A portable solution SHOULD be used if one exists.
- The program MUST build on Linux.
- The program MUST build on macOS.
- The program MUST build on FreeBSD.
- The program MUST run correctly on all supported platforms.
- The program's behavior MUST be consistent across all supported platforms.
- Endianness MUST NOT affect correctness.
- The program MUST NOT assume `/proc` exists unless guarded by platform
  checks.
- Platform differences MUST be documented in code comments.

## Platform-Specific Code

- Platform-specific implementations MAY be used only when no portable
  solution exists.
- Such code MUST be enclosed within conditional compilation blocks.
- Each platform-specific block MUST clearly document:
  - The reason for platform specialization.
  - The supported platforms.
- All platform-specific paths MUST be tested on each supported platform.

## Cross-Platform Behavioral Consistency

- The program MUST produce functionally equivalent behavior on all
  supported platforms.
- CPU limiting behavior MUST be consistent across platforms.
- Signal handling behavior MUST be consistent across platforms.
- Process suspension and resumption semantics MUST behave identically
  from a user perspective.
- Error handling behavior and exit codes MUST be identical across platforms.
- Timing behavior MUST NOT rely on platform-specific scheduling quirks.
- Platform-specific implementations MUST NOT introduce observable
  behavioral differences.
- If exact equivalence is impossible due to OS constraints, the difference MUST:
  - Be minimized.
  - Be documented clearly in code comments.
- Any platform-specific workaround MUST preserve the documented
  guarantees of the program.
- When modifying platform-specific code, the behavior on all platforms
  MUST be re-verified to ensure consistency is maintained.

## Documentation

- Every struct MUST have a documentation comment.
- Every struct member MUST have a documentation comment.
- Every macro MUST have a documentation comment.
- Every function MUST have a documentation comment.
- Non-trivial logic inside functions SHOULD be commented.
- Comments MUST use C89-style block syntax (`/* ... */`).
- Comment style MUST be consistent across the codebase.
- If a function is declared and defined in different files, the documentation
  comments MUST be identical.
- Public APIs MUST be clearly distinguished from internal functions.
- All error codes and return values MUST be documented.
- Complex algorithms MUST include high-level explanatory comments.

## Error Handling

- Errors MUST be reported to stderr.
- Error messages MUST be clear and actionable.
- Error messages MUST include relevant context.
- `errno` MUST be preserved where appropriate.
- The program MUST NOT silently ignore errors unless the error is external to
  the program and has no impact on its correctness, behavior, or guarantees.
- Fatal errors MUST cause immediate exit with non-zero status.
- Recoverable errors SHOULD be handled gracefully when possible.

## Resource Management

- Every allocation MUST have a corresponding free.
- Every opened file descriptor MUST be closed.
- All resources MUST be released properly.
- Signal handlers MUST only use async-signal-safe functions.
- Global state modified in signal handlers MUST be of type
  `volatile sig_atomic_t`.
- Cleanup logic MUST be centralized where feasible.
- The program MUST ensure target processes are not left permanently stopped.

## Concurrency and Signals

- Signal handling MUST be implemented carefully.
- Only async-signal-safe functions MAY be used in signal handlers.
- Shared state between signal handlers and main code MUST be protected.
- The program MUST handle all termination signals gracefully.
- The program MUST avoid race conditions.
- Timing logic MUST be robust against system time changes.

## Performance

- CPU usage measurement interval MUST be documented.
- Sampling frequency MUST balance accuracy and overhead.
- The program MUST scale reasonably with many child processes.
- Performance regressions MUST be avoided.

## Security

- The program MUST reject all invalid user inputs or system states.
- The program MUST correctly handle permission-denied errors.

## Identifiers

- Identifiers MUST be self-descriptive and reflect their intended purpose.
- Identifiers referring to the same logical entity MUST use a consistent name
  throughout the codebase.
- All identifiers MUST follow the dominant naming convention used in the file.
- Identifiers MUST NOT conflict with:
  - C language keywords
  - C++ language keywords
  - Standard library symbols
  - Public symbols from core dependencies
- An identifier MUST NOT share the same name as a struct, union, or enum tag.
- Variable shadowing MUST NOT be introduced.
- Identifiers MUST NOT be renamed unless explicitly requested.
- A rename operation MUST be purely syntactic and MUST NOT change program
  behavior.
- All references to a renamed identifier MUST be updated consistently.
- Renaming MUST NOT introduce:
  - Shadowing
  - Symbol collisions
  - Linkage changes
  - Visibility changes
- Renaming MUST NOT be combined with refactoring or logic changes.
- If a safe and complete rename cannot be guaranteed, the rename MUST NOT be
  performed.

## Style

- Formatting MUST follow `/.clang-format`.
- Code MUST be formatted using `clang-format` with the provided configuration.
- The command `make format` or `cmake --build build --target format`
  SHALL be used to format the codebase.
- Code formatting MUST be performed before every commit.
- Standalone `{ ... }` blocks without a control-flow statement MUST NOT be
  used.
- Every non-function `{ ... }` block within a function MUST be preceded by a
  control-flow statement (`if`, `for`, `while`, `switch`, etc.).
- Standalone `{ ... }` blocks introduced purely to limit variable scope are
  strictly prohibited, with no exceptions. To minimize variable scope in
  C89, declare variables at the start of the nearest enclosing block that
  is already required by control flow (e.g., `if` branch, loop body). If no
  suitable enclosing control-flow block exists, declare the variable at the
  start of the innermost enclosing function body.
- Unnecessary headers MUST NOT be included.
- Source code lines SHOULD NOT exceed 80 characters where reasonably possible.
- When a line exceeds 80 characters, string literals MUST NOT be split.
- Adjacent string literal concatenation MUST NOT be used.
- If a string literal exceeds 80 characters, the line MAY exceed the limit.
- The arguments passed to the assert macro MUST be extremely simple
  expressions only.
- Function calls, macro invocations, and other complex expressions MUST NOT
  be used as arguments to the assert macro.
- Variable scope MUST be strictly minimized. Variables' visibility MUST be
  limited to the smallest possible scope.

---

# Building

- cmake-based building MUST be tested with gcc:
  - Run `rm -rf build && cmake -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Release -B build && cmake --build build --target all` and ensure it builds successfully without warnings or errors.
- cmake-based building MUST be tested with clang:
  - Run `rm -rf build && cmake -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Release -B build && cmake --build build --target all` and ensure it builds successfully without warnings or errors.
- All warnings and errors MUST be resolved before submission.

---

# Testing

- Unit tests MUST be implemented for all public functions.
- Unit tests MUST cover:
  - All code paths and branches.
  - All edge cases.
  - All possible error conditions.
  - All possible invalid inputs.
  - All boundary values.
- Unit tests MUST be implemented in `tests/cpulimit_test.c`.
- New features MUST include corresponding tests.
- Bug fixes MUST include regression tests.
- All issues found by static and dynamic analysis MUST be fixed before
  submission.
- Tests MUST be run for both gcc and clang. For each compiler, follow
  these steps in order:
  1. Delete the build directory:
     - Run `rm -rf build`
  2. Configure and build the default target, ensuring it succeeds
     without warnings or errors:
     - For gcc: Run `cmake -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Release -B build && cmake --build build --target all`
     - For clang: Run `cmake -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Release -B build && cmake --build build --target all`
  3. Build the `check` target and inspect every check report file to
     confirm there are no warnings or errors:
     - Run `cmake --build build --target check`
     - Inspect the following report files and ensure each contains no
       warnings or errors:
       - `src/cppcheck-report.txt`
       - `src/clang-tidy-report.txt`
       - `tests/cppcheck-report.txt`
       - `tests/clang-tidy-report.txt`
  4. Build the `valgrind` target and confirm no errors or leaks are
     reported:
     - Run `cmake --build build --target valgrind`
- All test steps MUST NOT be skipped or omitted due to any reason.

---

# Continuous Integration

- Every pull request MUST:
  - Build successfully.
  - Pass all tests.
  - Pass static analysis.
  - Pass dynamic analysis.
- No pull request MAY be merged if checks fail.

---

# Code Review and Refactoring Policy

When performing code review, code fixing, refactoring, cleanup, or
improvement:
- The agent MUST identify as many issues as reasonably possible in a
  single pass.
- The agent MUST fix as many identified issues as reasonably possible in
  a single iteration.
- The agent MUST perform as many independent improvements as reasonably
  possible in a single iteration.
- The agent MUST NOT intentionally fix only one trivial issue if other
  clear issues are visible.
- The agent MUST consider:
  - Correctness
  - Memory safety
  - Undefined behavior
  - Resource leaks
  - Portability
  - Error handling
  - Concurrency safety
  - Signal safety
  - Performance
  - Test coverage
  - Documentation completeness
  - Build warnings
  - Static analysis warnings
  - Dynamic analysis issues
However:
- Each logically independent fix MUST be committed as a separate git commit.
- Each logically independent improvement MUST be committed as a
  separate git commit.
- Refactoring MUST be separated from behavior changes.
- Formatting-only changes MUST be separated from functional changes.
- Test additions MUST be separated from production code changes where
  feasible.
- Commits MUST remain buildable and testable independently.
- Commits MUST NOT introduce new warnings, test failures, or regressions.
- Large changes MUST be decomposed into minimal atomic commits.

---

# Git Commits

- Git commit messages MUST be written in English.
- Only ASCII characters MAY appear in git commit messages.
- Git commit messages MUST follow the conventional commit format.
- Git commit message header MUST NOT exceed 50 characters.
- Git commit message body MUST be wrapped at 72 characters.
- One git commit MUST only solve one issue or implement one feature.
- Git commit messages MUST be descriptive and informative.
- Breaking changes MUST be clearly indicated.
- Refactoring MUST NOT change observable behavior unless necessary and
  documented.
- Commits MUST compile and pass tests independently.
