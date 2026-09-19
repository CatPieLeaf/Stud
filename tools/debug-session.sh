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
# pass finished. Costs an extra recorded command per engine command, and
# is worth it exactly once.
export STUD_VK_ENGINE_CHECKPOINTS=1

# Mapped-memory traffic and the cost of the page-table scans, so the
# "is Stud's own bookkeeping the lag" question has numbers rather than a
# hypothesis.
export STUD_VK_MEM_STATS=1

# Every layout transition, and every one Stud drops. This is what
# identified the first-barrier bug.
export STUD_VK_TRACE_BARRIERS=1

# Image handles at creation, so a validation message naming a VkImage can
# be tied to what that image is.
export STUD_VK_TRACE_OBJECTS=1

# The validation layer, which reports the offending call at the moment it
# is made instead of leaving a freeze to be explained afterwards. It
# ships inside Stud's own ANGLE bundle and the loader can force it on;
# there is no entry for it in /usr/share/vulkan/explicit_layer.d, hence
# VK_LAYER_PATH.
#
# STUD_DEBUG_NO_VALIDATION=1 leaves it off. It is the heaviest thing
# here, and if it changes the timing enough to hide the bugs, that is the
# first switch to try without.
if [ -z "$STUD_DEBUG_NO_VALIDATION" ] && \
   [ -f /usr/lib/stud/angle/angledata/VkLayer_khronos_validation.json ]; then
    export VK_LAYER_PATH=/usr/lib/stud/angle/angledata
    export VK_LOADER_LAYERS_ENABLE=VK_LAYER_KHRONOS_validation
    # Synchronization validation finds one thing reading a resource while
    # another writes it, which is the class of bug that makes a GPU fail
    # to retire a submission -- what the freeze is.
    export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
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
        "$(grep -c 'stud flight recorder' "$FR" 2>/dev/null || echo 0)"
else
    printf '  %-28s %s\n' "flight recorder dumps" "0 (nothing triggered)"
fi
printf '  %-28s %s\n' "device losses" \
    "$(grep -c 'THE VULKAN DEVICE WAS LOST' "$LOG" 2>/dev/null || echo 0)"
printf '  %-28s %s\n' "stuck fences" "$(grep -c 'FENCE STUCK' "$LOG" 2>/dev/null || echo 0)"
printf '  %-28s %s\n' "slow fence waits" \
    "$(grep -c 'SLOW vkWaitForFences' "$LOG" 2>/dev/null || echo 0)"
printf '  %-28s %s\n' "slow presents" \
    "$(grep -c 'SLOW vkQueuePresentKHR' "$LOG" 2>/dev/null || echo 0)"
printf '  %-28s %s\n' "validation errors" \
    "$(grep -c 'Validation Error' "$LOG" 2>/dev/null || echo 0)"
echo
echo "Everything needed is in $OUT"
