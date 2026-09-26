# CPULIMIT

Cpulimit restricts the CPU usage of a process to a percentage you choose, by
sending `SIGSTOP` and `SIGCONT` to it. With `-i` the limit covers the process
and all of its descendants.

Cpulimit works on Linux, macOS, and FreeBSD. It was originally developed by
[Angelo Marletta](https://github.com/opsengine/cpulimit); this fork is
maintained by [HiGarfield](https://github.com/HiGarfield/cpulimit) and adds bug
fixes and improvements. Prebuilt binaries are published on
[Releases](https://github.com/HiGarfield/cpulimit/releases/latest).

## Limiting CPU Usage

### How it works

Cpulimit measures the target's CPU usage and pauses it (`SIGSTOP`) or resumes
it (`SIGCONT`) so that the average usage matches the limit. Nothing is reniced
or rescheduled.

### Setting the limit

  ```sh
  cpulimit OPTION... TARGET

  ```

**`-l LIMIT` is required**, and it is a percentage of **one CPU core**: `-l 50`
is half a core, `-l 100` is one fully used core, and the maximum is
`number_of_cores * 100` — `-l 250` on a 4-core machine allows about two and a
half cores.

| Option                  | Description                                     |
| ----------------------- | ----------------------------------------------- |
| -l LIMIT, --limit=LIMIT | CPU percentage limit, range (0, N_CPU*100]      |
| -v, --verbose           | show control statistics                         |
| -z, --lazy              | exit if the target process is not running       |
| -i, --include-children  | limit total CPU usage of target and descendants |
| -h, --help              | display the help message and exit               |

Exactly one target must be given:

| Target              | Description                                       |
| ------------------- | ------------------------------------------------- |
| -p PID, --pid=PID   | PID of the target process (implies -z)            |
| -e FILE, --exe=FILE | executable name or path (matched against argv[0]) |
| COMMAND [ARG]...    | run the command and limit CPU usage (implies -z)  |

Examples:

  ```sh
  # -l 50: limit to 50% of one core
  # -p 1234: limit process by PID
  cpulimit -l 50 -p 1234

  # -l 50: limit to 50% of one core
  # -e myapp: limit by executable name
  cpulimit -l 50 -e myapp

  # -l 50: limit to 50% of one core
  # --: end of options
  # command: myapp --option
  cpulimit -l 50 -- myapp --option

  # -l 50: limit to 50% of one core
  # -i: include child processes
  # -e myapp: limit by executable name
  cpulimit -l 50 -i -e myapp

  # -l 200: limit to 200% of one core
  # -v: show statistics
  # -i: include child processes
  # --: end of options
  # command: ffmpeg -i in.mkv -c:v libx264 out.mp4
  cpulimit -l 200 -v -i -- ffmpeg -i in.mkv -c:v libx264 out.mp4

  ```

### Things to keep in mind

- **The limit is an average, not a hard cap.** Bursts can exceed it; for
  guaranteed quotas use cgroups or a container runtime.
- **Children are only included with `-i`.**
- **A suspended process makes no progress**: it holds its locks and serves no
  I/O while stopped, so interactive and networked services gain latency.
- **Very short-lived children can be missed** and then run unthrottled.
- **Cpulimit can only control processes you own.**

## Choosing a Target

`-e` matches each process's **`argv[0]`** — the command string it was launched
with — not the resolved path of the executable on disk.

- An argument starting with `/` is compared in full, so `-e /usr/bin/myapp`
  does not match a process started as `myapp`.
- Any other argument is compared by basename only, so `-e myapp` matches
  `/usr/bin/myapp` and `./dir/myapp` alike.
- An argument with an empty basename (`-e /`, `-e bin/`) is rejected with
  `invalid match name`.

When several processes match, the topmost ancestor wins; if the matches are
unrelated, the smaller PID wins. A match cpulimit cannot signal loses to any
match it can control.

## Exit Codes

| Exit Code | Description                                            |
| --------- | ------------------------------------------------------ |
| 0         | Success                                                |
| 1         | Bad args, target not found (nowait), internal error    |
| 126       | Command found but not executable (command mode only)   |
| 127       | Command not found (command mode only)                  |
| 128+N     | Command terminated by signal N (command mode only)     |

Use `-z` — or `-p`, which implies it — when the run should end as soon as the
target is gone. Without it, `-e` keeps waiting and re-attaches whenever the
target reappears, so a program that starts later is still limited.

A run that did not limit its target to completion reports which of three things
happened: `CPU limit could not be applied` (nothing was throttled — check
permissions and how the target was named), `CPU limiting stopped early` (the
command ran unthrottled from that point on), or `left stopped` (the target
stayed suspended — release the PIDs named above it with `kill -CONT`).

## Installation

### Prebuilt Binary

Download the archive for your platform from
[Releases](https://github.com/HiGarfield/cpulimit/releases/latest), then:

  ```sh
  sudo mkdir -p /usr/local/bin
  sudo cp -f cpulimit-* /usr/local/bin/cpulimit
  sudo chmod 755 /usr/local/bin/cpulimit
  ```

### From Source

Requires a C compiler. Use **one of** the following methods:

- **Linux/macOS with `make`:**

  ```sh
  make
  sudo make install
  ```

- **FreeBSD with `gmake`:**

  ```sh
  gmake
  sudo gmake install
  ```

- **Linux/macOS/FreeBSD with `cmake` (version 3.5 or higher):**

  ```sh
  rm -rf build
  mkdir -p build
  cd build
  cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
  cmake --build .
  sudo cmake --build . --target install
  ```

## Uninstall

Use the method matching the one you installed with.

### Prebuilt Binary

  ```sh
  sudo rm -f /usr/local/bin/cpulimit
  ```

### From Source

- **Linux/macOS with `make`:**

  ```sh
  sudo make uninstall
  ```

- **FreeBSD with `gmake`:**

  ```sh
  sudo gmake uninstall
  ```

- **Linux/macOS/FreeBSD with `cmake`:**

  ```sh
  sudo cmake --build build --target uninstall
  ```

## Run Tests

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

## Source Code

Source code is available at <https://github.com/HiGarfield/cpulimit>, where bug
reports and feature requests are welcome.
