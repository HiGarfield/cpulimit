# CPULIMIT

Cpulimit is a utility that restricts a process's CPU usage by percentage. It is especially useful for managing batch jobs or other processes that should not consume excessive CPU resources. Instead of relying on nice values or scheduling priorities, cpulimit actively monitors CPU usage and enforces limits by sending `SIGSTOP` and `SIGCONT` POSIX signals to the target process. When the `-i` or `--include-children` option is used, it applies the limit to the process and all of its child processes.

Cpulimit works on Linux, macOS, and FreeBSD.

Originally developed by [Angelo Marletta](https://github.com/opsengine/cpulimit). You are encouraged to provide feedback, report bugs, request features, or show appreciation.

This fork, maintained by [HiGarfield](https://github.com/HiGarfield/cpulimit), includes significant improvements and bug fixes compared to the original version.

Prebuilt binaries for major platforms are available in [Releases](https://github.com/HiGarfield/cpulimit/releases/latest).

## Usage

  ```sh
  cpulimit OPTION... TARGET
  ```

- Options:

 | Option                  | Description                                     |
 | ----------------------- | ----------------------------------------------- |
 | -l LIMIT, --limit=LIMIT | CPU percentage limit, range (0, N_CPU*100]      |
 | -v, --verbose           | show control statistics                         |
 | -z, --lazy              | exit if the target process is not running       |
 | -i, --include-children  | limit total CPU usage of target and descendants |
 | -h, --help              | display the help message and exit               |

- TARGET **must be exactly one of**:

 | Target              | Description                                       |
 | ------------------- | ------------------------------------------------- |
 | -p PID, --pid=PID   | PID of the target process (implies -z)            |
 | -e FILE, --exe=FILE | executable name or path (matched against argv[0]) |
 | COMMAND [ARG]...    | run the command and limit CPU usage (implies -z)  |

> **Note:** The `-e` option identifies a target by comparing against each
> process's **`argv[0]`** — the command string the process supplied when it
> was launched — not the resolved path of the executable on disk. A process
> started as `./myapp` has `argv[0]` equal to `./myapp`, not
> `/usr/bin/myapp`.
>
> **Matching mode** depends on whether the argument starts with `/`:
>
> - **Absolute path** (starts with `/`, e.g., `-e /usr/bin/myapp`):
>   `argv[0]` of each process is compared **in full** against the given path.
>   Only processes whose `argv[0]` is exactly `/usr/bin/myapp` match.
>   Processes started as `myapp` or `./myapp` will **not** match, even if
>   the executable is the same file.
>
> - **Relative path or plain name** (does not start with `/`,
>   e.g., `-e myapp` or `-e ./dir/myapp`):
>   Only the **basename** of the argument is compared against the
>   **basename** of each process's `argv[0]`; directory components are
>   ignored on both sides. As a result, `-e ./dir1/myapp` also matches
>   a process started as `./dir2/myapp` or `/usr/bin/myapp`. If the
>   basename of the argument is empty (e.g., `-e bin/`), no process will
>   ever match.
>
> **Selecting among multiple matches** — when more than one running process
> matches, cpulimit applies the following rule while iterating over all
> processes:
>
> - A newly found matching process replaces the current candidate **only
>   if the current candidate is a descendant** of the new process.
> - If neither process is an ancestor of the other, the current candidate
>   is kept (the earlier-encountered process wins).
>
> This means that among all matching processes, **the topmost ancestor is
> always selected**: if processes A → B → C all match (A is the root
> ancestor), A wins regardless of iteration order. If two matching
> processes are unrelated (neither is an ancestor of the other), the one
> encountered first during system process iteration wins; that order is
> platform-defined, so the result is **non-deterministic** among those
> unrelated processes.
>
> _Example:_ If a process `myapp` spawns a child process also named
> `myapp`, `-e myapp` selects the parent process (the ancestor), not the
> child.

## Examples

- For the process with PID 1234, limit its CPU usage to 50% of one CPU core:

  ```sh
  cpulimit -l 50 -p 1234
  ```

- For the process named `myapp`, limit its CPU usage to 50% of one CPU core:

  ```sh
  cpulimit -l 50 -e myapp
  ```

- Run the command `myapp --option` and limit its CPU usage to 50% of one CPU core:

  ```sh
  cpulimit -l 50 -- myapp --option
  ```

- For the process named `myapp` and its child processes, limit their total CPU usage to 50% of one CPU core:

  ```sh
  cpulimit -l 50 -i -e myapp
  ```

## Exit Codes

| Exit Code | Description                                                  |
| --------- | ------------------------------------------------------------ |
| 0         | Success                                                      |
| 1         | Error (invalid arguments, target not found, internal error)  |
| 126       | Command found but not executable (command mode only)         |
| 127       | Command not found (command mode only)                        |
| 128+N     | Command terminated by signal N (command mode only)           |

## Get the Latest Source Code

Source code is available at <https://github.com/HiGarfield/cpulimit>.

## Instructions

### Build and Install

To build and install cpulimit from source, use **one of** the following methods:

- **Build and install with `make` on Linux/macOS:**

  ```sh
  make
  sudo make install
  ```

- **Build and install with `gmake` on FreeBSD:**

  ```sh
  gmake
  sudo gmake install
  ```

- **Build and install with `cmake` on Linux/macOS/FreeBSD (CMake version 3.5 or higher):**

  ```sh
  rm -rf build
  mkdir -p build
  cd build
  cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
  cmake --build .
  sudo cmake --build . --target install
  ```

- **Without a build environment:** Use prebuilt executables from [Releases](https://github.com/HiGarfield/cpulimit/releases/latest)
  ```sh
  sudo mkdir -p /usr/local/bin
  sudo cp -f cpulimit-* /usr/local/bin/cpulimit
  sudo chmod 755 /usr/local/bin/cpulimit
  ```

### Uninstall

To uninstall cpulimit, use **one of** the following methods:

- **Uninstall with `make` on Linux/macOS:**

  ```sh
  sudo make uninstall
  ```

- **Uninstall with `gmake` on FreeBSD:**

  ```sh
  sudo gmake uninstall
  ```

- **Uninstall with `cmake` on Linux/macOS/FreeBSD:**

  ```sh
  sudo cmake --build build --target uninstall
  ```

- **Without a build environment:** Remove the executable manually

  ```sh
  sudo rm -f /usr/local/bin/cpulimit
  ```

### Run Tests

Run the tests from the project build directory.

- **Run unit tests:**

  ```sh
  ./tests/cpulimit_test
  ```

- **Test cpulimit with a single process:**

  ```sh
  ./src/cpulimit -l 50 -v -- ./tests/busy
  ```

- **Test cpulimit with child processes:**

  ```sh
  ./src/cpulimit -l 50 -i -v -- ./tests/multi_process_busy
  ```

## Contributions

Contributions to cpulimit are welcome, including bug fixes, new features, or support for additional operating systems. Please submit pull requests to the `develop` branch and ensure all tests pass before merging.
