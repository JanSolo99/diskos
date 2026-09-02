# Host tests

Small programs that run on the **build machine**, not the Disc. They exist for the parts
of the UI whose behaviour depends on something you cannot see by reading the code -
LVGL's virtual filesystem, the scanner's tag parsers against real byte layouts - and
that would otherwise only be discovered on device.

| Test | Builds with | Covers |
|---|---|---|
| `fontcheck.c` | `make fontcheck` | The user-font path in `theme.c`: VFS path handling, the built-in fallback chain that keeps `LV_SYMBOL_*` icons rendering, per-size instance caching, and surviving a cold boot with the SD card not yet mounted. |

`fontcheck` needs any `.ttf` in `tests/sdcard/Fonts/` to act as the fake card's font, and
writes to the real config and font-cache paths - run it on a build host, not on a device.

The scanner has its own in-tree harness instead: build `scanner.c` with `-DSCANNER_TEST`
and point `-DSCAN_ROOT`/`-DDB_PATH` at a scratch directory and database.
