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

 | Target              | Description                                      |
 | ------------------- | ------------------------------------------------ |
 | -p PID, --pid=PID   | PID of the target process (implies -z)           |
 | -e FILE, --exe=FILE | name or path of the executable                   |
 | COMMAND [ARG]...    | run the command and limit CPU usage (implies -z) |

> **Note:** The input syntax for the `-e` option determines how cpulimit selects processes:
>
> - **Absolute Paths** (e.g., `-e /usr/bin/myapp`) use **exact path matching**:
>   - Matches only processes whose invocation path exactly matches the specified absolute path.
>   - Processes started without an absolute path will not match.
>   - _Example:_ `-e /usr/bin/myapp` will **not** match a process launched as `myapp`, even if the executable resides in `/usr/bin/`.
>
> - **Relative Paths or Filenames** (e.g., `-e ./dir/myapp` or `-e myapp`) use **basename-only matching**:
>   - Only the filename component is compared; directory components are ignored.
>   - Multiple processes in different directories with the same filename will all match.
>   - _Example:_ `-e ./dir1/myapp` also matches `./dir2/myapp`.
>
> - **When Multiple Processes Match**:
>   - cpulimit selects the most ancestral (highest in the process hierarchy) match.
>   - This heuristic prefers the parent or oldest ancestor over child processes.

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

The latest version of the code is available here:

- HiGarfield's Repository: <https://github.com/HiGarfield/cpulimit>

## Instructions

### Build and Install

To build and install cpulimit from source, use **one of** the following methods:

- **Build and install with `make` on Linux/macOS:**

  ```sh
  make  # For older compilers, try: make NOFLAGS=1
  sudo make install
  ```

- **Build and install with `gmake` on FreeBSD:**

  ```sh
  gmake  # For older compilers, try: gmake NOFLAGS=1
  sudo gmake install
  ```

- **Build and install with `cmake` on Linux/macOS/FreeBSD (CMake version 3.15 or higher):**

  ```sh
  rm -rf build
  cmake -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build
  sudo cmake --install build --prefix /usr/local
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

Run the following commands in the directory where the project was built.

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
