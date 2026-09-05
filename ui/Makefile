# diskOS native UI (LVGL 9.2.2) for the FiiO Snowsky Disc.
# Requires a static mipsel-linux-musl cross toolchain and LVGL 9.2.2 (vendored in ./lvgl).
# Override paths for your environment on the command line, in the environment, or in a local
# (gitignored) config.mk, e.g.:
#   make CROSS=/opt/x-tools/mipsel-linux-musl/bin/mipsel-linux-musl-  LVGL=/path/to/lvgl
-include config.mk
APP   := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
NUI   ?= $(APP)
LVGL  ?= $(APP)/lvgl
CROSS ?= mipsel-linux-musl-
CC    := $(CROSS)gcc

# Pin the default goal instead of leaving it to whichever rule happens to come first.
# The Docker builder runs a bare `make`, so anything that lands above mq_ui silently
# becomes what a plain `make` builds - and it exits 0 having produced no binary, which
# reads as "the build worked but the deploy did nothing".
.DEFAULT_GOAL := mq_ui

CFLAGS := -Os -pipe -std=gnu11 -static -pthread -D_GNU_SOURCE -DLV_CONF_INCLUDE_SIMPLE \
  -I$(APP) -I$(NUI) -Wall -MMD -MP
LDFLAGS := -static -pthread
LDLIBS  := -lm -lrt

LVGL_SRCS := $(shell find $(LVGL)/src -name "*.c")
APP_SRCS  := $(APP)/main.c $(APP)/theme.c $(APP)/fb_pan.c $(APP)/ipc.c $(APP)/ui.c $(APP)/art.c $(APP)/screenmgr.c $(APP)/home.c $(APP)/library.c $(APP)/musicdb.c $(APP)/settings.c $(APP)/config.c $(APP)/anim.c $(APP)/search.c $(APP)/saver.c $(APP)/quicksettings.c $(APP)/songinfo.c $(APP)/npmenus.c $(APP)/eqcustom.c $(APP)/apps.c $(APP)/kbinput.c $(APP)/playlistview.c $(APP)/artcache.c $(APP)/colorpick.c $(APP)/debug_ui.c $(APP)/modes.c $(APP)/wifi.c $(APP)/bt.c $(APP)/weather.c $(APP)/lyrics.c $(APP)/toast.c $(APP)/lastfm.c $(APP)/lastfm_ui.c $(APP)/md5.c $(APP)/scanner.c $(APP)/scanview.c $(APP)/fontpick.c $(APP)/sysconfig.c $(APP)/fileutil.c $(APP)/txtfold.c $(APP)/fwcaps.c $(APP)/queue.c $(APP)/about.c $(APP)/font_icons_28.c $(APP)/font_weather16.c
# bundled SQLite amalgamation (compiled in-process; replaces popen sqlite3 CLI)
SQLITE_FLAGS := -Os -pipe -static -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION \
  -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED
OBJS := $(LVGL_SRCS:.c=.o) $(APP_SRCS:.c=.o) $(APP)/sqlite3.o

# A build stamp, so the device can SAY which build it is running. Two of the entries in
# CLAUDE.md's symptom table are "my change did nothing" turning out to be the wrong
# binary on the device; this makes that answerable by looking at the screen instead of
# hashing the binary over SSH.
#
# Rewritten ONLY when the value changes, so it costs one recompile of about.c per real
# build and nothing on a no-op make. A plain UTC timestamp rather than a git SHA: the
# Docker builder bind-mounts ui/ alone, so there is no .git in the container to read,
# and a timestamp differs for uncommitted work too - which is exactly the case you are
# trying to tell apart.
#
# Done as a parse-time $(shell ...) rather than a rule, deliberately. A rule needs a
# FORCE prerequisite, and a bare `FORCE:` target sitting above mq_ui silently becomes
# make's DEFAULT GOAL - so `make` with no arguments builds nothing and exits 0. The
# Docker builder runs exactly that bare `make`, while the documented fast check names
# mq_ui explicitly, so the breakage showed up ONLY in the cross build. This form adds
# no target at all, so it cannot become the default goal.
$(shell id=`date -u +%Y%m%d-%H%M`; \
        echo "#define DISKOS_BUILD \"$$id\"" > $(APP)/buildid.h.tmp; \
        cmp -s $(APP)/buildid.h.tmp $(APP)/buildid.h 2>/dev/null \
          || mv -f $(APP)/buildid.h.tmp $(APP)/buildid.h; \
        rm -f $(APP)/buildid.h.tmp)
$(APP)/about.o: $(APP)/buildid.h

mq_ui: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)
	$(CROSS)strip $@ || true
	@ls -l $@

$(APP)/sqlite3.o: $(APP)/sqlite3.c
	$(CC) $(SQLITE_FLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# ---- host test: the user-font path (see tests/fontcheck.c) --------------------
# Builds for the BUILD MACHINE, not the device: it needs to actually run. Uses a fake
# SD card under tests/, so no hardware and no card are involved.
FONTCHECK_SD := $(APP)/tests/sdcard
fontcheck: $(APP)/tests/fontcheck.c $(APP)/theme.c $(APP)/config.c
	@mkdir -p $(FONTCHECK_SD)/Fonts
	@ls $(FONTCHECK_SD)/Fonts/*.ttf >/dev/null 2>&1 || { \
	   echo "put any .ttf in $(FONTCHECK_SD)/Fonts/ first"; exit 1; }
	gcc -O0 -std=gnu11 -D_GNU_SOURCE -DLV_CONF_INCLUDE_SIMPLE -I$(APP) -w \
	    -DFONT_DIR_A='"$(FONTCHECK_SD)/Fonts"' -DFONT_DIR_B='"$(FONTCHECK_SD)/fonts"' \
	    -DFONT_ROOT='"$(FONTCHECK_SD)"' \
	    -o $@ $(APP)/tests/fontcheck.c $(APP)/theme.c $(APP)/config.c $(APP)/fileutil.c \
	    $(shell find $(LVGL)/src -name '*.c') -lm -lrt -pthread

# Queue write-layer test: exercises the LIST_SONG_0 mutations against a fixture DB.
# musicdb.c hardcodes DB_PATH, so this writes to /usr/data/fiio/db/song.db - it needs
# that path to exist and be WRITABLE (the directory too, for SQLite's journal).
queuecheck: $(APP)/tests/queuecheck.c $(APP)/musicdb.c $(APP)/txtfold.c
	gcc -O0 -std=gnu11 -D_GNU_SOURCE -I$(APP) -w -fsanitize=address,undefined 	    -o $@ $(APP)/tests/queuecheck.c $(APP)/musicdb.c $(APP)/txtfold.c $(APP)/sqlite3.c 	    -lpthread -ldl -lm

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) mq_ui ipodos-ui fontcheck
-include $(OBJS:.o=.d)
