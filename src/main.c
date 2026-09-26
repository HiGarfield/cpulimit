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

    parse_result = parse_arguments(argc, argv, &cfg);
    if (parse_result != 0) {
        if (parse_result < 0) {
            return EXIT_SUCCESS;
        }
        return parse_result;
    }

    if (cfg.verbose) {
        check_y2038();
    }

    configure_signal_handler();

    if (cfg.command_mode) {
        status = run_command_mode(&cfg);
    } else {
        status = run_pid_or_exe_mode(&cfg);
    }

    /*
     * A run can end without ever reaching the limiting loop, and those paths --
     * searching for a target that has not appeared yet, or reaping a command's
     * child after limiting stopped early -- would otherwise leave the shell
     * prompt on the line the terminal's keyboard-quit echo is on. Calling this
     * unconditionally is safe: it writes nothing unless the quit came from the
     * keyboard on a terminal, and never more than one newline per run, so the
     * loop's own call takes care of the runs that do reach it.
     */
    finish_tty_quit_line();
    return status;
}
