#!/bin/sh
# Launch Stud with everything that can catch the freeze and the lag.
#
# This exists for ONE session. The two remaining bugs are rare and the
# user has, reasonably, refused to keep hunting them by hand, so this run
# has to be the last one: every diagnostic that could identify either is
# on at the same time, and each writes enough context that the cause can
# be read afterwards rather than guessed at.
#
#   sh tools/debug-session.sh            # play normally, then quit
#
# Afterwards, hand over BOTH files it names. The log is the narrative;
# the flight-recorder file is the evidence.

set -e
cd "$(dirname "$0")/.."

STAMP=$(date +%Y%m%d-%H%M%S)
OUT="${STUD_DEBUG_DIR:-$HOME/stud-debug-$STAMP}"
mkdir -p "$OUT"

LOG="$OUT/session.log"
FR="$OUT/flight-recorder.txt"

# The flight recorder: submits, fence waits, presents, acquires and
# barriers into a ring in memory, dumped with the surrounding system
# state whenever the device is lost, a fence sticks for 5s, a fence wait
# passes 250ms, a present passes 50ms, or a submit fails. Nothing is
# printed until one of those happens, so it costs a timestamp and four
# stores per event in the meantime.
export STUD_FLIGHT_RECORDER=1
export STUD_FLIGHT_RECORDER_PATH="$FR"

# Which engine command the GPU died in, rather than only that Stud's own
# pass finished.
#
# OFF by default, and that is a correction. This costs a
# vkCmdSetCheckpointNV per engine command and the engine records
# thousands per frame; the comment where it is implemented says plainly
# that it is "worth turning on only for a run that is hunting a hang",
# and then it was made a default here anyway. Measured against the same
# build with it and the validation layer off: render-host went from
# 62.3% CPU to 9.9%, load average 7.42 to 2.94.
#
# That tax was being paid by every session, including the ones being
# used to judge whether a fix had worked.
#
# STUD_DEBUG_CHECKPOINTS=1 turns it back on for a run that is
# specifically chasing a device loss and wants the failing command named.
if [ -n "$STUD_DEBUG_CHECKPOINTS" ]; then
    export STUD_VK_ENGINE_CHECKPOINTS=1
fi

# Mapped-memory traffic and the cost of the page-table scans, so the
# "is Stud's own bookkeeping the lag" question has numbers rather than a
# hypothesis.
export STUD_VK_MEM_STATS=1

# NOT enabled here, deliberately: STUD_VK_TRACE_BARRIERS and
# STUD_VK_TRACE_OBJECTS.
#
# They were the scaffolding that found the first-barrier bug, and they
# print one line per barrier with a flush. Measured on a real session:
# 363,311 barrier lines in the first 400,000, 354MB in a few minutes,
# and a multi-second hang when the engine loaded a game -- render-host
# blocked on write(), which LOOKS exactly like the freeze being hunted
# and is not it. A diagnostic that manufactures the symptom is worse
# than none.
#
# Nothing is lost by leaving them off: the flight recorder already
# records every barrier and every image creation into the ring, with no
# I/O at all, and prints them only when something actually goes wrong.
# Enable them by hand for a short targeted run, never for a long one.

# The validation layer, which reports the offending call at the moment it
# is made instead of leaving a freeze to be explained afterwards. It
# ships inside Stud's own ANGLE bundle and the loader can force it on;
# there is no entry for it in /usr/share/vulkan/explicit_layer.d, hence
# VK_LAYER_PATH.
#
# STUD_DEBUG_NO_VALIDATION=1 leaves it off. It is the heaviest thing
# here, and if it changes the timing enough to hide the bugs, that is the
# first switch to try without.
# OPT-IN, also a correction. Synchronization validation tracks every
# resource access on every command and is the single most expensive
# thing in this file. It earned its place -- it found the
# first-barrier-lies-about-UNDEFINED bug -- but leaving it on by default
# meant every freeze test ran on a build slowed enough to change the
# timing being measured.
#
# STUD_DEBUG_VALIDATION=1 enables it.
if [ -n "$STUD_DEBUG_VALIDATION" ] && \
   [ -f /usr/lib/stud/angle/angledata/VkLayer_khronos_validation.json ]; then
    export VK_LAYER_PATH=/usr/lib/stud/angle/angledata
    export VK_LOADER_LAYERS_ENABLE=VK_LAYER_KHRONOS_validation
    # Synchronization validation finds one thing reading a resource while
    # another writes it, which is the class of bug that makes a GPU fail
    # to retire a submission -- what the freeze is.
    export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
fi

# WAYLAND_DEBUG: every protocol message, with a timestamp.
#
# This is the decisive measurement for the 12006ms freeze and it is the
# one thing never captured. Everything is known about that stall except
# what the driver is waiting FOR: the thread spins inside
# vkQueuePresentKHR (state R, wchan 0) with the GPU idle, no Xid, and
# `waiting for the queue 0.0ms`, so it is not Stud's lock and not the
# GPU -- it is the driver waiting on the compositor. The protocol trace
# says which message it sent last and what never came back.
#
# It goes to the same file as everything else on purpose: the session
# log already carries the SLOW line, and interleaved output is what makes
# the two correlate without guessing at clocks.
#
# NOTE WHEN READING IT: the SLOW line prints when the present RETURNS,
# at the END of the twelve seconds. Anything logged just before it
# happened DURING the stall, not before it -- which is how a run of
# focus changes (a user alt-tabbing to see why the game had stopped)
# was once read as the cause.
#
# STUD_DEBUG_NO_WAYLAND_TRACE=1 turns it off if the volume is a problem.
if [ -z "$STUD_DEBUG_NO_WAYLAND_TRACE" ]; then
    export WAYLAND_DEBUG=1
fi

echo "Stud debug session"
echo "  log:             $LOG"
echo "  flight recorder: $FR"
echo
echo "Play normally. If it freezes or the desktop lags, KEEP PLAYING --"
echo "everything is captured automatically and nothing needs to be done"
echo "while it is happening. Quit Stud when you are finished."
echo

./build/ui/stud-ui > "$LOG" 2>&1 || true

echo
echo "Session ended. What was captured:"
printf '  %-28s %s\n' "session log" "$(wc -l < "$LOG" 2>/dev/null || echo 0) lines"
if [ -f "$FR" ]; then
    printf '  %-28s %s\n' "flight recorder dumps" \
        "$(grep -c 'stud flight recorder' "$FR" 2>/dev/null || true)"
else
    printf '  %-28s %s\n' "flight recorder dumps" "0 (nothing triggered)"
fi
printf '  %-28s %s\n' "device losses" \
    "$(grep -c 'THE VULKAN DEVICE WAS LOST' "$LOG" 2>/dev/null || true)"
printf '  %-28s %s\n' "stuck fences" "$(grep -c 'FENCE STUCK' "$LOG" 2>/dev/null || true)"
printf '  %-28s %s\n' "slow fence waits" \
    "$(grep -c 'SLOW vkWaitForFences' "$LOG" 2>/dev/null || true)"
printf '  %-28s %s\n' "slow presents" \
    "$(grep -c 'SLOW vkQueuePresentKHR' "$LOG" 2>/dev/null || true)"
printf '  %-28s %s\n' "validation errors" \
    "$(grep -c 'Validation Error' "$LOG" 2>/dev/null || true)"
echo
echo "Everything needed is in $OUT"
