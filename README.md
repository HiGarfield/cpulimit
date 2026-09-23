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
>   a process started as `./dir2/myapp` or `/usr/bin/myapp`.
>
> An argument whose basename is empty — `-e /`, `-e //`, `-e bin/`,
> `-e a/b/`, `-e /tmp/` — can never select a process and is rejected
> immediately with `invalid match name`.
>
> **Selecting among multiple matches** — when more than one running process
> matches, cpulimit applies the following rules while iterating over all
> processes:
>
> - A newly found matching process replaces the current candidate **only
>   if the current candidate is a descendant** of the new process.
> - If neither process is an ancestor of the other, the match with the
>   **smaller PID** replaces the current candidate, which makes the choice
>   deterministic and independent of the platform's iteration order.
>
> This means that among all matching processes, **the topmost ancestor is
> always selected**: if processes A → B → C all match (A is the root
> ancestor), A wins regardless of iteration order. If two matching
> processes are unrelated (neither is an ancestor of the other), the one
> with the smaller PID wins. After the scan, the selected process is
> rechecked for existence; if it has exited in the meantime, cpulimit
> **falls back to the best surviving candidate by the same rule**
> (ancestor first, otherwise the smaller PID) instead of reporting the
> target as not found.
>
> **Controllability outranks both rules.** A match cpulimit cannot signal
> — one that belongs to another user, is filtered out by seccomp, or lives
> in another PID namespace — loses to any match it *can* control, even when
> it would otherwise win as the ancestor or as the smaller PID. cpulimit
> reports `No permission to control process <pid>` only when **every**
> surviving match is uncontrollable; as long as one match can be limited, that
> one is used. In non-lazy mode that report repeats while the search goes on,
> exactly as it does for a target that is missing altogether: the process
> currently wearing the name may be replaced by one cpulimit is allowed to
> limit, and the replacement has to be picked up.
>
> _Example:_ If a process `myapp` spawns a child process also named
> `myapp`, `-e myapp` selects the parent process (the ancestor), not the
> child — unless signalling that parent fails with `EPERM`, in which case
> `-e myapp` limits the child instead of refusing to start.

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

| Exit Code | Description                                                 |
| --------- | ----------------------------------------------------------- |
| 0         | Success                                                     |
| 1         | Error (invalid arguments, target not found in a mode that does not wait, internal error) |
| 126       | Command found but not executable (command mode only)        |
| 127       | Command not found (command mode only)                       |
| 128+N     | Command terminated by signal N (command mode only)          |

If the process scan fails while the control loop is running, limiting stops
and the target is no longer limited from that point on. Command mode and lazy
mode (`-p`, or `-e` together with `-z`) have no second chance and exit with
code 1. Non-lazy mode (`-e` without `-z`) re-resolves the target and re-attaches
instead, and it keeps doing so for as long as it runs.

That is what non-lazy means here: the mode exists for a process whose start
time cannot be known in advance, so waiting for it is the point rather than a
cost. Nothing about the target ends the watch -- not being started yet, having
exited (whether it was running or suspended when it did), reappearing on a
recycled PID, refusing every signal, or a scan that fails mid-attempt --
because in every one of those cases the target can come back, and it has to be
limited again when it does. Only two things end it, and both are problems with
the run rather than with the target: a failure of the scanning machinery
(allocation, clock or process-iterator initialisation), after which there is
nothing left to search with, and an attempt that left a member stopped, which
needs `kill -CONT` by hand instead of another attempt. A termination signal
ends the whole run, as it does in any mode: when that signal came from the
keyboard (Ctrl+C or Ctrl+\ on a terminal) and both standard streams are
terminals, the run ends its line first, so the shell prompt does not start on
the same line as the interrupt echo -- whichever point of the run it was at.

Use `-z` -- or `-p`, which implies it -- when the run should end as soon as
the target is gone.

While it waits, the message saying why the last attempt limited nothing
repeats on every attempt (`Process 'NAME' cannot be found, retrying...`,
`No permission to control process PID, retrying...`, once per two-second
wait), because that is the only sign of life a waiting run gives. The scan
diagnostic is reported once per streak instead, so a run whose scans keep
failing does not print the same line on every attempt and bury whatever
follows it -- including the hints that name a process left stopped. A streak
ends when a run limits to completion, which is when that diagnostic becomes
due again.

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

Contributions to cpulimit are welcome, including bug fixes, new features, or support for additional operating systems. Please submit pull requests to the `master` branch and ensure all tests pass before merging. (`develop` is a mirror that CI keeps in sync with `master`, not an integration branch.)
