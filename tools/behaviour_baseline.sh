#!/bin/bash
#
# tools/behaviour_baseline.sh - replayable behaviour snapshot for cpulimit
#
# The refactorings in R-A .. R-H must be behaviour preserving. "Behaviour"
# here means: every line on stdout and stderr (wording, stream, order), the
# exit code, every signal cpulimit sends, and whether any process is left
# stopped when a run ends. This script pins all of that down so a later
# change can be replayed against it.
#
#   bash tools/behaviour_baseline.sh          # rewrite baseline/*.txt
#   bash tools/behaviour_baseline.sh --check  # replay and diff (exit 1 on drift)
#
# baseline/ is scratch and is not tracked by git: the snapshots describe the
# machine that recorded them (its PID sequence, its CPU count, its temporary
# directories) and the first form above recreates them at any time.  Record on
# the revision that is the reference before changing anything, keep the files
# in the working tree, and replay with --check as the work goes on.
#
# Each snapshot is baseline/<name>.txt with the fields:
#
#   COMMAND      the cpulimit invocation, with volatile paths collapsed
#   EXIT_CODE    what waitpid() reported
#   STOPPED_AFTER  number of processes left in state T when the run ended;
#                 must be 0 for every scenario (requirement 17)
#   STDOUT       normalized stdout
#   STDERR       normalized stderr
#
# ---------------------------------------------------------------------------
# Normalization rules - why each one exists and what it costs
# ---------------------------------------------------------------------------
# Nothing below is optional: without them the snapshots flap between runs and
# the check is useless. The cost is that some distinctions stop being visible
# here; those are covered by the unit test suite instead, while this
# script guards the things unit tests cannot see: real stdout/stderr wording,
# real exit codes, real signals, real leaving-processes-stopped.
#
#  1. PID. Any process id becomes <PID>. A fresh PID is minted per run, so
#     this is unavoidable. Exit statuses and signal numbers are NOT touched:
#     "exited with status 137" keeps the 137, because that value is exactly
#     the behaviour we care about.
#  2. CPU count and derived limit range. The machine has a fixed but
#     environment-specific core count, so "6 CPUs detected" becomes
#     "<NCPUS> CPUs detected" and "range (0, 600]" becomes "range (0, <MAX>]".
#  3. Verbose statistics rows. The values are measured microseconds and
#     instantaneous CPU percentages, and how many rows fit in the sample
#     window depends on scheduling. Rows are collapsed to <STATS_ROW>, so the
#     table header and every surrounding message stay comparable but the
#     arithmetic itself is left to the unit tests.
#  4. Group membership. "N member(s)" is normalized to <MEMBERS> because how
#     many children of the load generator exist at the instant cpulimit scans
#     is a race, not a decision.
#  5. Repeated lines. Runs that poll (the "retrying..." loop) print a count
#     that depends only on how long the harness happened to sleep. A run of
#     identical consecutive lines collapses to the line plus <REPEAT>, which
#     keeps ordering and wording while dropping the count.
#  6. Scratch paths. Temporary directories are rendered as <SCRATCH> so the
#     snapshot does not depend on mktemp's random suffix.
#
# Anything a scenario derives from a PID before rule 1 erases it is recorded
# explicitly, as a derived field such as SELECTED_TARGET.
# ---------------------------------------------------------------------------

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$REPO_ROOT" || exit 1

BUILDDIR=${BUILDDIR:-build}
CL="$BUILDDIR/src/cpulimit"
BUSY="$BUILDDIR/tests/busy"
MULTI="$BUILDDIR/tests/multi_process_busy"
HOOK_SO="$SCRIPT_DIR/hook.so"
BASELINE_DIR="$REPO_ROOT/baseline"

MODE=record
if [ "${1:-}" = "--check" ]; then
    MODE=check
fi

if [ ! -x "$CL" ]; then
    echo "baseline: $CL not built; run cmake --build $BUILDDIR --target all" >&2
    exit 2
fi
if [ ! -x "$BUSY" ] || [ ! -x "$MULTI" ]; then
    echo "baseline: load helpers missing in $BUILDDIR/tests" >&2
    exit 2
fi

# The injection hook is required only by scenarios 18-21; build it on demand
# and continue without it rather than failing the whole run, reporting the
# gap at the end.
HOOK_READY=0
if gcc -shared -fPIC -o "$HOOK_SO" "$SCRIPT_DIR/hook.c" -ldl 2>/dev/null; then
    HOOK_READY=1
fi

SCRATCH=$(mktemp -d)
OUT="$SCRATCH/stdout"
ERR="$SCRATCH/stderr"
GEN="$SCRATCH/generated"
mkdir -p "$GEN"

# Results set by every scenario: EC (exit code), DESC (command line shown in
# the snapshot) and optionally EXTRA (derived observations).
EC=0
DESC=""
EXTRA=""
# Set by reap_artifacts: stopped processes observed before cleanup.
LAST_STOPPED=0

cleanup() {
    rm -rf "$SCRATCH"
}
trap cleanup EXIT

# --- observable helpers -----------------------------------------------------

stopped_count() {
    ps -eo pid,stat --no-headers 2>/dev/null | awk '$2 ~ /T/' | wc -l
}

# Resume and then reap only the processes this script could have created, so
# a leaked stopped process never poisons the next scenario.
#
# Matching uses the full command line, not the comm column: the kernel
# truncates comm to 15 bytes, so the multi-process load generator shows up as
# "multi_process_b" and any match on its real name silently finds nothing.
reap_artifacts() {
    # Sample before cleaning: how many processes the run itself left stopped
    # is behaviour worth recording, and the cleanup below would erase it.
    LAST_STOPPED=$(stopped_count)
    # Only processes in state T are ever signalled, which keeps this from
    # touching a running process that merely mentions the same path: the
    # harness itself lives under a directory named cpulimit, so matching on
    # the bare project name would reach the script running right now.
    ps -eo pid,stat,args --no-headers 2>/dev/null |
        awk -v scratch="$SCRATCH" -v cl="$CL" \
            -v busy="$BUSY" -v multi="$MULTI" \
            '$2 ~ /T/ {
                 if (index($0, scratch) > 0) { print $1; next }
                 if (index($0, cl) > 0)     { print $1; next }
                 if (index($0, busy) > 0)   { print $1; next }
                 if (index($0, multi) > 0)  { print $1; next }
             }' |
        while read -r p; do
            kill -CONT "$p" 2>/dev/null
            kill -KILL "$p" 2>/dev/null
        done
    pkill -f "$BUILDDIR/tests/busy" 2>/dev/null
    pkill -f "$BUILDDIR/tests/multi_process_busy" 2>/dev/null
    return 0
}

normalize_stream() {
    sed \
        -e "s#'kill -CONT [0-9][0-9]*'#'kill -CONT <PID>'#g" \
        -e "s#\(cannot resume PID \)[0-9][0-9]*#\1<PID>#g" \
        -e "s#\(to PID \)[0-9][0-9]*#\1<PID>#g" \
        -e "s#\(kill(\)[0-9][0-9]*#\1<PID>#g" \
        -e "s#\(applied to process \)[0-9][0-9]*#\1<PID>#g" \
        -e "s#\(stopped early for process \)[0-9][0-9]*#\1<PID>#g" \
        -e "s#\(No permission to control process \)[0-9][0-9]*#\1<PID>#g" \
        -e "s#\(Process \)[0-9][0-9]*\( is no longer\)#\1<PID>\2#g" \
        -e "s#\(Process with PID \)[0-9][0-9]*#\1<PID>#g" \
        -e "s#\(process group for PID \)[0-9][0-9]*#\1<PID>#g" \
        -e "s#\(target process \)[0-9][0-9]*\( is cpulimit itself\)#\1<PID>\2#g" \
        -e "s#\(^Limiting process \)[0-9][0-9]*#\1<PID>#g" \
        -e "s#\(^Process group of PID \)[0-9][0-9]*\(: \)[0-9][0-9]*\( member\)#\1<PID>\2<MEMBERS>\3#g" \
        -e "s#\(^Process \)[0-9][0-9]*\( exited with status\| found\| terminated\| timed out\)#\1<PID>\2#g" \
        -e "s#^\([0-9][0-9]*\) CPUs detected\$#<NCPUS> CPUs detected#" \
        -e "s#range (0, [0-9][0-9]*]#range (0, <MAX>]#g" \
        -e "s#^[[:space:]]*\([0-9][0-9]*\.[0-9][0-9]*%\|n/a\)\([[:space:]]*[0-9][0-9]* us\)\{2\}[[:space:]]*[0-9][0-9]*\.[0-9][0-9]*%[[:space:]]*\$#<STATS_ROW>#" \
        -e "s#$SCRATCH#<SCRATCH>#g" \
        "$1" |
        awk '{
                if (NR > 1 && $0 == prev) { dup = 1; next }
                if (dup) { print "<REPEAT>"; dup = 0 }
                print $0
                prev = $0
             }
             END { if (dup) print "<REPEAT>" }'
}

emit_snapshot() {
    _name=$1
    _file=$2
    {
        echo "# scenario: $_name"
        echo "COMMAND: $DESC"
        if [ -n "$EXTRA" ]; then
            echo "$EXTRA"
        fi
        echo "EXIT_CODE: $EC"
        # Sampled before the harness cleaned up. A non-zero value is the
        # process really being left stopped by that run and must stay equal
        # across replays; scenario 19 is deliberately non-zero, because "can
        # suspend but cannot resume" is exactly what it exercises.
        echo "STOPPED_AFTER: $LAST_STOPPED"
        echo "STDOUT:"
        normalize_stream "$OUT"
        echo "STDERR:"
        normalize_stream "$ERR"
    } > "$_file"
}

# --- runners ----------------------------------------------------------------

# Run cpulimit to completion. Used for scenarios that terminate on their own.
run_complete() {
    DESC="cpulimit $*"
    : > "$OUT"
    : > "$ERR"
    timeout 20 "$CL" "$@" >"$OUT" 2>"$ERR"
    EC=$?
    reap_artifacts
}

# Start cpulimit, let it run for $1 seconds, deliver $2, then wait for it to
# exit. Used for every scenario whose target would otherwise run forever.
run_bounded() {
    _slp=$1
    _sig=$2
    shift 2
    DESC="cpulimit $* (then SIG$_sig after ${_slp}s)"
    : > "$OUT"
    : > "$ERR"
    "$CL" "$@" >"$OUT" 2>"$ERR" &
    _cpid=$!
    sleep "$_slp"
    kill -"$_sig" "$_cpid" 2>/dev/null
    # Wait generously so the SIGKILL-escalation scenario can complete, but
    # never indefinitely: a wedged run must fail loudly instead of hanging
    # the harness.
    _ticks=0
    while kill -0 "$_cpid" 2>/dev/null && [ "$_ticks" -lt 40 ]; do
        sleep 0.5
        _ticks=$((_ticks + 1))
    done
    if kill -0 "$_cpid" 2>/dev/null; then
        kill -KILL "$_cpid" 2>/dev/null
        wait "$_cpid" 2>/dev/null
        EC=124
    else
        wait "$_cpid"
        EC=$?
    fi
    reap_artifacts
}

run_preloaded() {
    _mode=$1
    _target=$2
    shift 2
    if [ -n "$_target" ]; then
        CPULIMIT_HOOK_MODE="$_mode" CPULIMIT_HOOK_TARGET_PID="$_target" \
            LD_PRELOAD="$HOOK_SO" timeout 20 "$CL" "$@" >"$OUT" 2>"$ERR"
    else
        CPULIMIT_HOOK_MODE="$_mode" LD_PRELOAD="$HOOK_SO" timeout 20 \
            "$CL" "$@" >"$OUT" 2>"$ERR"
    fi
    EC=$?
    reap_artifacts
}

# Scenario specific: which of several candidate PIDs did cpulimit pick up?
# Must be derived before normalization hides the PID.
selected_target_field() {
    _small=$1
    _large=$2
    # The chosen target appears under a different wording per mode: command
    # mode announces "Limiting process <pid>", while -e mode announces
    # "Process <pid> found".
    _got=$(sed -n 's/^Limiting process \([0-9][0-9]*\).*/\1/p' "$OUT" \
           2>/dev/null | head -1)
    if [ -z "$_got" ]; then
        _got=$(sed -n 's/^Process \([0-9][0-9]*\) found.*/\1/p' "$OUT" \
               2>/dev/null | head -1)
    fi
    if [ -z "$_got" ]; then
        _got=$(sed -n 's/^Process group of PID \([0-9][0-9]*\).*/\1/p' "$OUT" \
               2>/dev/null | head -1)
    fi
    if [ -z "$_got" ]; then
        _got=$(sed -n 's/^Failed to initialize process group for PID \([0-9][0-9]*\).*/\1/p' \
                   "$ERR" 2>/dev/null | head -1)
    fi
    if [ -z "$_got" ]; then
        echo "SELECTED_TARGET: none"
    elif [ "$_got" = "$_large" ]; then
        echo "SELECTED_TARGET: larger_pid"
    elif [ "$_got" = "$_small" ]; then
        echo "SELECTED_TARGET: smaller_pid"
    else
        echo "SELECTED_TARGET: other"
    fi
}

# --- scenarios --------------------------------------------------------------

sc_01_cmd_busy() {
    run_bounded 2 TERM -l 50 -- "$BUSY" 1
}

sc_02_cmd_multi_incl_children() {
    run_bounded 2 TERM -l 50 -i -- "$MULTI" 3
}

sc_03_cmd_multi_verbose() {
    run_bounded 2 TERM -l 30 -i -v -- "$MULTI" 2
}

sc_04_cmd_true() {
    run_complete -l 50 -- /bin/true
}

sc_05_cmd_exit7() {
    run_complete -l 50 -- /bin/sh -c 'exit 7'
}

sc_06_cmd_notfound() {
    run_complete -l 50 -- /nonexistent_xyz
}

# A script whose interpreter does not exist: exit 126. Both spellings matter,
# because R-B touches the explicit-path branch and the bare-name branch.
sc_07_cmd_shebang_explicit() {
    _script="$SCRATCH/badshebang.sh"
    printf '#!/nonexistent_interp_xyz\ntrue\n' > "$_script"
    chmod +x "$_script"
    DESC="cpulimit -l 50 -- <SCRATCH>/badshebang.sh"
    : > "$OUT"
    : > "$ERR"
    timeout 20 "$CL" -l 50 -- "$_script" >"$OUT" 2>"$ERR"
    EC=$?
    reap_artifacts
}

sc_07b_cmd_shebang_bare() {
    _script="$SCRATCH/badshebang_bare.sh"
    printf '#!/nonexistent_interp_xyz\ntrue\n' > "$_script"
    chmod +x "$_script"
    DESC="cpulimit -l 50 -- badshebang_bare.sh (found on PATH)"
    : > "$OUT"
    : > "$ERR"
    PATH="$SCRATCH:$PATH" timeout 20 "$CL" -l 50 -- badshebang_bare.sh \
        >"$OUT" 2>"$ERR"
    EC=$?
    reap_artifacts
}

sc_08_pid_mode() {
    "$BUSY" 1 >/dev/null 2>&1 &
    _bp=$!
    sleep 1
    run_bounded 2 TERM -l 50 -p "$_bp"
    # Replace the PID that run_bounded interpolated, otherwise the snapshot
    # records a number that changes on every replay.
    DESC="cpulimit -l 50 -p <TARGET> (then SIGTERM after 2s)"
    kill -KILL "$_bp" 2>/dev/null
    wait "$_bp" 2>/dev/null
    reap_artifacts
}

sc_09_exe_lazy_exists() {
    "$BUSY" 1 >/dev/null 2>&1 &
    _bp=$!
    sleep 1
    run_bounded 2 TERM -l 50 -z -e busy
    kill -KILL "$_bp" 2>/dev/null
    wait "$_bp" 2>/dev/null
    reap_artifacts
}

sc_10_exe_lazy_missing() {
    run_complete -l 50 -z -e nonexistent_zzz
}

sc_11_exe_nonlazy_missing_retry() {
    run_bounded 3 TERM -l 50 -e nonexistent_zzz
}

sc_12_exe_self() {
    run_complete -l 50 -e cpulimit
}

sc_13a_cli_no_target() {
    run_complete
}

sc_13b_cli_bad_limit() {
    run_complete -l abc -p 1
}

sc_13c_cli_pid_zero() {
    run_complete -l 50 -p 0
}

sc_13d_cli_pid_notnum() {
    run_complete -l 50 -p abc
}

sc_13e_cli_exe_empty() {
    run_complete -l 50 -e ''
}

sc_13f_cli_exe_slashdir() {
    run_complete -l 50 -e bin/
}

sc_13g_cli_exe_root() {
    run_complete -l 50 -e /
}

sc_13h_cli_limit_zero() {
    run_complete -l 0 -p 1
}

sc_13i_cli_limit_toobig() {
    run_complete -l 99999 -p 1
}

sc_14_cmd_empty_name() {
    run_complete -l 50 -- ''
}

# Signals: the child (/bin/sleep) does not install handlers, so forwarding
# terminates it and cpulimit must report 128+N.
sc_15a_signal_int() {
    run_bounded 2 INT -l 50 -- /bin/sleep 100
}

sc_15b_signal_term() {
    run_bounded 2 TERM -l 50 -- /bin/sleep 100
}

sc_15c_signal_hup() {
    run_bounded 2 HUP -l 50 -- /bin/sleep 100
}

sc_16_sigkill_escalation() {
    DESC="cpulimit -l 50 -- sh -c 'trap \"\" TERM; sleep 30' (then SIGTERM)"
    : > "$OUT"
    : > "$ERR"
    "$CL" -l 50 -- /bin/sh -c 'trap "" TERM; sleep 30' >"$OUT" 2>"$ERR" &
    _cpid=$!
    sleep 1
    kill -TERM "$_cpid" 2>/dev/null
    _ticks=0
    while kill -0 "$_cpid" 2>/dev/null && [ "$_ticks" -lt 40 ]; do
        sleep 0.5
        _ticks=$((_ticks + 1))
    done
    if kill -0 "$_cpid" 2>/dev/null; then
        kill -KILL "$_cpid" 2>/dev/null
        wait "$_cpid" 2>/dev/null
        EC=124
    else
        wait "$_cpid"
        EC=$?
    fi
    reap_artifacts
}

# Injection: the target exists but every signal to it fails.
sc_18_hook_eperm_all() {
    if [ "$HOOK_READY" -eq 0 ]; then
        skip_scenario "hook not built"
        return
    fi
    "$BUSY" 1 >/dev/null 2>&1 &
    _bp=$!
    sleep 1
    DESC="cpulimit -l 50 -p <TARGET> with hook MODE=eperm_all TARGET=<TARGET>"
    : > "$OUT"
    : > "$ERR"
    run_preloaded eperm_all "$_bp" -l 50 -p "$_bp"
    kill -KILL "$_bp" 2>/dev/null
    wait "$_bp" 2>/dev/null
    reap_artifacts
}

# Injection: SIGSTOP works, SIGCONT never does. The group ends up stranded.
sc_19_hook_deny_cont() {
    if [ "$HOOK_READY" -eq 0 ]; then
        skip_scenario "hook not built"
        return
    fi
    DESC="cpulimit -l 50 -i -p <TARGET> with hook MODE=deny_cont (global)"
    : > "$OUT"
    : > "$ERR"
    "$MULTI" 2 >/dev/null 2>&1 &
    _mp=$!
    sleep 1
    CPULIMIT_HOOK_MODE=deny_cont LD_PRELOAD="$HOOK_SO" \
        "$CL" -l 50 -i -p "$_mp" >"$OUT" 2>"$ERR" &
    _cpid=$!
    sleep 3
    kill -TERM "$_cpid" 2>/dev/null
    _ticks=0
    while kill -0 "$_cpid" 2>/dev/null && [ "$_ticks" -lt 40 ]; do
        sleep 0.5
        _ticks=$((_ticks + 1))
    done
    if kill -0 "$_cpid" 2>/dev/null; then
        kill -KILL "$_cpid" 2>/dev/null
        wait "$_cpid" 2>/dev/null
        EC=124
    else
        wait "$_cpid"
        EC=$?
    fi
    kill -KILL "$_mp" 2>/dev/null
    wait "$_mp" 2>/dev/null
    reap_artifacts
}

# Injection: SIGSTOP never works, so nothing can be limited at all.
sc_20_hook_deny_stop() {
    if [ "$HOOK_READY" -eq 0 ]; then
        skip_scenario "hook not built"
        return
    fi
    DESC="cpulimit -l 50 -p <TARGET> with hook MODE=deny_stop (global)"
    : > "$OUT"
    : > "$ERR"
    "$MULTI" 2 >/dev/null 2>&1 &
    _mp=$!
    sleep 1
    CPULIMIT_HOOK_MODE=deny_stop LD_PRELOAD="$HOOK_SO" \
        "$CL" -l 50 -i -p "$_mp" >"$OUT" 2>"$ERR" &
    _cpid=$!
    sleep 3
    kill -TERM "$_cpid" 2>/dev/null
    _ticks=0
    while kill -0 "$_cpid" 2>/dev/null && [ "$_ticks" -lt 40 ]; do
        sleep 0.5
        _ticks=$((_ticks + 1))
    done
    if kill -0 "$_cpid" 2>/dev/null; then
        kill -KILL "$_cpid" 2>/dev/null
        wait "$_cpid" 2>/dev/null
        EC=124
    else
        wait "$_cpid"
        EC=$?
    fi
    kill -KILL "$_mp" 2>/dev/null
    wait "$_mp" 2>/dev/null
    reap_artifacts
}

# Injection: two same-named processes; the smaller PID is uncontrollable, so
# -e must select the larger one even though the smaller PID wins by default.
sc_21_hook_select_controllable() {
    if [ "$HOOK_READY" -eq 0 ]; then
        skip_scenario "hook not built"
        return
    fi
    "$BUSY" 1 >/dev/null 2>&1 &
    _first=$!
    sleep 1
    "$BUSY" 1 >/dev/null 2>&1 &
    _second=$!
    sleep 1
    if [ "$_first" -lt "$_second" ]; then
        _small=$_first
        _large=$_second
    else
        _small=$_second
        _large=$_first
    fi
    # -v because which candidate was chosen is only observable through the
    # "Limiting process <pid>" line, and SELECTED_TARGET derives from it
    # before normalization hides the number.
    DESC="cpulimit -l 50 -v -e busy with hook MODE=eperm_all TARGET=<smaller pid>"
    : > "$OUT"
    : > "$ERR"
    CPULIMIT_HOOK_MODE=eperm_all CPULIMIT_HOOK_TARGET_PID="$_small" \
        LD_PRELOAD="$HOOK_SO" timeout 20 "$CL" -l 50 -v -e busy \
        >"$OUT" 2>"$ERR" &
    _cpid=$!
    sleep 3
    kill -TERM "$_cpid" 2>/dev/null
    wait "$_cpid" 2>/dev/null
    EC=$?
    EXTRA=$(selected_target_field "$_small" "$_large")
    kill -KILL "$_first" 2>/dev/null
    kill -KILL "$_second" 2>/dev/null
    wait "$_first" 2>/dev/null
    wait "$_second" 2>/dev/null
    reap_artifacts
}

# A refused target must not end an unbounded search: the process wearing that
# name may be restarted, and the replacement may be one cpulimit is allowed to
# signal.  -e without -z is the only way to ask for that search, because -p
# implies -z, so the refusal can only be sat out here.  The target outlives the
# scenario window on purpose: a target that had exited would be reported as
# missing instead, which is a different diagnostic.
sc_22_hook_exe_nonlazy_eperm() {
    if [ "$HOOK_READY" -eq 0 ]; then
        skip_scenario "hook not built"
        return
    fi
    "$BUSY" 30 >/dev/null 2>&1 &
    _bp=$!
    sleep 1
    DESC="cpulimit -l 50 -e busy with hook MODE=eperm_all TARGET=<the only busy>"
    : > "$OUT"
    : > "$ERR"
    # Signalled by hand rather than through timeout: the exit status recorded
    # then belongs to cpulimit, and a run that stopped on its own would show
    # up as the failure status it used to exit with.
    CPULIMIT_HOOK_MODE=eperm_all CPULIMIT_HOOK_TARGET_PID="$_bp" \
        LD_PRELOAD="$HOOK_SO" "$CL" -l 50 -e busy >"$OUT" 2>"$ERR" &
    _cpid=$!
    sleep 3
    kill -TERM "$_cpid" 2>/dev/null
    wait "$_cpid" 2>/dev/null
    EC=$?
    kill -KILL "$_bp" 2>/dev/null
    wait "$_bp" 2>/dev/null
    reap_artifacts
}

skip_scenario() {
    : > "$OUT"
    : > "$ERR"
    echo "SKIPPED: $1" > "$ERR"
    EC=0
    EXTRA="SKIPPED: $1"
}

SCENARIOS="
01_cmd_busy
02_cmd_multi_incl_children
03_cmd_multi_verbose
04_cmd_true
05_cmd_exit7
06_cmd_notfound
07_cmd_shebang_explicit
07b_cmd_shebang_bare
08_pid_mode
09_exe_lazy_exists
10_exe_lazy_missing
11_exe_nonlazy_missing_retry
12_exe_self
13a_cli_no_target
13b_cli_bad_limit
13c_cli_pid_zero
13d_cli_pid_notnum
13e_cli_exe_empty
13f_cli_exe_slashdir
13g_cli_exe_root
13h_cli_limit_zero
13i_cli_limit_toobig
14_cmd_empty_name
15a_signal_int
15b_signal_term
15c_signal_hup
16_sigkill_escalation
18_hook_eperm_all
19_hook_deny_cont
20_hook_deny_stop
21_hook_select_controllable
22_hook_exe_nonlazy_eperm
"

if [ "$MODE" = record ]; then
    mkdir -p "$BASELINE_DIR"
fi

FAILED=0
PASSED=0
SKIPPED=0

for name in $SCENARIOS; do
    EC=0
    DESC=""
    EXTRA=""
    : > "$OUT"
    : > "$ERR"
    sc_$name
    emit_snapshot "$name" "$GEN/$name.txt"
    case "$MODE" in
        record)
            cp "$GEN/$name.txt" "$BASELINE_DIR/$name.txt"
            echo "recorded $name"
            ;;
        check)
            if [ ! -f "$BASELINE_DIR/$name.txt" ]; then
                echo "FAIL $name: no stored baseline"
                FAILED=$((FAILED + 1))
            elif diff -u "$BASELINE_DIR/$name.txt" "$GEN/$name.txt" > \
                "$GEN/$name.diff"; then
                PASSED=$((PASSED + 1))
            else
                echo "FAIL $name:"
                cat "$GEN/$name.diff"
                FAILED=$((FAILED + 1))
            fi
            ;;
    esac
    stopped_now=$(stopped_count)
    if [ "$stopped_now" -ne 0 ]; then
        echo "WARN $name: $stopped_now process(es) still stopped afterwards"
    fi
done

reap_artifacts

if [ "$MODE" = record ]; then
    echo "baseline written to $BASELINE_DIR ($(echo "$SCENARIOS" | wc -w) scenarios)"
    if [ "$HOOK_READY" -eq 0 ]; then
        echo "NOTE: hook.so could not be built; scenarios 18-21 are recorded as skipped"
    fi
else
    echo "replayed: $PASSED equal, $FAILED differing"
    if [ "$HOOK_READY" -eq 0 ]; then
        echo "NOTE: hook.so could not be built; scenarios 18-21 not exercised"
    fi
    if [ "$FAILED" -ne 0 ]; then
        exit 1
    fi
fi
exit 0
