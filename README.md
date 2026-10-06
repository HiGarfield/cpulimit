# CPULIMIT

Cpulimit limits a process's CPU usage to a percentage you choose, by sending
`SIGSTOP` to pause it and `SIGCONT` to resume it as needed. With `-i` the
limit covers the process and all of its descendants.

Cpulimit works on Linux, macOS, and FreeBSD. It was originally developed by
[Angelo Marletta](https://github.com/opsengine/cpulimit); this fork is
maintained by [HiGarfield](https://github.com/HiGarfield/cpulimit) and adds bug
fixes and improvements. Prebuilt binaries are published on
[Releases](https://github.com/HiGarfield/cpulimit/releases/latest).

## Installation

Pick **one** of the four methods below. You do not need more than one.

### 1. Prebuilt binary

Download the archive for your platform from
[Releases](https://github.com/HiGarfield/cpulimit/releases/latest), then:

  ```sh
  sudo install -d /usr/local/bin
  sudo install -m 755 cpulimit-* /usr/local/bin/cpulimit
  ```

### 2. `make` / `gmake`

Requires a C compiler.

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

### 3. `cmake` (version 3.5 or higher)

  ```sh
  rm -rf build
  mkdir -p build
  cd build
  cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
  cmake --build .
  sudo cmake --build . --target install
  ```

### 4. `gcc` / `clang` only (no `make` / `cmake`)

Compile all sources directly and install. `gcc` and `clang` are
interchangeable; only the trailing link flag differs per platform
(`-lrt` on Linux, `-lproc` on macOS, `-lkvm` on FreeBSD).

  ```sh
  # Linux
  gcc -O2 -D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64 src/*.c -o cpulimit -lrt

  # macOS
  clang -O2 -D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64 src/*.c -o cpulimit -lproc

  # FreeBSD
  gcc -O2 -D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64 src/*.c -o cpulimit -lkvm

  # install (any platform)
  sudo install -d /usr/local/bin
  sudo install -m 755 cpulimit /usr/local/bin/cpulimit
  ```

## Usage

  ```sh
  cpulimit OPTION... TARGET
  ```

### Options

| Option                  | Description                                     |
| ----------------------- | ----------------------------------------------- |
| -l LIMIT, --limit=LIMIT | CPU percentage limit, range (0, N_CPU*100]      |
| -v, --verbose           | show control statistics                         |
| -z, --lazy              | exit if the target process is not running       |
| -i, --include-children  | limit total CPU usage of target and descendants |
| -h, --help              | display this help message and exit              |

**`-l LIMIT` is required.** It is a percentage of **one CPU core**: `-l 50`
is half a core, `-l 100` is one fully used core, and the maximum is
`number_of_cores * 100` — `-l 250` on a 4-core machine allows about two and a
half cores.

### Choosing a target

Exactly one target must be given:

| Target              | Description                                      |
| ------------------- | ------------------------------------------------ |
| -p PID, --pid=PID   | PID of the target process (implies -z)           |
| -e FILE, --exe=FILE | executable name or path                          |
| COMMAND [ARG]...    | run the command and limit CPU usage (implies -z) |

With `-e`, cpulimit matches the name each process was started with, not the
path of the executable on disk:

- An argument starting with `/` is compared in full, so `-e /usr/bin/myapp`
  does not match a process started as `myapp`.
- Any other argument is compared by name only, so `-e myapp` matches
  `/usr/bin/myapp` and `./dir/myapp` alike.
- An argument with an empty name (`-e /`, `-e bin/`) is rejected with
  `invalid match name`.

When several processes match, the topmost ancestor wins; if the matches are
unrelated, the smaller PID wins. A match cpulimit cannot signal loses to any
match it can control.

### Examples

  ```sh
  # Limit process 1234 to 50% of one core.
  cpulimit -l 50 -p 1234

  # Limit every process named "myapp" to 50% of one core.
  cpulimit -l 50 -e myapp

  # Run "myapp --option" limited to 50% of one core.
  cpulimit -l 50 -- myapp --option

  # Limit "myapp" and its children to 50% of one core.
  cpulimit -l 50 -i -e myapp

  # Run ffmpeg and its children, limited to 200% (two cores), with stats.
  cpulimit -l 200 -v -i -- ffmpeg -i in.mkv -c:v libx264 out.mp4
  ```

## Things to keep in mind

- **Children are only included with `-i`.**
- **A suspended process makes no progress**: `SIGSTOP` freezes it, holding its
  locks and serving no I/O until `SIGCONT` arrives, so interactive and networked
  services gain latency.
- **Very short-lived children can be missed** and then run unthrottled.
- **Cpulimit can only control processes you own.**
- **PID 1 (init) is never limited.** Suspending it would freeze or crash the
  system, so there is no exception on any platform.
- **`-p` and command mode exit when the target stops.** Use `-z` (or `-p`,
  which implies it) when the run should end as soon as the target is gone.
  Without it, `-e` keeps waiting and re-attaches whenever the target
  reappears, so a program that starts later is still limited.
- **Do not kill cpulimit with `SIGKILL`.** For example, `killall -9 cpulimit`,
  `killall -KILL cpulimit`, `kill -9 <cpulimit's PID>`, or
  `kill -KILL <cpulimit's PID>` skip the cleanup that sends `SIGCONT`, so any
  process cpulimit has paused with `SIGSTOP` stays frozen and never resumes.
  Stop cpulimit with `SIGTERM`/`SIGINT` (for example `kill -TERM <cpulimit's
  PID>` or `Ctrl-C`) instead, so it can restore the target to a running state.

## Exit codes

| Exit Code | Description                                              |
| --------- | -------------------------------------------------------- |
| 0         | Success                                                  |
| 1         | Bad args, target not found (-z), internal error          |
| 126       | Command found but not executable (command mode only)     |
| 127       | Command not found (command mode only)                    |
| 128+N     | Command terminated by signal N (command mode only)       |
| any other | In command mode, whatever the command itself exited with |

The three command-mode rows are the shell's own conventions, used when the
command never ran at all. Once it has run, cpulimit steps aside and reports
the command's own status instead: `cpulimit -l 50 -- sh -c 'exit 7'` exits 7.
With `-p` or `-e` there is no command to speak for, so the status is always
cpulimit's own.

## Uninstall

Use the method matching the one you installed with.

### 1. Prebuilt binary

  ```sh
  sudo rm -f /usr/local/bin/cpulimit
  ```

### 2. `make` / `gmake`

- **Linux/macOS with `make`:**

  ```sh
  sudo make uninstall
  ```

- **FreeBSD with `gmake`:**

  ```sh
  sudo gmake uninstall
  ```

### 3. `cmake`

  ```sh
  sudo cmake --build build --target uninstall
  ```

### 4. `gcc` / `clang` only

  ```sh
  sudo rm -f /usr/local/bin/cpulimit
  ```

## For developers

### Run tests

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

### Source code

Source code is available at <https://github.com/HiGarfield/cpulimit>, where bug
reports and feature requests are welcome.
