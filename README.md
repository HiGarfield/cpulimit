# CPULIMIT

Cpulimit is a tool that limits the CPU usage of a process (expressed in percentage, not in CPU time). It is useful for controlling batch jobs when you don't want them to consume too many CPU cycles. The goal is to prevent a process from running for more than a specified time ratio. It does not change the nice value or other scheduling priority settings, but instead focuses on the real CPU usage. Additionally, it adapts dynamically and quickly to the overall system load.

Cpulimit is supported on Linux, macOS, and FreeBSD.

The control of CPU usage is achieved by sending `SIGSTOP` and `SIGCONT` POSIX signals to processes. If the `-i` option is enabled, all descendant processes of the specified process will share the same percentage of CPU usage.

Developed by [Angelo Marletta](https://github.com/opsengine/cpulimit). Please send your feedback, bug reports, feature requests, or just thanks.

This forked repository is maintained by [HiGarfield](https://github.com/HiGarfield/cpulimit). **This repository has undergone extensive bug fixes and code improvements based on the original repository.**

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

### Uninstall

- On Linux/macOS:

  ```sh
  sudo make uninstall
  ```

- On FreeBSD:

  ```sh
  sudo gmake uninstall
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

You are welcome to contribute to cpulimit with bug fixes, new features, or support for a new OS. If you want to submit a pull request, please do it on the `develop` branch and ensure that all tests pass.
