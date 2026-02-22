# Project Overview

cpulimit is a utility that limits the CPU usage of a process to a specified percentage.
It enforces the limit by monitoring CPU consumption and sending the POSIX signals SIGSTOP and SIGCONT.

If `-i` or `--include-children` is specified, the limit MUST also apply to all descendant processes.

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
- The program MUST restore the target process to a running state before exiting.
- The program MUST handle the case where the target process exits during monitoring.
- The program MUST handle the case of processes that rapidly spawn and terminate children.
- If `-i` or `--include-children` is specified:
  - All descendant processes MUST be detected.
  - Newly spawned children MUST be detected and limited.
  - The implementation MUST avoid missing short-lived children where reasonably possible.
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

- `/src`: main source code.
- `/tests`: test code.
- `/Makefile`: build configuration.
- `/.clang-format`: formatting configuration.
- `/README.md`: user documentation.
- `/AGENTS.md`: development rules and guidelines.
- `/LICENSE`: license file.

---

# Dev Environment Tips

- `gcc` MUST be installed to build and test the project.
- `clang` MUST be installed to build and test the project.
- `make` MUST be installed to build and test the project.
- `clang-format` MUST be installed to format the codebase.
- `bear` MUST be installed to generate `compile_commands.json` for static analysis tools.
- `cppcheck` MUST be installed to perform static analysis on the codebase.
- `clang-tidy` MUST be installed to perform static analysis on the codebase.
- `valgrind` MUST be installed to perform dynamic analysis on the codebase.

In Ubuntu, the following command SHALL be used:

    sudo apt-get update && sudo apt-get -qqy install build-essential clang-format bear cppcheck clang-tidy valgrind

---

# Coding Standards

## Language

- Code MUST be written in C programming language.
- Code MUST conform to C89 standard.
- Code MUST be implemented using functions defined in:
  - The C89 standard.
  - The POSIX.1-2001 standard.
- Implementations MAY supplement with GNU extensions only when the required functionality cannot be achieved using only the above standards.
- Code MUST NOT use any other non-standard libraries or compiler-specific features.
- Code MUST compile under C89 and all later C standards.
- Code MUST compile under C++98 and all later C++ standards.
- Undefined behavior and implementation-defined behavior MUST be avoided.
- Code MUST NOT depend on any specific C library implementation.
- All integer conversions MUST be explicit where narrowing may occur.
- Signed/unsigned comparisons MUST be handled carefully.
- Integer overflow MUST be avoided.
- Pointer arithmetic MUST be safe and validated.
- All memory allocations MUST be checked for failure.
- All system calls MUST have return values checked.
- Errors or warnings MUST NOT occur across all compiler optimization levels.

## Extensions

- POSIX.1-2001 extensions MAY be used when required.
- GNU extensions MAY be used only when the required functionality cannot be achieved using C89 and POSIX.1-2001 standards.
- Fallback portable implementations SHOULD be provided when feasible.

## Portability

- A portable solution SHOULD be used if one exists.
- The program MUST build on Linux.
- The program MUST build on macOS.
- The program MUST build on FreeBSD.
- The program MUST run correctly on all supported platforms.
- The program's behavior MUST be consistent across all supported platforms.
- Endianness MUST NOT affect correctness.
- The program MUST NOT assume `/proc` exists unless guarded by platform checks.
- Platform differences MUST be documented in code comments.

## Platform-Specific Code

- Platform-specific implementations MAY be used only when no portable solution exists.
- Such code MUST be enclosed within conditional compilation blocks.
- Each platform-specific block MUST clearly document:
  - The reason for platform specialization.
  - The supported platforms.
- All platform-specific paths MUST be tested on each supported platform.

## Cross-Platform Behavioral Consistency

- The program MUST produce functionally equivalent behavior on all supported platforms.
- CPU limiting behavior MUST be consistent across platforms.
- Signal handling behavior MUST be consistent across platforms.
- Process suspension and resumption semantics MUST behave identically from a user perspective.
- Error handling behavior and exit codes MUST be identical across platforms.
- Timing behavior MUST NOT rely on platform-specific scheduling quirks.
- Platform-specific implementations MUST NOT introduce observable behavioral differences.
- If exact equivalence is impossible due to OS constraints, the difference MUST:
  - Be minimized.
  - Be documented clearly in code comments.
- Any platform-specific workaround MUST preserve the documented guarantees of the program.
- When modifying platform-specific code, the behavior on all platforms MUST be re-verified to ensure consistency is maintained.

---

# Comment and Documentation

- Every struct MUST have a documentation comment.
- Every struct member MUST have a documentation comment.
- Every macro MUST have a documentation comment.
- Every function MUST have a documentation comment.
- Non-trivial logic inside functions SHOULD be commented.
- Comments MUST use C89-style block syntax (`/* ... */`).
- Comment style MUST be consistent across the codebase.
- If a function is declared and defined in different files, the documentation comments MUST be identical.
- Public APIs MUST be clearly distinguished from internal functions.
- All error codes and return values MUST be documented.
- Complex algorithms MUST include high-level explanatory comments.

---

# Error Handling

- Errors MUST be reported to stderr.
- Error messages MUST be clear and actionable.
- Error messages MUST include relevant context.
- `errno` MUST be preserved where appropriate.
- The program MUST NOT silently ignore errors unless the error is external to the program and has no impact on its correctness, behavior, or guarantees.
- Fatal errors MUST cause immediate exit with non-zero status.
- Recoverable errors SHOULD be handled gracefully when possible.

---

# Resource Management

- Every allocation MUST have a corresponding free.
- Every opened file descriptor MUST be closed.
- All resources MUST be released properly.
- Signal handlers MUST only use async-signal-safe functions.
- Global state modified in signal handlers MUST be of type `volatile sig_atomic_t`.
- Cleanup logic MUST be centralized where feasible.
- The program MUST ensure target processes are not left permanently stopped.

---

# Concurrency and Signals

- Signal handling MUST be implemented carefully.
- Only async-signal-safe functions MAY be used in signal handlers.
- Shared state between signal handlers and main code MUST be protected.
- The program MUST handle all termination signals gracefully.
- The program MUST avoid race conditions.
- Timing logic MUST be robust against system time changes.

---

# Performance

- CPU usage measurement interval MUST be documented.
- Sampling frequency MUST balance accuracy and overhead.
- The program MUST scale reasonably with many child processes.
- Performance regressions MUST be avoided.

---

# Security Considerations

- The program MUST reject all invalid user inputs or system states.
- The program MUST correctly handle permission-denied errors.

---

# Style

- Formatting MUST follow `/.clang-format`.
- Code MUST be formatted using `clang-format` with the provided configuration.
- The command `make format` SHALL be used to format the codebase.
- Code formatting MUST be performed before every commit.
- Unnecessary headers MUST NOT be included.
- Source code lines SHOULD NOT exceed 80 characters where reasonably possible.
- When a line exceeds 80 characters, string literals MUST NOT be split.
- Adjacent string literal concatenation MUST NOT be used.
- If a string literal exceeds 80 characters, the line MAY exceed the limit.

---

# Building

- The project MUST be built using `make CHECK=1` without any warnings or errors.
- gcc and clang MUST be tested using:
  - `make CHECK=1 CC=gcc`
  - `make CHECK=1 CC=clang`
- Any warnings or errors during the build process MUST be fixed.

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
- Unit tests MUST be run using `tests/cpulimit_test` without any errors.
- The command `make check` MUST be used to perform all checks, including:
  - Run unit tests.
  - Run clang-tidy.
  - Run cppcheck.
- `clang-tidy-report.txt` and `cppcheck-report.txt` MUST be generated in `src` and `tests` directories by `make check` and MUST contain no warnings or errors.
- Dynamic analysis MUST be performed with:

    valgrind --tool=memcheck --leak-check=full tests/cpulimit_test

- No memory leaks or invalid memory accesses are allowed.
- All issues found by static and dynamic analysis MUST be fixed.
- New features MUST include corresponding tests.
- Bug fixes MUST include regression tests.

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

When performing code review, code fixing, refactoring, cleanup, or improvement:
- The agent MUST identify as many issues as reasonably possible in a single pass.
- The agent MUST fix as many identified issues as reasonably possible in a single iteration.
- The agent MUST perform as many independent improvements as reasonably possible in a single iteration.
- The agent MUST NOT intentionally fix only one trivial issue if other clear issues are visible.
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
- Each logically independent improvement MUST be committed as a separate git commit.
- Refactoring MUST be separated from behavior changes.
- Formatting-only changes MUST be separated from functional changes.
- Test additions MUST be separated from production code changes where feasible.
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
- One git commit message MUST only solve one issue or implement one feature.
- Git commit messages MUST be descriptive and informative.
- Breaking changes MUST be clearly indicated.
- Refactoring MUST NOT change observable behavior unless necessary and documented.
- Commits MUST compile and pass tests independently.
