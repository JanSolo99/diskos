# Play queue - design and RE plan

Status: **design, nothing implemented.** Written 2026-09-05.

The `Queue` screen that exists today (`ui/queue.c`, uncommitted) is a read-only mirror of
whatever list the player already built, with tap-to-jump. This document specs the real
thing: **add to queue, play next, reorder, remove** - and the reverse engineering needed
to choose between three routes to it.

---

## 1. Goal and non-goals

**Goal.** The queue operations a DAP is expected to have:

| Operation | Meaning |
|---|---|
| Play next | insert one track directly after the playing one |
| Add to queue | append one track, or a whole album/artist, to the end |
| Reorder | move a pending track within the queue |
| Remove | drop a pending track |
| Clear | empty everything after the playing track |
| Replace | "Play" on an album/artist wipes the queue and starts it |

Plus: the queue survives a reboot, and none of the above interrupts the track that is
currently playing.

**Non-goals for v1.** Cross-device sync, smart/auto-queue ("radio"), history stack with
undo, and multi-select. Each is a feature that earns its place later or not at all; the
brief is a queue that works perfectly, not a queue with options.

---

## 2. Why it does not exist yet

Not a hardware limitation. Nothing about the X2000 or the CS43131 has any concept of a
playlist - this is entirely a software-ownership question.

`mq_player` owns playback. It keeps its active queue in **`LIST_SONG_0`**, a plain table in
`/usr/data/fiio/db/song.db`, and it builds that table itself with
`INSERT INTO LIST_SONG_%d ... SELECT ... FROM SONG` (RE_CATALOGUE section 7, marked
verified). diskOS drives playback by asking for a *scope* - a `list_type` plus a name - and
the player rebuilds the whole table from that scope before starting.

So today the queue is only ever "everything matching one filter". There is no path from the
UI that says "and also this one track, right after the current one".

**What already exists and is easy to miss:** `ui_play_list()` in `main.c` has two forms.
When the requested scope differs from the loaded one it sends the rebuild form. When the
scope *matches*, it sends a bare position jump instead:

```
0100 0010 <pos> 0000        <- no list_type, no name
```

That is *"play position N of whatever is already in LIST_SONG_0"*, and it ships today - it
is the fast path every time you tap a second track in the same album, and it is what makes
that instant instead of a ~3 second rebuild. **The ability to play an arbitrary
already-present list is therefore already proven on hardware.** The missing half is only
the ability to put rows in that list ourselves.

---

## 3. Evidence: what is actually known

Confidence labels follow RE_CATALOGUE's own convention. **V** = string-verified in the
binary, **L** = live-tested on device, **I** = inferred from family, **U** = unknown.

| Fact | Confidence | Source |
|---|---|---|
| `LIST_SONG_0/1/2` are the active play queues in `song.db` | V | RE_CATALOGUE section 7 |
| The player builds them via `INSERT ... SELECT ... FROM SONG` | V | RE_CATALOGUE section 7 |
| Full column list of `LIST_SONG_0` | V | RE_CATALOGUE section 7 |
| `0100` = main play, jumptable by `list_type` | V + L | COMMAND_MAP; ships in diskOS |
| Bare-position jump plays the existing list without rebuilding | L | `ui_play_list()`, in production |
| `POS_ID` is the `MEMORY_PLAY.MUSIC_ID` join key (resume) | V | RE_CATALOGUE section 7 |
| `0112` = playlist add song | V | RE_CATALOGUE section 8, Table B |
| `0115` = add to custom list | V | RE_CATALOGUE section 8, Table B |
| `0117` = playlist reorder (src/dst) | V | RE_CATALOGUE section 8, Table B |
| `0114` = custom-list delete | V | RE_CATALOGUE section 8, Table B |
| `0815` = set `MEMORY_PLAY` position | V | RE_CATALOGUE section 8, Table A |
| Whether the player re-reads `LIST_SONG_0` mid-playback | **U** | - |
| Whether `0112`/`0115`/`0117` touch the ACTIVE list or only saved playlists | **U** | - |
| Whether any of those tags exist on **V2.28** | **U** | - |

### The version trap

Every tag above is from the **V2.09** dispatch tables. The device runs **V2.28**. These are
not interchangeable, and the failure is silent: `fwcaps.c` documents that gain moved from
`0645` to `0649`, and that on V2.28 **the `0645` handler is a NULL pointer** - sending it
does nothing at all, with no error. The house rule already in `fwcaps.c` is to map verified
{version, tag} pairs exactly and **fail closed** on anything else.

That rule governs this whole document. No tag in section 5 gets used on V2.28 until it has
been verified on V2.28.

---

## 4. Three possible routes

### Route A - a reserved "Up Next" playlist

Back the queue with a normal `CUSTOM_PLAYLIST` row and play it as `list_type 5`.

*Everything needed already exists:* `mdb_playlist_create()`, `mdb_playlist_add_song()`,
`mdb_playlist_songs()`, `ui_play_playlist()`. Zero new reverse engineering, zero new tags.

**The problem is latency of effect.** The player only sees the new row when it rebuilds, and
a rebuild is a `0100` with a scope, which restarts playback at a position. So "add to queue"
either does nothing until the queue is exhausted, or costs an audible restart of the current
track. Neither is acceptable for the polish bar we are aiming at.

Verdict: **acceptable fallback, not the target.** Ships immediately if the probe kills both
other routes.

### Route B - write `LIST_SONG_0` directly

Insert our own rows into the player's active queue table, never send a rebuild, and drive
playback with the bare-position jump that already works.

The write pattern is one we already use elsewhere - `mdb_playlist_add_song()` does exactly
this shape against `CUSTOM_PLAYLIST`:

```sql
INSERT INTO LIST_SONG_0 (<cols>) SELECT <cols> FROM SONG WHERE PATH = ?;
```

Copying straight out of `SONG` is deliberate: it reproduces the player's own verified
statement, so every column carries the semantics the player itself wrote.

**Lives or dies on one unknown:** whether the player re-reads the table as it advances, or
loads it into memory when it builds it. If it caches, our rows are invisible until a rebuild
and Route B collapses into Route A.

Verdict: **the target, if the probe says the table is re-read.**

### Route C - the player's own list commands

`0112` (playlist add song), `0115` (add to custom list), `0117` (playlist reorder src/dst)
are string-verified command tags we have never used. If any of them mutate the *active*
list, this is the native answer: the player updates its own in-memory state and there is no
cache-coherency question at all.

The names suggest they operate on saved playlists rather than `LIST_SONG_0`, which would
make them a slower equivalent of what `musicdb.c` already does locally. But "playlist
reorder(src/dst)" is precisely a queue-reorder primitive, and it is worth an hour to find
out.

Verdict: **best outcome if it pans out; needs the most RE.**

---

## 5. Phase 0 - the probe (do this first)

Nothing gets built until these three questions are answered. All of it runs over Debug Mode
SSH on a device with a **verified restore point already saved**.

### P1. What is actually in `LIST_SONG_0`?

Read-only, zero risk. Play an album, then:

```sh
sqlite3 /usr/data/fiio/db/song.db ".schema LIST_SONG_0"
sqlite3 /usr/data/fiio/db/song.db \
  "SELECT ID,LIST_ID,POS_ID,TRACK,TITLE FROM LIST_SONG_0 ORDER BY ID LIMIT 20;"
sqlite3 /usr/data/fiio/db/song.db "SELECT COUNT(*) FROM LIST_SONG_0;"
sqlite3 /usr/data/fiio/db/song.db "SELECT * FROM MEMORY_PLAY;"
sqlite3 /usr/data/fiio/db/song.db "SELECT * FROM PLAY_LIST;"
```

Establishes: real column order, whether play order is `ID` or `POS_ID`, what `LIST_ID`
holds, how `MEMORY_PLAY.MUSIC_ID` joins, and what `LIST_SONG_1`/`_2` are for.

### P2. Does the player re-read the table while playing? (decides Route B)

The pivotal test. With an album playing and several tracks still pending:

```sh
# rename the title of a track that has NOT played yet
sqlite3 /usr/data/fiio/db/song.db \
  "UPDATE LIST_SONG_0 SET TITLE='PROBE_MARKER' WHERE ID=(SELECT MAX(ID) FROM LIST_SONG_0);"
```

Let playback reach that track.

- Now Playing shows `PROBE_MARKER` -> **the table is re-read. Route B is viable.**
- It shows the original title -> the list was cached at build time. **Route B is dead;**
  fall back to A, or to C if P3 succeeds.

Then the stronger test - append a row and see whether the player plays past the old end:

```sh
sqlite3 /usr/data/fiio/db/song.db \
  "INSERT INTO LIST_SONG_0 (LIST_ID,POS_ID,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,
     IS_CUE,IS_ISO,OFFSET,DURATION,ADD_TIME,IS_SELECT,SONG_TYPE,ALBUM_ARTIST)
   SELECT (SELECT LIST_ID FROM LIST_SONG_0 LIMIT 1), ID, PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,
     DISC,TRACK,IS_CUE,IS_ISO,OFFSET,DURATION,ADD_TIME,IS_SELECT,0,ALBUM_ARTIST
   FROM SONG WHERE TITLE='<some track not in this album>' LIMIT 1;"
```

Risk: worst case the player ignores it, or plays a wrong track. This is a **database write,
not a guessed IPC frame** - the failure mode is confusion, not the `0666` class where a bad
frame SIGSEGVs the player, frees the SD card and hard-reboots the device from the MCU.
Recoverable by letting diskOS rebuild the list (play anything from the Library).

### P3. What do `0112` / `0115` / `0117` do on V2.28? (decides Route C)

`mqcap` sends one candidate frame to `/player` and then watches `/ui` for the reaction.
Its argument order is `mqcap [sendframe] [watch_secs] [label]`:

```sh
mqcap 0112000C0001 15 probe-0112
```

**Operational wrinkle:** `mqcap` is a SOLE-READER capture - it drains `/ui`, so nothing else
may be reading it. That means stopping `mq_ui` first, and `fiio_init.sh`'s watchdog will
respawn the STOCK UI within seconds and start reading `/ui` again. Either prune the stock
binary for the duration the way `tools/diskos-deploy.sh` does, or accept a short capture
window and repeat. Do NOT stop `mq_player` to get around this - that frees the SD card and
hard-reboots the device.

Diff `song.db` before and after each send (`LIST_SONG_0`, `CUSTOM_PLAYLIST`, `PLAY_LIST`,
`PLAYLIST_INFO`) to see which table actually moved; a tag that changes nothing anywhere is
indistinguishable from a NULL handler by observation alone.

**Confirm the handler is not NULL on V2.28 before drawing any conclusion** - a silent no-op
looks exactly like a wrong payload, and that ambiguity is precisely what made the
`0645`/`0649` gain bug expensive. The cheapest disambiguation is a tag known to work on
V2.28 (e.g. `0649` gain) sent through the same harness as a positive control.

**Order matters:** P1, then P2, then P3. P2 alone may settle the design.

---

## 6. Design (assuming P2 passes - Route B)

### Ownership: the scope sentinel

The hard part is not writing rows, it is deciding who owns `LIST_SONG_0`. Today any Library
tap sends a rebuild that wipes it.

Introduce a queue-owned scope. `g_play_scope` in `main.c` already caches `"<type>:<name>"`;
add a reserved value - `"Q:"` - meaning *the user's queue owns the list; never rebuild*.

Rules:

| User action | Effect |
|---|---|
| Play (album / artist / track) | rebuild as today, scope becomes that list, queue is replaced |
| Add to queue | append rows; if scope is not `Q:`, first adopt the current list as the queue |
| Play next | insert after the playing row |
| Reorder / remove | rewrite the affected rows only |
| Anything while scope is `Q:` | **never** send a rebuild form of `0100` |

"Adopt the current list" matters: the first *Add to queue* while an album plays must not
discard what is already playing. Since the rows are already in `LIST_SONG_0`, adoption is
just flipping the scope marker - no data movement at all.

### Ordering

P1 decides whether play order follows `ID` or `POS_ID`. If it is `ID`, inserting between two
rows needs either a renumber (expensive, and racy under the playhead) or a gapped sequence.
**Allocate `ID`/`POS_ID` in steps of 1000** on rebuild so an insert can usually take a
midpoint with no renumber; renumber only when a gap is exhausted, and only ahead of the
playhead.

### Persistence

Free - `LIST_SONG_0` is on disk, and `MEMORY_PLAY` already stores the resume position
against it. The queue survives reboot with no extra work, provided `POS_ID` stays consistent
with `MEMORY_PLAY.MUSIC_ID` (P1).

### UI

- **Long-press any track row** (Library, Search, Album, Artist, Playlist) -> a sheet with
  *Play next*, *Add to queue*. The long-press gesture and the hold-hint toast already exist
  in `library.c` for group rows.
- **Album / artist rows** get *Add to queue* too - append the whole scope.
- **Now Playing -> Queue** (the existing screen) gains: current track pinned at top, "Up
  Next" below, swipe-left to remove, long-press-drag to reorder, and a *Clear* action.
- Adding shows a toast, matching every other confirmation in the UI.

Reuse `ui_header()`, `th_*()` tokens and `th_font()` throughout - no raw colours or font
constants. The queue list must respect the round screen: rows stay inside the chord at their
lowest edge, per the 360x360 rule.

---

## 7. Performance and battery budget

The brief is a player that stays smooth and does not burn battery, so:

- **No new polling.** Every queue mutation is user-initiated. Nothing is added to the main
  loop and no new timer is created.
- **No rebuilds.** Avoiding the ~3s full-library rebuild is the entire point of Route B; the
  bare-position jump is effectively free by comparison.
- **Bounded DB work on the LVGL thread.** A single-row `INSERT ... SELECT` against an
  indexed table is sub-millisecond and matches what `mdb_record_play()` already does on that
  thread. A *whole-album* append is up to a few hundred rows - wrap it in one transaction,
  and if it measures above ~50ms, move it to the existing worker pattern rather than letting
  it touch frame timing.
- **No extra writes per track.** The queue is written when the user changes it, never on
  advance. `MEMORY_PLAY` position saving stays the player's job.

---

## 8. Failure modes

| Failure | Detection | Response |
|---|---|---|
| Player caches the list (P2 fails) | probe | fall back to Route A |
| Tags absent on V2.28 (P3 fails) | probe | Route C off the table; B or A |
| Our rows race a player rebuild | queue silently reverts to a scope | re-assert `Q:` scope; never send a rebuild while queue-owned |
| `POS_ID` wrong -> resume breaks | reboot resumes wrong track | derive `POS_ID` from `SONG.ID` exactly as the player does |
| Queue drifts from what plays | tap plays the wrong track | same invariant as `mdb_play_pos()`: display order MUST equal the player's order |
| Corrupt `LIST_SONG_0` | playback stops or misbehaves | any Library "Play" issues a rebuild and restores a sane list - this is the user-reachable escape hatch, and it must always work |

Every mutation is a DB write, so there is no path here that can wedge the audio pipeline or
reboot the device. That is a deliberate property of choosing B over inventing a send tag.

---

## 9. Test plan

- **Host:** extend `tests/` with a queue harness in the style of the scanner harness -
  build a `song.db` fixture, exercise insert / reorder / remove / adopt, and assert the
  resulting row order matches what `mdb_play_pos()` would compute. Run under ASan/UBSan.
- **Ordering invariant:** an automated check that queue display order equals player order
  for every scope type. This is the bug class that silently plays the wrong song.
- **Device:** add to queue mid-playback (current track must not glitch); play next; reorder
  ahead of the playhead; remove the next track; clear; reboot and confirm resume.
- **Regression:** the existing fast-path jump must still skip rebuilds for same-scope taps.

---

## 10. Follow-on leads found while writing this

Not part of the queue, recorded so they are not lost:

- **`0820` / `0821` / `0822` = key single / double / long action** (Table A, string-verified).
  This is the seam for the physical key behaviour - directly relevant to making volume,
  pause and power feel right, which currently depend on the stock player's own timing.
- **`0408` = file/folder browse** (Table B, string-verified). The folder browser listed as a
  known gap in CLAUDE.md may already have a player-side implementation.
- **`0815` = set MEMORY_PLAY position.** Useful if diskOS ever needs to control resume.

---

## 11. Decision

1. Run **P1** and **P2**. They are cheap and P2 alone probably decides the design.
2. If P2 passes -> build **Route B** as specced in section 6.
3. If P2 fails -> run **P3**; take **Route C** if the tags are live on V2.28.
4. If both fail -> ship **Route A** and accept that additions land at the next track
   boundary rather than instantly.

Do not write a line of queue code before P1 and P2 have run. The design branches on them,
and building against the wrong branch means throwing the work away.
