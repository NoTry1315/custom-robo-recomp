/* cr_agent: what tools/hw_loop.py talks to on the 3DS during unattended hardware runs.
 * One TCP port (5001), one client at a time, one line per command, one line per reply:
 *   PING                 -> OK cr_agent <ver> up <s> wifi <0-3> free <KB> batt <0-5> pct <%|-1> charging <0|1>
 *                           adapter <0|1>   (battery: the 3DS powered off at 2026-10-27 01:57 with an empty battery)
 *   PUT <size> <path>    -> READY, then <size> raw bytes from the client -> OK <size>   (written to <path>.part,
 *                           then put in place)
 *   GET <path>           -> OK <size>, then <size> raw bytes
 *   MV <src> <dst>       -> OK (dst must not exist)
 *   LS <dir>             -> OK, then one "<size> <name>" line per entry (size -1 = directory), then "."
 *   RUN <path> [args..]  -> OK, then the agent exits and Luma's 3dsx loader starts <path> (hb:ldr SetTarget /
 *                           SetArgv, as the Homebrew Launcher does). The game build returns here by itself when
 *                           its argv is "--return <agent path>" (runtime/main.c recomp_hw_return).
 *   QUIT                 -> OK, then the agent exits to the Homebrew Launcher (/boot.3dsx)
 *   anything else        -> ERR <why>
 * Paths are SD paths starting with "/", no spaces. Nothing is ever deleted, and nothing under /custom-robo/save
 * (the user's memory card) is written or renamed. START on the console also exits to the Homebrew Launcher.
 * Every command is appended to sdmc:/cr_agent.log, so the last thing done before a crash is on the SD.
 * The agent replaces ftpd + the netloader + Rosalina InputRedirection in the run loop: with Rosalina's network
 * features on, the 3DS lost Wi-Fi and froze 4 times in an hour on 2026-10-26 (README "Unattended hardware runs"). */
#include <3ds.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define VERSION "2"
#define PORT 5001
#define SOC_SIZE 0x100000
#define IDLE_MS 20000      /* a client that sends nothing for this long is dropped */
#define ARGBUF 0x400       /* hb:ldr argv buffer: u32 argc, then NUL-separated strings */

static u64 t_start;
static int mcu_ok;   /* mcu::HWC (battery percent) is not in every service list: PING says pct -1 then */
static char status[16][64];
static int status_n;

static void say(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    u64 s = (osGetTime() - t_start) / 1000;
    FILE *f = fopen("sdmc:/cr_agent.log", "a");
    if (f) { fprintf(f, "%6llu %s\n", s, line); fclose(f); }
    if (status_n == 16) { memmove(status[0], status[1], sizeof status[0] * 15); status_n = 15; }
    snprintf(status[status_n++], sizeof status[0], "%5llu %s", s, line);
    printf("\x1b[6;0H");
    for (int i = 0; i < 16; i++) printf("\x1b[2K%s\n", i < status_n ? status[i] : "");
}

/* hb:ldr, Luma3DS's 3dsx loader: the next start of the homebrew title loads this path (default /boot.3dsx) */
static Result hbldr_set(const char *path, const void *argbuf) {
    static char pbuf[ARGBUF];
    Handle h;
    Result rc = svcConnectToPort(&h, "hb:ldr");
    if (R_FAILED(rc)) return rc;
    memset(pbuf, 0, sizeof pbuf);
    strncpy(pbuf, path, sizeof pbuf - 1);
    u32 *cmd = getThreadCommandBuffer();
    cmd[0] = IPC_MakeHeader(2, 0, 2);
    cmd[1] = IPC_Desc_StaticBuffer(strlen(pbuf) + 1, 0);
    cmd[2] = (u32)pbuf;
    rc = svcSendSyncRequest(h);
    if (R_SUCCEEDED(rc)) rc = cmd[1];
    if (R_SUCCEEDED(rc)) {
        cmd = getThreadCommandBuffer();
        cmd[0] = IPC_MakeHeader(3, 0, 2);
        cmd[1] = IPC_Desc_StaticBuffer(ARGBUF, 1);
        cmd[2] = (u32)argbuf;
        rc = svcSendSyncRequest(h);
        if (R_SUCCEEDED(rc)) rc = cmd[1];
    }
    svcCloseHandle(h);
    return rc;
}

static int wait_in(int fd, int ms) {
    struct pollfd p = { .fd = fd, .events = POLLIN };
    return poll(&p, 1, ms) > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR));
}

static int send_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        ssize_t k = send(fd, p, n, 0);
        if (k <= 0) return -1;
        p += k; n -= k;
    }
    return 0;
}

static int reply(int fd, const char *fmt, ...) {
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof line - 2) n = sizeof line - 2;
    line[n++] = '\n';
    return send_all(fd, line, n);
}

/* one command line, without the newline; -1 on close, error or IDLE_MS of silence */
static int read_line(int fd, char *buf, int max) {
    int n = 0;
    while (n < max - 1) {
        if (!wait_in(fd, IDLE_MS)) return -1;
        char c;
        if (recv(fd, &c, 1, 0) != 1) return -1;
        if (c == '\n') break;
        if (c != '\r') buf[n++] = c;
    }
    buf[n] = 0;
    return n;
}

static int path_ok(const char *p) {
    return p && p[0] == '/' && !strstr(p, "..") && strlen(p) < 200;
}
static int path_writable(const char *p) {
    return path_ok(p) && strncasecmp(p, "/custom-robo/save", 17) != 0;
}
static void sd(char *out, const char *p) { snprintf(out, 256, "sdmc:%s", p); }

static u8 xfer[0x10000];

static void do_put(int fd, long size, const char *path) {
    char full[256], part[264];
    if (!path_writable(path) || size < 0) { reply(fd, "ERR bad path or size"); return; }
    sd(full, path);
    snprintf(part, sizeof part, "%s.part", full);
    FILE *f = fopen(part, "wb");
    if (!f) { reply(fd, "ERR open %d", errno); return; }
    reply(fd, "READY");
    long left = size;
    while (left > 0) {
        if (!wait_in(fd, IDLE_MS)) break;
        ssize_t k = recv(fd, xfer, left < (long)sizeof xfer ? left : (long)sizeof xfer, 0);
        if (k <= 0) break;
        if (fwrite(xfer, 1, k, f) != (size_t)k) break;
        left -= k;
    }
    fclose(f);
    if (left) { say("PUT %s short by %ld", path, left); reply(fd, "ERR short by %ld", left); return; }
    struct stat st;
    if (stat(full, &st) == 0) remove(full);   /* replaced, as ftpd's STOR does */
    if (rename(part, full) != 0) { reply(fd, "ERR rename %d", errno); return; }
    say("PUT %s %ld", path, size);
    reply(fd, "OK %ld", size);
}

static void do_get(int fd, const char *path) {
    char full[256];
    struct stat st;
    if (!path_ok(path)) { reply(fd, "ERR bad path"); return; }
    sd(full, path);
    FILE *f = fopen(full, "rb");
    if (!f || fstat(fileno(f), &st) != 0) { if (f) fclose(f); reply(fd, "ERR open %d", errno); return; }
    reply(fd, "OK %ld", (long)st.st_size);
    long left = st.st_size;
    while (left > 0) {
        size_t k = fread(xfer, 1, left < (long)sizeof xfer ? left : (long)sizeof xfer, f);
        if (k == 0 || send_all(fd, xfer, k) != 0) break;
        left -= k;
    }
    fclose(f);
    say("GET %s %ld%s", path, (long)st.st_size, left ? " (cut)" : "");
}

static void do_mv(int fd, const char *a, const char *b) {
    char fa[256], fb[256];
    struct stat st;
    if (!path_writable(a) || !path_writable(b)) { reply(fd, "ERR bad path"); return; }
    sd(fa, a); sd(fb, b);
    if (stat(fb, &st) == 0) { reply(fd, "ERR %s exists", b); return; }
    if (rename(fa, fb) != 0) { reply(fd, "ERR rename %d", errno); return; }
    say("MV %s %s", a, b);
    reply(fd, "OK");
}

static void do_ls(int fd, const char *path) {
    char full[256], ent[300];
    if (!path_ok(path)) { reply(fd, "ERR bad path"); return; }
    sd(full, path);
    DIR *d = opendir(full);
    if (!d) { reply(fd, "ERR opendir %d", errno); return; }
    reply(fd, "OK");
    struct dirent *e;
    while ((e = readdir(d))) {
        struct stat st;
        snprintf(ent, sizeof ent, "%s/%s", full, e->d_name);
        long size = stat(ent, &st) == 0 && !S_ISDIR(st.st_mode) ? (long)st.st_size : -1;
        reply(fd, "%ld %s", size, e->d_name);
    }
    closedir(d);
    reply(fd, ".");
}

/* RUN: argv buffer for hb:ldr; argv[0] is the sdmc: path, as the Homebrew Launcher passes it */
static u32 argbuf[ARGBUF / 4];
static int do_run(int fd, char *rest) {
    char *path = strtok(rest, " ");
    char full[256];
    struct stat st;
    if (!path_ok(path)) { reply(fd, "ERR bad path"); return 0; }
    sd(full, path);
    if (stat(full, &st) != 0) { reply(fd, "ERR %s missing", path); return 0; }
    memset(argbuf, 0, sizeof argbuf);
    char *p = (char *)&argbuf[1], *end = (char *)argbuf + ARGBUF;
    u32 argc = 0;
    for (char *a = full; a; a = strtok(NULL, " ")) {
        size_t n = strlen(a) + 1;
        if (p + n > end) { reply(fd, "ERR args too long"); return 0; }
        memcpy(p, a, n); p += n; argc++;
    }
    argbuf[0] = argc;
    Result rc = hbldr_set(path, argbuf);
    if (R_FAILED(rc)) { reply(fd, "ERR hb:ldr %08lx", rc); return 0; }
    say("RUN %s (%lu args)", path, argc);
    reply(fd, "OK");
    return 1;
}

/* one client; returns 1 when the agent should exit (RUN / QUIT) */
static int session(int fd) {
    char line[512];
    while (read_line(fd, line, sizeof line) >= 0) {
        char *cmd = strtok(line, " "), *rest = strtok(NULL, "");
        if (!cmd) continue;
        if (!strcmp(cmd, "PING")) {
            struct statvfs v;
            unsigned long long kb = statvfs("sdmc:/", &v) == 0 ? (unsigned long long)v.f_bavail * v.f_frsize / 1024 : 0;
            u8 level = 0, charging = 0, pct = 0;
            bool adapter = false;
            PTMU_GetBatteryLevel(&level);
            PTMU_GetBatteryChargeState(&charging);
            PTMU_GetAdapterState(&adapter);
            int have_pct = mcu_ok && R_SUCCEEDED(MCUHWC_GetBatteryLevel(&pct));
            reply(fd, "OK cr_agent %s up %llu wifi %u free %llu batt %u pct %d charging %u adapter %u", VERSION,
                  (osGetTime() - t_start) / 1000, (unsigned)osGetWifiStrength(), kb, level, have_pct ? pct : -1,
                  charging, adapter);
        } else if (!strcmp(cmd, "PUT") && rest) {
            char *sz = strtok(rest, " "), *path = strtok(NULL, " ");
            do_put(fd, sz ? atol(sz) : -1, path);
        } else if (!strcmp(cmd, "GET") && rest) do_get(fd, strtok(rest, " "));
        else if (!strcmp(cmd, "MV") && rest) {
            char *a = strtok(rest, " "), *b = strtok(NULL, " ");
            do_mv(fd, a, b);
        } else if (!strcmp(cmd, "LS")) do_ls(fd, rest ? strtok(rest, " ") : "/");
        else if (!strcmp(cmd, "RUN") && rest) { if (do_run(fd, rest)) return 1; }
        else if (!strcmp(cmd, "QUIT")) { say("QUIT"); reply(fd, "OK"); return 1; }
        else reply(fd, "ERR unknown command");
    }
    return 0;
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    t_start = osGetTime();
    ptmuInit();
    mcu_ok = R_SUCCEEDED(mcuHwcInit());
    printf("cr_agent " VERSION " (custom-robo-recomp tools/hw/agent)\nSTART: back to the Homebrew Launcher\n");
    u32 *socbuf = memalign(0x1000, SOC_SIZE);
    if (!socbuf || R_FAILED(socInit(socbuf, SOC_SIZE))) {
        printf("socInit failed\n");
        while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_START) break; gspWaitForVBlank(); }
        gfxExit();
        return 0;
    }
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = INADDR_ANY };
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 2) != 0) {
        printf("listen on %d failed: %d\n", PORT, errno);
    }
    struct in_addr ip = { .s_addr = gethostid() };
    printf("\x1b[3;0Hlistening on %s:%d\n", inet_ntoa(ip), PORT);
    say("start, ip %s", inet_ntoa(ip));
    int quit = 0;
    while (!quit && aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) { say("START pressed"); break; }
        if (!wait_in(ls, 100)) continue;
        struct sockaddr_in peer;
        socklen_t pl = sizeof peer;
        int fd = accept(ls, (struct sockaddr *)&peer, &pl);
        if (fd < 0) continue;
        quit = session(fd);
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    close(ls);
    socExit();
    free(socbuf);
    if (mcu_ok) mcuHwcExit();
    ptmuExit();
    gfxExit();
    return 0;   /* libctru's aptExit chainloads to the homebrew title again; the loader starts the hb:ldr target */
}
