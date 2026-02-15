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

- Code MUST conform to C89.
- Features introduced after C89 MUST NOT be used.
- Code SHOULD compile with a C++ compiler.
- Undefined behavior defined by the C89 standard MUST be avoided.

### Extensions

- POSIX or GNU extensions MAY be used when required.

### Portability

- A portable solution SHOULD be preferred when one exists.
- The program MUST build and run correctly on Linux, macOS, and FreeBSD.

### Platform-Specific Code

- Platform-specific implementations MAY be used only if no portable solution exists.
- Such code MUST be enclosed in conditional compilation blocks.

### Documentation

- Every struct MUST have a documentation comment.
- Every struct member MUST have a documentation comment.
- Every macro MUST have a documentation comment.
- Every function MUST have a documentation comment.
- Non-trivial logic inside functions SHOULD be commented.
- Comments MUST use C-style block syntax (`/* ... */`).
- Comment style MUST be consistent across the codebase.
- If a function is declared in a header and defined in a source file, the documentation comments MUST be identical.

### Linkage

- Functions used only within their defining source file MUST be declared `static`.

### Character Set

- Code and comments MUST be written in English.
- Only ASCII characters are permitted.

### Style

- Formatting MUST follow `/.clang-format`.
