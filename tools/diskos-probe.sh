#!/usr/bin/env bash
# diskos-probe.sh - answer the open questions that need real hardware, in one trip.
#
# Two pieces of work are currently blocked on facts only the device can supply:
#
#   docs/QUEUE_DESIGN.md    P1/P2 - what is in LIST_SONG_0, and (the pivotal one)
#                           whether mq_player RE-READS that table while playing or
#                           caches it when it builds it. The whole queue design
#                           branches on the answer.
#   docs/POWER_OPTIMIZATION_PLAN.md
#                           whether the kernel already wakes the CPU at HZ regardless
#                           of what the UI loop does. If it does, cutting the loop's
#                           30ms floor buys little and the effort belongs elsewhere.
#   docs/ROADMAP.md Tier 5  whether SONG(PATH) is indexed. The table is the stock
#                           player's, so our own schema says nothing about it, and the
#                           scanner does one WHERE PATH=? per file - section 3b.
#
# Usage:
#   DISKOS_IP=<ip> DISKOS_PW=<debug-mode-password> ./diskos-probe.sh            # read-only
#   DISKOS_IP=... DISKOS_PW=... ./diskos-probe.sh --marker [ROWID]              # P2 test
#   DISKOS_IP=... DISKOS_PW=... ./diskos-probe.sh --append <WORD>               # P2b test
#   DISKOS_IP=... DISKOS_PW=... ./diskos-probe.sh --watch [SECS]                 # P2c, automatic
#
# --watch is the test that needs no one looking at the screen. The player writes its
# position to MEMORY_PLAY.MUSIC_ID as it advances, so polling that says exactly which
# LIST_SONG_0 row it moved to. Append a row, run --watch, let the queue reach the end:
# if MUSIC_ID lands on the appended row, the player is reading the live table.
#
# --append takes ONE WORD, matched as a LIKE substring against SONG.TITLE. ssh
# flattens its argument list, so a multi-word title would arrive split; pick a
# distinctive single word from a track that is NOT in the queue you are playing.
#
# Get the IP and a one-time SSH password from Debug Mode (Settings > System). The
# password is regenerated on every enable and does not survive a reboot.
#
# SAFETY. This script NEVER touches mq_player. Killing it frees the SD card and the
# hardware MCU hard-reboots the whole device - that is the one unrecoverable mistake
# available here. The default run is strictly read-only. --marker and --append write
# to LIST_SONG_0, which is the player's SCRATCH queue table: it is rebuilt from SONG
# every time you play anything from the Library, so any damage is undone by one tap.
# Neither flag sends an IPC command, so neither can wedge the audio pipeline.
#
# Requires on the host: sshpass, ssh. Output lands in ./diskos-probe-<timestamp>.txt
set -eu

IP="${DISKOS_IP:?set DISKOS_IP to the device IP (shown in Debug Mode)}"
export SSHPASS="${DISKOS_PW:?set DISKOS_PW to the Debug Mode SSH password}"

MODE="read"
ARG=""
case "${1:-}" in
  --marker) MODE="marker"; ARG="${2:-}" ;;
  --watch)  MODE="watch";  ARG="${2:-90}" ;;
  --append) MODE="append"; ARG="${2:?--append needs a track TITLE that is NOT in the current queue}" ;;
  "")       ;;
  *)        echo "unknown option: $1" >&2; exit 2 ;;
esac

SSH=(sshpass -e ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=8)
OUT="diskos-probe-$(date +%Y%m%d-%H%M%S).txt"
DB=/usr/data/fiio/db/song.db

# ---------------------------------------------------------------------------
# Everything below runs ON THE DEVICE. Kept as one heredoc so it is a single
# round trip; `sh -s` because the device has busybox ash, not bash.
# ---------------------------------------------------------------------------
run_remote() {
"${SSH[@]}" "root@$IP" 'sh -s' "$1" "$2" <<'REMOTE'
MODE="${1:-read}"
ARG="${2:-}"
DB=/usr/data/fiio/db/song.db
# Fed on STDIN, one statement per line, for two reasons:
#   - dot-commands (.schema) are only recognised at the start of a line; passing
#     "PRAGMA ...; .schema X" as a single SQL argument fails with a syntax error.
#   - .timeout is the CLI's busy-timeout and prints nothing, where
#     "PRAGMA busy_timeout=4000;" would emit a stray 4000 into every result.
# The timeout itself matters because mq_player holds this database open: without
# it a write can lose the lock race and report "database is locked", which reads
# as an inconclusive probe rather than the retryable conflict it actually is.
q() { printf '.timeout 4000\n%s\n' "$1" | sqlite3 "$DB" 2>&1; }

echo "======================================================================"
echo "diskOS probe   $(date)   mode=$MODE"
echo "======================================================================"

echo
echo "--- 0. identity -------------------------------------------------------"
echo "uname:      $(uname -a)"
echo "uptime:     $(cat /proc/uptime)"
echo "firmware:   $(grep -h MAIN_OS_VER /etc/product_version/version.in 2>/dev/null || echo '(version.in unreadable)')"
for p in $(pidof mq_ui 2>/dev/null); do
  echo "mq_ui $p -> $(readlink /proc/$p/exe 2>/dev/null)"
done
echo "mq_player:  $(pidof mq_player 2>/dev/null || echo 'NOT RUNNING')"
command -v sqlite3 >/dev/null 2>&1 || echo "*** sqlite3 CLI MISSING - every DB section below will be empty ***"

echo
echo "--- 1. kernel tick (decides POWER plan finding #1) --------------------"
# If CONFIG_NO_HZ_IDLE is unset and HZ is 100, the timer interrupt wakes the CPU
# 100x/s no matter what the UI loop does, and loop-frequency work is wasted effort.
if [ -r /proc/config.gz ]; then
  zcat /proc/config.gz 2>/dev/null | grep -E '^CONFIG_(HZ|NO_HZ|HZ_PERIODIC|TICK_ONESHOT|HIGH_RES_TIMERS|CPU_IDLE|PREEMPT)' || echo "(no matching CONFIG_ lines)"
else
  echo "/proc/config.gz absent - infer from timer_list below"
fi
echo
echo "timer_list - tick/nohz state (the decisive lines):"
# grep, not head: the nohz_mode / tick_stopped / jiffies fields appear per-CPU well
# past the first 20 lines, and truncating before them was what made the first run
# inconclusive. tick_sched_timer's next expiry lands on a round 10ms boundary when
# HZ=100, which is the fallback inference if nohz_mode is absent entirely.
grep -E 'nohz|tick_stopped|jiffies|tick_sched_timer|Timer List|^cpu:' /proc/timer_list 2>/dev/null | head -40 \
  || echo "(no /proc/timer_list)"
echo
echo "next tick_sched_timer expiry (round 10ms boundary => HZ=100):"
grep -A1 tick_sched_timer /proc/timer_list 2>/dev/null | head -4

echo
echo "--- 2. measured wake rate (10s sample) --------------------------------"
# Interrupt deltas tell us what is ACTUALLY waking the core, and mq_ui's own
# jiffies tell us what our loop costs. Run this with the screen OFF for the
# number that matters.
UIPID=$(pidof mq_ui 2>/dev/null | awk '{print $1}')
echo "sampling 10s (put the screen to sleep NOW for the meaningful figure)..."
cp /proc/interrupts /tmp/_irq0 2>/dev/null
[ -n "$UIPID" ] && awk '{print $14, $15}' /proc/$UIPID/stat > /tmp/_cpu0 2>/dev/null
sleep 10
cp /proc/interrupts /tmp/_irq1 2>/dev/null
[ -n "$UIPID" ] && awk '{print $14, $15}' /proc/$UIPID/stat > /tmp/_cpu1 2>/dev/null
echo
echo "interrupt deltas over 10s (a bare number tells you nothing - carry the NAME):"
awk '
  NR==FNR { if (NF>2) { c[$1]=$2 } ; next }
  NF>2 && $1 in c {
    d=$2-c[$1];
    if (d>0) {
      name="";
      for (i=3; i<=NF; i++) if ($i !~ /^[0-9]+$/) name = name " " $i;
      printf "  %-8s %8d %8.1f/s  %s\n", $1, d, d/10.0, name
    }
  }
' /tmp/_irq0 /tmp/_irq1 2>/dev/null || echo "  (could not diff /proc/interrupts)"
if [ -n "$UIPID" ] && [ -r /tmp/_cpu0 ]; then
  U0=$(awk '{print $1}' /tmp/_cpu0); S0=$(awk '{print $2}' /tmp/_cpu0)
  U1=$(awk '{print $1}' /tmp/_cpu1); S1=$(awk '{print $2}' /tmp/_cpu1)
  echo
  echo "mq_ui cpu jiffies over 10s: user=$((U1-U0)) sys=$((S1-S0))  (USER_HZ is normally 100 => jiffies/10 = % of one core)"
fi
rm -f /tmp/_irq0 /tmp/_irq1 /tmp/_cpu0 /tmp/_cpu1

echo
echo "--- 3. QUEUE P1: what is actually in LIST_SONG_0 ----------------------"
echo "schema:"
q ".schema LIST_SONG_0"
echo
echo "row counts:  LIST_SONG_0=$(q 'SELECT COUNT(*) FROM LIST_SONG_0;')  SONG=$(q 'SELECT COUNT(*) FROM SONG;')"
echo "(LIST_SONG_1/2 are documented for V2.09 but do NOT exist on V2.28 - listing what does:)"
q "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name;" | tr '\n' ' '
echo
echo
echo "first 25 rows (ID | LIST_ID | POS_ID | DISC | TRACK | TITLE):"
q "SELECT ID||' | '||IFNULL(LIST_ID,'-')||' | '||IFNULL(POS_ID,'-')||' | '||IFNULL(DISC,'-')||' | '||IFNULL(TRACK,'-')||' | '||IFNULL(TITLE,'-') FROM LIST_SONG_0 ORDER BY ID LIMIT 25;"
echo
echo "is ID contiguous, and does POS_ID track SONG.ID?"
q "SELECT 'ID  min='||MIN(ID)||' max='||MAX(ID)||' count='||COUNT(*) FROM LIST_SONG_0;"
q "SELECT 'POS_ID min='||MIN(POS_ID)||' max='||MAX(POS_ID) FROM LIST_SONG_0;"
q "SELECT 'rows whose POS_ID matches a SONG.ID: '||COUNT(*) FROM LIST_SONG_0 l JOIN SONG s ON s.ID=l.POS_ID;"
echo
echo "MEMORY_PLAY schema (V2.28 has more columns than the V2.09 catalogue records):"
q ".schema MEMORY_PLAY"
echo "MEMORY_PLAY row:"
q "SELECT * FROM MEMORY_PLAY;"
echo "  -> what MUSIC_ID joins to. POS_ID came back NULL on V2.28, so ID is the"
echo "     likely key; both are tested here rather than assumed:"
q "SELECT 'via ID:     ID='||l.ID||'  TRACK='||IFNULL(l.TRACK,'-')||'  TITLE='||IFNULL(l.TITLE,'-') FROM LIST_SONG_0 l JOIN MEMORY_PLAY m ON m.MUSIC_ID=l.ID;"
q "SELECT 'via POS_ID: POS_ID='||IFNULL(l.POS_ID,'NULL')||'  TITLE='||IFNULL(l.TITLE,'-') FROM LIST_SONG_0 l JOIN MEMORY_PLAY m ON m.MUSIC_ID=l.POS_ID;"
echo "  (whichever prints a row is the join the player actually uses)"
echo
echo "--- 3b. is SONG(PATH) indexed? (read-only) ----------------------------"
# Nothing in OUR SQL creates an index on PATH, but song.db was built by the stock
# player and our CREATE TABLE IF NOT EXISTS is a no-op against it - so what indexes
# exist is the stock schema's choice and cannot be read off our source. It matters:
# the scanner does one "WHERE PATH=?" UPDATE per file, so without an index a rescan
# is roughly O(N^2) in library size. EXPLAIN QUERY PLAN is the decisive line - SCAN
# means no index is being used, SEARCH ... USING INDEX means there is one.
echo "indexes on SONG:"
q "PRAGMA index_list(SONG);"
echo "  (empty above = no index at all on this table)"
echo
echo "plan for the scanner's per-file lookup:"
q "EXPLAIN QUERY PLAN SELECT ID FROM SONG WHERE PATH='probe';"
echo "  -> 'SCAN SONG'                 = full table scan per file; a rescan is O(N^2)"
echo "  -> 'SEARCH SONG USING INDEX'   = already indexed, nothing to do"
echo
echo "library size (the multiplier on the above): $(q 'SELECT COUNT(*) FROM SONG;') rows"
echo
echo "PLAY_LIST (the queue registry):"
q "SELECT * FROM PLAY_LIST;"

if [ "$MODE" = "marker" ]; then
  echo
  echo "--- 4. QUEUE P2: does the player RE-READ the table? -------------------"
  TARGET="$ARG"
  [ -z "$TARGET" ] && TARGET=$(q "SELECT MAX(ID) FROM LIST_SONG_0;")
  echo "target row ID = $TARGET   (defaults to the LAST row - pass an ID from"
  echo "                           section 3 to pick a nearer one instead)"
  echo "NOTE: this only proves anything on a track that has NOT played yet this"
  echo "      session. A row the player already read may be cached either way."
  q "SELECT 'before: ID='||ID||'  TRACK='||IFNULL(TRACK,'-')||'  TITLE='||IFNULL(TITLE,'-') FROM LIST_SONG_0 WHERE ID=$TARGET;"
  q "UPDATE LIST_SONG_0 SET TITLE='PROBE_MARKER' WHERE ID=$TARGET;"
  q "SELECT 'after:  ID='||ID||'  TITLE='||IFNULL(TITLE,'-') FROM LIST_SONG_0 WHERE ID=$TARGET;"
  echo
  echo "NOW: let playback reach that track (skip forward to it)."
  echo "  Now Playing shows PROBE_MARKER  -> the table IS re-read. Route B is viable."
  echo "  Now Playing shows the real title -> the list was cached at build time."
  echo "                                      Route B is dead; fall back to A or C."
  echo "To undo: play anything from the Library - that rebuilds LIST_SONG_0 from SONG."
fi

if [ "$MODE" = "append" ]; then
  echo
  echo "--- 4b. QUEUE P2b: can we APPEND a track the player will play? --------"
  echo "appending first SONG row whose TITLE matches: $ARG"
  BEFORE=$(q "SELECT COUNT(*) FROM LIST_SONG_0;")
  q "INSERT INTO LIST_SONG_0 (LIST_ID,POS_ID,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,
       IS_CUE,IS_ISO,OFFSET,DURATION,ADD_TIME,IS_SELECT,SONG_TYPE,ALBUM_ARTIST)
     SELECT (SELECT LIST_ID FROM LIST_SONG_0 LIMIT 1), s.ID, s.PATH, s.NAME, s.TITLE, s.ALBUM,
            s.ARTIST, s.GENRE, s.DISC, s.TRACK, s.IS_CUE, s.IS_ISO, s.OFFSET, s.DURATION,
            s.ADD_TIME, s.IS_SELECT, 0, s.ALBUM_ARTIST
       FROM SONG s WHERE s.TITLE LIKE '%$ARG%' LIMIT 1;"
  AFTER=$(q "SELECT COUNT(*) FROM LIST_SONG_0;")
  echo "rows: $BEFORE -> $AFTER"
  q "SELECT 'appended: ID='||ID||'  TITLE='||IFNULL(TITLE,'-') FROM LIST_SONG_0 ORDER BY ID DESC LIMIT 1;"
  echo
  echo "NOW: let the queue run to its old end."
  echo "  It carries on into the appended track -> we can build a real queue (Route B)."
  echo "  It stops / wraps instead           -> the end of the list was decided at build time."
  echo "To undo: play anything from the Library."
fi

if [ "$MODE" = "watch" ]; then
  SECS="$ARG"
  [ -z "$SECS" ] && SECS=90
  echo
  echo "--- 5. WATCH: follow the player through the queue (no screen needed) --"
  echo "The player stamps its position into MEMORY_PLAY.MUSIC_ID as it advances, so"
  echo "this reads out which LIST_SONG_0 row it actually moved to. Prints only on"
  echo "CHANGE, for ${SECS}s. Let tracks finish (or skip) while it runs."
  echo
  echo "  If it reaches a row you APPENDED  -> the player re-reads the table. Route B."
  echo "  If it stops at the old last row   -> the list was fixed when it was built."
  echo
  i=0
  last=""
  while [ "$i" -lt "$SECS" ]; do
    row=$(q "SELECT m.MUSIC_ID||'  TRACK='||IFNULL(m.TRACK,'-')||'  row-by-ID='||IFNULL(l.TITLE,'<NO SUCH ID>') FROM MEMORY_PLAY m LEFT JOIN LIST_SONG_0 l ON l.ID=m.MUSIC_ID;")
    if [ "$row" != "$last" ]; then
      echo "  t+${i}s   MUSIC_ID=$row"
      last="$row"
    fi
    sleep 1
    i=$((i + 1))
  done
  echo "  watch finished (${SECS}s)."
  echo
  echo "queue as it stands now:"
  q "SELECT '  ID='||ID||'  TRACK='||IFNULL(TRACK,'-')||'  '||IFNULL(TITLE,'-') FROM LIST_SONG_0 ORDER BY ID;"
fi

echo
echo "======================================================================"
echo "probe complete - mq_player was never touched"
echo "======================================================================"
REMOTE
}

echo ">> probing root@$IP  (mode: $MODE)"
run_remote "$MODE" "$ARG" | tee "$OUT"
echo
echo ">> saved to $OUT"
echo ">> paste that file back for analysis; sections 1 and 2 decide the power work,"
echo "   section 3 (and 4 if you ran --marker) decide the queue design."
