/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <ucontext.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>   /* mmap /dev/mem: read the Vol-Up GPIO pin level directly at boot */
#include <stdint.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mount.h>     /* mount(): direct SD mount fallback when the player's uevent listener won't (V2.40) */
#include <sys/stat.h>      /* mkdir() for the SD mount point */
#include <linux/input.h>   /* EVIOCGKEY / KEY_MAX for the boot-time Vol-Up override */
#include <net/if.h>
#include <limits.h>     /* PATH_MAX for the watchdog's /proc/self/exe re-exec */
#include "lvgl/lvgl.h"
#include "fb_pan.h"
#include "screens.h"
#include "anim.h"
#include "ipc.h"
#include "fwcaps.h"
#include "musicdb.h"
#include "lvgl/src/drivers/evdev/lv_evdev.h"
#include "config.h"
#include "lastfm.h"
#include "scanner.h"

static lv_indev_t *g_touch = NULL;
static int         g_touch_raw = -1;   /* a 2nd, RAW read-only fd on the touch evdev, used ONLY to
                                          wake a screen-off panel: LVGL coalesces a quick tap into a
                                          final RELEASED (no press edge -> no wake), so we watch the
                                          raw stream where ANY event means "a finger touched". */

/* Manual "Sleep" (Quick Settings tile): a deliberate screen-off request. It must dim+power
 * the panel even when the auto-screensaver is Off, so it can't ride the saver_timeout>0 path.
 * g_manual_sleep is armed by ui_request_sleep(); the saver block below honours it and clears
 * it once the panel is woken (touch takes us off SCR_SAVER). */
static volatile int g_manual_sleep = 0;
static uint32_t g_manual_sleep_at = 0;
void ui_request_sleep(void){ g_manual_sleep = 1; g_manual_sleep_at = lv_tick_get(); }
static lv_obj_t   *g_dbgdot = NULL;
static int         g_dbg = 0;          /* show tap dot (only if /usr/data/touch_dbg) */

#define SWIPE_THRESH_DEFAULT 60
/* The back-swipe must START on the left side of the 360px screen (and travel
 * rightwards), so a horizontal swipe on the right side never goes back. */
#define BACK_START_MAX_X 64    /* back-swipe must START near the left edge (iOS-style) so it
                                * doesn't fight horizontal drift during a vertical list scroll */
/* On Now Playing the seek ring fills the screen, so a seek can start anywhere.
 * A navigation swipe (back / hub) is told apart from a seek by being a LONG,
 * STRAIGHT, horizontal slide - a seek drag follows the ring's curve, so it is
 * shorter and/or carries a vertical component. */
#define NP_NAV_DIST 95    /* min horizontal travel for a back/hub swipe on NP (eased from 110) */
#define NP_NAV_STRAIGHT 3 /* require |dx| > this * |dy| (nearly horizontal) */
static int g_swipe_thresh = SWIPE_THRESH_DEFAULT;   /* cached for the hot loop */

int ui_get_swipe_thresh(void){ return g_swipe_thresh; }

/* idle mirror for the prewarm worker: 1 once the screen has dimmed/off (saver), i.e.
 * the user isn't actively looking. Updated each main-loop iteration from bl_state.
 * _Atomic: written here (main thread), read by the prewarm worker thread. */
static _Atomic int g_bl_idle = 0;
int ui_main_is_idle(void){ return g_bl_idle; }
void ui_apply_swipe_thresh(int px){   /* live, no flash write */
    if(px < 20) px = 20; if(px > 200) px = 200;
    g_swipe_thresh = px;
}
void ui_set_swipe_thresh(int px){     /* live + persist */
    ui_apply_swipe_thresh(px);
    cfg_set_int("swipe_thresh", g_swipe_thresh);
}
static void swipe_thresh_load(void){
    g_swipe_thresh = cfg_get_int("swipe_thresh", SWIPE_THRESH_DEFAULT);
}

static void dbgdot_init(void){
    g_dbgdot = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_dbgdot);
    lv_obj_set_size(g_dbgdot, 26, 26);
    lv_obj_set_style_radius(g_dbgdot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_dbgdot, lv_color_hex(0x00FF66), 0);
    lv_obj_set_style_bg_opa(g_dbgdot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_dbgdot, th_text(), 0);
    lv_obj_set_style_border_width(g_dbgdot, 2, 0);
    lv_obj_add_flag(g_dbgdot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_dbgdot, LV_OBJ_FLAG_CLICKABLE);
}

static void go_settings(void){ screen_show(SCR_SETTINGS); }

static void clock_tick(lv_timer_t *t){
    (void)t;
    time_t now = time(NULL);
    struct tm lt; localtime_r(&now, &lt);
    char hm[12], date[24];
    int h24 = cfg_get_int("time_24h", 1);
    if(h24){
        strftime(hm, sizeof hm, "%H:%M", &lt);
    } else {
        strftime(hm, sizeof hm, "%I:%M %p", &lt);   /* 12-hour with AM/PM */
        if(hm[0]=='0') memmove(hm, hm+1, strlen(hm)); /* drop leading zero */
    }
    strftime(date, sizeof date, "%a %d %b", &lt);
    home_set_clock(hm, date);
    saver_set_clock(hm, date);
}
void ui_clock_refresh(void){ clock_tick(NULL); }

/* Status indicators: poll battery (cw221X fuel gauge), wifi (wlan0 has an IP),
 * and bt (any active HCI connection) from sysfs/tools, push to the home row.
 * One short popen on a slow timer - battery/link state changes slowly. */
/* ---- Bluetooth connection probe (OFF the LVGL thread) ----------------------
 * There is no sysfs "a device is connected" node for this BT stack, so the only
 * answer available is `hcitool con`. That used to run as a bare popen() on the
 * LVGL thread every ~12s - INCLUDING while the screen was off, because the status
 * poll is deliberately left running at screen-off as an I2C keep-alive.
 *
 * popen() is unbounded. When the BT stack wedges (an adapter mid-teardown, a
 * half-connected headset), hcitool blocks in the kernel and takes the whole UI
 * thread down with it: no repaint, no touch handling, and - the symptom users
 * actually reported - a screen that will not wake again until the device is
 * force-restarted, because the wake path itself lives in that same loop.
 *
 * So the probe now runs on a short-lived detached worker, and it is bounded twice
 * over: the child is killed after PROBE_MS, and only one probe is ever in flight,
 * so a genuinely stuck hcitool costs one leaked worker rather than the UI. The
 * LVGL thread only ever reads the published answer. */
#define BT_PROBE_MS 2500
static _Atomic int g_bt_conn;        /* last published answer (0/1) */
static _Atomic int g_bt_probe_busy;  /* 1 while a worker is in flight */

/* Run `hcitool con` with its stdout on a pipe, bounded. 1 = a connection exists. */
static int btconn_probe_blocking(void)
{
    int fds[2];
    if(pipe(fds) != 0) return 0;
    pid_t pid = fork();
    if(pid < 0){ close(fds[0]); close(fds[1]); return 0; }
    if(pid == 0){
        setpgid(0, 0);                     /* own group: one kill takes the whole probe */
        dup2(fds[1], 1);
        int nul = open("/dev/null", O_RDWR);
        if(nul >= 0){ dup2(nul, 0); dup2(nul, 2); if(nul > 2) close(nul); }
        for(int fd = 3; fd < 256; fd++) close(fd);
        execlp("hcitool", "hcitool", "con", (char*)NULL);
        _exit(127);
    }
    setpgid(pid, pid);
    close(fds[1]);
    if(fcntl(fds[0], F_SETFL, O_NONBLOCK) != 0){ /* keep going: the timeout still bounds us */ }

    char buf[512]; int len = 0, found = 0, rc;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for(;;){
        int n = (int)read(fds[0], buf + len, (int)sizeof buf - 1 - len);
        if(n > 0){
            len += n; buf[len] = 0;
            if(strstr(buf, "handle")) found = 1;
            if(len >= (int)sizeof buf - 1) len = 0;   /* only the marker matters; recycle */
        }
        int status = 0;
        pid_t w = waitpid(pid, &status, WNOHANG);
        if(w == pid){ pid = 0; break; }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - t0.tv_sec)*1000 + (now.tv_nsec - t0.tv_nsec)/1000000;
        if(ms >= BT_PROBE_MS) break;
        usleep(20000);
    }
    if(pid > 0){                            /* timed out: kill and reap, bounded */
        kill(-pid, SIGKILL);
        int status = 0;
        for(int k = 0; k < 100; k++){ if(waitpid(pid, &status, WNOHANG) == pid) break; usleep(10000); }
        found = 0;                          /* a wedged stack is not a live connection */
    }
    /* drain whatever the child wrote before exiting */
    while((rc = (int)read(fds[0], buf, sizeof buf - 1)) > 0){ buf[rc] = 0; if(strstr(buf, "handle")) found = 1; }
    close(fds[0]);
    return found;
}
static void *btconn_worker(void *arg)
{
    (void)arg;
    atomic_store(&g_bt_conn, btconn_probe_blocking());
    atomic_store(&g_bt_probe_busy, 0);
    return NULL;
}
static void btconn_probe_request(void)
{
    int expected = 0;
    if(!atomic_compare_exchange_strong(&g_bt_probe_busy, &expected, 1)) return;  /* one at a time */
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 64*1024);
    if(pthread_create(&th, &at, btconn_worker, NULL) != 0) atomic_store(&g_bt_probe_busy, 0);
    pthread_attr_destroy(&at);
}
static int btconn_get(void){ return atomic_load(&g_bt_conn); }

/* ---- UI liveness watchdog --------------------------------------------------
 * Belt and braces for the same class of failure. The main loop stamps a heartbeat
 * every iteration - even in deep idle it runs ~30x/s - so a heartbeat that has not
 * moved for WATCHDOG_MS means the LVGL thread is genuinely stuck, not merely idle.
 * The screen is then unwakeable by touch (the wake path is in that loop), which is
 * exactly the "won't wake up, had to force restart it" report.
 *
 * Rather than leave the user holding the power button, re-exec ourselves. Only the
 * UI restarts: mq_player keeps the music and the SD card, which is the one thing
 * that must never be interrupted on this device (killing it hard-reboots the Disc).
 * Same PID, so anything supervising us is undisturbed. */
/* ---- restart the UI in place ----------------------------------------------
 * Re-exec our own binary. This is the ONE restart that is safe on this device: only
 * mq_ui goes down, mq_player keeps the music and the SD card (killing the player
 * releases the card and the Disc's controller hard-reboots). Same pid, so anything
 * supervising us is undisturbed, and playback does not even pause.
 *
 * Used by the theme and font pickers (screens resolve colours and faces once, when
 * they are built) and by the liveness watchdog below.
 *
 * Inherited descriptors are marked close-on-exec rather than closed: if execv fails
 * we are still alive with a working framebuffer and player queues, whereas closing
 * first would leave a UI that cannot draw. Without this, every restart would leak
 * the previous image's fds into the next one, which a watchdog restart loop would
 * eventually turn into fd exhaustion.
 *
 * Returns only on failure. */
void ui_restart_self(int flush_config)
{
    if(flush_config){ cfg_flush(); sync(); }
    for(int fd = 3; fd < 256; fd++){
        int fl = fcntl(fd, F_GETFD);
        if(fl >= 0) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
    }
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if(n <= 0) return;
    self[n] = 0;
    /* The marker goes in the ENVIRONMENT, never in argv. fiio_init's watchdog finds us
     * by process name - main()'s argv[0] normaliser exists precisely because getting
     * that match wrong made the watchdog kill diskOS and respawn the stock UI - and an
     * extra argv element would change the command line it inspects. The environment is
     * not part of that, carries the same "cannot be left stale" property (it exists
     * only in the image we are about to exec), and needs no second exec: passing the
     * bare name as argv[0] means the normaliser has nothing to fix. */
    setenv("DISKOS_UI_RESTART", "1", 1);
    char *av[] = { (char *)"mq_ui", NULL };
    execv(self, av);
}

#define WATCHDOG_MS 45000
static _Atomic uint32_t g_heartbeat;
/* Launching an external app deliberately blocks the LVGL loop for as long as the app
 * runs (it owns the framebuffer meanwhile), which is indistinguishable from a stall.
 * Suspend the watchdog for the duration so a long-running app is never mistaken for
 * a hang - the one legitimate reason the heartbeat stops. */
static _Atomic int g_watchdog_paused;
void ui_watchdog_pause(int on);   /* exported: see screens.h */
static void watchdog_pause(int on){
    atomic_store(&g_watchdog_paused, on ? 1 : 0);
    if(!on) atomic_store(&g_heartbeat, lv_tick_get());   /* fresh stamp on resume */
}
static void *ui_watchdog(void *arg)
{
    (void)arg;
    for(;;){
        sleep(5);
        if(atomic_load(&g_watchdog_paused)) continue;
        uint32_t hb = atomic_load(&g_heartbeat);
        if(hb == 0) continue;                       /* main loop hasn't started stamping yet */
        uint32_t age = lv_tick_elaps(hb);
        if(age < WATCHDOG_MS) continue;
        fprintf(stderr, "WATCHDOG: UI thread stalled %ums - restarting mq_ui\n", age);
        fflush(stderr);
        /* No config flush here: this is crash recovery, not a settings change, and the
         * stuck main thread may be mid-write. Just re-exec. */
        ui_restart_self(0);
        /* exec failed - keep watching rather than exiting (exiting would leave the
         * user with no UI at all, which is strictly worse than a frozen one). */
        atomic_store(&g_heartbeat, lv_tick_get());
    }
    return NULL;
}
/* Exposed so any other deliberately-blocking main-thread operation can declare
 * itself, rather than being mistaken for a hang. */
void ui_watchdog_pause(int on){ watchdog_pause(on); }

static void ui_watchdog_start(void)
{
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 64*1024);
    pthread_create(&th, &at, ui_watchdog, NULL);
    pthread_attr_destroy(&at);
}

/* battery + wifi read directly (no shell spawn); only BT keeps a light call since the
 * device exposes no sysfs connection indicator and the HCI ioctl layout is fragile. */
static void status_poll_cb(lv_timer_t *t){
    (void)t;
    /* Runs on the fast (3s) timer so charging/battery/wifi track plug/unplug promptly. The BT check
     * is the one shell spawn (hcitool), so it runs only every 4th tick (~12s) and caches otherwise. */
    static int bt_cached = 0, tick = 0;   /* bt_cached mirrors the worker's last answer */
    int batt = -1, charging = 0, wifi = 0, bt;

    FILE *bf = fopen("/sys/class/power_supply/cw221X-bat/capacity", "r");
    if(bf){ if(fscanf(bf, "%d", &batt) != 1) batt = -1; fclose(bf); }
    /* This cw221X fuel gauge exposes NO `status` node - charging shows via `current_now`, which is a
     * crude boolean on this driver: 1 while charging (USB/charger present), 0 on battery. (Live-probed
     * 2026-08-11: plugged=1 @3.99V, unplugged=0 @3.72V.) */
    long cur = 0;
    FILE *cf = fopen("/sys/class/power_supply/cw221X-bat/current_now", "r");
    if(cf){ if(fscanf(cf, "%ld", &cur) != 1) cur = 0; fclose(cf); }
    charging = (cur > 0);

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if(s >= 0){
        struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
        strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ-1);
        if(ioctl(s, SIOCGIFADDR, &ifr) == 0) wifi = 1;   /* has an IPv4 address */
        close(s);
    }
    /* "has an IP" alone is NOT enough: a DHCP lease lingers on wlan0 after the AP vanishes, so the
     * icon stayed lit while actually disconnected. Require the link to be ASSOCIATED. Measured on this
     * bcmdhd chip (2026-08-23): on disconnect `carrier` STAYS 1 (unreliable), but `operstate` goes
     * up->dormant and wpa_state->DISCONNECTED. So gate on operstate=="up". A failed/absent read
     * (iface down, no wlan0) falls through to disconnected - the correct/safe default. */
    if(wifi){
        char op[16] = {0};
        FILE *of = fopen("/sys/class/net/wlan0/operstate", "r");
        if(of){ if(!fgets(op, sizeof op, of)) op[0] = 0; fclose(of); }
        wifi = (strncmp(op, "up", 2) == 0);
    }

    if(tick++ % 4 == 0) btconn_probe_request();   /* refresh BT ~every 12s, OFF this thread */
    bt = bt_cached = btconn_get();

    home_set_status(batt, charging, wifi, bt);
}
void ui_status_refresh(void){ status_poll_cb(NULL); }

/* Persistent clock: the device has a (backed) RTC but the boot init never loads
 * it, so the system clock starts at the 2020 kernel default. Like stock
 * mq_player, load the system clock FROM the RTC at startup (offline-safe) and
 * write it BACK periodically so the RTC stays current (ntpd corrects the system
 * clock when WiFi is up, and that corrected time then gets saved to the RTC). */
static int run_bounded(char *const argv[], int timeout_ms);   /* defined below (killable child + hard timeout) */
/* Bounded so a busy I2C/RTC can't freeze the LVGL thread while the screen is idle: a blocking
 * system("hwclock -w") on the main thread was a credible alive-but-hung idle path. */
static void hwclock_save_tick(lv_timer_t *t){ (void)t; char *a[] = { "hwclock", "-w", NULL }; run_bounded(a, 3000); }

/* ---- IPC command frames (decoded from mq_player's command table @0x7b7c90
 * and live-verified against the player log oracle) --------------------------
 * Frame = TAG(4) + LEN(4hex total strlen) + DATA.  DATA field classes:
 *   class1: <idx 4hex>          + string
 *   class2: <f1 4hex><f2 4hex>  + string
 * SEEK     : 0103 (class2) <ms hi16><ms lo16>   e.g. 0103001000001388 = 5000ms  [VERIFIED]
 * PLAY     : 0100 (class2) <startpos-1 4hex><list_type 4hex><name>             [VERIFIED]
 *            list_type 0001=all-songs 0002=artist 0003=album 0006=favourites
 *            000A=genre (0000=resume current, no rebuild); player rebuilds
 *            LIST_SONG_0 from SONG via its own SQL, then starts at the track.
 * WORKMODE : 0102 000C <4-hex mode>   (Roon loop/shuffle; no-op in LOCAL - verify by ear)
 * EQ       : 0689 000C <4-hex preset> (selector - verify)
 * RESCAN   : 0622000C0001 (NAS-family; unverified) */
void ui_seek_to(long ms){
    if(ms < 0) ms = 0;
    char f[32]; snprintf(f, sizeof f, "01030010%08lX", (unsigned long)ms);
    ipc_send_cmd(f);
    fprintf(stderr,"seek %ldms -> %s\n", ms, f); fflush(stderr);
}
void ui_apply_eq(int preset){
    /* 0..10 = built-in presets (off..retro, sibilance 1/2); 11..20 = user/custom PEQ slots
     * (User 1 = 11). Cap at 20 so the custom EQ can select its slot. */
    if(preset < 0) preset = 0; if(preset > 20) preset = 20;
    char f[16]; snprintf(f, sizeof f, "0689000C%04X", preset);
    ipc_send_cmd(f);
    fprintf(stderr,"eq %d -> %s\n", preset, f); fflush(stderr);
}

/* --- Audio/DAC cluster (commands reverse-engineered 2026-06-30, RE_CATALOGUE §5c) ---------
 * These apply LIVE only; the player persists to SYSCONFIG on shutdown, not on the command.
 * So diskOS owns persistence: settings.c stores the chosen value in diskos.conf and we
 * re-send the commands once the player is ready (ui_reapply_audio). A value of -1 in cfg =
 * "unmanaged / System default" -> we send nothing (don't change the user's sound). */
void ui_set_dre(int on){        if(on<0) return; ipc_send_cmd(on   ? "0812000C0000" : "0812000C0001"); }  /* on=0000, off=0001 */
void ui_set_gain(int high){     if(high<0) return; const char *tag=fw_gain_tag(); if(!tag){ fprintf(stderr,"gain: unverified firmware (MAIN_OS_VER=%d) -> not sending\n", fw_os_ver()); return; } char f[16]; snprintf(f,sizeof f,"%s000C%04X", tag, high?1:0); ipc_send_cmd(f); } /* Low=0, High=1; tag firmware-gated (V2.09=0645, V2.28=0649); fail closed on unknown fw */
void ui_set_output(int spdif){  if(spdif<0) return; ipc_send_cmd(spdif ? "0666000C0004" : "0666000C0006"); } /* SPDIF=4, analog=6 */
void ui_set_dac_filter(int idx){ if(idx<0||idx>5) return; char f[16]; snprintf(f,sizeof f,"0653000C%04X",idx); ipc_send_cmd(f); }
/* ReplayGain / volume-levelling: 0718 (RE-confirmed, on-device-safe 2026-08-24). 0=Off 1=Track 2=Album. */
void ui_set_replay_gain(int v){ if(v<0||v>2) return; char f[16]; snprintf(f,sizeof f,"0718000C%04X",(unsigned)v); ipc_send_cmd(f); }
/* more settings decoded 2026-06-30 (RE_CATALOGUE §5d) - config/mixer commands, applied on
 * live user change only (never blind-sent at boot). */
void ui_set_gapless(int on){   if(on<0) return; ipc_send_cmd(on ? "0647000C0001" : "0647000C0000"); }
void ui_set_memory(int mode){  if(mode<0||mode>2) return; char f[16]; snprintf(f,sizeof f,"0684000C%04X",mode); ipc_send_cmd(f); }
void ui_set_maxvol(int v){     if(v<0) v=0; if(v>120) v=120; char f[16]; snprintf(f,sizeof f,"0711000C%04X",v); ipc_send_cmd(f); }
/* balance: v in -N..+N; 0=center, v>0 -> 0x00NN (one side), v<0 -> 0x01NN (other side). */
void ui_set_balance(int v){ if(v < -10 || v > 10) return;   /* enforce UI range; avoids -INT_MIN UB */
                            int fv = (v>0) ? (v & 0xFF) : (v<0) ? (0x0100 | ((-v) & 0xFF)) : 0;
                            char f[16]; snprintf(f,sizeof f,"0713000C%04X",fv); ipc_send_cmd(f); }

/* ---- Bluetooth audio output routing (live-captured from working stock mq_ui) --------------
 * Follow stock's pre-stop/mode/work/BT-init/route/work/codec/volume transition. The
 * out_dev=LOCAL(6) pre-stop normalizes player state before out_dev=BT(2), avoiding the
 * observed mq_player crash/reboot. Codec VALUE1=0 is SBC; keep the MAC payload so the
 * frame shape matches stock. Pause/seek/play resumes the current track after routing.
 * g_route_mac = the MAC we've routed to ("" = local/analog; kept for validation + dedup). */
static char g_route_mac[20] = "";
static int g_playing = 0;   /* published each tick by the main loop's play/pause inference; the routing
                             * fns read THIS, not the raw st.state (which reports 0 while playing). */
static void route_uppercase(const char *in, char *out, int cap){
    int j=0; for(int i=0; in[i] && j<cap-1; i++){ char c=in[i]; if(c>='a'&&c<='z') c-=32; out[j++]=c; } out[j]=0;
}
int ui_route_bt(const char *mac){
    if(!mac) return -1;
    char norm[20]; route_uppercase(mac, norm, sizeof norm);
    if((int)strlen(norm) != 17) return -1;             /* must be AA:BB:CC:DD:EE:FF */
    for(int i=0;i<17;i++){
        if((i+1)%3 == 0){ if(norm[i] != ':') return -1; }
        else if(!((norm[i]>='0'&&norm[i]<='9') || (norm[i]>='A'&&norm[i]<='F'))) return -1;
    }
    if(!strcmp(norm, g_route_mac)) return 0;            /* already routed to this device */
    track_state_t st; ipc_get_state(&st);
    int was_playing = g_playing;   /* real play state; raw st.state is unreliable (reports 0 while playing) so
                                    * the old `st.state==1` was false during playback -> pause was SKIPPED */
    if(was_playing && ipc_send_cmd("0201000C0000") < 0) return -1;   /* pause; abort route if it can't be sent (don't switch output unpaused) */
    if(ipc_send_cmd("0666000C0006") < 0) return -1;     /* stock pre-stop: normalize local output state */
    ipc_send_cmd("0642000C0000");                       /* reset network/output mode */
    ipc_send_cmd("0657000C0008");                       /* work-mode 8 (required orchestration, not our work_mode) */
    ipc_send_cmd("06c1000C0000");                       /* initialize the player's BT subsystem */
    if(ipc_send_cmd("0666000C0002") < 0) return -1;     /* out_dev = BT source */
    ipc_send_cmd("0657000C0008");
    char f[48]; snprintf(f, sizeof f, "06b3%04X0000%s", (unsigned)(12+strlen(norm)), norm);
    if(ipc_send_cmd(f) < 0) return -1;                   /* SBC codec, stock-shaped MAC payload */
    if(st.volume_seq){ char v[16]; snprintf(v, sizeof v, "0715000C%04X", st.volume); ipc_send_cmd(v); }
    /* Safe minimal resume; deterministic load/play remains a future improvement. */
    if(st.have_track && st.position_ms > 0) ui_seek_to(st.position_ms);  /* resume position, not restart */
    if(was_playing) ipc_send_cmd("0201000C0000");       /* play */
    snprintf(g_route_mac, sizeof g_route_mac, "%s", norm);
    fprintf(stderr,"route BT %s (playing=%d pos=%ldms)\n", norm, was_playing, st.position_ms); fflush(stderr);
    return 0;
}
int ui_route_analog(void){
    if(!g_route_mac[0]) return 0;                       /* already on local/analog */
    track_state_t st; ipc_get_state(&st);
    int was_playing = g_playing;   /* real play state (raw st.state reports 0 while playing) */
    if(was_playing && ipc_send_cmd("0201000C0000") < 0) return -1;   /* pause; abort route if it can't be sent (don't switch output unpaused) */
    ipc_send_cmd("0666000C0006");                       /* out_dev = analog/local DAC */
    ipc_send_cmd("0657000C0008");
    if(st.have_track && st.position_ms > 0) ui_seek_to(st.position_ms);
    if(was_playing) ipc_send_cmd("0201000C0000");       /* play */
    g_route_mac[0] = 0;
    fprintf(stderr,"route analog (playing=%d)\n", was_playing); fflush(stderr);
    return 0;
}

/* Working-mode (audio SOURCE) switch. Replays the stock V2.28 sequences captured + live-verified
 * 2026-08-23: each sets the gadget selector byte (0642) + audio route (0657); the
 * player's state-machine thread reads the 0642 byte edge-triggered and (re)builds the USB gadget.
 * 0=Local 1=USB-DAC 2=BT-Receiving 3=USB-Storage. Pause first so the path isn't reconfigured mid-
 * output; only Local resumes (the others hand audio to the host/BT, where local playback is moot). */
/* Best-effort mirror of the current source mode. diskOS boots the player forced to LOCALPLAYER, so
 * 0 is correct at startup; it is NOT authoritative after a bare mq_ui restart mid-mode, so it is used
 * ONLY to show the picker checkmark, never to block a switch (Local must always be re-issuable). */
/* _Atomic: WRITTEN on the UI thread (ui_set_source_mode) and READ on the coldplug worker thread
 * (coldplug_should_run / coldplug_thread). A plain int here is a C11 data race; the atomic makes the
 * load/store well-defined (seq_cst) without a mutex for this single word. */
static _Atomic int g_source_mode = 0;
static _Atomic uint32_t g_source_switch_at;   /* tick of our last explicit switch (see below) */
int ui_get_source_mode(void){ return g_source_mode; }

/* What the device is ACTUALLY doing, as opposed to what we last asked for.
 *
 * g_source_mode is only a mirror of our own commands, and mq_player can enter USB
 * storage on its own the moment a computer is plugged in - without going through
 * ui_set_source_mode(). That is what makes the Working Mode screen show a tick
 * beside "Local Playback" while nothing will play, and why re-tapping Local
 * "forces" it: the tick was reporting our intent, not the truth.
 *
 * The gadget state is authoritative, so consult it - but only once our own last
 * switch has had time to land. Tearing the storage gadget down is asynchronous, so
 * for a few seconds after switching to Local the card is still exported and a naive
 * read would report Storage, flicker the tick back, and re-trigger the USB watcher
 * below in a loop. */
static int sd_exported_to_host(void);   /* defined with the coldplug code below */
#define SOURCE_SETTLE_MS 6000
int ui_source_mode_effective(void)
{
    int mine = g_source_mode;
    uint32_t at = atomic_load(&g_source_switch_at);
    if(at && lv_tick_elaps(at) < SOURCE_SETTLE_MS) return mine;   /* our switch is still settling */
    if(mine == 0 && sd_exported_to_host()) return 3;              /* the player exported the card itself */
    return mine;
}

/* Serialises the coldplug worker's "check Local + emit SD add" against ui_set_source_mode's "publish
 * mode + queue the gadget export". A plain flag cannot close the check->emit TOCTOU (the worker can
 * pass its Local check, be preempted, and emit after export is queued); this mutex makes the two
 * critical sections mutually exclusive, so no 'add' is ever emitted while an export is being
 * initiated. Held only for microseconds on each side (a sysfs write / a few mq_sends). */
static pthread_mutex_t g_sd_mode_mu = PTHREAD_MUTEX_INITIALIZER;

/* SD-SAFETY INVARIANT: entering Storage/USB-DAC hands the card to the host via the player's gadget
 * builder, which unmounts /dev/mmcblk0 device-side first (stock behaviour). diskOS's own writable
 * state (song DB, config, logs) lives on /usr/data (NAND), not the SD; it only READS media off the
 * card and those reads fail cleanly once it is exported -> concurrent-access corruption isn't
 * reachable from here. (Art-cache writes to the SD are lazy/paused; a future belt-and-braces step is
 * to quiesce them explicitly before Storage - tracked separately.) */
int ui_set_source_mode(int mode){
    if(mode < 0 || mode > 3) return -1;                  /* validate BEFORE touching the audio path */
    int was_playing = g_playing;
    if(was_playing && ipc_send_cmd("0201000C0000") < 0) return -1;   /* pause; abort if it won't queue */
    /* Take the SD-mode lock for the whole publish+export sequence so the coldplug worker cannot emit
     * an SD 'add' while this export is being initiated. Publish the intended mode FIRST (before any
     * gadget command) so a worker that runs the instant we unlock already sees the non-Local mode. */
    pthread_mutex_lock(&g_sd_mode_mu);
    g_source_mode = mode;
    int rc = 0;
    #define SND(f) do{ if(ipc_send_cmd(f) < 0) rc = -1; }while(0)
    SND("0666000C0006");                                  /* mandatory force-local pre-stop - ALWAYS first */
    switch(mode){
        case 0: SND("0642000C0000"); SND("0657000C0008"); break;                     /* Local */
        case 1: SND("0642000C0002"); SND("0657000C0008"); break;                     /* USB-DAC -> uac2 */
        case 2: SND("0818000C0000"); SND("0642000C0000"); SND("0657000C0006"); break;/* BT sink */
        case 3: SND("0642000C0001"); SND("0657000C0008"); break;                     /* Storage -> mass_storage */
    }
    #undef SND
    if(rc < 0){
        /* a frame failed to queue -> the transition may be partial. Fail CLOSED to local so we never
         * strand the SD exported / a half-built gadget while reporting success, then report failure. */
        ipc_send_cmd("0666000C0006"); ipc_send_cmd("0642000C0000"); ipc_send_cmd("0657000C0008");
        g_source_mode = 0;
        pthread_mutex_unlock(&g_sd_mode_mu);
        fprintf(stderr,"source mode %d FAILED mid-sequence -> forced local\n", mode); fflush(stderr);
        return -1;
    }
    if(mode == 0 && was_playing) ipc_send_cmd("0201000C0000");   /* only Local resumes playback */
    /* g_source_mode was already published above (before the gadget commands) for coldplug safety. */
    pthread_mutex_unlock(&g_sd_mode_mu);
    atomic_store(&g_source_switch_at, lv_tick_get());   /* settle window for ui_source_mode_effective */
    fprintf(stderr,"source mode -> %d (was_playing=%d)\n", mode, was_playing); fflush(stderr);
    return 0;
}

/* ---- what happens when a computer is plugged in ---------------------------
 * mq_player opens the card as USB storage on its own as soon as a host enumerates
 * it, whatever the Disc was doing - so plugging in to charge stops the music and
 * hands the card away. "On USB Connect" (Settings -> System) decides what diskOS
 * does about that:
 *   0 Ask           - a prompt, so a charge-only cable doesn't silently stop playback
 *   1 Keep Playing  - put us straight back into Local Playback
 *   2 USB Storage   - the stock behaviour; leave it alone
 *   3 USB DAC       - switch to the sound-card gadget instead
 *
 * We act on the EDGE into an unrequested export only, so the user can still pick
 * Storage by hand from the Working Mode screen and have it stick, and unplugging
 * re-arms it for the next connection. */
static void usb_connect_watch(lv_timer_t *t)
{
    (void)t;
    static int armed = 1;                 /* 1 = not currently in an unrequested export */
    static int saw_user_export = 0;       /* a user-chosen storage session actually came up */
    uint32_t at = atomic_load(&g_source_switch_at);
    if(at && lv_tick_elaps(at) < SOURCE_SETTLE_MS) return;   /* our own switch still settling */

    int host_has_card = sd_exported_to_host();
    if(g_source_mode == 3){
        /* While a user-chosen storage session is up, keep the unrequested-export
         * detector armed. Leaving it disarmed here meant a prompt already answered
         * earlier in the session silenced the NEXT one: switch back to Local by hand
         * while still plugged in, let the player re-export, and nothing would fire. */
        armed = 1;
        /* A storage session the user asked for. Track whether it actually came up, so
         * that a Storage selection which never exports (no host, slow gadget) is not
         * mistaken below for a session that has ended. */
        if(host_has_card){ saw_user_export = 1; return; }
        if(!saw_user_export) return;                       /* not up yet - wait, don't bounce */
        /* It was up and now it is not: the computer let go. Put the player back into
         * Local Playback for real rather than just editing our mirror to say so - the
         * mirror would then claim Local while the player sat in a mode that will not
         * play, which is the confusion this whole path exists to remove. Local is
         * documented as always re-issuable.
         *
         * Clear the session flag ONLY on success: ui_set_source_mode() bails out
         * without changing anything if a frame will not queue (a full queue, or the
         * player restarting), and clearing first would take the retry with it - leaving
         * the mirror stuck on Storage until the UI restarts. */
        armed = 1;
        if(ui_set_source_mode(0) != 0) return;             /* try again on the next tick */
        saw_user_export = 0;
        ui_toast("Back to local playback");
        return;
    }
    saw_user_export = 0;
    int exported = (g_source_mode == 0) && host_has_card;
    if(!exported){ armed = 1; return; }   /* unplugged / back to local: re-arm */
    if(!armed) return;                    /* already handled this connection */
    armed = 0;

    switch(cfg_get_int("usb_connect", 0)){
        case 1:                                            /* Keep Playing */
            if(ui_set_source_mode(0) == 0) ui_toast("Charging \xE2\x80\x93 still playing");
            break;
        case 3:                                            /* USB DAC */
            if(ui_set_source_mode(1) == 0) ui_toast("USB DAC");
            break;
        case 2:                                            /* Storage: the stock behaviour */
            ui_toast("Card open on the computer");
            break;
        default:                                           /* Ask */
            usbprompt_show();
            break;
    }
}

/* Re-send every MANAGED audio setting (cfg value >= 0). Called once the player is confirmed
 * ready and again on reconnect; unmanaged (-1) settings are left as the player has them. */
void ui_reapply_audio(void){
    /* NOTE: the v2.40 local-init (0666 route + 0657 LOCALPLAYER work-mode, which fixes the "g_fiio_local
     * is null" / NO_WORK_MODE start_local failure) is DELIBERATELY NOT sent here. This runs at boot-ready
     * and on reconnect, when a freshly-booted player is still idle-fresh - sending 0666 then either wedges
     * it or doesn't stick (device-verified: work-mode stayed NULL despite this running at cold boot). The
     * local-init is instead handled by the v2.40 work-mode handshake (v240_workmode_cb), which uses the
     * player's a607 mode oracle to set + confirm LOCALPLAYER once the player is ready - see there. */
    /* defaults match the device's observed current state (DRE on, Gain low, analog out,
     * Slow-LL filter) so a boot re-apply doesn't change the sound until the user does. */
    ui_set_dre(cfg_get_int("audio_dre",    1));
    ui_set_gain(cfg_get_int("audio_gain",  0));
    /* Output route (raw 0666) is deliberately NOT reapplied here: this also runs on player
     * RECONNECT, and sending 0666 to a freshly-booted player can wedge it (see the reconnect
     * warning below). Analog is the boot default and SPDIF was dropped from Settings, so
     * there is no safe output route to restore generically. */
    ui_set_dac_filter(cfg_get_int("audio_filter", 1));
    ui_set_replay_gain(cfg_get_int("replay_gain", 0));
    /* The rest of the managed settings, reapplied so a player restart/reconnect can't leave the
     * player out of sync with what Settings shows. Each is sent ONLY when
     * a real value is stored - the setters have inconsistent unmanaged handling, so guard here
     * rather than trust their internal clamps (e.g. ui_set_maxvol(-1) would send volume 0). */
    {
        int wm = cfg_get_int("work_mode", -1);   if(wm >= 0 && wm <= 4)   ui_set_workmode(wm);  /* stored play mode; don't let a garbage cfg get rewritten to 0 */
        int mp = cfg_get_int("memory_play", -1); if(mp >= 0)             ui_set_memory(mp);    /* Resume Playback */
        int gp = cfg_get_int("gapless", -1);     if(gp >= 0)             ui_set_gapless(gp);
        int mv = cfg_get_int("max_vol", -1);     if(mv >= 10)            ui_set_maxvol(mv);    /* <10 or unset = skip */
        int bl = cfg_get_int("balance", -100);   if(bl >= -10 && bl<=10) ui_set_balance(bl);   /* -1 is a VALID balance */
    }
}
/* "<type>:<name>" of the list currently built into the player's LIST_SONG_0 (see
 * ui_play_list). Cleared whenever that cache could be stale so we never send the
 * type-0 "jump in current list" shortcut against the wrong/rebuilt list. */
static char g_play_scope[260] = "";       /* the list the player has CONFIRMED loaded (jump target) */
static char g_play_pendscope[260] = "";   /* scope of an in-flight rebuild; committed to g_play_scope on confirm */
static int  g_play_dirty = 0;             /* a rebuild overlapped another -> don't trust the next confirm's scope
                                           * (the confirm oracle can't tell which build's track appeared) */
static char g_play_target[256] = "";      /* path of the song a song-tap is about to play (set by caller, consumed
                                           * at ui_play_list entry); "" for play-all/shuffle (no single target) */
static char g_play_pendtarget[256] = "";  /* the in-flight build's target path: scope commits only once THIS track
                                           * is confirmed playing - so a natural advance / HW key can't fake it */
/* When a play triggers a full list rebuild (seconds), we show "Starting..." and
 * confirm via the next a2 track update (the verified oracle); g_play_pending holds
 * the start tick (0 = nothing pending). Cleared on track update or a 6s timeout. */
static uint32_t g_play_pending = 0;
static char g_play_initpath[256] = "";   /* track path at play-initiation (for the pending-clear) */
static long g_play_initpos = 0;          /* position at play-init: a backward jump = restart (same-track replay) */
/* V2.40 LOCALPLAYER work-mode one-shot. Mechanism (RE'd from mq_player_v240):
 *  - v2.40's player needs 0666(local route)+0657(LOCALPLAYER work-mode) set at runtime or local
 *    start_local fails ("work mode NULL"/NO_WORK_MODE -> g_fiio_local null). Protocol 0657=8 maps to
 *    internal work-mode 1 (LOCALPLAYER); 0 = NO_WORK_MODE.
 *  - WHY an early 0657 doesn't stick: an async mode-control thread (player 0x4dca64) keeps running AFTER
 *    the /player command server goes live. It runs ~50x100ms polls + up to ~2s of 1s sleeps (~7s worst
 *    case), calling close_player() (resets work-mode to 0) and re-initing per late hardware state - which
 *    clobbers any 0657 sent inside that window. So we wait past it (~8s guard; we use 9s) before sending.
 *  - WHY there is no a607 oracle on /ui: the 0607 query handler writes its a607 reply to a SEPARATE
 *    fd-based transport (mask 0, raw write to fd 0x83e5f4), never the /ui mqueue - so we can never see it.
 *    There is NO deterministic /ui frame meaning "late init finished". a1=needs playback, a2=server
 *    liveness only, a714=volume. The best anchor is: solicit a2 with 02020008, then a fixed settle delay.
 *  - IDLE PLAYER IS SILENT: a fresh cold-boot player emits NOTHING unsolicited, so ipc_rx_frames() stays
 *    0 forever unless we poke it. We therefore actively send 02020008 every tick until it answers (a2),
 *    anchor the settle timer on that first response, then send 0666+0657 EXACTLY ONCE per player
 *    generation. Gated Local source + analog output (g_route_mac empty) so it never stomps a BT A2DP
 *    route, never while a play is pending. The play-timeout handler re-asserts once as a safety net. */
static void v240_workmode_cb(lv_timer_t *t){
    static unsigned done_gen = 0xFFFFFFFFu;   /* generation whose one-shot we've already sent */
    static unsigned wait_gen = 0xFFFFFFFFu;   /* generation whose settle timer is running */
    static uint32_t wait_start = 0;
    static unsigned rx_gen  = 0xFFFFFFFFu;    /* generation the rx_base snapshot belongs to */
    static unsigned rx_base = 0;              /* cumulative rx_frames at the start of THIS generation */
    if(fw_os_ver() != 240){ lv_timer_del(t); return; }
    if(ui_get_source_mode() != 0 || g_route_mac[0]) return;   /* only while Local source + analog output */
    if(g_play_pending) return;                                 /* never inject 0666 into a starting play */
    unsigned gen = ipc_generation();
    unsigned rx  = ipc_rx_frames();           /* CUMULATIVE across generations -> anchor per-generation via rx_base */
    if(rx_gen != gen){ rx_gen = gen; rx_base = rx; }           /* new generation -> snapshot the count */
    if(rx == rx_base){ ipc_send_probe("02020008"); return; }   /* SOLICIT: THIS generation's player hasn't answered yet (idle player is silent) */
    if(gen == done_gen) return;                                /* already sent this generation */
    uint32_t now = lv_tick_get();
    if(wait_gen != gen){ wait_gen = gen; wait_start = now; return; }   /* first response this gen -> start settle */
    if(now - wait_start < 9000) return;                        /* settle past the ~7s mode-control-thread window */
    ipc_send_cmd("0666000C0006");   /* local output route -> (re)inits the local device (creates g_fiio_local) */
    ipc_send_cmd("0657000C0008");   /* LOCALPLAYER work-mode -> sets runtime work_mode (fixes NO_WORK_MODE) */
    done_gen = gen;
    fprintf(stderr,"v2.40 workmode: local-init sent once (gen %u)\n", gen); fflush(stderr);
}
void ui_rescan_library(void){
    g_play_scope[0] = '\0'; g_play_pendscope[0] = '\0';
    scanner_start();   /* diskOS's own SD scan -> rebuild song.db (stock 0622 is a no-op on V2.09) */
}
/* Poll for scan completion: reload the library from the rebuilt DB, refresh the view, toast. */
static void scanner_poll(lv_timer_t *t){
    (void)t;
    if(scanner_take_finished()){
        mdb_load();               /* reload the in-memory library (also invalidates the group caches) */
        library_ensure_capacity(); /* grow row buffers if a clean first-boot 1-song alloc just gained thousands */
        library_refresh();        /* rebuild whatever Library view is showing */
        int done=0, total=0; scanner_progress(&done, &total);
        char b[64];
        if(scanner_no_sd())  snprintf(b, sizeof b, "Insert an SD card to scan");    /* SD not mounted; library kept */
        else if(total>0)     snprintf(b, sizeof b, "Scanned %d song%s", total, total==1?"":"s");
        else if(done>0)      snprintf(b, sizeof b, "Scan failed \xE2\x80\x94 library kept");   /* rolled back */
        else                 snprintf(b, sizeof b, "No music found");
        ui_toast(b);
    }
}
/* Invalidate the LIST_SONG_0 scope cache so the next play does a full rebuild
 * instead of a "jump in current list" shortcut. Call after any edit that could
 * change the contents of a list the player may have loaded (playlist add/remove,
 * playlist delete, favourite toggle/remove). */
void ui_invalidate_play_scope(void){ g_play_scope[0] = '\0'; g_play_pendscope[0] = '\0'; }
/* set absolute volume 0..120 via the decoded 0715 command (class-2:
 * 0715 + LEN(000C) + <level 4hex>).  The player maps this through the same
 * cs43131 gain path as the hardware vol keys. */
int ui_set_volume(int vol){
    if(vol < 0) vol = 0; if(vol > VOL_MAX) vol = VOL_MAX;
    char f[16]; snprintf(f, sizeof f, "0715000C%04X", vol);
    int rc = ipc_send_cmd(f);
    fprintf(stderr,"set volume %d -> %s (rc=%d)\n", vol, f, rc); fflush(stderr);
    return rc;   /* 0 = queued OK, -1 = send failed (caller can retry) */
}
/* Play a library list. The player builds the list itself (drop LIST_SONG_0 then
 * INSERT...SELECT FROM SONG WHERE <type filter>) then starts at the given
 * 1-based track. Rebuilding the WHOLE library (~3700 rows) takes a few seconds,
 * so we cache the scope currently loaded in LIST_SONG_0: when the next play
 * targets the SAME list, we send list_type 0 ("play position in current list")
 * which skips the rebuild and starts almost instantly. (g_play_scope declared
 * above, near ui_rescan_library which invalidates it.) */
void ui_play_list(int list_type, const char *name, int pos1){
    if(pos1 < 1) pos1 = 1;
    if(!name) name = "";
    char key[260]; snprintf(key, sizeof key, "%d:%s", list_type, name);
    char target[256]; snprintf(target, sizeof target, "%s", g_play_target); g_play_target[0] = 0;   /* consume */
    char f[420];
    if(strcmp(key, g_play_scope) == 0 && !g_play_pending){
        /* same list already loaded AND its rebuild confirmed - jump to the track, no rebuild.
         * While g_play_pending (rebuild queued but not yet confirmed) a jump could hit a
         * not-yet-built LIST_SONG_0, so fall through and re-issue the build instead. */
        snprintf(f, sizeof f, "0100%04X%04X0000", 16, (pos1-1) & 0xFFFF);
        ipc_send_cmd(f);
        fprintf(stderr,"play(jump) pos=%d scope='%s' -> %s\n", pos1, key, f);
    } else {
        int datalen = 8 + (int)strlen(name);          /* f1(4)+f2(4)+name */
        snprintf(f, sizeof f, "0100%04X%04X%04X%s",
                 8 + datalen, (pos1-1) & 0xFFFF, list_type & 0xFFFF, name);
        if(ipc_send_cmd(f) == 0){
            /* hold the scope as PENDING; commit to g_play_scope only when playback is
             * confirmed (track change/restart below). If the build times out, both are
             * cleared - so a failed build never leaves a jumpable-but-unbuilt scope. */
            if(g_play_pending) g_play_dirty = 1;   /* overlapping rebuild: the confirm can't tell which list loaded */
            snprintf(g_play_pendscope, sizeof g_play_pendscope, "%s", key);
            snprintf(g_play_pendtarget, sizeof g_play_pendtarget, "%s", target);   /* prove THIS track before committing scope */
            fprintf(stderr,"play(build) type=%d pos=%d name='%s' -> %s\n", list_type, pos1, name, f);
            /* rebuild can take seconds - acknowledge + arm completion/timeout.
             * Capture the current track path so we clear only on a REAL track change
             * (a1 position frames from the old track must not falsely clear it). */
            track_state_t cur; ipc_get_state(&cur);
            snprintf(g_play_initpath, sizeof g_play_initpath, "%s", cur.path);
            g_play_initpos = cur.position_ms;
            ui_toast("Starting...");
            g_play_pending = lv_tick_get(); if(!g_play_pending) g_play_pending = 1;
        } else {
            g_play_scope[0] = '\0'; g_play_pendscope[0] = '\0'; g_play_pendtarget[0] = '\0';   /* send failed: next tap rebuilds */
        }
    }
    fflush(stderr);
}
/* Play a custom playlist (list_type 5) by its id, from 1-based track pos. */
void ui_play_playlist(long pid, int pos){
    char ids[24]; snprintf(ids, sizeof ids, "%ld", pid);
    ui_play_list(5, ids, pos);
}
/* Favourite/unfavourite the CURRENT song (0104: 1=love -> MY_LOVE, 0=unlove). */
void ui_set_favorite(int on){ g_play_scope[0] = '\0'; g_play_pendscope[0] = '\0'; ipc_send_cmd(on ? "0104000C0001" : "0104000C0000"); }
/* Song tap -> play the exact track.  Inside an album/artist/genre drill we use
 * that list as the context; from a flat list we fall back to the song's album
 * (else all-songs).  The 1-based position is computed with the player's own
 * ORDER BY so playback lands on the tapped song. */
static void on_song_play(int id){
    int lt = 0; char name[256] = "";
    /* Inside an album/artist/genre drill, play THAT list (so the playing
     * context - and shuffle/next - stays within what the user opened).  From a
     * flat list (Songs/Search/Favourites) play the WHOLE library so shuffle and
     * next/prev span the entire collection, not a single album.
     * NB: in the 0100 jump table, list_type 1 = all songs (rebuilds the full
     * LIST_SONG_0); type 0 is "resume current list" and does NOT rebuild. */
    if(!library_drill_context(&lt, name, sizeof name)){
        lt = 1; name[0] = 0;                       /* all songs (full library) */
    }
    int pos = mdb_play_pos(id, lt, name);
    if(pos < 1 && lt != 1){
        /* the player's exact list (e.g. ARTIST='X') doesn't contain this song - happens
         * for a multi-artist track "A, X" shown under split artist X (the player can't
         * tokenise). Fall back to the all-songs list so the tap still plays it. */
        lt = 1; name[0] = 0;
        pos = mdb_play_pos(id, 1, "");
    }
    if(pos < 1){ ui_toast("Couldn't find that song"); return; }  /* don't fall back to track 1 */
    mdb_song_path(id, g_play_target, sizeof g_play_target);   /* prove this exact track starts before caching scope */
    ui_play_list(lt, name, pos);
}
/* Search result tap: ALWAYS play in the all-songs scope. Search spans the whole
 * library and must not inherit a stale Library album/artist/genre drill context. */
static void on_search_play(int id){
    int pos = mdb_play_pos(id, 1, "");          /* lt=1 = full library */
    if(pos < 1){ ui_toast("Couldn't find that song"); return; }
    mdb_song_path(id, g_play_target, sizeof g_play_target);   /* prove this exact track starts before caching scope */
    ui_play_list(1, "", pos);
}
void ui_set_workmode(int mode){
    /* LOCAL play-mode setter = command 0102 (class 1): 0102 000C <mode 4hex>.
     * GROUND TRUTH captured 2026-06-25 by strace'ing the stock UI's mq_timedsend
     * while tapping its play-mode control (see COMMAND_MAP.md). Stock cycles 5
     * modes 0..4: 0=Sequential, 1=Shuffle, 2=Repeat One, 3=Repeat All, 4=Single.
     * (This replaces the WRONG 0657, which is the audio SOURCE switch and could
     * wedge the device.) */
    if(mode < 0 || mode > 4) mode = 0;
    /* Persist here so cfg is the single source of truth: every play path that sets the
     * mode (Playlist Play/Shuffle, Library long-press, NP toggle, Settings, Tune) goes
     * through here, so the NP icon / Settings / Tune (all read cfg) can't drift from the
     * mode the player is actually using. Redundant cfg_set_int() at other call sites are
     * harmless. */
    cfg_set_int("work_mode", mode);
    char f[16]; snprintf(f, sizeof f, "0102000C%04X", mode);
    ipc_send_cmd(f);
    fprintf(stderr,"workmode %d -> %s\n", mode, f); fflush(stderr);
}

/* sleep timer: when armed, pause playback once the interval elapses */
static uint32_t g_sleep_start=0, g_sleep_ms=0;
void ui_set_sleep_timer(int minutes){
    if(minutes>0){ g_sleep_start=lv_tick_get(); g_sleep_ms=(uint32_t)minutes*60000u; }
    else g_sleep_ms=0;
}

/* apps: tapping an app row sets a pending exec; the main loop runs it between
 * frames so LVGL isn't mid-render. While the app runs we block in waitpid (so
 * diskOS draws nothing and the app owns fb0 + touch); on exit we force a full
 * redraw. mq_player keeps playing throughout. */
static char g_app_exec[256];
static int  g_app_pending = 0;
void app_launch(const char *exec){ snprintf(g_app_exec, sizeof g_app_exec, "%s", exec); g_app_pending = 1; }
static void app_run(const char *exec){
    /* preflight: a missing/non-executable app shouldn't blank the screen for nothing */
    if(access(exec, X_OK) != 0){
        lv_obj_invalidate(lv_screen_active()); lv_refr_now(NULL);
        ui_toast("Can't launch app");
        return;
    }
    int failed = 0;
    watchdog_pause(1);          /* the app owns the screen + this thread until it exits */
    pid_t pid = fork();
    if(pid == 0){
        execl(exec, exec, (char*)NULL);
        _exit(127);
    } else if(pid > 0){
        int status = 0;
        if(waitpid(pid, &status, 0) < 0) failed = 1;                          /* wait failed: status undefined */
        else if(WIFEXITED(status) && WEXITSTATUS(status) == 127) failed = 1;  /* exec failed */
    } else {
        failed = 1;  /* fork failed */
    }
    watchdog_pause(0);
    /* reclaim the screen: invalidate everything and force an immediate redraw */
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    if(failed) ui_toast("Launch failed");
}

/* one-shot: re-request the player's current-track metadata shortly after launch,
 * in case the player wasn't ready when we sent the first 0202 at startup. */
static void state_sync_retry_cb(lv_timer_t *t){
    (void)t;
    ipc_send_probe("02020008");   /* silent: a miss during the connect race must not toast */
}

static int run_bounded(char *const argv[], int timeout_ms);   /* fork+exec with a hard timeout (defined below) */

/* Background ipc connect: at cold boot the player creates its /ui queue well
 * after we start (slower than launched-standalone), so a one-shot retry isn't
 * enough. Keep trying until /ui exists, then sync state and stop. */
static void ipc_connect_retry_cb(lv_timer_t *t){
    if(ipc_start()==0){
        fprintf(stderr,"ipc connected (deferred)\n"); fflush(stderr);
        /* re-assert LOCALPLAYER on reconnect: a player restart can reset WORK_MODE,
         * which silently kills local tap-to-play. diskOS is local-only. Bounded tight
         * (2.5s) so a locked DB can't visibly freeze the main loop from this runtime
         * timer callback (a longer stall reads as a hang to the user). */
        { char *a[] = { "sqlite3", "/usr/data/fiio/db/sysconfig.db",
                        "UPDATE SYSCONFIG SET WORK_MODE=4 WHERE ID=1 AND WORK_MODE<>4", NULL };
          run_bounded(a, 2500); }
        ipc_send_probe("02020008");   /* request live track once connected (silent) */
        /* audio re-apply is handled by the main-loop seq gate (covers this deferred connect
         * AND a later player restart via seq-reset detection) - no premature send here. */
        lv_timer_del(t);
    }
}

/* Periodic IPC health check: re-establish the /ui receive queue if it was ever
 * lost. Does NOT poll the player (a periodic 02020008 would bump seq every tick
 * and needlessly re-push the Now-Playing/backdrop surfaces). A player restart is
 * self-healed on the next real send instead (ipc_send_internal reopens /player). */
static void ipc_health_cb(lv_timer_t *t){
    (void)t;
    if(!ipc_is_ready()) ipc_start();   /* cold-start retry if the rx thread never came up (idempotent) */
    else ipc_health_check();           /* detect + request recovery from a player queue restart */
}

/* async-signal-safe: log the fatal signal number to the boot log so a hard crash
 * is visible after the watchdog respawns. Runs on a dedicated alt stack. */
static void crash_log(int sig, siginfo_t *si, void *ucv){
    const char *m = "=== diskos CAUGHT FATAL SIGNAL ";
    int fd = open("/usr/data/diskos_boot.log", O_WRONLY|O_APPEND);
    if(fd>=0){ (void)si; (void)ucv;
        char b[8]; int n=sig, i=0; b[i++]=' ';
        if(n>=10){ b[i++]='0'+(n/10); } b[i++]='0'+(n%10); b[i++]='\n';
        write(fd,m,30); write(fd,b,i); close(fd); }
    signal(sig, SIG_DFL); raise(sig);
}

/* Boot-hang protection ------------------------------------------------------
 * Two independent guards keep the device from ever sitting in a black-screen
 * hang at boot (fiio_init's watchdog only respawns a DEAD mq_ui via `pgrep -x`,
 * never one that's alive-but-stuck in init - so an init hang is invisible to it):
 *
 *  1) run_bounded(): the blocking boot `system()` calls (sqlite WORK_MODE fix,
 *     hwclock -s) run as a killable child in its own process group with a hard
 *     timeout, so a locked DB or a busy RTC/I2C can't wedge init forever.
 *  2) an alarm(BOOT_DEADLINE_S) armed before risky init and disarmed once the
 *     first frame paints: if we never reach the main loop, SIGALRM _exit()s us
 *     (only our own process - never mq_player) so fiio_init respawns a fresh
 *     mq_ui instead of leaving a permanent black screen. */
/* PGID of the in-flight run_bounded child (0 = none), so the boot watchdog can
 * KILL it before exiting instead of abandoning a stuck child. A child stuck
 * before exec is still named "mq_ui" and would satisfy fiio_init's `pgrep -x mq_ui`
 * -> suppress our respawn; killing its group closes that hole. */
static volatile sig_atomic_t g_bounded_pgid = 0;

static void boot_alarm_handler(int sig){
    (void)sig;
    /* stderr is already freopen'd to the boot log; write to it directly (no open() -
     * a filesystem open can itself block and prevent the _exit). */
    static const char m[] = "=== diskos BOOT WATCHDOG timeout -> exit for respawn ===\n";
    (void)write(2, m, sizeof m - 1);
    if(g_bounded_pgid > 0) kill(-(pid_t)g_bounded_pgid, SIGKILL);  /* never abandon a stuck child */
    _exit(124);   /* async-signal-safe; fiio_init's `pgrep -x mq_ui` misses -> respawn */
}

/* Run argv[] as a child in its own process group; wait up to timeout_ms.
 * On timeout, SIGKILL the group and reap with a bound. Returns 0 on clean exit,
 * -1 otherwise. Registers the child in g_bounded_pgid so the boot watchdog can
 * kill it on timeout rather than leaving an "mq_ui"-named process alive. */
static int run_bounded(char *const argv[], int timeout_ms){
    pid_t pid = fork();
    if(pid < 0) return -1;
    if(pid == 0){
        setpgid(0, 0);                 /* own group: one kill takes any grandchildren too */
        for(int fd = 3; fd < 256; fd++) close(fd);   /* don't inherit our fb/mqueue/etc fds */
        int nul = open("/dev/null", O_RDWR);
        if(nul >= 0){ dup2(nul, 0); if(nul > 2) close(nul); }  /* no stdin; keep 1/2 = boot log */
        execvp(argv[0], argv);
        _exit(127);                    /* exec failed */
    }
    setpgid(pid, pid);                 /* parent side of the setpgid race (ignore EACCES/ESRCH) */
    g_bounded_pgid = pid;              /* the watchdog will kill this group if it fires mid-run */
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = -1;
    for(;;){
        int status = 0;
        pid_t w = waitpid(pid, &status, WNOHANG);
        if(w == pid){ rc = (WIFEXITED(status) && WEXITSTATUS(status)==0) ? 0 : -1; break; }
        if(w < 0 && errno != EINTR && errno != ECHILD){ rc = -1; break; }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - t0.tv_sec)*1000 + (now.tv_nsec - t0.tv_nsec)/1000000;
        if(ms >= timeout_ms){
            kill(-pid, SIGKILL);
            /* bounded reap: try up to ~1s, then give up (a D-state child reparents to
             * init on our exit) - never block the caller indefinitely. */
            for(int k = 0; k < 100; k++){ if(waitpid(pid, &status, WNOHANG) == pid) break; usleep(10000); }
            rc = -1; break;
        }
        usleep(10000);                 /* 10ms poll granularity */
    }
    g_bounded_pgid = 0;
    return rc;
}

/* the frequent polls (album-art 120ms, lyrics 500ms, weather 1s); paused while the
 * screen is fully off so the main loop can deep-idle instead of waking ~8x/s. */
static lv_timer_t *g_t_art, *g_t_lyr, *g_t_wx, *g_t_status;
static void polls_set_paused(int paused){
    if(!g_t_art) return;
    /* IMPORTANT: g_t_status (battery/wifi/BT, 3s) is DELIBERATELY left running at screen-off. Its
     * periodic fuel-gauge I2C read + timer wake is a keep-alive that prevents a device-level idle
     * FREEZE: with NOTHING polling at full screen-off, the stock player's idle DAC power-down leaves
     * the CS43131 (I2C bus 3) half-powered and a later transaction NOACKs, wedging the Ingenic I2C
     * controller and the whole SoC (device-verified 2026-08-26: stock never sleeps the screen and
     * never freezes; pausing this poll at screen-off is what introduced the freeze). Only the visual
     * polls (art/lyrics/weather) pause - they wake nothing useful behind a dark screen. */
    if(paused){ lv_timer_pause(g_t_art);  lv_timer_pause(g_t_lyr);  lv_timer_pause(g_t_wx);  }
    else      { lv_timer_resume(g_t_art); lv_timer_resume(g_t_lyr); lv_timer_resume(g_t_wx); }
}
/* Bounded external command, exposed so saver.c can decode the vinyl cover without an unbounded popen. */
int ui_run_bounded(char *const argv[], int timeout_ms){ return run_bounded(argv, timeout_ms); }

/* ---- hardware volume keys: immediate on-screen acknowledgement -------------
 * We never see the volume keys through the input layer - mq_player holds an
 * EXCLUSIVE grab on /dev/input/event0 (see the boot-time note in main()), and it
 * owns what a press means: it also watches for a double-press to skip a track, so
 * it deliberately sits on a single press for a moment before acting on it. The
 * only notification we get is the a714 frame it emits AFTER all of that, which is
 * why the volume overlay lagged the button - every press paid the player's
 * hold-off before anything at all appeared on screen.
 *
 * So sample the pin level ourselves, exactly as the boot-time Vol-Up override
 * does: x2000 pinctrl @ 0x10010000, GPB PxPIN at +0x00 of the port (port B is at
 * +0x100), active-low, bit 13 = Vol-Up and bit 14 = Vol-Down. That register map
 * was validated on-device 2026-08-13 against known-released states.
 *
 * This is one volatile read of an already-mapped page per loop - no syscall and
 * nothing that can block the LVGL thread. We only make the bar APPEAR sooner; the
 * real level still comes from the a714 and corrects the number a moment later. */
static volatile uint32_t *g_gpb_pin;   /* GPB PxPIN, or NULL when /dev/mem is unavailable */
static int g_volkey_down;              /* last sample: 1 while either volume key is held */

static void volkeys_init(void){
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if(fd < 0) return;                 /* no /dev/mem -> feature simply off, a714 still drives the bar */
    void *map = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0x10010000);
    close(fd);                         /* the mapping outlives the descriptor */
    if(map == MAP_FAILED) return;
    g_gpb_pin = (volatile uint32_t *)((char *)map + 0x100);
}
/* 1 on a press EDGE of either volume key, 0 otherwise. A HELD key (auto-repeat)
 * reports one edge; the a714 stream keeps the number moving after that. */
static int volkeys_pressed(void){
    if(!g_gpb_pin) return 0;
    uint32_t pin = *g_gpb_pin;
    int down = (((pin >> 13) & 1u) == 0) || (((pin >> 14) & 1u) == 0);   /* active low */
    int edge = down && !g_volkey_down;
    g_volkey_down = down;
    return edge;
}

/* ---- rim (circumference) scroll -------------------------------------------
 * Drag a finger around the screen EDGE to fly through long lists, like a crown.
 * State machine IDLE->CANDIDATE->OWNED:
 *  - press inside the rim band arms a CANDIDATE but does NOT consume (taps/normal
 *    centre scroll keep working; LVGL still owns the touch).
 *  - on move, commit to OWNED only when travel is mostly TANGENTIAL (around the
 *    rim) and exceeds a threshold -> this is what distinguishes a rim drag from
 *    the radial back-swipe / pull-down. On commit, lv_indev_wait_release() makes
 *    LVGL drop its own list-scroll so we don't double-scroll; we drive
 *    lv_obj_scroll_by() ourselves from the angular delta.
 *  - release consumes the gesture (skips nav) only if it actually committed. */
#define RIM_R_MIN       146
#define RIM_R_MAX       182
#define RIM_REL_R_MIN   122      /* hysteresis: keep owning until finger drifts well inside */
#define RIM_COMMIT_TANG 22.0f    /* tangential travel (px) to commit */
#define RIM_PX_PER_RAD  180.0f   /* ~quarter-turn = one screen on the ~268px list */
#define RIM_MAX_OVER    34       /* max elastic overscroll (px) past either end before it springs back */
enum { RIM_IDLE, RIM_CAND, RIM_OWN };
static int       rim_state = RIM_IDLE;
static float     rim_last_ang = 0;
static int       rim_sx = 0, rim_sy = 0;
static float     rim_accum = 0;          /* fractional scroll carry, so slow drags don't quantise to 0 */
static uint32_t  rim_last_ms = 0;
static lv_obj_t *rim_scroller = NULL;

static lv_obj_t *ui_active_scroller(void){
    switch(screen_current()){
        case SCR_LIBRARY: return library_scroller();
        case SCR_PLVIEW:  return playlistview_scroller();
        case SCR_SEARCH:  return search_scroller();
        case SCR_APPS:    return apps_scroller();
        default:          return NULL;
    }
}
static float rim_ang(int x, int y){ return atan2f((float)(y-180), (float)(x-180)); }
static float rim_wrap(float d){ if(d>(float)M_PI) d-=2.0f*(float)M_PI; else if(d<-(float)M_PI) d+=2.0f*(float)M_PI; return d; }

static void rim_press(int x, int y){
    rim_state = RIM_IDLE; rim_scroller = NULL;
    int dx=x-180, dy=y-180; float r=sqrtf((float)(dx*dx+dy*dy));
    if(r < RIM_R_MIN || r > RIM_R_MAX) return;        /* not on the rim band */
    lv_obj_t *sc = ui_active_scroller();
    if(!sc) return;                                   /* screen has no long list */
    lv_anim_delete(sc, NULL);                         /* cancel any in-flight spring-back so it won't fight the new drag */
    rim_scroller=sc; rim_sx=x; rim_sy=y; rim_last_ang=rim_ang(x,y);
    rim_accum=0; rim_last_ms=lv_tick_get(); rim_state=RIM_CAND;
}
/* returns 1 once OWNED so the caller skips other gestures */
static int rim_move(int x, int y){
    if(rim_state==RIM_IDLE) return 0;
    int rdx=x-180, rdy=y-180; float r=sqrtf((float)(rdx*rdx+rdy*rdy)); if(r<1) r=1;
    if(rim_state==RIM_CAND){
        int dx=x-rim_sx, dy=y-rim_sy;
        float radial = (dx*rdx + dy*rdy)/r;           /* + = outward/inward along radius */
        float tang   = fabsf((float)dx*rdy - (float)dy*rdx)/r;  /* perpendicular (around-rim) travel */
        if(tang < RIM_COMMIT_TANG || tang < fabsf(radial)) return 0;  /* not a rim drag (yet) */
        if(r < RIM_REL_R_MIN){ rim_state=RIM_IDLE; rim_scroller=NULL; return 0; }  /* wandered off the rim before committing -> let normal handling have it */
        lv_indev_wait_release(g_touch);               /* LVGL: drop this touch, we own it now */
        lv_obj_scroll_to_y(rim_scroller, lv_obj_get_scroll_y(rim_scroller), LV_ANIM_OFF); /* stop any momentum */
        rim_state=RIM_OWN; rim_last_ang=rim_ang(x,y); rim_last_ms=lv_tick_get(); rim_accum=0;
        return 1;
    }
    /* OWNED */
    if(rim_scroller != ui_active_scroller()) return 1;  /* screen changed under us: hold gesture, don't scroll a stale list */
    if(r < RIM_REL_R_MIN) return 1;                   /* drifted inward: hold gesture, no scroll this frame */
    float ang=rim_ang(x,y); float dAng=rim_wrap(ang-rim_last_ang); rim_last_ang=ang;
    uint32_t now=lv_tick_get(); uint32_t dt=now-rim_last_ms; rim_last_ms=now;
    if(dAng > 1.2f || dAng < -1.2f) return 1;         /* clamp coalesced jumps (>~70deg/frame = noise) */
    float omega = dt>0 ? fabsf(dAng)/((float)dt/1000.0f) : 0.0f;
    float gain  = 1.0f + omega/4.0f; if(gain>6.0f) gain=6.0f;   /* mild acceleration for the 3000-item list */
    rim_accum += dAng * RIM_PX_PER_RAD * gain;        /* clockwise (dAng>0) = scroll down */
    int step=(int)rim_accum; rim_accum -= (float)step;
    if(step){
        int dy  = -step;
        int top = lv_obj_get_scroll_top(rim_scroller);     /* room to scroll up (neg = already past top) */
        int bot = lv_obj_get_scroll_bottom(rim_scroller);  /* room to scroll down (neg = past bottom) */
        int roomUp = top>0?top:0, roomDn = bot>0?bot:0;
        int inb = dy;                                      /* in-bounds portion of this step */
        if(dy>0 && inb >  roomUp) inb =  roomUp;
        if(dy<0 && inb < -roomDn) inb = -roomDn;
        if(inb) lv_obj_scroll_by(rim_scroller, 0, inb, LV_ANIM_OFF);
        int excess = dy - inb;                             /* travel past an edge (same sign as dy) */
        if(excess){                                        /* elastic rubber-band, damped + capped */
            int over = excess>0 ? (top<0?-top:0) : (bot<0?-bot:0);  /* overscroll depth this side (>=0) */
            int remain = RIM_MAX_OVER - over;              /* rubber-band px still available */
            if(remain > 0){
                float damp = 1.0f - (float)over/(float)RIM_MAX_OVER;
                int add = (int)((float)excess * 0.40f * damp);
                if(add==0) add = excess>0?1:-1;
                if(add >  remain) add =  remain;           /* never let one big step blow past the cap */
                if(add < -remain) add = -remain;
                lv_obj_scroll_by(rim_scroller, 0, add, LV_ANIM_OFF);
            }
        }
    }
    if(rim_scroller == library_scroller()) library_scroll_letter_tick();  /* show A-Z position */
    return 1;
}
/* Animate an overscrolled list back to its bound (the "bounce") - the rim path bypasses
 * LVGL's indev release-snap, so we snap it ourselves with an ease-out scroll animation. */
static void rim_spring_back(lv_obj_t *sc){
    if(!sc || !lv_obj_is_valid(sc)) return;
    int top = lv_obj_get_scroll_top(sc);
    int bot = lv_obj_get_scroll_bottom(sc);
    int dy = 0;
    if(top < 0)      dy = top;    /* past the top -> scroll down to remove it */
    else if(bot < 0) dy = -bot;   /* past the bottom -> scroll up */
    if(dy) lv_obj_scroll_by(sc, 0, dy, LV_ANIM_ON);
}
static int rim_release(void){
    int owned=(rim_state==RIM_OWN);
    if(owned && rim_scroller) rim_spring_back(rim_scroller);
    rim_state=RIM_IDLE; rim_scroller=NULL; return owned;
}

/* Last.fm: every 1s, feed the current play-state to the watcher (now-playing + scrobble
 * eligibility) and pump the network worker / offline queue. All on the LVGL thread. */
static void lastfm_tick(lv_timer_t *t){
    (void)t; track_state_t st; ipc_get_state(&st);
    lastfm_watch(&st); lastfm_poll();
}

/* ---- diskOS boot splash -------------------------------------------------------------------
 * A branded startup moment shown after the u-boot logo, over the (already-built) home screen:
 * a "diskOS" wordmark rises + fades in on black with an accent underline, holds, then the whole
 * overlay fades out to reveal home. OPACITY + a tiny y-rise only (no scale - scale resamples and
 * stutters on this GPU-less renderer); guaranteed smooth. Lives on lv_layer_top so it's above
 * every screen, and deletes itself when done. */
static lv_obj_t *s_splash;
static void splash_del(lv_anim_t *a){ (void)a; if(s_splash){ lv_obj_delete(s_splash); s_splash = NULL; } }
static void splash_out_cb(lv_timer_t *t){
    lv_timer_delete(t);
    if(s_splash) anim_fade(s_splash, LV_OPA_COVER, LV_OPA_TRANSP, 520, splash_del);
}
static void boot_splash_start(void){
    if(s_splash) return;   /* one splash at a time - guard the global-pointer design against re-entry */
    s_splash = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_splash);
    lv_obj_set_size(s_splash, 360, 360);
    lv_obj_set_pos(s_splash, 0, 0);
    lv_obj_set_style_bg_color(s_splash, th_bg(), 0);
    lv_obj_set_style_bg_opa(s_splash, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_splash, LV_OBJ_FLAG_CLICKABLE);       /* swallow taps during the splash */
    lv_obj_clear_flag(s_splash, LV_OBJ_FLAG_SCROLLABLE);

    /* The diskOS ring - the signature Now-Playing arc geometry, drawing ITSELF in around the round
     * face (a disc coming to life). Accent stroke on an invisible track; no knob; non-interactive.
     * Arc-value sweep is a vector redraw (cheap), not a transform. */
    lv_obj_t *ring = lv_arc_create(s_splash);
    lv_obj_set_size(ring, 300, 300);
    lv_obj_center(ring);
    lv_arc_set_rotation(ring, 270);                        /* start at 12 o'clock */
    lv_arc_set_bg_angles(ring, 0, 360);
    lv_arc_set_range(ring, 0, 3600);                       /* fine steps -> smooth sweep */
    lv_arc_set_value(ring, 0);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_opa(ring, LV_OPA_TRANSP, LV_PART_MAIN);          /* track invisible */
    lv_obj_set_style_arc_width(ring, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ring, lv_color_hex(0xFF375F), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(ring, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, LV_PART_KNOB);           /* hide the knob */
    lv_obj_set_style_pad_all(ring, 0, LV_PART_KNOB);

    /* "diskOS" wordmark, centred inside the ring. */
    lv_obj_t *w = lv_label_create(s_splash);
    lv_label_set_text(w, "diskOS");
    lv_obj_set_width(w, 360);
    lv_obj_set_style_text_align(w, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(w, th_font(36), 0);
    lv_obj_set_style_text_color(w, th_text(), 0);
    lv_obj_set_pos(w, 0, 172);                             /* rises to 160, ring-centred */
    lv_obj_set_style_opa(w, LV_OPA_TRANSP, 0);

    anim_arc_value(ring, 0, 3600, 1050, NULL);            /* hero: the ring draws itself in (~1s) */
    anim_slide_y(w, 174, 160, 780, NULL);                 /* wordmark settles as the ring closes */
    anim_fade(w, LV_OPA_TRANSP, LV_OPA_COVER, 780, NULL);
    lv_timer_create(splash_out_cb, 2400, NULL);           /* ring done ~1s, hold ~1.4s, then fade out */
}

/* ---- microSD coldplug worker (native thread) -------------------------------
 * mq_player mounts the card by listening for the DISK 'add' uevent on
 * /sys/block/mmcblk0 (device-verified: re-emitting the PARTITION uevent does
 * nothing). Its Netlink listener isn't bound until ~100s into boot, so it MISSES
 * the already-inserted card's boot-time add event and the library comes up empty
 * until a physical reinsert. This worker re-emits the disk 'add' every few
 * seconds until the card mounts (bounded), then exits - same effect as a reinsert.
 * A native thread (not a shell subprocess) can't be orphaned/reaped ambiguously,
 * runs on every UI generation, and logs observably. */
static int coldplug_mounted(void){
    FILE *m = fopen("/proc/mounts", "r");
    if(!m) return 0;
    char line[512]; int ok = 0;
    while(fgets(line, sizeof line, m)) if(strstr(line, " /tmp/sdcard ")){ ok = 1; break; }
    fclose(m);
    return ok;
}
static void coldplug_log(const char *msg){
    FILE *f = fopen("/usr/data/coldplug.log", "a");
    if(f){ fprintf(f, "%ld %s\n", (long)time(NULL), msg); fclose(f); }
}
/* Is the mass-storage gadget exporting the card to a USB host (so the host owns /dev/mmcblk0 and a
 * device-side mount would dual-access-corrupt exFAT)? Reads the REAL gadget state, not our g_source_mode
 * mirror - the player can enter a USB mode without going through ui_set_source_mode(). storage_demo is
 * NOT configured at boot (S99usbserial only builds the serial gadget); it appears only when a storage
 * session starts. The stock storage_config.sh sets the LUN backing file BEFORE binding the UDC and blanks
 * the UDC BEFORE clearing the LUN, so a blank UDC alone does NOT prove the card is free - we also check
 * the LUN. Fail-CLOSED: node present but UDC unreadable -> assume exported. */
static int sd_exported_to_host(void){
    const char *udcp = "/sys/kernel/config/usb_gadget/storage_demo/UDC";
    const char *lunp = "/sys/kernel/config/usb_gadget/storage_demo/functions/mass_storage.0/lun.0/file";
    struct stat sb;
    /* FULLY fail-closed: only a confirmed ENOENT counts as "node absent"; any other stat/read failure is
     * ambiguous gadget state -> assume exported. */
    errno = 0; int have_udc = (stat(udcp, &sb) == 0); int udc_err = (!have_udc && errno != ENOENT);
    errno = 0; int have_lun = (stat(lunp, &sb) == 0); int lun_err = (!have_lun && errno != ENOENT);
    if(udc_err || lun_err) return 1;                            /* ambiguous -> exported */
    if(!have_udc && !have_lun) return 0;                        /* storage_demo genuinely absent -> not exported */
    if(have_udc){
        FILE *f = fopen(udcp, "r");
        if(!f) return 1;                                        /* present but unreadable -> exported */
        char b[64] = {0}; size_t n = fread(b, 1, sizeof b - 1, f); int rerr = ferror(f); fclose(f);
        if(rerr) return 1;                                      /* read error -> exported */
        for(size_t i = 0; i < n; i++) if(b[i] > ' ') return 1;  /* UDC bound -> exported */
    }
    /* UDC blank/absent: the LUN backing file is authoritative during the blank-UDC transition windows. */
    if(have_lun){
        FILE *g = fopen(lunp, "r");
        if(!g) return 1;                                        /* present but unreadable -> exported */
        char l[300] = {0}; size_t m = fread(l, 1, sizeof l - 1, g); int rerr = ferror(g); fclose(g);
        if(rerr) return 1;                                      /* read error -> exported */
        if(m && strstr(l, "mmcblk0")) return 1;                 /* LUN backs /dev/mmcblk0 -> exported */
    }
    return 0;
}
/* May the V2.40 worker direct-mount the card RIGHT NOW? Checked (under g_sd_mode_mu) immediately before
 * EACH mount attempt: absolute cold-boot window (fail-closed on unreadable uptime) AND the card is not
 * exported to a host. Re-evaluated per attempt so a slow first mount can't let a second begin outside the
 * window / after a host grabbed the card. The window matches coldplug_should_run()'s 150s worker-start
 * gate (a 45s cliff here starved first-boot mounts when the S97 install delay pushed mq_ui start past
 * 45s: worker ran but every mount hit "window elapsed" -> SD never mounted). The authoritative host-safety
 * guard is sd_exported_to_host() (fail-closed UDC+LUN); the uptime bound is a belt-and-suspenders proxy. */
static int sd_cold_mount_allowed(void){
    double up = -1.0; FILE *pu = fopen("/proc/uptime", "r");
    if(pu){ if(fscanf(pu, "%lf", &up) != 1) up = -1.0; fclose(pu); }
    return (up >= 0.0 && up <= 150.0 && !sd_exported_to_host());
}
static void *coldplug_thread(void *arg){
    (void)arg;
    const int v240 = (fw_os_ver() == 240);
    /* Mount the already-inserted microSD at /tmp/sdcard at COLD boot. V2.09/V2.28: the player's uevent
     * listener mounts it once bound (~100s in); we re-emit the disk 'add' to trigger it. V2.40: the
     * player no longer mounts on that nudge, so we mount the card DIRECTLY. Either way this is ONE-SHOT:
     * we exit as soon as it's mounted and never re-mount (an indefinite writable remounter is unsafe -
     * it could mount during an async Storage->Local transition before the gadget detaches). Non-tethered
     * users - the shipping case - keep the card mounted after this; the tethered "player releases it"
     * case is dev-only. Every mount is under g_sd_mode_mu AND gated on Local mirror + real gadget state
     * (sd_exported_to_host) so we never touch the block device while a USB host owns it. */
    if(coldplug_mounted()) return NULL;            /* already mounted (player did it / warm restart) */
    coldplug_log("coldplug worker start");
    for(int i = 0; i < 120; i++){                  /* ~6 min cap (120 x 3s) covers the ~100s listener */
        if(coldplug_mounted()){ coldplug_log("SD mounted - done"); return NULL; }
        pthread_mutex_lock(&g_sd_mode_mu);
        if(ui_get_source_mode() == 0 && !sd_exported_to_host() && !coldplug_mounted()){
            int fd = open("/sys/block/mmcblk0/uevent", O_WRONLY | O_CLOEXEC);
            if(fd >= 0){ ssize_t w = write(fd, "add\n", 4); (void)w; close(fd); }   /* V2.09/V2.28 nudge */
            if(v240){                              /* V2.40: nudge won't mount -> mount directly, ONCE */
                /* Guard EACH mount attempt with sd_cold_mount_allowed() (absolute cold-boot window + not
                 * exported), re-evaluated per attempt so a slow exfat mount can't let the vfat fallback
                 * begin outside the window or after a host grabbed the card. The window keeps the safety
                 * argument on "early boot, no storage session yet" rather than the check-then-act race
                 * (the stock Storage transition doesn't take g_sd_mode_mu). */
                mkdir("/tmp/sdcard", 0755);
                if(sd_cold_mount_allowed() && mount("/dev/mmcblk0p1", "/tmp/sdcard", "exfat", 0, NULL) == 0){
                    coldplug_log("SD direct-mounted (V2.40 exfat)"); pthread_mutex_unlock(&g_sd_mode_mu); return NULL;
                }
                if(sd_cold_mount_allowed() && mount("/dev/mmcblk0p1", "/tmp/sdcard", "vfat", 0, NULL) == 0){
                    coldplug_log("SD direct-mounted (V2.40 vfat)"); pthread_mutex_unlock(&g_sd_mode_mu); return NULL;
                }
                if(!sd_cold_mount_allowed()){      /* window elapsed / card exported -> give up (one-shot) */
                    pthread_mutex_unlock(&g_sd_mode_mu);
                    coldplug_log("V2.40 SD one-shot: window elapsed or card exported (SD not mounted)");
                    return NULL;
                }
                { char b[96]; snprintf(b, sizeof b, "SD direct-mount failed errno=%d(%s)", errno, strerror(errno)); coldplug_log(b); }
            }
        }
        pthread_mutex_unlock(&g_sd_mode_mu);
        struct timespec ts = { 3, 0 }; nanosleep(&ts, NULL);
    }
    coldplug_log("coldplug TIMEOUT - SD never mounted");
    return NULL;
}
/* Only nudge on a genuine COLD boot, while the player's SD listener is not yet bound (~first 100s).
 * On a late/bare mq_ui RESTART the listener is long up (a normal insert auto-mounts) AND the device
 * may be in Storage/USB-DAC mode with the card EXPORTED to a USB host - re-emitting the disk 'add'
 * then risks concurrent host/device access + SD corruption. Gate on system uptime (a restart-proof
 * proxy for "cold boot") plus the Local-mode mirror. */
static int coldplug_should_run(void){
    double up = -1.0;
    FILE *u = fopen("/proc/uptime", "r");
    if(u){ if(fscanf(u, "%lf", &up) != 1) up = -1.0; fclose(u); }
    /* fail-closed: an unreadable/malformed uptime is NOT treated as a cold boot (0 would be fail-open,
     * enabling the SD mount on a late restart where the card may be exported to a host). */
    if(up < 0.0 || up > 150.0){ coldplug_log("skip: uptime unreadable or late (not a verified cold boot)"); return 0; }
    if(ui_get_source_mode() != 0){ coldplug_log("skip: not in Local mode (card may be exported to a host)"); return 0; }
    return 1;
}
static void coldplug_start(void){
    if(!coldplug_should_run()) return;
    pthread_t th;
    if(pthread_create(&th, NULL, coldplug_thread, NULL) == 0) pthread_detach(th);
}

#ifdef DISKOS_DIAG_SDTRACE
/* DIAGNOSTIC ONLY (v240 boot-hang bisection): append a boot-stage marker to the SD
 * card, which is readable post-mortem via a card reader without any shell/serial.
 * Mounts the SD best-effort (it may not be auto-mounted this early). NOT for release. */
#include <sys/mount.h>
#include <sys/stat.h>
static void diag_sd(const char *msg){
    mkdir("/tmp/sdcard", 0755);
    mount("/dev/mmcblk0p1", "/tmp/sdcard", "exfat", 0, NULL);
    mount("/dev/mmcblk0p1", "/tmp/sdcard", "vfat", 0, NULL);
    int fd = open("/tmp/sdcard/diskos_boottrace.txt", O_WRONLY|O_CREAT|O_APPEND, 0644);
    if(fd >= 0){ char b[160]; int n = snprintf(b, sizeof b, "%ld %s\n", (long)time(NULL), msg);
        if(n > 0) { ssize_t w = write(fd, b, (size_t)n); (void)w; } fsync(fd); close(fd); }
}
#else
#define diag_sd(m) ((void)0)
#endif

int main(int argc, char **argv){
#ifdef DISKOS_DIAG_FBMARK
    /* DIAGNOSTIC: paint the framebuffer magenta the instant our main() runs, so a hung boot can be
     * told apart by eye: WHITE (rcS done) but no magenta => our binary never reached main(); magenta
     * but no real UI => we reached main() and died during init. Never ship. */
    system("/bin/sh /etc/diag_fb.sh magenta");
#endif
    /* argv[0] dispatch for the read-only fiio_init watchdog.
     * busybox `pgrep -x NAME` matches the FULL cmdline (argv[0]), not comm, and
     * the watchdog greps bare "mq_ui"/"mq_player". fiio_init launches the override
     * with FULL paths (/usr/data/mq_ui, /usr/data/mq_player) -> pgrep -x never
     * matches -> it kills us and respawns stock. So normalise argv[0] to the bare
     * name the watchdog expects:
     *   - invoked as .../mq_player (the player symlink -> this binary): exec the
     *     real stock player with argv[0]="mq_player".
     *   - invoked as anything other than exactly "mq_ui": re-exec self with
     *     argv[0]="mq_ui" so `pgrep -x mq_ui` matches. */
    {   const char *a0 = (argv[0] && argv[0][0]) ? argv[0] : "mq_ui";
        const char *slash = strrchr(a0,'/');
        const char *base = slash ? slash+1 : a0;
        if(strcmp(base,"mq_player")==0){
            char *pa[2]; pa[0]="mq_player"; pa[1]=NULL;
            execv("/usr/bin/mq_player", pa);
            _exit(127);   /* if exec fails, let the watchdog respawn stock */
        }
        if(strcmp(a0,"mq_ui")!=0){
            char *ua[3]; ua[0]="mq_ui"; ua[1]=(argc>1?argv[1]:NULL); ua[2]=NULL;
            execv("/usr/data/mq_ui", ua);   /* fall through and run if exec fails */
        }
    }
    /* Boot-default UI + Vol-Up override. fiio_init always launches OUR binary first, so we
     * decide here whether to run diskOS or hand off to the STOCK UI. Settings->System->"Default
     * UI" writes /usr/data/boot_default_stock (present => default = Stock). Holding Vol-Up at
     * power-on boots the OTHER one for this boot - the recovery path back to diskOS from stock,
     * and vice-versa. Player is always stock (handled by the mq_player symlink).
     *
     * DETECTION: we read the Vol-Up GPIO PIN LEVEL directly via /dev/mem - NOT the input layer.
     * Everything through /dev/input/event0 fails at boot: mq_player grabs event0 (our stream reads
     * see nothing), and a key held from POWER-ON leaves no EVIOCGKEY state (its GPIO edge predates
     * the input core, so no press event ever fires). The physical pin level has no such problem.
     * x2000 pinctrl @ 0x10010000 (one 4KB region per /proc/iomem); ports A-E at 0x100 stride;
     * PxPIN (live level) @ +0x00. Vol-Up = GPB pin 13 (DT vol-up-key). Register map validated
     * on-device 2026-08-13 against released states: GPB(0x10010100)=0xF6EFE127 has bits 13/14/15
     * (vol-up/down/play) high, GPE(0x10010400) bit31 (power) high. Active-low: pressed => bit 0. */
    /* ONLY on a real power-on. ui_restart_self() re-execs this binary to apply a theme
     * or font (and as watchdog recovery), which re-enters main() and would re-run the
     * hand-off below: with Default UI = Stock - i.e. the user normally boots stock and
     * reached diskOS by holding Vol-Up - Vol-Up is obviously not held now, so changing
     * the theme would silently launch the STOCK UI. The marker says "this is a restart,
     * not a boot"; it is an environment variable rather than a flag file so it cannot be
     * left stale, and unlike an argv element it does not alter the command line that
     * fiio_init's watchdog matches us by. */
    int is_ui_restart = (getenv("DISKOS_UI_RESTART") != NULL);
    unsetenv("DISKOS_UI_RESTART");   /* consume it: never inherited by a launched app */
    if(!is_ui_restart)
    {
        int flag_stock = (access("/usr/data/boot_default_stock", F_OK) == 0);
        int volup = 0, mem_ok = 0;
        uint32_t gpb = 0xFFFFFFFFu;   /* default = all-released if the read fails (never false-switch) */
        int mem = open("/dev/mem", O_RDONLY | O_SYNC);
        if(mem >= 0){
            void *map = mmap(NULL, 4096, PROT_READ, MAP_SHARED, mem, 0x10010000);
            if(map != MAP_FAILED){
                gpb = *(volatile uint32_t *)((char *)map + 0x100);   /* GPB PxPIN (live pin level) */
                volup = ((gpb >> 13) & 1u) ? 0 : 1;                  /* bit13 low => Vol-Up held */
                mem_ok = 1;
                munmap(map, 4096);
            }
            close(mem);
        }
        /* Persist what the override saw so a failed hand-off is debuggable. GATED behind
         * /usr/data/volup_dbg so a release image logs nothing by default (no unbounded growth);
         * create that flag file to enable the diagnostic (e.g. for the cold-boot Vol-Up test). */
        FILE *bl = (access("/usr/data/volup_dbg", F_OK) == 0) ? fopen("/usr/data/volup_boot.log", "a") : NULL;
        if(bl){
            fprintf(bl, "boot flag=%d mem_ok=%d gpb=0x%08x volup=%d boots=%s\n",
                    flag_stock, mem_ok, gpb, volup, (flag_stock ^ volup) ? "STOCK" : "diskOS");
            fclose(bl);
        }
        if(flag_stock ^ volup){                      /* effective = default XOR override */
            char *sa[2]; sa[0]="mq_ui"; sa[1]=NULL;
            execv("/usr/bin/mq_ui", sa);             /* hand off to stock; fall through to diskOS if exec fails */
        }
    }
    /* Boot diagnostics: route BOTH stdout and stderr through ONE fd (dup2) so they
     * share a file offset and don't clobber each other; line-buffered so the last
     * step before a crash is flushed. Truncates each launch (boot launch is what
     * we care about; avoids unbounded NAND growth). */
    freopen("/usr/data/diskos_boot.log", "w", stderr);
    dup2(fileno(stderr), fileno(stdout));
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    diag_sd("mqui: main() entered, boot log open");
    { static char altstk[32768];   /* run the handler on its own stack so a stack-overflow crash can still report */
      stack_t ss; ss.ss_sp = altstk; ss.ss_size = sizeof altstk; ss.ss_flags = 0; sigaltstack(&ss, NULL);
      struct sigaction sa; memset(&sa, 0, sizeof sa);
      sa.sa_sigaction = crash_log; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&sa.sa_mask);
      sigaction(SIGSEGV,&sa,NULL); sigaction(SIGBUS,&sa,NULL);
      sigaction(SIGABRT,&sa,NULL); sigaction(SIGFPE,&sa,NULL); }
    /* Boot-hang backstop: if we don't reach the main loop + first paint within
     * BOOT_DEADLINE_S, SIGALRM _exit()s us so fiio_init respawns a fresh mq_ui
     * (an alive-but-hung mq_ui is invisible to its `pgrep -x` watchdog). The
     * ceiling is generous so a slow-but-progressing boot never trips it; it's
     * disarmed (alarm(0)) the instant the first frame is on screen. */
    #define BOOT_DEADLINE_S 45
    { struct sigaction al; memset(&al,0,sizeof al);
      al.sa_handler = boot_alarm_handler; sigemptyset(&al.sa_mask); al.sa_flags = 0;
      sigaction(SIGALRM,&al,NULL); alarm(BOOT_DEADLINE_S); }
    fprintf(stderr,"=== diskos main start argc=%d ===\n", argc); fflush(stderr);
    /* WiFi bring-up + keep-alive is handled by wifi_supervise() in the main loop (which
     * also restarts the supplicant if it dies, so a known net returning in range auto-
     * joins). Intent is seeded from SYSCONFIG.WIFI_STATUS after cfg_load, below. */
    /* Robustness: a stray WORK_MODE persisted by a USB-storage / work-mode switch
     * (e.g. left at 0=NO_DEFINED) silently kills local tap-to-play - mq_player builds
     * the queue but never starts the decoder. diskOS only does local playback, so force
     * LOCALPLAYER(4) at boot. Self-heals across a reboot if it was ever left bad. */
    { char *a[] = { "sqlite3", "/usr/data/fiio/db/sysconfig.db",
                    "UPDATE SYSCONFIG SET WORK_MODE=4 WHERE ID=1 AND WORK_MODE<>4", NULL };
      run_bounded(a, 8000);   /* bounded: a locked/corrupt sysconfig.db can't wedge boot */ }
    /* Start the SSH server at boot (if installed) so a wedged USB serial can never
     * strand dev access again: dropbear listens once wlan0 gets an address. Harmless
     * if /usr/data/sshd isn't present; start-ssh.sh no-ops if already running. */
    system("[ -x /usr/data/sshd/start-ssh.sh ] && /usr/data/sshd/start-ssh.sh >/dev/null 2>&1 &");
    (void)fw_os_ver();  /* populate fwcaps' static cache on THIS (main) thread before coldplug_start()
                         * creates the worker - pthread_create is a memory barrier, so the worker reads
                         * the already-initialised cache (no first-init data race between the threads). */
    coldplug_start();   /* auto-mount the already-inserted microSD at boot (see coldplug_thread) */
    { unsigned seed=0; FILE *r=fopen("/dev/urandom","rb"); if(r){ if(fread(&seed,1,sizeof seed,r)!=sizeof seed) seed=(unsigned)time(NULL); fclose(r);} else seed=(unsigned)time(NULL); srand(seed); }  /* seed RNG (shuffle start pos) */
    lv_init();
    /* /dev/fb0 may not be ready the instant fiio_init launches us at boot; retry. */
    int fbok=0;
    for(int i=0;i<20;i++){ if(fbpan_create("/dev/fb0")){ fbok=1; break; }
        fprintf(stderr,"fbpan try %d failed, retry\n",i); fflush(stderr); usleep(250000); }
    if(!fbok){ fprintf(stderr,"fbpan failed (gave up)\n"); return 1; }
    cfg_load();
    /* Theme + font come straight after cfg_load and BEFORE anything paints: every
     * screen reads its colours and faces from these at create time, so they must be
     * resolved before screens_init() builds the widget tree. */
    theme_init();
    theme_font_init();
    swipe_thresh_load();
    settings_apply_startup();   /* restore saved brightness */
    wifi_init_intent();         /* seed wifi_on intent from stock WIFI_STATUS (first run only) */
    fprintf(stderr,"step:screens_init\n");fflush(stderr); screens_init();
    home_set_settings_click_cb(go_settings);
    library_set_song_click_cb(on_song_play);   /* tap a song -> play it */
    search_set_song_click_cb(on_search_play);   /* search result -> play in all-songs scope (no stale drill ctx) */
    { char *a[] = { "hwclock", "-s", NULL };
      run_bounded(a, 6000); }                    /* load system clock from RTC; bounded so a busy I2C/RTC can't wedge boot */
    lv_timer_create(hwclock_save_tick, 300000, NULL); /* write system time back to the RTC every 5 min */
    lv_timer_create(clock_tick, 10000, NULL);   /* refresh wall clock every 10s */
    clock_tick(NULL);                           /* set immediately */
    volkeys_init();                              /* map GPB so a volume press paints the bar without waiting on the player */
    g_t_status = lv_timer_create(status_poll_cb, 3000, NULL); /* charging/battery/wifi every 3s (BT every ~12s inside); paused at screen-off */
    status_poll_cb(NULL);                        /* populate immediately */
    bt_boot_restore();                           /* re-enable BT + arm auto-route if it was on (persist like WiFi) */
    g_t_wx  = lv_timer_create(weather_poll, 1000, NULL);  /* apply weather + retry/refresh */
    weather_fetch_async();                       /* kick off first fetch */
    g_t_lyr = lv_timer_create(lyrics_poll, 500, NULL);    /* apply finished lyrics fetch */
    lastfm_init();                                        /* load Last.fm config + offline queue */
    lv_timer_create(lastfm_tick, 1000, NULL);            /* watch play-state + drive scrobbles */
    lv_timer_create(scanner_poll, 500, NULL);            /* apply a finished library rescan */
    lv_timer_create(usb_connect_watch, 1000, NULL);      /* act on an unrequested USB-storage export */
    if(mdb_song_count()==0 && !mdb_load_failed()) scanner_start();   /* first run / GENUINELY empty DB -> auto-scan
                                                                     * the SD; skip on a transient DB load error so
                                                                     * we don't rebuild the library needlessly */
    g_t_art = lv_timer_create(ui_art_poll, 120, NULL);    /* apply finished album-art decode (worker thread) */
    lv_timer_create(v240_workmode_cb, 500, NULL);  /* V2.40: solicit a2, settle past the mode-control thread, then set LOCALPLAYER work-mode once */
    /* Background cover/accent prewarm. The worker self-gates on the user's "Album art
     * caching" setting (off/idle/charging) + a battery-temp throttle, so it's safe to
     * always spawn - it just sleeps while disabled or while the player is warm. */
    ui_start_art_prewarm();
    /* Armed only once the UI is fully up, so a slow boot can never trip it. */
    atomic_store(&g_heartbeat, lv_tick_get());
    ui_watchdog_start();
    /* Try to connect now; if the player's /ui queue isn't up yet (cold boot),
     * keep retrying in the background so the UI still comes up immediately. */
    if(ipc_start()!=0){
        fprintf(stderr,"ipc not ready, retrying in background\n"); fflush(stderr);
        lv_timer_create(ipc_connect_retry_cb, 1000, NULL);
    }
    /* Startup state-sync: ask the player to re-emit its a2 metadata for the LIVE
     * track so Now Playing is populated on launch (instead of "No Track").
     * 0202 (frame "02020008") is the decoded "request current state" command -
     * verified: player responds with an a202 full-metadata frame.
     * (MEMORY_PLAY was tried before and reverted: that row is the resume
     * BOOKMARK, not the live track, so it showed the wrong song/art/time.)
     * Sent once now (player already running in our launch) plus a one-shot retry
     * to cover the boot case where the player comes up slightly after the UI. */
    ipc_send_probe("02020008");   /* silent: pre-connect miss must not toast "Player didn't respond" */
    /* Retry a few times: at cold boot the player's /player queue appears a few
     * seconds after we start, so a single retry can miss it. */
    lv_timer_t *sync_t = lv_timer_create(state_sync_retry_cb, 2000, NULL);
    lv_timer_set_repeat_count(sync_t, 5);   /* 2,4,6,8,10s then auto-deletes */
    lv_timer_create(ipc_health_cb, 30000, NULL);   /* keep IPC warm + self-heal a player restart */

    /* Touch is default-ON: the whole UI is dead without it, and a fresh install carries no
     * /usr/data/touch_on marker, so opt-in gating meant a stock image booted untouchable.
     * /usr/data/touch_off is the recovery override (e.g. a wedged panel). */
    if(access("/usr/data/touch_off", 0)!=0){
        g_touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event1");
        /* 2nd RAW fd on the same evdev (evdev fans events out to every open fd) - drained each loop
         * purely to detect ANY touch for the screen-off wake, immune to LVGL's tap-coalescing. */
        g_touch_raw = open("/dev/input/event1", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if(g_touch){
            /* panel mounted 180deg, fb does reverse-copy -> invert both axes */
            lv_evdev_set_calibration(g_touch, 359, 359, 0, 0);
            /* cst816t is noisy on a small round panel: require more travel before a
             * press becomes a scroll, so deliberate taps aren't eaten as scrolls
             * (default scroll_limit=10). NB: scroll_throw is a slowdown-%, left at its
             * default - coast feel is a user-testing call, not safe to guess blind. */
            lv_indev_set_scroll_limit(g_touch, 18);
            g_dbg = (access("/usr/data/touch_dbg", 0)==0);
            if(g_dbg) dbgdot_init();
            fprintf(stderr,"touch ENABLED indev=%p dbg=%d thresh=%d\n",(void*)g_touch,g_dbg,g_swipe_thresh);
        } else fprintf(stderr,"touch create FAILED\n");
    } else fprintf(stderr,"touch OFF (no /usr/data/touch_on)\n");
    fflush(stderr);

    int start = SCR_HOME;   /* no-arg default = home (boot supervisor launches with no args) */
    if(argc>1){
        if(!strcmp(argv[1],"home"))    start = SCR_HOME;
        else if(!strcmp(argv[1],"library")) start = SCR_LIBRARY;
        else if(!strcmp(argv[1],"nowplaying")) start = SCR_NOWPLAYING;
        else if(!strcmp(argv[1],"settings")) start = SCR_SETTINGS;
        else if(!strcmp(argv[1],"workmode")) start = SCR_WORKMODE;
        else if(!strcmp(argv[1],"search")) start = SCR_SEARCH;
        else if(!strcmp(argv[1],"quick")) start = SCR_QUICK;
        else if(!strcmp(argv[1],"songinfo")) start = SCR_SONGINFO;
        else if(!strcmp(argv[1],"tune")) start = SCR_TUNE;
        else if(!strcmp(argv[1],"ctx")) start = SCR_NPMENU;
        else if(!strcmp(argv[1],"eq")) start = SCR_EQ;
        else if(!strcmp(argv[1],"apps")) start = SCR_APPS;
        else if(!strcmp(argv[1],"weather")) start = SCR_WEATHER;
        else if(!strcmp(argv[1],"lyrics")) start = SCR_LYRICS;
        else if(!strcmp(argv[1],"saver")) start = SCR_SAVER;
        else if(!strcmp(argv[1],"hub")) start = SCR_NPHUB;
    }
    screen_show(start);
    /* Force ONE full-screen repaint at startup. The framebuffer may still hold the stock
     * boot splash (switching stock->diskOS, or our own relaunch); the partial-render engine
     * only copies widget-dirty areas, so background regions would show stale splash pixels
     * until the next full transition. Invalidating the whole active screen marks the entire
     * 360x360 dirty -> fb_pan copies the full frame -> clean screen from the first frame. */
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    if(argc>1 && !strcmp(argv[1],"wifi")) wifi_open();   /* deep-link: open Wi-Fi + scan */
    if(argc>1 && !strcmp(argv[1],"bt"))   bt_open();     /* deep-link: open Bluetooth + scan */
    if(argc>1 && !strcmp(argv[1],"lyrics")) lyrics_open(); /* deep-link: load + show lyrics */
    if(argc>1 && !strcmp(argv[1],"runhello")) app_launch("/usr/data/apps/hello/app");
    if(argc>1 && !strcmp(argv[1],"setdemo")) settings_open_detail(0);  /* Play Mode detail */
    printf("diskOS up (screen %d)\n", start); fflush(stdout);

    track_state_t st; unsigned last=~0u;
    lv_indev_state_t prev_ts = LV_INDEV_STATE_RELEASED;
    int sx=0, sy=0, lastx=0, lasty=0; uint32_t sms=0;   /* swipe tracking */
    int cover_tap=0;   /* press landed on the NP cover -> LVGL's cover click opens full-screen art (not a seek) */
    int fsart_touch=0; /* press began while full-screen art was up -> LVGL's overlay click closes it; we only swallow */
    long last_pos=-1; uint32_t pos_change_tick=0; int last_playing=-1;
    /* screensaver + screen-off timeouts come from the Settings cyclers (index ->
     * seconds via TMAP), re-read each loop so changes apply live. */
    static const int TMAP[5] = {0,30,60,120,300};
    uint32_t last_activity = lv_tick_get();
    int woke = 0;   /* a press that only wakes the saver is swallowed */
    int bl_state = 0;   /* 0 = normal, 1 = saver-dim, 2 = off */
    unsigned last_vol_seq = 0;
    /* Start the boot splash HERE - after all the blocking startup init (hwclock, bt_boot_restore,
     * IPC connect, etc.). Created earlier, its time-based animation would elapse during that
     * blocking init (the loop isn't ticking yet) and get skipped. Here it animates cleanly in the
     * loop from t=0. It's on lv_layer_top so it covers whatever's already drawn, then reveals it. */
    boot_splash_start();
    /* Init is complete. Force the FIRST FRAME to actually paint (lv_refr_now flushes
     * synchronously on this fb), then disarm the boot watchdog - a painted frame, not
     * merely "lv_timer_handler returned", is what proves we're alive. Log BEFORE
     * disarming (a blocked write must never leave us alive-but-disarmed), and mask
     * SIGALRM across alarm(0) so an already-pending timer can't fire after we disarm. */
    lv_refr_now(NULL);
    diag_sd("mqui: reached main loop, first frame painted");
    { static const char okm[] = "=== diskos reached main loop (first frame painted; boot watchdog disarmed) ===\n";
      (void)write(2, okm, sizeof okm - 1);
      sigset_t as; sigemptyset(&as); sigaddset(&as, SIGALRM);
      sigprocmask(SIG_BLOCK, &as, NULL);
      alarm(0);
      signal(SIGALRM, SIG_IGN);
      sigprocmask(SIG_UNBLOCK, &as, NULL); }
    for(;;){
        if(ipc_take_reconnected()){       /* player restarted + /ui reattached: resync UI-owned state */
            ui_reapply_audio();           /* re-send managed audio config (DRE/filter/etc.); the v2.40 local-init
                                           * is NOT here - it's sent from the play path when the player is ready. */
            ui_invalidate_play_scope();   /* the player's LIST_SONG_0 is gone - don't jump against it */
        }
        ipc_get_state(&st);
        wifi_supervise();   /* keep wpa_supplicant alive when Wi-Fi should be on (auto-reconnect) */
        /* NOTE: the MAIN LOOP never re-sends audio settings per-tick. Managed settings are (re)applied
         * only by ui_reapply_audio() on a CONFIRMED player reconnect (the ipc_take_reconnected() gate
         * above) and by the live apply_* handlers on a user change. Sending 0666 (output route) to an
         * IDLE, freshly-booted player re-inits its path and wedges it (g_fiio_local null) - which is
         * exactly why the v2.40 Local-init in ui_reapply_audio() rides on the reconnect (the player is
         * re-initialised by then), gated to Local + fw==240, rather than a blind boot send. At cold boot
         * the player recreates /ui after we start (main.c ~L590 note), so that reconnect reliably fires.
         * Volume persistence relies on the player's shutdown-save. */
        /* VOLUME MEMORY: handled NATIVELY by the player, which saves SYSCONFIG.VOLUME on real
         * shutdown and restores it on boot. diskOS does NOT restore/save volume - an earlier
         * attempt to infer it from a714 frames could corrupt cfg under a lossy/backlogged /ui
         * (no ack to correlate our restore's echo). The "boots at 0" seen in dev was our
         * `reboot`s skipping the player's shutdown-save, not a real power-off. VERIFY on device:
         * set a volume, power OFF with the button, power ON -> should return to that volume. */
        /* volume change (hardware buttons -> player -> a714 frame): show the bar,
         * count it as activity, and wake the screen so the bar is visible. */
        int volkey_edge  = volkeys_pressed();
        int vol_changed   = (st.volume_seq != last_vol_seq);
        if(vol_changed) last_vol_seq = st.volume_seq;
        /* A real volume change (a714) wakes the screen, as it always did. A bare KEY
         * EDGE only paints the bar on an already-lit screen - deliberately not a wake
         * source, so double-pressing Vol-Up to skip a track in a pocket cannot light
         * the panel (the player emits no a714 for a skip, so today it stays dark). */
        if(vol_changed || (volkey_edge && !bl_state)){
            last_activity = lv_tick_get();
            if(vol_changed && bl_state){ ui_backlight(ui_get_brightness()); bl_state = 0; }
            if(screen_current()==SCR_SAVER) screen_back();
            ui_show_volume(st.volume);
        }
        /* the metadata 'state' field is unreliable (reports 0 while playing);
         * infer play/pause from whether position is advancing. */
        if(st.position_ms != last_pos){ last_pos = st.position_ms; pos_change_tick = lv_tick_get(); }
        int playing = (st.have_track && st.position_ms > 0 && lv_tick_elaps(pos_change_tick) < 1600);
        g_playing = playing;   /* publish for ui_route_bt/ui_route_analog (raw st.state is unreliable) */
        st.state = playing ? 2 : 1;
        /* when the backlight is off (deep idle) nothing is visible - skip the whole
         * UI refresh; it catches up on wake (last/ last_playing stay stale). */
        /* clear a pending play on a REAL track change (different path) OR a restart of
         * the SAME track (position jumped backwards >1.5s - covers replaying the current
         * track, which keeps the same path). NOT on st.seq, which a1 frames from the old
         * still-streaming track also bump (would clear prematurely). */
        /* Two-part confirm, deliberately decoupled:
         *  (a) PENDING-CLEAR on the path-change/restart heuristic - acknowledges playback so
         *      "Starting..." / the 6s "Couldn't start" timeout behave exactly as before (no
         *      regression even if the path strings ever differ).
         *  (b) SCOPE-COMMIT only when PROVEN: no overlapping build, a pendscope exists, AND
         *      either there's no single target (play-all) or the target track is the one now
         *      playing (a natural advance / HW key can't fake this). Otherwise leave the jump
         *      scope empty so the next tap safely rebuilds - never a wrong-list jump. */
        if(g_play_pending && st.have_track &&
           (strcmp(st.path, g_play_initpath) != 0 || st.position_ms + 1500 < g_play_initpos)){
            g_play_pending = 0;
            /* commit the jump scope ONLY with a proven target match. Targetless plays
             * (play-all/shuffle) therefore never populate a jumpable scope on the
             * heuristic alone - so a later song-tap can't jump a play-all scope that
             * a natural advance committed prematurely (hunt#6-4b). The first song-tap
             * after a play-all simply rebuilds, then the fast-path resumes. */
            int proven = !g_play_dirty && g_play_pendscope[0] &&
                         g_play_pendtarget[0] && strcmp(st.path, g_play_pendtarget) == 0;
            if(proven) snprintf(g_play_scope, sizeof g_play_scope, "%s", g_play_pendscope);
            else       g_play_scope[0] = '\0';
            g_play_dirty = 0;
        }
        if(bl_state != 2 && (st.seq != last || playing != last_playing)){
            last = st.seq; last_playing = playing;
            ui_update(&st);
            home_set_now_playing(st.have_track?st.title:NULL,
                                 st.have_track?st.artist:NULL,
                                 ui_current_accent(), playing);
            home_set_art_src(ui_current_thumb_src());
            home_set_backdrop(ui_current_backdrop_src());
            saver_set_track(st.have_track?st.title:NULL,
                            st.have_track?st.artist:NULL,
                            ui_current_backdrop_src());
            quicksettings_refresh(playing);
            songinfo_set(&st);
            npmenu_set(st.have_track ? &st : NULL, playing, ui_current_thumb_src());
        }
        /* album art decodes off-thread now, so it usually lands AFTER the seq-gated
         * push above - re-push the art-dependent surfaces when a decode completes.
         * Test bl_state FIRST so the one-shot flag is NOT consumed while the screen is
         * off (bl_state==2): it stays set and is applied on wake, else art goes stale. */
        if(bl_state != 2 && ui_take_art_applied()){
            home_set_art_src(ui_current_thumb_src());
            home_set_backdrop(ui_current_backdrop_src());
            saver_set_track(st.have_track?st.title:NULL,
                            st.have_track?st.artist:NULL,
                            ui_current_backdrop_src());
            npmenu_set(st.have_track ? &st : NULL, playing, ui_current_thumb_src());
        }
        /* playback-start timeout: no track update within 6s of a rebuild-play */
        if(g_play_pending && lv_tick_elaps(g_play_pending) > 6000){
            g_play_pending = 0; ui_toast("Couldn't start playback");
            g_play_scope[0] = '\0'; g_play_pendscope[0] = '\0'; g_play_pendtarget[0] = '\0'; g_play_dirty = 0;   /* build failed */
            /* V2.40 safety net: a failed local start is most likely NO_WORK_MODE (the one-shot's settle
             * delay wasn't enough, or the player restarted). Re-assert the local-init ONCE so the user's
             * NEXT tap succeeds. Gated Local + analog output; not a retry loop (one send on a real failure). */
            if(fw_os_ver() == 240 && ui_get_source_mode() == 0 && !g_route_mac[0]){
                ipc_send_cmd("0666000C0006"); ipc_send_cmd("0657000C0008");
            }
        }
        /* spin the vinyl only while it's the visible, playing, lit Now Playing */
        ui_vinyl_spin(playing && screen_current()==SCR_NOWPLAYING && bl_state==0);
        /* spin while the saver is visible, the screen is on, AND audio is actually
         * playing - a real platter stops when paused. `playing` (position advancing)
         * is the reliable signal; it freezes the vinyl ~1.6s after pause/stop. The saver
         * dims to bl_state==1 the moment it appears, so gate on !=2 (not ==0) for the
         * lit test; a FULLY-off screen (bl_state==2) stops the spin anyway. */
        saver_vinyl_spin(playing && screen_current()==SCR_SAVER && bl_state != 2);

        /* surface silent failures (sticky flags persist until the screen is on, so a
         * failure during screen-off is shown on wake rather than lost). */
        if(bl_state != 2){
            /* one toast per tick - toast.c shows only one at a time, so an `if/if`
             * would let the 2nd clobber the 1st. else-if leaves the other flag set
             * to surface on the next tick (both are rare; the 1-tick stagger is unseen). */
            if(cfg_take_save_error())       ui_toast("Couldn't save settings");
            else if(ipc_take_send_error())  ui_toast("Player didn't respond");
        }
        atomic_store(&g_heartbeat, lv_tick_get());   /* watchdog liveness stamp (see ui_watchdog) */
        uint32_t wait = lv_timer_handler();
        /* (boot watchdog was already disarmed before the loop, after the first frame
         * actually painted - see lv_refr_now above.) */

        if(g_app_pending){          /* run a launched app between frames */
            g_app_pending = 0;
            screen_back();          /* leave the Apps screen first */
            app_run(g_app_exec);
            last = ~0u;             /* force a state refresh after returning */
            last_activity = lv_tick_get();   /* using an app counts as activity */
        }

        if(g_touch){
            lv_indev_state_t ts = lv_indev_get_state(g_touch);
            lv_point_t p; lv_indev_get_point(g_touch, &p);
            if(kbinput_active()){
                /* the modal keyboard owns all touch - don't run nav gestures */
            } else if(ts==LV_INDEV_STATE_PRESSED && prev_ts==LV_INDEV_STATE_RELEASED){
                /* press edge: start of a potential swipe */
                last_activity = lv_tick_get();
                if(screen_current()==SCR_SAVER){
                    /* any touch wakes the saver; restore backlight, swallow gesture */
                    if(bl_state){ ui_backlight(ui_get_brightness()); bl_state = 0; }
                    screen_back();
                    woke = 1;
                } else {
                    sx=p.x; sy=p.y; lastx=p.x; lasty=p.y; sms=lv_tick_get();
                    cover_tap = 0;
                    fsart_touch = ui_np_fsart_active();   /* latch: overlay owns this whole gesture */
                    if(fsart_touch){
                        /* full-screen art is up -> LVGL's overlay click will close it; arm nothing here */
                    } else if(screen_current()==SCR_NOWPLAYING){
                        /* A press within the album-cover square (centre 180,116; ~148px) is a
                         * candidate to open full-screen art - DON'T arm a seek there, so the tap
                         * can't be eaten by the ring (LVGL's cover click opens it). Presses
                         * elsewhere on NP drive the seek. */
                        if(sx>=100 && sx<=260 && sy>=38 && sy<=194) cover_tap = 1;
                        else ui_np_seek_press(sx, sy);
                    }
                    else rim_press(sx, sy);   /* arm rim-scroll candidate on long-list screens */
                    fprintf(stderr,"TAP x=%d y=%d\n",sx,sy); fflush(stderr);
                    if(g_dbg && g_dbgdot){
                        lv_obj_set_pos(g_dbgdot, sx-13, sy-13);
                        lv_obj_clear_flag(g_dbgdot, LV_OBJ_FLAG_HIDDEN);
                    }
                }
            } else if(ts==LV_INDEV_STATE_PRESSED){
                last_activity = lv_tick_get();
                lastx=p.x; lasty=p.y;
                if(screen_current()==SCR_NOWPLAYING && !ui_np_fsart_active()) ui_np_seek_move(p.x, p.y);
                else rim_move(p.x, p.y);   /* drives list scroll while a rim drag owns the gesture */
            } else if(ts==LV_INDEV_STATE_RELEASED && prev_ts==LV_INDEV_STATE_PRESSED){
                if(woke){
                    woke = 0;   /* swallow the release that woke the saver */
                } else if(rim_release()){
                    /* rim-scroll owned this gesture -> no nav/tap classification */
                } else {
                    /* release edge: classify the swipe */
                    int dx=lastx-sx, dy=lasty-sy;
                    int adx=dx<0?-dx:dx, ady=dy<0?-dy:dy;
                    uint32_t dt=lv_tick_elaps(sms);
                    int cur=screen_current();
                    /* fsart is owned entirely by LVGL (cover click opens, overlay click closes).
                     * Swallow (never open/close/seek/nav) when the overlay is up / the press began
                     * on it (fsart_touch), or when this is a genuine TAP that began on the cover
                     * (cover_tap + small travel) - LVGL's cover click opens that one. A SWIPE that
                     * merely started on the cover is NOT LVGL's (its cover click ignores slides), so
                     * it must still fall through to nav (back/hub). Order-independent: only LVGL
                     * mutates fsart state, so there's no open/close race. */
                    int cover_tap_gesture = cover_tap && adx < 20 && ady < 20;
                    int lvgl_owned = fsart_touch || cover_tap_gesture;
                    /* a committed seek on Now Playing consumes the gesture */
                    int seek_consumed = (!lvgl_owned && cur==SCR_NOWPLAYING && !ui_np_fsart_active())
                                        ? ui_np_seek_release(lastx, lasty) : 0;
                    int vert = ady > adx*2, horiz = adx > ady*2;
                    if(lvgl_owned || ui_np_fsart_active()){
                        /* swallow - LVGL handled (or will handle) the open/close */
                    } else if(!seek_consumed){
                    if(cur==SCR_QUICK && vert && dy<0 && ady>=g_swipe_thresh && dt<700){
                        screen_back();                       /* swipe up closes quick settings */
                    } else if(cur!=SCR_QUICK && cur!=SCR_SAVER && sy<40 &&
                              vert && dy>0 && dy>=g_swipe_thresh && dt<700){
                        screen_show(SCR_QUICK);              /* pull down from top edge */
                        quicksettings_refresh(playing);
                    } else if(cur==SCR_NOWPLAYING){
                        /* On Now Playing a nav swipe must be a LONG, STRAIGHT,
                         * horizontal slide (so it's not confused with a seek drag
                         * along the ring): right = hub, left = back. */
                        if(adx >= NP_NAV_DIST && adx > ady*NP_NAV_STRAIGHT && dt<900){
                            if(dx < 0) screen_show(SCR_NPHUB);
                            else       screen_back();
                        }
                    } else if(cur==SCR_HOME && horiz && dx<0 && adx>=g_swipe_thresh && dt<700){
                        apps_reload();
                        screen_show(SCR_APPS);    /* slide in the Apps panel */
                    } else if(horiz && dx>0 && adx>=g_swipe_thresh && dt<700 &&
                              sx < BACK_START_MAX_X){
                        fprintf(stderr,"BACK swipe sx=%d dx=%d dy=%d dt=%u\n",sx,dx,dy,dt); fflush(stderr);
                        /* inside Library, step through its sub-views first */
                        if(!(cur==SCR_LIBRARY && library_back())) screen_back();
                    }
                    }
                }
            }
            prev_ts = ts;
        }

        /* sleep timer: pause when it elapses (only if actually playing) */
        if(g_sleep_ms && lv_tick_elaps(g_sleep_start) >= g_sleep_ms){
            g_sleep_ms = 0;
            if(playing) ipc_send_cmd("0201000C0000");   /* toggle = pause while playing */
            cfg_set_int("sleep_idx", 0);
        }

        /* screensaver + backlight power saving (the screen is the biggest drain):
         * idle > saver_timeout       -> show saver, dim backlight
         * idle > saver+screenoff      -> backlight off entirely */
        int si = cfg_get_int("saver_idx", 2), oi = cfg_get_int("screenoff_idx", 3);
        int saver_timeout   = (si>=0 && si<5) ? TMAP[si] : 60;
        int screenoff_extra = (oi>=0 && oi<5) ? TMAP[oi] : 120;
        /* Never sleep while the on-screen keyboard is open: it owns all touch, so a
         * dimmed/off saver couldn't be woken mid-typing (e.g. entering a Wi-Fi password). */
        /* Screen-off/dim wake on RAW touch: drain the raw evdev fd; ANY event means a finger touched,
         * so we wake even when LVGL coalesced the whole tap into a no-press-edge RELEASED (the reason
         * a dark panel needed many taps to wake). Drained every loop (even awake) so it never backs
         * up; only acts when the panel is dimmed/off (bl_state != 0). */
        if(g_touch_raw >= 0){
            char rb[512]; int any = 0;
            while(read(g_touch_raw, rb, sizeof rb) > 0) any = 1;
            if(any && bl_state){
                last_activity = lv_tick_get();
                ui_backlight(ui_get_brightness()); bl_state = 0;
                if(screen_current() == SCR_SAVER) screen_back();
            }
        }
        if(kbinput_active()) last_activity = lv_tick_get();
        int in_saver = (screen_current()==SCR_SAVER);
        /* a manual Sleep request ends the moment the panel is woken (we leave SCR_SAVER) */
        if(g_manual_sleep && !in_saver) g_manual_sleep = 0;
        int manual = g_manual_sleep;
        /* Art-based savers (cover=0, vinyl=4) are a NOW-PLAYING display - only show them while
         * something is playing; with nothing playing they're pointless (and were showing when
         * they shouldn't). Time/info savers (analog=1, minim=2, weather=3) stay useful idle.
         * A suppressed art saver still power-saves the backlight below. */
        int sstyle    = cfg_get_int("saver_style", 0);
        int art_saver = (sstyle == 0 || sstyle == 4);
        int saver_ok  = (!art_saver || playing);
        if(saver_timeout > 0 || manual){
            /* manual sleep runs its dim/off countdown from the tile tap, independent of the
             * (possibly disabled) auto-saver timer */
            uint32_t idle = manual ? lv_tick_elaps(g_manual_sleep_at) : lv_tick_elaps(last_activity);
            int past_dim = (saver_timeout > 0 && idle > (uint32_t)saver_timeout*1000);
            /* auto-enter only in timeout mode AND only when the saver is appropriate now (art
             * savers need playback). The Sleep tile already opened SCR_SAVER (manual). */
            if(past_dim && !in_saver && (saver_ok || manual)){
                screen_show(SCR_SAVER); in_saver = 1;
            }
            /* dim once idle past the timeout (manual: immediately) - whether or not a saver
             * screen is up, so a suppressed art saver still sleeps the screen normally. */
            if((in_saver || past_dim || manual) && bl_state==0){
                /* the vinyl art-showcase saver stays at the user's brightness; everything else
                 * (incl. a suppressed art saver just dimming the current screen) crushes to a
                 * low dim. bl_state still -> 1 so the screen-off timer powers the panel down. */
                int dim = ui_get_brightness();
                if(dim > 6 && !(in_saver && saver_wants_bright())) dim = 6;
                ui_backlight(dim); bl_state = 1;
            }
            /* full off after the screen-off delay. Manual sleep uses the configured Screen Off
             * delay when set, else a 10s default so the tile actually powers the panel down. */
            uint32_t off_at = manual ? (uint32_t)(screenoff_extra > 0 ? screenoff_extra : 10)*1000
                                     : (uint32_t)(saver_timeout+screenoff_extra)*1000;
            if(bl_state==1 && (manual || screenoff_extra > 0) && idle > off_at){
                ui_backlight(0); bl_state = 2;
            }
        }

        /* adaptive sleep: poll fast (5ms) while animating or finger-down for
         * responsiveness, otherwise let the CPU idle longer (30ms) to save power.
         * lv_timer_handler already returns a large 'wait' when nothing is pending. */
        /* screen fully off (pocket playback): pause the frequent polls + idle longer
         * so the loop wakes ~5x/s (touch poll) instead of ~33x/s. Derived from
         * bl_state each iteration so every wake path is covered. */
        polls_set_paused(bl_state == 2);
        g_bl_idle = (bl_state >= 1);   /* prewarm worker reads this: only work while dimmed/off */
        int busy = lv_anim_count_running() > 0 || prev_ts == LV_INDEV_STATE_PRESSED;
        if(bl_state == 2 && !busy){
            /* deep idle: OVERRIDE LVGL's ~33ms indev wait (cap is only an upper bound). At 60ms a
             * QUICK tap could land entirely inside one sleep -> lv_evdev coalesces it to a final
             * RELEASED -> no press edge -> no wake (device-observed 2026-08-26: taps didn't wake the
             * screen-off panel). 30ms is well below finger-contact time, so even a fast tap spans a
             * sample and produces a PRESSED edge -> wake. Still deep-idle (~33/s -> ~33/s cap but only
             * when a touch is pending; idle CPU cost is negligible vs. a reliably-wakeable screen). */
            wait = 30;
        } else {
            uint32_t cap = busy ? 5 : 30;
            if(wait > cap) wait = cap;
        }
        usleep(wait*1000);
    }
    return 0;
}
