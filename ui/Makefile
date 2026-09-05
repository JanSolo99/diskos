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

CFLAGS := -Os -pipe -std=gnu11 -static -pthread -D_GNU_SOURCE -DLV_CONF_INCLUDE_SIMPLE \
  -I$(APP) -I$(NUI) -Wall -MMD -MP
LDFLAGS := -static -pthread
LDLIBS  := -lm -lrt

LVGL_SRCS := $(shell find $(LVGL)/src -name "*.c")
APP_SRCS  := $(APP)/main.c $(APP)/theme.c $(APP)/fb_pan.c $(APP)/ipc.c $(APP)/ui.c $(APP)/art.c $(APP)/screenmgr.c $(APP)/home.c $(APP)/library.c $(APP)/musicdb.c $(APP)/settings.c $(APP)/config.c $(APP)/anim.c $(APP)/search.c $(APP)/saver.c $(APP)/quicksettings.c $(APP)/songinfo.c $(APP)/npmenus.c $(APP)/eqcustom.c $(APP)/apps.c $(APP)/kbinput.c $(APP)/playlistview.c $(APP)/artcache.c $(APP)/colorpick.c $(APP)/debug_ui.c $(APP)/modes.c $(APP)/wifi.c $(APP)/bt.c $(APP)/weather.c $(APP)/lyrics.c $(APP)/toast.c $(APP)/lastfm.c $(APP)/lastfm_ui.c $(APP)/md5.c $(APP)/scanner.c $(APP)/scanview.c $(APP)/fontpick.c $(APP)/sysconfig.c $(APP)/fileutil.c $(APP)/txtfold.c $(APP)/fwcaps.c $(APP)/queue.c $(APP)/font_icons_28.c $(APP)/font_weather16.c
# bundled SQLite amalgamation (compiled in-process; replaces popen sqlite3 CLI)
SQLITE_FLAGS := -Os -pipe -static -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION \
  -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED
OBJS := $(LVGL_SRCS:.c=.o) $(APP_SRCS:.c=.o) $(APP)/sqlite3.o

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

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) mq_ui ipodos-ui fontcheck
-include $(OBJS:.o=.d)
