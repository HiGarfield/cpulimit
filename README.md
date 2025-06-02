# CPULIMIT

Cpulimit is a tool that limits the CPU usage of a process (expressed as a percentage, not CPU time). It is useful for controlling batch jobs that should not consume excessive CPU resources. The tool prevents a process from using more than a specified percentage of CPU capacity. Instead of modifying nice values or scheduling priorities, it operates by monitoring actual CPU usage and dynamically adapts to the overall system load.

Cpulimit is supported on Linux, macOS, and FreeBSD.

CPU usage control is achieved by sending `SIGSTOP` and `SIGCONT` POSIX signals to processes. When the `-i` or `--include-children` option is enabled, cpulimit limits the total CPU usage of the target process and its descendant processes.

Originally developed by [Angelo Marletta](https://github.com/opsengine/cpulimit). Please submit feedback, bug reports, feature requests, or expressions of appreciation.

This forked repository is maintained by [HiGarfield](https://github.com/HiGarfield/cpulimit). **This repository features extensive improvements and bug fixes compared to the original repository.**

Prebuilt binaries for common platforms are available in [Releases](https://github.com/HiGarfield/cpulimit/releases/latest).​

## Usage

  ```sh
  cpulimit [OPTIONS...] TARGET
  ```

- `OPTIONS`:

 | Option                  | Description                                             |
 | ----------------------- | ------------------------------------------------------- |
 | -l LIMIT, --limit=LIMIT | CPU percentage limit from `0` to `N_CPU*100` (required) |
 | -v, --verbose           | show control statistics                                 |
 | -z, --lazy              | exit if the target process is missing or stopped        |
 | -i, --include-children  | limit total CPU usage of target and descendants         |
 | -h, --help              | display the help message and exit                       |

- `TARGET`: **must be exactly one of these**:

 | Target              | Description                                      |
 | ------------------- | ------------------------------------------------ |
 | -p PID, --pid=PID   | PID of the target process (implies -z)           |
 | -e FILE, --exe=FILE | name or path of the executable file              |
 | COMMAND [ARGS]      | run the command and limit CPU usage (implies -z) |

> **Note:** The `-e` option's input syntax determines cpulimit's process selection logic through pattern matching rules:
>
> - **Absolute Paths** (e.g., `-e /usr/bin/myprogram`) trigger **exact path matching**:
>   - Only matches processes whose execution command _exactly contains_ the provided absolute path
>   - Processes invoked without absolute paths remain unmatched
>   - _Example:_ `-e /usr/bin/myprogram` will **NOT** match a process launched directly with `myprogram`, even when the executable resides in `/usr/bin/`
> - **Relative Paths or Filenames** (e.g., `-e ./dir/myprogram` or `-e myprogram`) use **basename-only matching**:
>   - Directory components are ignored in target selection
>   - Potential for ambiguous matches across different directories
>   - _Example:_ `-e ./dir1/myprogram` could match `./dir2/myprogram`
> - **Multiple Eligible Process Handling**: When multiple processes satisfy matching criteria:
>   1. **Initial Candidate Selection**
>      - Process iterator selects the **first matched process** as the initial candidate
>   2. **Ancestry Validation**
>      - Subsequent matches are compared through parent/child relationships:
>      - New match replaces current candidate **only if** it is:
>        - A valid pattern match
>        - An ancestor of the current candidate in the process hierarchy chain
>   3. **Final Process Selection**
>      - Last candidate after full process traversal becomes the selected process

## Get the Latest Source Code

This repository is forked from:

- Original Repository: <https://github.com/opsengine/cpulimit>

The latest available code is here:

- HiGarfield's Repository: <https://github.com/HiGarfield/cpulimit>

## Instructions

### Build and Install

- On Linux/macOS:

  ```sh
  make  # For older compilers, try: make NOFLAGS=1
  sudo make install
  ```

- On FreeBSD:

  ```sh
  gmake  # For older compilers, try: gmake NOFLAGS=1
  sudo gmake install
  ```

- Without a build environment, install directly using the platform-specific binaries from [Releases](https://github.com/HiGarfield/cpulimit/releases/latest):​
  ```sh
  sudo mkdir -p /usr/local/bin
  sudo cp -f cpulimit-* /usr/local/bin/cpulimit
  sudo chmod 755 /usr/local/bin/cpulimit
  ```

### Uninstall

- On Linux/macOS:

  ```sh
  sudo make uninstall
  ```

- On FreeBSD:

  ```sh
  sudo gmake uninstall
  ```

- To uninstall without a build environment:
  ```sh
  sudo rm -f /usr/local/bin/cpulimit
  ```

### Run Tests

- Run unit tests

  ```sh
  ./tests/process_iterator_test
  ```

- Test cpulimit with a single process

  ```sh
  ./src/cpulimit -l 40 -v ./tests/busy
  ```

- Test cpulimit with child processes

  ```sh
  ./src/cpulimit -l 40 -i -v ./tests/multi_process_busy
  ```

## Contributions

Contributions to cpulimit are welcome, whether bug fixes, new features, or support for additional operating systems. When submitting pull requests, please submit them to the `develop` branch and ensure all tests pass successfully.
