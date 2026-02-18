# Project Overview

cpulimit is a utility that limits the CPU usage of a process to a specified percentage.
It enforces the limit by monitoring CPU consumption and sending the POSIX signals SIGSTOP and SIGCONT.

If `-i` or `--include-children` is specified, the limit MUST also apply to all descendant processes.

Language: C
C standard: C89
Supported platforms: Linux, macOS, FreeBSD

## Detailed Usage

See `/README.md`.

## Repository Structure

- `/src`: main source code.
- `/tests`: test code.

## Dev environment tips
- `gcc` MUST be installed to build and test the project.
- `clang` MUST be installed to build and test the project.
- `make` MUST be installed to build and test the project.
- `clang-format` MUST be installed to format the codebase.
- `bear` MUST be installed to generate `compile_commands.json` for static analysis tools.
- `cppcheck` MUST be installed to perform static analysis on the codebase.
- `clang-tidy` MUST be installed to perform static analysis on the codebase.
- `valgrind` MUST be installed to perform dynamic analysis on the codebase.

## Coding Standards

### Language

- Code MUST be written in C programming language.
- Code MUST conform to C89 standard.
- Code must be implemented using functions defined in the C89 standard and the POSIX.1-2001 standard.
- Implementations MAY supplement with GNU extensions only when the required functionality cannot be achieved using only the above standards.
- Code MUST NOT use any other non-standard libraries or compiler-specific features.
- Code MUST compile under C89 and all later C standards.
- Code MUST compile under C++98 and all later C++ standards.
- Undefined behavior and implementation-defined behavior MUST be avoided.
- Code MUST NOT depend on any specific C library implementation.

### Extensions

- POSIX or GNU extensions MAY be used when required.

### Portability

- A portable solution SHOULD be used if one exists.
- The program MUST build on Linux.
- The program MUST build on macOS.
- The program MUST build on FreeBSD.
- The program MUST run correctly on all supported platforms.
- The program's behavior MUST be consistent across all supported platforms.

### Platform-Specific Code

- Platform-specific implementations MAY be used only when no portable solution exists.
- Such code MUST be enclosed within conditional compilation blocks.

### Documentation

- Every struct MUST have a documentation comment.
- Every struct member MUST have a documentation comment.
- Every macro MUST have a documentation comment.
- Every function MUST have a documentation comment.
- Non-trivial logic inside functions SHOULD be commented.
- Comments MUST use C89-style block syntax (`/* ... */`).
- Comment style MUST be consistent across the codebase.
- If a function is declared in a header and defined in a source file, the documentation comments MUST be identical.
- In a multi-line tagged documentation comment, each tag's continuation lines MUST align with the first character of its description, independently for each tag.

### Linkage

- Functions that are not referenced outside their defining source file MUST be declared `static`.

### Character Set

- Code and comments MUST be written in English.
- Only ASCII characters MAY appear.

### Style

- Formatting MUST follow `/.clang-format`.
- Code MUST be formatted using `clang-format` with the provided configuration.
- The command `make format` SHALL be used to format the codebase.

## Building

- The project MUST be built using `make CHECK=1` without any warnings or errors.
- gcc and clang MUST be tested using `make CHECK=1 CC=gcc` and `make CHECK=1 CC=clang` to build the project without any warnings or errors.

## Testing

- Unit tests MUST be implemented for all public functions.
- Unit tests MUST be implemented in `tests/cpulimit_test.c`.
- Unit tests MUST be run using `tests/cpulimit_test` without any errors.
- `clang-tidy-report.txt` and `cppcheck-report.txt` MUST be generated in `src` and `tests` directories by `make check` and MUST contain no warnings or errors.
- Dynamic analysis MUST be performed with the command `valgrind --tool=memcheck --leak-check=full tests/cpulimit_test` without any errors or memory leaks.
