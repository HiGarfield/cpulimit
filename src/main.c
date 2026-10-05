/*
 * cpulimit - a CPU usage limiter for Linux, macOS, and FreeBSD
 *
 * Copyright (C) 2005-2012  Angelo Marletta
 * <angelo dot marletta at gmail dot com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see
 * <https://www.gnu.org/licenses/>.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "cli.h"
#include "limiter.h"
#include "signal_handler.h"
#include "time_util.h"

#include <stdlib.h>

int main(int argc, char *argv[]) {
    struct cpulimit_cfg cfg;
    int parse_result;
    int status;

    /*
     * Install the handlers before parsing: parse_arguments() prints the
     * usage block on stderr, and a reader that has already gone away
     * (e.g. "cpulimit 2>&1 | head -1") turns those writes into SIGPIPE.
     * With the default disposition still in place that killed the process
     * mid-usage, so a usage error reported 141 instead of 1. It is a race
     * with the reader's exit, so the wrong status appeared only sometimes.
     * configure_signal_handler() only installs handlers and clears the
     * internal flags, so it does not depend on cfg and can run first.
     */
    configure_signal_handler();

    /*
     * Turn off terminal echo for the run so the driver does not echo the
     * "^C" of a Ctrl+C. The original attributes are saved first and restored
     * on the way out. Both are no-ops when standard input is not a terminal.
     */
    save_terminal_attributes();
    disable_terminal_echo();

    parse_result = parse_arguments(argc, argv, &cfg);
    if (parse_result != 0) {
        status = (parse_result < 0) ? EXIT_SUCCESS : parse_result;
    } else {
        if (cfg.verbose) {
            check_y2038();
        }

        if (cfg.command_mode) {
            status = run_command_mode(&cfg);
        } else {
            status = run_pid_or_exe_mode(&cfg);
        }
    }

    restore_terminal_attributes();
    return status;
}
