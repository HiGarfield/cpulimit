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
- Code MUST NOT use features introduced after C89.
- Code MUST remain compatible with C++ compilers where practical.
- Code MUST avoid undefined behavior as defined by the C89 standard.

### Extensions

- POSIX or GNU extensions MAY be used when required.
- A portable solution SHOULD be preferred when available.

### Portability

- The program MUST build and run correctly on Linux, macOS, FreeBSD.

### Platform-Specific Code

- Platform-specific implementations MAY be used if no portable solution exists.
- Such code MUST be protected by conditional compilation.

### Documentation

- Every struct MUST have a documentation comment.
- Every struct member MUST have a documentation comment.
- Every macro MUST have a documentation comment.
- Every function MUST have a documentation comment.
- Non-trivial logic inside functions SHOULD be commented.
- Comments MUST use C89 comment syntax (`/* ... */`).
- Comment style MUST be consistent across the codebase.

### Linkage

- Functions used only within their defining source file MUST be declared `static`.

### Character Set

- Code and comments MUST use English.
- Code and comments MUST contain ASCII characters only.

### Style

- Formatting MUST follow `/.clang-format`.
