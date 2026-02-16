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

## Coding Standards (Normative)

### Language

- Code MUST be written in C programming language.
- Code MUST conform to C89 standard.
- Implementations MUST use only C89 language features and library functions. Language features and library functions from later standards SHALL be used only if they are specified in POSIX.1-2001 and are available on all supported operating systems.
- Code MUST compile under C89 and all later C standards.
- Code MUST compile under C++98 and all later C++ standards.
- Undefined behavior and implementation-defined behavior MUST be avoided.

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
