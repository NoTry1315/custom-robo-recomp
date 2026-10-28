/* DVD module at the dvdfs level: the FST is loaded to the 0x80000038 pointer,
 * paths resolve against it as dvdfs.c does, and reads come from the disc's
 * files under sdmc:/custom-robo/files/ (never from the repository).
 * Reads run on a reader thread; an async read's callback runs at the next
 * wait point (VIWaitForRetrace, DVDGetCommandBlockStatus, DVDGetDriveStatus)
 * once the read is done, the way an interrupt would land between frames.
 * DVDCommandBlock and DVDFileInfo follow the SDK layout:
 *   block: next 0, prev 4, command 8, state 0xc, offset 0x10, length 0x14,
 *          addr 0x18, currTransferSize 0x1c, transferredSize 0x20, id 0x24,
 *          callback 0x28, userData 0x2c
 *   fileinfo: block 0..0x30, startAddr 0x30, length 0x34, callback 0x38 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include "ppc.h"

#define LOG() RECOMP_LOG()
#define DISC_DIR "sdmc:/custom-robo/"
#define DVD_STATE_END 0
#define DVD_STATE_BUSY 1
#define DVD_STATE_CANCELED 10
#define MAXPENDING 64

static u32 fst, nentries, strings, curdir;
static char **paths;    /* host path per entry (files only) */
u32 recomp_dvd_slow_n, recomp_dvd_late_n;   /* "# DVD slow" / "# DVD late" lines so far (bottom-screen stats) */
static void reader_start(void);

static int  e_isdir(u32 e) { return rd8(fst + 12 * e) != 0; }
static u32  e_name(u32 e) { return strings + (rd32(fst + 12 * e) & 0xffffff); }
static u32  e_w1(u32 e) { return rd32(fst + 12 * e + 4); }   /* file: disc offset, dir: parent */
static u32  e_w2(u32 e) { return rd32(fst + 12 * e + 8); }   /* file: length, dir: next entry */

static void guest_str(u32 a, char *out, int n) {
    int i = 0;
    for (; i < n - 1 && rd8(a + i); i++) out[i] = rd8(a + i);
    out[i] = 0;
}

/* host paths for every file: walk the tree with an end-of-dir stack */
static void build_paths(void) {
    u32 end[32]; char *pre[32]; int sp = 0;
    paths = calloc(nentries, sizeof *paths);
    end[0] = nentries; pre[0] = strdup(DISC_DIR "files/");
    for (u32 e = 1; e < nentries; e++) {
        while (sp && e >= end[sp]) sp--;
        char name[256], *p;
        guest_str(e_name(e), name, sizeof name);
        p = malloc(strlen(pre[sp]) + strlen(name) + 2);
        sprintf(p, "%s%s%s", pre[sp], name, e_isdir(e) ? "/" : "");
        if (e_isdir(e) && sp < 31) { sp++; end[sp] = e_w2(e); pre[sp] = p; }
        else paths[e] = p;
    }
}

void sdk_DVDInit(void) {
    LOG();
    FILE *f = fopen(DISC_DIR "fst.bin", "rb");
    fst = rd32(0x80000038);
    if (!f) { recomp_log("# DVD: no " DISC_DIR "fst.bin\n"); return; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if ((u32)n > rd32(0x8000003c)) recomp_log("# DVD: fst.bin %ld bytes > FST max\n", n);
    if ((recomp_mark_ram(fst, n), fread(ram + (fst & RAM_MASK), 1, n, f)) != (size_t)n) recomp_log("# DVD: short fst.bin read\n");
    fclose(f);
    nentries = e_w2(0);
    strings = fst + 12 * nentries;
    curdir = 0;
    build_paths();
    recomp_log("# DVD: FST %08lx, %lu entries\n", (unsigned long)fst, (unsigned long)nentries);
    reader_start();
}

/* dvdfs.c DVDConvertPathToEntrynum: '/' absolute, "." and "..", case-insensitive
 * names (the SDK compares with tolower), -1 if not found */
static s32 path_to_entry(u32 path) {
    char buf[256], *s = buf;
    guest_str(path, buf, sizeof buf);
    u32 dir = curdir;
    for (;;) {
        if (*s == 0) return dir;
        if (*s == '/') { dir = 0; s++; continue; }
        char *end = s; while (*end && *end != '/') end++;
        int len = end - s, isdir_req = *end == '/';
        if (len == 1 && s[0] == '.') { s = end + isdir_req; continue; }
        if (len == 2 && s[0] == '.' && s[1] == '.') { dir = e_w1(dir); s = end + isdir_req; continue; }
        u32 e = dir + 1, found = 0;
        while (e < e_w2(dir)) {
            char nm[256];
            guest_str(e_name(e), nm, sizeof nm);
            if ((int)strlen(nm) == len && !strncasecmp(nm, s, len)) { found = 1; break; }
            e = e_isdir(e) ? e_w2(e) : e + 1;
        }
        if (!found) return -1;
        if (!*end) return e;
        if (!e_isdir(e)) return -1;
        dir = e; s = end + 1;
    }
}

void sdk_DVDConvertPathToEntrynum(void) { LOG(); R[3] = path_to_entry(R[3]); }

static int fast_open(s32 e, u32 fi) {
    wr32(fi + 0x30, e_w1(e));
    wr32(fi + 0x34, e_w2(e));
    wr32(fi + 0x38, 0);
    wr32(fi + 0x0c, DVD_STATE_END);
    return 1;
}

void sdk_DVDFastOpen(void) { LOG(); R[3] = fast_open((s32)R[3], R[4]); }

void sdk_DVDOpen(void) {
    LOG();
    s32 e = path_to_entry(R[3]);
    if (e < 0) {
        char nm[256]; guest_str(R[3], nm, sizeof nm);
        recomp_log("# DVDOpen: %s not found\n", nm);
    }
    R[3] = fast_open(e, R[4]);
}

void sdk_DVDClose(void) { LOG(); R[3] = 1; }

void sdk_DVDChangeDir(void) {
    LOG();
    s32 e = path_to_entry(R[3]);
    if (e < 0 || !e_isdir(e)) { R[3] = 0; return; }
    curdir = e; R[3] = 1;
}

/* DVDDir: entryNum 0, location 4, next 8; DVDDirEntry: entryNum 0, isDir 4, name 8 */
void sdk_DVDOpenDir(void) {
    LOG();
    s32 e = path_to_entry(R[3]);
    if (e < 0 || !e_isdir(e)) { R[3] = 0; return; }
    wr32(R[4], e); wr32(R[4] + 4, e + 1); wr32(R[4] + 8, e_w2(e));
    R[3] = 1;
}

void sdk_DVDReadDir(void) {
    LOG();
    u32 d = R[3], ent = R[4], loc = rd32(d + 4);
    if (loc <= rd32(d) || loc >= rd32(d + 8)) { R[3] = 0; return; }
    wr32(ent, loc); wr32(ent + 4, e_isdir(loc)); wr32(ent + 8, e_name(loc));
    wr32(d + 4, e_isdir(loc) ? e_w2(loc) : loc + 1);
    R[3] = 1;
}

void sdk_DVDCloseDir(void) { LOG(); R[3] = 1; }

/* Reads run on a reader thread. Hw runs 15-21: a single SD access sometimes
 * blocked for 2-4 s (0x20 bytes of an .hps stream open for hundreds of
 * frames, a failed fopen of a flag file), and the whole game froze with it.
 * The game thread still waits for each read at the next wait point, but only
 * as long as a GameCube drive would need (read_budget); a read still running
 * then finishes in the background and its callback runs at a later wait
 * point, as with a slow disc. Citra's SD model (0.6 ms + 183 ns/byte) stays
 * inside the budget, so replays deliver every callback where they did before.
 * sdmc:/cr_dvdasync_off.txt: the game waits for each read at issue, no limit
 * (the old timing). */
static struct req {
    u32 fi, cb, addr, len, off, e, r_issue, r_late;
    s32 result;
    int bad, late;               /* bad: 1 rejected at issue, 2 open failed */
    u64 waited;                  /* game thread time spent waiting for it */
    u64 t_start, t_open, t_seek, t_read;   /* reader: ticks */
    u32 logw_busy, logw_n;       /* log write already running at start (ticks), log writes finished during */
    int logw_end;                /* log write running at the end */
    volatile u32 started, done;
} q[MAXPENDING];
static volatile u32 q_head, q_work;    /* free-running: requests issued, finished by the reader */
static u32 q_tail;                     /* requests delivered */
static LightSemaphore q_sem;
static LightEvent q_ev;                /* the reader finished one */
static int reader_ok, async_off;

/* 50 ms + 2.5 MB/s, about a GameCube drive's seek and transfer */
static u64 read_budget(u32 len) { return SYSCLOCK_ARM11 / 20 + (u64)len * (SYSCLOCK_ARM11 / 2500000); }

/* host read of [off, off+len) of the request's file, straight into guest RAM */
static void do_read(struct req *r) {
    u64 t0 = recomp_ticks();
    u32 ls = recomp_logw_since, ln = recomp_logw_n;
    r->logw_busy = ls ? (u32)t0 - ls : 0;
    r->t_start = t0;
    __sync_synchronize();
    r->started = 1;
    /* hw run 15: fopen per read cost ~54 ms even for 0x80 bytes; keep the last few files open (LRU) */
    enum { NHANDLES = 8 };
    static struct { u32 e; FILE *f; u32 used; } h[NHANDLES];
    static u32 use_clock;
    int slot = -1, victim = 0;
    for (int i = 0; i < NHANDLES; i++) {
        if (h[i].f && h[i].e == r->e) { slot = i; break; }
        if (!h[i].f || h[i].used < h[victim].used) victim = i;
    }
    if (slot < 0) {
        slot = victim;
        if (h[slot].f) fclose(h[slot].f);
        h[slot].f = fopen(paths[r->e], "rb");
        h[slot].e = r->e;
        if (!h[slot].f) { r->bad = 2; r->result = -1; return; }
        setvbuf(h[slot].f, NULL, _IONBF, 0);   /* reads go straight to the guest buffer */
    }
    h[slot].used = ++use_clock;
    FILE *f = h[slot].f;
    u64 t1 = recomp_ticks();
    fseek(f, r->off, SEEK_SET);
    u64 t2 = recomp_ticks();
    size_t got = fread(ram + (r->addr & RAM_MASK), 1, r->len, f);
    u64 t3 = recomp_ticks();
    if (got < r->len) memset(ram + ((r->addr + got) & RAM_MASK), 0, r->len - got);   /* padding past EOF */
    r->t_open = t1 - t0; r->t_seek = t2 - t1; r->t_read = t3 - t2;
    r->logw_n = recomp_logw_n - ln;
    r->logw_end = recomp_logw_since != 0;
    r->result = (s32)r->len;
}

static void reader(void *arg) {
    (void)arg;
    for (;;) {
        LightSemaphore_Acquire(&q_sem, 1);
        struct req *r = &q[q_work % MAXPENDING];
        if (!r->bad) do_read(r);
        __sync_synchronize();
        r->done = 1;
        q_work++;
        LightEvent_Signal(&q_ev);
    }
}

static int file_exists(const char *path) { FILE *f = fopen(path, "rb"); if (f) fclose(f); return f != NULL; }

static void reader_start(void) {
    async_off = file_exists("sdmc:/cr_dvdasync_off.txt");
    LightSemaphore_Init(&q_sem, 0, MAXPENDING);
    LightEvent_Init(&q_ev, RESET_ONESHOT);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    reader_ok = recomp_aux_thread(reader, 0x4000, prio - 1) != NULL;
    recomp_log("# DVD: %s\n", !reader_ok ? "no reader thread, reads on the game thread at issue"
               : async_off ? "reader thread, game waits for each read at issue (cr_dvdasync_off.txt)"
               : "reader thread, game waits up to 50 ms + 2.5 MB/s per read, then goes on");
}

/* 1 once r is done; until then waits while its read has run less than its budget (force: no limit) */
static int wait_read(struct req *r, int force) {
    if (!r->done && (force || !r->late)) {
        u64 t0 = recomp_ticks();
        while (!r->done) {
            if (!force && r->started) {
                __sync_synchronize();
                if (recomp_ticks() - r->t_start > read_budget(r->len)) { r->late = 1; r->r_late = recomp_retrace_count(); break; }
            }
            LightEvent_WaitTimeout(&q_ev, 1000000);
        }
        u64 w = recomp_ticks() - t0;
        recomp_prof[PROF_DVD] += w;
        r->waited += w;
    }
    __sync_synchronize();
    return r->done;
}

/* the entry behind a DVDFileInfo's startAddr */
static u32 entry_at(u32 start) {
    static u32 last;
    if (last < nentries && !e_isdir(last) && e_w1(last) == start) return last;
    for (u32 e = 1; e < nentries; e++)
        if (!e_isdir(e) && e_w1(e) == start) return last = e;
    return ~0u;
}

/* queues a read of [off, off+len) of the file behind a DVDFileInfo */
static struct req *issue(u32 fi, u32 addr, u32 len, u32 off, u32 cb) {
    struct req *r = &q[q_head % MAXPENDING];
    u32 e = entry_at(rd32(fi + 0x30)), rt = recomp_retrace_count();
    *r = (struct req){ .fi = fi, .cb = cb, .addr = addr, .len = len, .off = off, .e = e, .r_issue = rt };
    if (e >= nentries || !paths[e] || off + len > rd32(fi + 0x34) + 0x20) {   /* SDK allows the 32-byte round-up */
        recomp_log("# DVD: bad read entry %lu off %lx len %lx\n", (unsigned long)e, (unsigned long)off, (unsigned long)len);
        r->bad = 1; r->result = -1;
    } else {
        static u8 seen[4096];   /* first read of each file is logged by name and frame */
        if (e < 4096 && !seen[e]) { seen[e] = 1; recomp_log("# DVD read %s (frame %lu)\n", paths[e] + sizeof DISC_DIR + 5, (unsigned long)rt); }
        static u32 frame, nreads;
        if (rt != frame) { frame = rt; nreads = 0; }
        nreads++;
        if ((nreads & (nreads - 1)) == 0 && nreads >= 8)
            recomp_log("# DVD many r%lu: %lu reads (%s off %lx len %lx)\n", (unsigned long)rt, (unsigned long)nreads,
                       paths[e] + sizeof DISC_DIR + 5, (unsigned long)off, (unsigned long)len);
        recomp_mark_ram(addr, len);
        wr32(fi + 0x10, off); wr32(fi + 0x14, len); wr32(fi + 0x18, addr);
        wr32(fi + 0x20, len);
    }
    if (!reader_ok) {
        if (!r->bad) do_read(r);
        r->done = 1;
        q_head++; q_work++;
        return r;
    }
    __sync_synchronize();
    q_head++;
    LightSemaphore_Release(&q_sem, 1);
    if (async_off) wait_read(r, 1);
    return r;
}

/* a finished read on the game thread: its log lines, state and callback */
static void deliver(struct req *r) {
    u32 fi = r->fi;
    if (r->bad == 2) recomp_log("# DVD: cannot open %s\n", paths[r->e]);
    if (!r->bad) {
        recomp_mark_ram(r->addr, r->len);   /* the reader wrote it after the issue-time mark */
        /* hw run 7: the frame that first reads sound/cubog39.hps took 4 s; time every read */
        static u32 frame, nreads;
        static u64 sum;
        if (r->r_issue != frame) { frame = r->r_issue; nreads = 0; sum = 0; }
        u64 dt = r->t_open + r->t_seek + r->t_read;
        sum += dt; nreads++;
        const char *nm = paths[r->e] + sizeof DISC_DIR + 5;
        double ms = 1000.0 / SYSCLOCK_ARM11;
        if (dt > 268111856 / 20 || (sum > 268111856 / 5 && nreads % 16 == 0))
            recomp_dvd_slow_n++, recomp_log("# DVD slow r%lu: %s off %lx len %lx %.0f ms (frame: %lu reads, %.0f ms); open %.0f, seek %.0f, read %.0f; "
                       "log writer: busy %.0f ms at start, %lu writes done during, %s at end\n", (unsigned long)r->r_issue,
                       nm, (unsigned long)r->off, (unsigned long)r->len, dt / 268111.856,
                       (unsigned long)nreads, sum / 268111.856, r->t_open * ms, r->t_seek * ms, r->t_read * ms,
                       r->logw_busy * ms, (unsigned long)r->logw_n, r->logw_end ? "busy" : "idle");
        if (r->late)
            recomp_dvd_late_n++, recomp_log("# DVD late r%lu: %s off %lx len %lx: game waited %.0f ms, went on at r%lu, callback at r%lu\n",
                       (unsigned long)r->r_issue, nm, (unsigned long)r->off, (unsigned long)r->len, r->waited * ms,
                       (unsigned long)r->r_late, (unsigned long)recomp_retrace_count());
    }
    if (rd32(fi + 0x0c) == DVD_STATE_BUSY) wr32(fi + 0x0c, r->result < 0 ? -1 : DVD_STATE_END);
    if (r->cb) recomp_call_guest2(r->cb, r->result, fi);
}

/* the "interrupt": finished reads call back in issue order; force waits for
 * the oldest without limit */
static void dvd_deliver(int force) {
    static int busy;    /* a callback that waits on DVD must not nest a guest call */
    if (busy) return;
    busy = 1;
    while (q_tail != q_head) {
        struct req *r = &q[q_tail % MAXPENDING];
        if (!wait_read(r, force)) break;   /* still reading: a later wait point delivers it */
        force = 0;
        deliver(r);   /* reads queued by its callback are delivered in this pass too */
        q_tail++;
    }
    busy = 0;
}

void recomp_dvd_service(void) { if (q_tail != q_head) dvd_deliver(0); }

/* memory dump (memdump.c): the reads issued and not yet delivered; a late one may still be writing */
void recomp_dvd_describe(void *file) {
    FILE *f = file;
    fprintf(f, "dvd reads in flight %lu (slow so far %lu, late %lu)\n", (unsigned long)(q_head - q_tail),
            (unsigned long)recomp_dvd_slow_n, (unsigned long)recomp_dvd_late_n);
    for (u32 i = q_tail; i != q_head; i++) {
        struct req *r = &q[i % MAXPENDING];
        fprintf(f, "  %s off %lx len %lx -> %08lx, issued r%lu, %s%s\n", r->e < nentries && paths[r->e] ? paths[r->e] + sizeof DISC_DIR + 5 : "?",
                (unsigned long)r->off, (unsigned long)r->len, (unsigned long)r->addr, (unsigned long)r->r_issue,
                r->done ? "read done" : r->started ? "reading" : "queued", r->late ? ", late" : "");
    }
}

static int queue_full(void) {
    if (q_head - q_tail == MAXPENDING) dvd_deliver(1);
    if (q_head - q_tail < MAXPENDING) return 0;
    recomp_log("# DVD: pending queue full\n");
    return 1;
}

void sdk_DVDReadAsyncPrio(void) {
    LOG();
    u32 fi = R[3];
    if (queue_full()) { R[3] = 0; return; }
    issue(fi, R[4], R[5], R[6], R[7]);
    wr32(fi + 0x38, R[7]);
    wr32(fi + 0x0c, DVD_STATE_BUSY);
    R[3] = 1;
}

void sdk_DVDReadPrio(void) {
    LOG();
    u32 fi = R[3];
    if (queue_full()) { R[3] = -1; return; }
    struct req *r = issue(fi, R[4], R[5], R[6], 0);
    wait_read(r, 1);
    wr32(fi + 0x0c, r->result < 0 ? -1 : DVD_STATE_END);
    R[3] = r->result;
}

void sdk_DVDGetCommandBlockStatus(void) {
    LOG();
    u32 b = R[3];
    recomp_wait_point();
    R[3] = rd32(b + 0x0c);
}

/* busy only while a read past its budget is still on the card */
void sdk_DVDGetDriveStatus(void) {
    LOG();
    recomp_wait_point();
    R[3] = q_tail != q_head && q[q_tail % MAXPENDING].late ? DVD_STATE_BUSY : DVD_STATE_END;
}
void sdk_DVDCheckDisk(void) { LOG(); R[3] = 1; }
void sdk_DVDPause(void) { LOG(); }
void sdk_DVDResume(void) { LOG(); }

/* a host read cannot be stopped; cancel lets it land and drops its callback */
void sdk_DVDCancel(void) {
    LOG();
    for (u32 i = q_tail; i != q_head; i++) {
        struct req *r = &q[i % MAXPENDING];
        if (r->fi == R[3]) { wait_read(r, 1); r->cb = 0; wr32(R[3] + 0x0c, DVD_STATE_CANCELED); }
    }
    R[3] = 0;
}
