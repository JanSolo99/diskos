/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "receive.h"
#include "txtfold.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <poll.h>

/* Receive Music - drop files onto the card from a browser on the same network.
 *
 * The server shape is lifted from lastfm.c's credential-setup server, which is already
 * reviewed and shipping: bound to the wlan0 address rather than INADDR_ANY, an
 * unguessable path token, poll() with a stop flag instead of a blocking accept, per
 * socket send/recv timeouts, and a total deadline so a slow client cannot pin the
 * worker. What is new here is the body handling.
 *
 * THE BODY IS STREAMED. The device has ~60 MB available and albums are larger than
 * that, so nothing is buffered: bytes go to the card as they arrive, in a 32 KB chunk,
 * into a dot-prefixed .part file that is renamed into place only when the declared
 * Content-Length has actually been received. A half-transfer therefore never exists
 * under a name the scanner would index or the player would try to open, and an
 * interrupted upload leaves one hidden file rather than a corrupt track. */

/* Port 80 so the URL has no ":port" to read off a round screen and type. We are root,
 * so binding it is allowed; nothing on the stock firmware listens there (checked on
 * device: 22, 53, 111, 12100, 12103, 47220, 50411, 50902). Falls back to 8081 if the
 * bind is ever refused, so a future firmware taking port 80 degrades instead of
 * breaking. lastfm's setup server owns 8080, hence not that. */
#define RX_PORT        80
#define RX_PORT_ALT    8081
#define RX_MUSIC_DIR   "/tmp/sdcard/Music"
#define RX_INBOX       "Received"    /* subfolder, so it is obvious what arrived this way */
#define RX_CHUNK       (32*1024)
#define RX_MIN_FREE    (256ULL*1024*1024)   /* refuse an upload that would fill the card */
#define RX_MAX_NAME    160
#define RX_IDLE_MS     15000         /* no bytes for this long -> drop the connection */

static pthread_t   g_th;
static int         g_th_live;
static atomic_int  g_run;
static int         g_fd = -1;
/* The socket currently being served, or -1. receive_stop() runs on the LVGL THREAD and
 * joins the worker, so it must be able to make a blocked recv() return AT ONCE:
 * shutdown() on this fd does that. Without it, leaving the screen during an upload
 * would block the UI for a whole SO_RCVTIMEO - a visible freeze on a screen change,
 * and exactly the "nothing slow on the LVGL thread" rule this codebase is built on. */
static atomic_int  g_client = -1;
static char        g_url[96];
static char        g_token[20];

/* Written by the server thread, read by the UI loop. Ints are atomic; the strings are
 * guarded, because a half-written filename on screen is worse than no filename. */
static atomic_int  g_done;           /* completed files this session */
static atomic_int  g_busy;           /* 1 while a body is being written */
static atomic_long g_bytes;          /* bytes of the transfer in flight */
static pthread_mutex_t g_str_mu = PTHREAD_MUTEX_INITIALIZER;
static char        g_last[RX_MAX_NAME];
static char        g_err[128];

/* ---------------------------------------------------------------- small helpers */

static int64_t now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (int64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}

static void set_str(char *dst, size_t cap, const char *s){
    pthread_mutex_lock(&g_str_mu);
    snprintf(dst,cap,"%s",s?s:"");
    pthread_mutex_unlock(&g_str_mu);
}

static int wlan_ip(char *out, int cap){
    int s = socket(AF_INET, SOCK_DGRAM, 0); if(s<0) return -1;
    struct ifreq ifr; memset(&ifr,0,sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "wlan0");
    int r = ioctl(s, SIOCGIFADDR, &ifr); close(s);
    if(r<0) return -1;
    struct sockaddr_in *sin = (struct sockaddr_in*)&ifr.ifr_addr;
    const char *p = inet_ntoa(sin->sin_addr); if(!p) return -1;
    snprintf(out,cap,"%s",p);
    return 0;
}

/* Unguessable path token. /dev/urandom or nothing: a predictable token would make the
 * "only while the screen is open" guarantee the ONLY thing standing between the card
 * and anyone on the network, and that is too much weight for one guarantee to carry. */
/* 6 characters, not 12. The whole URL has to be readable off a 360px circle and typed
 * by hand, and 6 chars of a 32-symbol alphabet is still 2^30 - about a billion - which
 * is not brute-forceable over HTTP on a LAN in the few minutes this server is even
 * listening. The alphabet already omits l/o/0/1 so nothing is ambiguous when read aloud. */
static int gen_token(char *out, int cap){
    static const char AL[] = "abcdefghijkmnpqrstuvwxyz23456789";
    unsigned char r[6];
    int f = open("/dev/urandom", O_RDONLY); if(f<0) return -1;
    ssize_t n = read(f, r, sizeof r); close(f);
    if(n != (ssize_t)sizeof r) return -1;
    int i=0; for(; i<(int)sizeof r && i<cap-1; i++) out[i] = AL[r[i] & 31];
    out[i]=0;
    return 0;
}

static int enough_free(void){
    struct statvfs v;
    if(statvfs("/tmp/sdcard",&v)!=0) return 0;
    return ((uint64_t)v.f_bavail * v.f_frsize) > RX_MIN_FREE;
}

/* Only extensions the scanner actually indexes. Anything else would sit on the card
 * invisible to the library, which reads as "my upload vanished". */
static int indexable(const char *n){
    const char *d = strrchr(n,'.'); if(!d) return 0;
    static const char *ok[] = {".mp3",".flac",".ogg",".oga",".opus",".m4a",".m4b",".mp4",
                               ".ape",".wv",".mpc",".tta",".dsf",".wav",".aif",".aiff",
                               ".dff",".wma",".aac",NULL};
    for(int i=0; ok[i]; i++) if(!strcasecmp(d,ok[i])) return 1;
    return 0;
}

/* Reduce whatever the browser sent to a SAFE basename.
 *
 * This is the load-bearing security check: the client names the file, and the name goes
 * straight into a path we open for writing. Everything up to the last slash or backslash
 * is discarded (so "../../etc/x" becomes "x"), a leading dot is refused (no hidden files,
 * and no chance of writing our own .part convention), and the result must still look
 * like a track. Returns 0 on success. */
static int safe_name(const char *raw, char *out, int cap){
    if(!raw || !*raw) return -1;
    const char *b = raw;
    for(const char *p=raw; *p; p++) if(*p=='/' || *p=='\\') b = p+1;
    if(!*b || *b=='.') return -1;
    int o=0;
    for(const char *p=b; *p && o<cap-1; p++){
        unsigned char c=(unsigned char)*p;
        if(c < 0x20 || c==0x7f) continue;            /* control bytes never reach the card */
        out[o++] = *p;
    }
    out[o]=0;
    if(!out[0] || !indexable(out)) return -1;
    return 0;
}

static void send_simple(int c, const char *status, const char *ctype, const char *body){
    char hdr[256];
    int n = snprintf(hdr,sizeof hdr,
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
        "Connection: close\r\nCache-Control: no-store\r\n\r\n",
        status, ctype, (unsigned)strlen(body));
    if(n>0 && n<(int)sizeof hdr){ (void)!write(c,hdr,(size_t)n); }
    (void)!write(c, body, strlen(body));
}

/* ---------------------------------------------------------------- the page */

/* One self-contained page: drag-and-drop, or a file picker for phones. Each file is
 * PUT individually to /<token>/up/<name>, so the device never has to parse multipart -
 * one fewer parser between the network and a file descriptor. */
static const char PAGE_A[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Send music to diskOS</title><style>"
    "body{font:16px/1.5 system-ui,sans-serif;max-width:34em;margin:2em auto;padding:0 1em;color:#eee;background:#111}"
    "#d{border:2px dashed #555;border-radius:14px;padding:2.5em 1em;text-align:center;color:#aaa;margin:1.5em 0}"
    "#d.o{border-color:#3c8;color:#3c8}ul{list-style:none;padding:0}"
    "li{display:flex;justify-content:space-between;gap:1em;padding:.4em 0;border-bottom:1px solid #222}"
    ".k{color:#3c8}.x{color:#e55}small{color:#888}</style>"
    "<h2>Send music to diskOS</h2>"
    "<div id=d>Drop files here<br><small>or</small><br><input type=file id=f multiple></div>"
    "<ul id=l></ul><script>"
    "var T='";
static const char PAGE_B[] =
    "';var d=document.getElementById('d'),l=document.getElementById('l');"
    "function add(n){var li=document.createElement('li');li.innerHTML='<span></span><span class=s>...</span>';"
    "li.firstChild.textContent=n;l.appendChild(li);return li.querySelector('.s')}"
    "function up(fs,i){if(i>=fs.length)return;var f=fs[i],s=add(f.name);"
    "var x=new XMLHttpRequest();x.open('PUT',T+'up/'+encodeURIComponent(f.name));"
    "x.upload.onprogress=function(e){if(e.lengthComputable)s.textContent=Math.round(e.loaded/e.total*100)+'%'};"
    "x.onload=function(){if(x.status==200){s.textContent='done';s.className='s k'}"
    "else{s.textContent=x.responseText||('error '+x.status);s.className='s x'}up(fs,i+1)};"
    "x.onerror=function(){s.textContent='failed';s.className='s x';up(fs,i+1)};x.send(f)}"
    "d.ondragover=function(e){e.preventDefault();d.className='o'};"
    "d.ondragleave=function(){d.className=''};"
    "d.ondrop=function(e){e.preventDefault();d.className='';up(e.dataTransfer.files,0)};"
    "document.getElementById('f').onchange=function(e){up(e.target.files,0)};"
    "</script>";

static void send_page(int c){
    char page[sizeof PAGE_A + sizeof PAGE_B + 64];
    int n = snprintf(page,sizeof page,"%s/%s/%s",PAGE_A,g_token,PAGE_B);
    if(n<0 || n>=(int)sizeof page){ send_simple(c,"500 Internal Server Error","text/plain","err"); return; }
    send_simple(c,"200 OK","text/html; charset=utf-8",page);
}

/* ---------------------------------------------------------------- upload */

/* Stream `len` bytes from the socket into the inbox. `pre`/`prelen` is whatever of the
 * body already arrived in the header buffer. */
static int recv_to_file(int c, const char *name, long len,
                        const char *pre, int prelen)
{
    if(len <= 0){ set_str(g_err,sizeof g_err,"Empty file"); return -1; }
    if(!enough_free()){ set_str(g_err,sizeof g_err,"Card is nearly full"); return -1; }

    char dir[256], part[512], fin[512];
    snprintf(dir ,sizeof dir ,"%s/%s",RX_MUSIC_DIR,RX_INBOX);
    mkdir(RX_MUSIC_DIR,0755);                 /* ignore EEXIST */
    mkdir(dir,0755);
    /* The dot prefix is why an interrupted upload is invisible rather than broken: the
     * scanner skips dot-files, so a .part is never indexed and never half-played. */
    snprintf(part,sizeof part,"%s/.%s.part",dir,name);
    snprintf(fin ,sizeof fin ,"%s/%s",dir,name);

    int fd = open(part, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if(fd < 0){ set_str(g_err,sizeof g_err,"Could not write to the card"); return -1; }

    long got = 0;
    atomic_store(&g_busy,1);
    atomic_store(&g_bytes,0);

    if(prelen > 0){
        int w = (int)((long)prelen < len ? prelen : len);
        if(write(fd,pre,(size_t)w) != w){
            close(fd); unlink(part); atomic_store(&g_busy,0);
            set_str(g_err,sizeof g_err,"Write failed"); return -1;
        }
        got = w;
        atomic_store(&g_bytes,got);
    }

    char *buf = malloc(RX_CHUNK);
    if(!buf){ close(fd); unlink(part); atomic_store(&g_busy,0);
              set_str(g_err,sizeof g_err,"Out of memory"); return -1; }

    int64_t last = now_ms();
    while(got < len && atomic_load(&g_run)){
        long want = len - got; if(want > RX_CHUNK) want = RX_CHUNK;
        int r = (int)recv(c, buf, (size_t)want, 0);
        if(r > 0){
            if(write(fd,buf,(size_t)r) != r){
                free(buf); close(fd); unlink(part); atomic_store(&g_busy,0);
                set_str(g_err,sizeof g_err,"Write failed - card full or removed?");
                return -1;
            }
            got += r;
            atomic_store(&g_bytes,got);
            last = now_ms();
            continue;
        }
        if(r == 0) break;                                   /* client hung up */
        if(errno == EINTR) continue;
        if(errno == EAGAIN || errno == EWOULDBLOCK){
            /* An idle timeout rather than a total one: a big album over slow Wi-Fi is
             * legitimate and must not be cut off just for taking a while. */
            if(now_ms() - last > RX_IDLE_MS) break;
            continue;
        }
        break;
    }
    free(buf);

    /* fsync before the rename: the rename is what makes the file visible, so it must
     * not become visible before its contents are durable. */
    int complete = (got == len);
    if(complete && fsync(fd) != 0) complete = 0;
    close(fd);
    atomic_store(&g_busy,0);
    atomic_store(&g_bytes,0);

    if(!complete){
        unlink(part);
        set_str(g_err,sizeof g_err,"Transfer interrupted");
        return -1;
    }
    if(rename(part,fin) != 0){
        unlink(part);
        set_str(g_err,sizeof g_err,"Could not finish the file");
        return -1;
    }
    char folded[RX_MAX_NAME];
    snprintf(folded,sizeof folded,"%s",name);
    txt_fold_ascii(folded);                  /* the DISPLAY copy; the file keeps its bytes */
    set_str(g_last,sizeof g_last,folded);
    set_str(g_err,sizeof g_err,"");
    atomic_fetch_add(&g_done,1);
    return 0;
}

/* ---------------------------------------------------------------- one request */

static void handle(int c){
    struct timeval tv={5,0};
    setsockopt(c,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
    setsockopt(c,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof tv);

    /* Read headers only. The body is NOT drained into this buffer - that is the whole
     * point - so stop at the blank line and hand the socket to the streamer. */
    char buf[4096]; int n=0, hlen=-1;
    int64_t deadline = now_ms() + 10000;
    while(atomic_load(&g_run) && now_ms() < deadline){
        if(n >= (int)sizeof buf - 1) break;
        int r = (int)recv(c, buf+n, sizeof buf - 1 - n, 0);
        if(r <= 0){ if(r<0 && errno==EINTR) continue; break; }
        n += r; buf[n]=0;
        char *he = strstr(buf,"\r\n\r\n");
        if(he){ hlen = (int)(he - buf) + 4; break; }
    }
    if(hlen < 0){ send_simple(c,"400 Bad Request","text/plain","bad request"); return; }

    char method[8]={0}, path[512]={0};
    { char sv = buf[hlen]; buf[hlen]=0;
      int ok = sscanf(buf,"%7s %511s",method,path) == 2;
      buf[hlen]=sv;
      if(!ok){ send_simple(c,"400 Bad Request","text/plain","bad request"); return; } }

    char base[24]; snprintf(base,sizeof base,"/%s",g_token);
    size_t bl = strlen(base);
    if(strncmp(path,base,bl) != 0){ send_simple(c,"404 Not Found","text/plain","not found"); return; }
    const char *rest = path + bl;

    if(!strcmp(method,"GET") && (!strcmp(rest,"") || !strcmp(rest,"/"))){ send_page(c); return; }

    if(!strcmp(method,"PUT") && !strncmp(rest,"/up/",4)){
        char raw[512]={0}, name[RX_MAX_NAME];
        /* percent-decode the name the browser encoded */
        const char *s = rest+4; int o=0;
        for(; *s && o<(int)sizeof raw-1; s++){
            if(*s=='%' && s[1] && s[2]){
                int hi = s[1], lo = s[2];
                hi = hi>='0'&&hi<='9'?hi-'0':(hi|32)>='a'&&(hi|32)<='f'?(hi|32)-'a'+10:-1;
                lo = lo>='0'&&lo<='9'?lo-'0':(lo|32)>='a'&&(lo|32)<='f'?(lo|32)-'a'+10:-1;
                if(hi>=0 && lo>=0){ raw[o++] = (char)(hi*16+lo); s+=2; continue; }
            }
            raw[o++] = *s;
        }
        raw[o]=0;
        if(safe_name(raw,name,sizeof name) != 0){
            send_simple(c,"400 Bad Request","text/plain","not a music file");
            return;
        }
        long len = -1;
        { char sv = buf[hlen]; buf[hlen]=0;
          char *cl = strcasestr(buf,"\r\ncontent-length:");
          if(cl) len = strtol(cl+17,NULL,10);
          buf[hlen]=sv; }
        if(len <= 0){ send_simple(c,"411 Length Required","text/plain","no length"); return; }

        int prelen = n - hlen;                       /* body bytes already in hand */
        if(recv_to_file(c,name,len,buf+hlen,prelen) == 0) send_simple(c,"200 OK","text/plain","ok");
        else {
            pthread_mutex_lock(&g_str_mu);
            char e[128]; snprintf(e,sizeof e,"%s",g_err[0]?g_err:"failed");
            pthread_mutex_unlock(&g_str_mu);
            send_simple(c,"500 Internal Server Error","text/plain",e);
        }
        return;
    }
    send_simple(c,"404 Not Found","text/plain","not found");
}

static void *worker(void *arg){
    (void)arg;
    while(atomic_load(&g_run)){
        struct pollfd pf = { g_fd, POLLIN, 0 };
        /* 150ms, not 500: this bounds how long receive_stop() can block the LVGL thread
         * waiting to join. The screen is only open while someone is looking at it, so
         * ~7 idle wakeups a second costs nothing that matters. */
        if(poll(&pf,1,150) <= 0) continue;
        int c = accept(g_fd,NULL,NULL);
        if(c < 0) continue;
        atomic_store(&g_client,c);
        handle(c);
        atomic_store(&g_client,-1);
        close(c);
    }
    return NULL;
}

/* ---------------------------------------------------------------- public */

int receive_start(void){
    if(g_th_live) return 0;
    char ip[24];
    if(wlan_ip(ip,sizeof ip) != 0) return -1;                 /* needs Wi-Fi */
    if(gen_token(g_token,sizeof g_token) != 0) return -1;     /* require real randomness */

    int fd = socket(AF_INET,SOCK_STREAM,0); if(fd<0) return -1;
    int one=1; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    struct sockaddr_in a; memset(&a,0,sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr(ip);       /* wlan0 only - never INADDR_ANY */
    if(a.sin_addr.s_addr == INADDR_NONE){ close(fd); return -1; }

    int port = RX_PORT;
    a.sin_port = htons(port);
    if(bind(fd,(struct sockaddr*)&a,sizeof a) != 0){
        port = RX_PORT_ALT;                  /* 80 refused -> fall back rather than fail */
        a.sin_port = htons(port);
        if(bind(fd,(struct sockaddr*)&a,sizeof a) != 0){ close(fd); return -1; }
    }
    if(listen(fd,4)!=0){ close(fd); return -1; }
    fcntl(fd,F_SETFL,O_NONBLOCK);

    /* Port omitted from the printed URL when it is 80 - browsers assume it, and those
     * three characters matter on a screen this size. */
    if(port == 80) snprintf(g_url,sizeof g_url,"http://%s/%s/",ip,g_token);
    else           snprintf(g_url,sizeof g_url,"http://%s:%d/%s/",ip,port,g_token);
    g_fd = fd;
    atomic_store(&g_run,1);
    atomic_store(&g_done,0); atomic_store(&g_busy,0); atomic_store(&g_bytes,0);
    set_str(g_last,sizeof g_last,""); set_str(g_err,sizeof g_err,"");
    if(pthread_create(&g_th,NULL,worker,NULL)!=0){
        close(fd); g_fd=-1; atomic_store(&g_run,0); return -1;
    }
    g_th_live = 1;
    return 0;
}

void receive_stop(void){
    if(!g_th_live) return;
    atomic_store(&g_run,0);
    /* Kick any in-flight transfer awake BEFORE joining, so a blocked recv() returns now
     * rather than at its 5s timeout. Joining (rather than detaching) is deliberate: it
     * is what proves no socket outlives the screen. */
    int c = atomic_load(&g_client);
    if(c >= 0) shutdown(c, SHUT_RDWR);
    pthread_join(g_th,NULL);
    if(g_fd>=0){ close(g_fd); g_fd=-1; }
    g_th_live = 0;
    g_url[0] = 0;
}

const char *receive_url(void){ return g_th_live ? g_url : ""; }
int  receive_files_done(void){ return atomic_load(&g_done); }
int  receive_active(void){ return atomic_load(&g_busy); }
long receive_active_bytes(void){ return atomic_load(&g_bytes); }
int  receive_got_files(void){ return atomic_load(&g_done) > 0; }

const char *receive_last_name(void){
    static char s[RX_MAX_NAME];
    pthread_mutex_lock(&g_str_mu); snprintf(s,sizeof s,"%s",g_last); pthread_mutex_unlock(&g_str_mu);
    return s;
}
const char *receive_last_error(void){
    static char s[128];
    pthread_mutex_lock(&g_str_mu); snprintf(s,sizeof s,"%s",g_err); pthread_mutex_unlock(&g_str_mu);
    return s;
}
