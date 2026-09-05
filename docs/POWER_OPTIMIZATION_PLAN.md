# diskOS Power Optimization - R&D Report + Plan

## 1. The Power Reality

The device's biggest power costs are the backlight (LCD-class panel, 41-step driver, and a
`bl_power` setting that actually cuts the LED driver), the Wi-Fi and Bluetooth radios, and an
always-waking CPU. The good news is that diskOS's background threads are correctly blocking: the
IPC thread parks in `mq_timedreceive` with about one wake per second while idle, the watchdog uses
`sleep(5)`, scanner and prewarm work is user-gated, and art, lyrics, and weather polls already
pause at full screen-off. The bad news is that the main UI loop wakes the MIPS core about 33 times
per second in every state, including fully screen-off, and Wi-Fi is enabled by default with a
supervisor running every 5 seconds and no automatic-off policy.

The most important caveat is that the kernel tick behavior is unknown. If Linux 4.4 boots with
`HZ=100` and without `CONFIG_NO_HZ_IDLE`, the kernel timer interrupt wakes the CPU 100 times per
second regardless of application behavior. Reducing application loop frequency alone would then
buy little; reducing work per wake becomes the primary lever. Phase 0 probes this first.

## 2. Findings Ranked By Expected Impact

| Rank | Finding | Evidence | Cost class |
|---|---|---|---|
| 1 | Main loop `usleep(30ms)` floor applies in every state; `lv_timer_handler()`'s time-until-next-timer return is always capped down, never honored. Screen-off uses the same 33 Hz cadence as screen-on because of the 2026-08-26 tap-wake regression workaround. | `main.c:2008-2021`, ending at `usleep` | CPU wakeups, all states |
| 2 | Wi-Fi defaults to `wifi_on=1` with no screen-off or automatic-off policy. `wifi_supervise` performs a full `/proc` command scan every 5 seconds forever and may restart the supplicant after a mismatch. | `wifi.c:164-186`, `wifi.c:84-108`, default at `wifi.c:171` | Radio current and wakeups |
| 3 | BT auto-route runs `popen("bluealsa-cli ... | grep -m1 a2dpsrc")` on the LVGL thread every 3 seconds while BT has been enabled, including screen-off. This forks, executes, and greps about 20 times per minute and violates the no-slow-work-on-the-LVGL-thread rule. | `bt.c:197-226` | Process spawning |
| 4 | The UI renders while the panel is powered off. The 10-second clock push (`saver_set_clock` -> unconditional `lv_label_set_text`) and the 1-second analog-saver hands invalidate and render/flush to a dark `bl_power=4` panel. | `main.c:100-115`, `saver.c:170-178`, `saver.c:326`, `saver.c:357-358` | Wasted rendering |
| 5 | Vinyl saver style 4 performs a full 360 px anti-aliased software rotation at about 19 fps, keeps the user's full brightness while dimmed, and spins while `bl_state==1`. It stops only at full-off. | `saver.c:70-74`, `saver.c:186-216`, `main.c:1812` | Rendering in dim state |
| 6 | Dimming uses a fixed minimum of level 6/40; levels 1-5 are unused. Default brightness is 16/40 and dimming is not battery-aware. | `main.c:1987-1989`, `settings.c:364` | Backlight |
| 7 | Small screen-off wastes remain: `lastfm_tick` runs every second even when disabled; `usb_connect_watch` performs two sysfs stats and an `fopen` every second; `lhint_timer_cb` runs every 150 ms for the process lifetime; and the status poll formats/renders a battery label that Home cannot see while hidden. | `main.c:1562`, `main.c:1564`, `library.c:836`, `main.c:349`, `home.c:180` | Syscall and formatting noise |
| 8 | Weather retries forever without connectivity. With `weather_on=1`, a failed fetch retries every 20 seconds until the first success, then every 10 minutes. | `weather.c:183-184`, `settings.c:156` | Worker process bursts |
| 9 | No cpufreq, DVFS, thermal, or cpuidle interface has been exposed by the kernel; userspace is the only known lever. | `docs/HARDWARE.md:24-25`, `docs/HARDWARE.md:252` | Constraint |

### Not Problems

The following are verified not to be current power problems:

- The IPC receive thread blocks in `mq_timedreceive` (`ipc.c:273-276`).
- The watchdog sleeps for 5 seconds.
- Scanner work is user-triggered, except for the one-time automatic scan on an empty database.
- Art, lyrics, and weather timers pause at full screen-off (`main.c:1026`).
- The 3-second fuel-gauge poll is deliberate. It is an I2C keep-alive that prevents the verified
  CS43131 freeze and must not be removed (`main.c:1019-1025`, device-verified 2026-08-26).
- UI text and arc updates are already diff-guarded (`set_label_text_changed` and
  `set_progress_changed`, `ui.c:145-249`).
- Rendering uses dirty-rectangle partial updates into about 84 KB of banded buffers
  (`fb_pan.c:88-103`).

## 3. The Plan

### Phase 0 - Measure First

No code changes. Perform this on hardware over about half a day.

The fuel gauge at `/sys/class/power_supply/cw221X-bat/current_now` is a mA meter. Build a drain
model before optimizing.

#### Kernel tick and idle checks

Run:

```sh
cat /sys/devices/system/cpu/cpu0/cpuidle/*/name
cat /sys/power/state
grep -i -e '^HZ' /boot/.config 2>/dev/null
dmesg | grep -i -e 'clockevent' -e 'tick' -e 'cpuidle'
head -20 /proc/interrupts
sleep 10
head -20 /proc/interrupts
grep -c '' /proc/timer_list 2>/dev/null
```

The two `/proc/interrupts` snapshots measure timer interrupt frequency. The result determines
whether loop-frequency optimization or per-wake work reduction should receive priority.

#### Current baselines

Sample `current_now` for 60 seconds in each state and record the mean:

- Screen-on Home.
- Now Playing lit and playing.
- Dim saver with cover style.
- Dim saver with vinyl style.
- Full-off paused.
- Full-off playing.
- Full-off with Wi-Fi on.
- Full-off with Wi-Fi off.
- Full-off with Bluetooth on.

Write the values down as the acceptance metric, for example, "screen-off idle <= X mA".

#### Cheap A/B operations

- Compare `bl_power=4` against `brightness=0`.
- Check whether `wifi_down.sh` actually cuts radio current or only stops the supplicant.
- Determine whether the LCD controller continues scanning out black. This may require a small
  `FBIOBLANK` test binary; compare `current_now` with and without it.

### Phase 1 - Safe, High-Value Changes

Each item should be independently verifiable.

#### P1-1. Event-driven loop at screen-off

Replace the `usleep(30ms)` in the `bl_state==2` path with `poll()` on the raw-touch fd
(`g_touch_raw`) plus an IPC wake pipe. Use a timeout equal to the time until the next LVGL timer,
clamped to about 5 seconds so the watchdog heartbeat is stamped frequently enough.

Touch input then wakes the loop when `poll` reports `POLLIN`; drain the fd and wake immediately.
This is better than the current 30 ms polling and fixes the tap-swallowing concern that caused the
30 ms override in the first place. Power-key wake uses the same mechanism: the IPC thread writes to
a small `eventfd` or pipe whenever it sets `g_power_event` or `g_reconnected`, and the main loop
polls both descriptors.

Expected result: screen-off wake frequency falls from about 33 Hz to about 1-5 Hz, bounded by the
next LVGL timer, with no functional regression.

Risk is medium-low. The change is local to the loop tail (`main.c:2008-2021`) plus a pipe in
`ipc.c`. Also correct the inconsistency between the comments at `main.c:2000-2005` and the current
code; the comments describe a 5 Hz path that does not exist.

#### P1-2. Stop rendering to a dark panel

Pause the LVGL display refresh at full-off. Use `lv_timer_pause(lv_display_get_refr_timer(...))`
and `lv_display_enable_invalidation(disp, false)` if those LVGL 9 hooks are appropriate. On wake,
resume refresh and invalidate the full display.

Gate `saver_set_clock()` and the analog saver hands in `saver_anim_cb` on `bl_state != 2`. Keep the
3-second sysfs reads for the I2C keep-alive, but skip Home status-label formatting and rendering
while full-off.

The current render frequency is low, so this is a small saving, but it establishes the invariant
that a powered-off panel has no render or flush path. Risk is low.

#### P1-3. Move BT auto-route off the LVGL thread

Move the `bluealsa-cli` probe to the existing bounded-worker pattern used by `btconn_probe` in
`main.c:137-208`. The worker performs bounded `popen` work every 3-5 seconds and publishes a MAC
under a mutex. The LVGL timer reads only a new-sink flag.

This preserves behavior while removing process spawning and blocking I/O from the UI thread. Risk
is low to medium.

#### P1-4. Add small activity gates

Each item is expected to be about five lines and near-zero risk:

- Pause `lastfm_tick` when Last.fm is disabled (`main.c:1562`) and resume it when enabled.
- Make `usb_connect_watch` skip the `fopen` and LUN reads when `source_mode==0` and nothing has
  changed at screen-off. Its interval can drop to 3 seconds.
- Pause `lhint_timer_cb` unless `screen_current()==SCR_LIBRARY`, following the scanview pattern.
- Do not retry weather every 20 seconds without a `wlan0` link. Stretch retries to 5 minutes until
  a link appears.
- Back `wifi_supervise`'s 5-second `/proc` scan off to about 30 seconds while screen-off and the
  supplicant is stable.

#### P1-5. Battery-aware default dimming

This is a behavior decision rather than a bug. Consider an adaptive dim level, for example level 3
at screen-off, instead of the fixed 6/40. Consider a later "Dim on battery" default-tightening
option, but treat that as a user-facing decision.

### Phase 2 - Structural Changes

These require product decisions or broader changes.

#### P2-1. Radio sleep policy

Add Settings -> System -> `Wi-Fi sleep` with behavior-neutral defaults:

- Off, the current behavior.
- With screen.
- After 15 minutes idle.

When enabled, at full-off and after the selected idle period, run `wifi_down.sh`, remember the
user's intent through the existing `wifi_on` supervision, and re-enable Wi-Fi on wake, power-key
activity, or playback.

This is likely the largest real-world radio saving, but it must remain opt-in because Last.fm,
remote applications, and Bluetooth passthrough can depend on Wi-Fi. First verify that
`wifi_down.sh` powers down the BCM43438 regulator; if it only kills the supplicant, the saving is
limited to process and `/proc` activity.

#### P2-2. Vinyl saver policy

Choose one of these behaviors:

- Keep full brightness and full-speed rotation only while lit (`bl_state==0`); when dimmed, reduce
  to about 2-4 fps and dim like other savers. This removes the `saver_wants_bright` exception in
  `saver.c:74` and `main.c:1987`.
- Keep the current behavior and document vinyl saver as a deliberate battery cost.

The vinyl saver is the largest deliberate render cost while dimmed.

#### P2-3. Event-driven loop in all states

Extend P1-1 beyond full-off. When awake, use LVGL's next-timer discipline so an idle Home screen
can sleep until its next timer rather than waking at a fixed 33 Hz. During animation or touch,
LVGL timers still keep the loop responsive. Use the same poll machinery in every state so CPU
wakeups are proportional to work rather than a fixed clock.

### Phase 3 - Hardware Probes and Out-of-Scope Items

- Probe cpuidle and suspend presence in Phase 0. If `CONFIG_NO_HZ` and cpuidle exist, Phase 1
  numbers may improve by roughly 8-30 times. If not, rely on per-wake work reduction and accept
  the tick cost.
- Build a small standalone `FBIOBLANK` test, modeled after `fbshot`, to determine whether the LCD
  controller can stop scanning black. This may reveal panel-level savings behind `bl_power`.
- The MCU deep-sleep path is documented as unreachable from mqueue. `COMMAND_MAP.md:243-249`
  lists 63 `SET_POWER_DOWN_TO_MCU` call sites inside `mq_player`. Real suspend engineering would
  require a custom player and is explicitly out of scope.

## 4. Safety Rails

- Never remove or lengthen the 3-second fuel-gauge poll. It is the verified CS43131/I2C freeze fix
  (`main.c:1019-1025`). The P1 design keeps the sysfs reads and gates only rendering.
- Do not add a sleep path that breaks tap-wake. The 2026-08-26 regression, in which a 60 ms sleep
  swallowed quick taps, is why P1-1 uses `poll()` rather than a plain longer `usleep`.
- Do not touch `mq_player`, read `event0`, or invent IPC tags. Wi-Fi, Bluetooth, and backlight
  changes are sysfs or script operations and do not require player involvement.
- Keep radio policy off by default until Phase 0 proves it saves mA.
- Any longer sleep must still stamp `g_heartbeat` within the 45-second watchdog window. A poll
  timeout of at most 5 seconds satisfies this.
- Hand-deployed binaries revert at the next boot because of S97. Cross-reboot battery tests need
  `tools/diskos-deploy.sh` per boot or a real flash.
- Every build-checked change must pass `file ui/mq_ui` with a MIPS result before deployment.

## 5. Sequencing And Acceptance Gates

1. Run Phase 0 probes and write the mA table for at least six states. Decide whether loop frequency
   (P1-1/P2-3) or per-wake work (P1-2 through P1-4 and P2-2) dominates.
2. Land P1-2, P1-3, and P1-4 as small independent changes. Verify no wake or rendering regression
   and leave the battery module untouched.
3. Land P1-1 and verify tap-wake, power-key wake, watchdog behavior, and screen-off current.
4. Treat P2-1 and P2-2 as product decisions and test them with the user on hardware.
5. Run Phase 3 probes only if Phase 0 shows headroom or to justify kernel work, which is a separate
   project.

## Honest Estimate

P1-2, P1-3, and P1-4 are nearly free and strictly reduce waste. P1-1 is the real CPU win, but it
only pays off fully if the kernel tick permits it; Phase 0 will tell us. The two radio policies,
Wi-Fi sleep and moving BT auto-route to a worker, are the largest real-world battery items users
would feel. The vinyl saver is the largest deliberate but hidden cost while dimmed.

The planned Queue and Now Playing additions should carry almost no idle power cost because they
use no perpetual timers and build one list on demand, provided they follow the pause-at-screen-off
and self-delete timer conventions.
