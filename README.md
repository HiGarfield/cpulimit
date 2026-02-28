# CPULIMIT

Cpulimit is a utility that restricts a process's CPU usage by percentage. It is especially useful for managing batch jobs or other processes that should not consume excessive CPU resources. Instead of relying on nice values or scheduling priorities, cpulimit actively monitors CPU usage and enforces limits by sending `SIGSTOP` and `SIGCONT` POSIX signals to the target process. When the `-i` or `--include-children` option is used, it applies the limit to the process and all of its child processes.

Cpulimit works on Linux, macOS, and FreeBSD.

Originally developed by [Angelo Marletta](https://github.com/opsengine/cpulimit). You are encouraged to provide feedback, report bugs, request features, or show appreciation.

This fork, maintained by [HiGarfield](https://github.com/HiGarfield/cpulimit), includes significant improvements and bug fixes compared to the original version.

Prebuilt binaries for major platforms are available in [Releases](https://github.com/HiGarfield/cpulimit/releases/latest).​

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

> **Note:** The input syntax for the `-e` option determines how cpulimit selects processes using pattern matching rules:
>
> - **Absolute Paths** (e.g., `-e /usr/bin/myprogram`) use **exact path matching**:
>   - Matches only processes whose execution command _exactly includes_ the specified absolute path.
>   - Processes started without an absolute path will not match.
>   - _Example:_ `-e /usr/bin/myprogram` will **NOT** match a process launched simply as `myprogram`, even if the executable resides in `/usr/bin/`.
>
> - **Relative Paths or Filenames** (e.g., `-e ./dir/myprogram` or `-e myprogram`) use **basename-only matching**:
>   - Directory components are ignored when selecting targets.
>   - Multiple matches may occur if different directories contain files with the same basename.
>   - _Example:_ `-e ./dir1/myprogram` could also match `./dir2/myprogram`.
>
> - **Handling Multiple Matching Processes**:
>   1. **Initial Candidate Selection**  
>      - The process iterator selects the **first matched process** as the initial candidate.
>   2. **Ancestry Validation**  
>      - Subsequent matches are evaluated based on parent/child relationships.  
>      - A new match replaces the current candidate **only if** it:
>        - Is a valid pattern match, and  
>        - Is an ancestor of the current candidate in the process hierarchy.
>   3. **Final Selection**  
>      - After scanning all processes, the last valid candidate becomes the selected process.

## Examples

- For the process with PID 1234, limit its CPU usage to 50% of one CPU core:

  ```sh
  cpulimit -l 50 -p 1234
  ```

- For the process named `myprogram`, limit its CPU usage to 50% of one CPU core:

  ```sh
  cpulimit -l 50 -e myprogram
  ```

- Run the command `myprogram --option` and limit its CPU usage to 50% of one CPU core:

  ```sh
  cpulimit -l 50 -- myprogram --option
  ```

- For the process named `myprogram` and its child processes, limit their total CPU usage to 50% of one CPU core:

  ```sh
  cpulimit -l 50 -i -e myprogram
  ```

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

- **Uninstall with `cmake` installed executable on Linux/macOS/FreeBSD:**

  ```sh
  sudo xargs rm -f < build/install_manifest.txt
  ```

- **Without a build environment:** Remove the executable manually

  ```sh
  sudo rm -f /usr/local/bin/cpulimit
  ```

### Run Tests

The following examples are demonstrated using the executables built with `make` or `gmake`. If you built with `cmake`, replace both the `./src/` and `./tests/` directories in the commands below with `./build/`.

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
