/* DVD module at the dvdfs level: the FST is loaded to the 0x80000038 pointer,
 * paths resolve against it as dvdfs.c does, and reads come from the disc's
 * files under sdmc:/custom-robo/files/ (never from the repository).
 * Reads finish at once on the host; an async read's callback runs at the
 * next wait point (VIWaitForRetrace, DVDGetCommandBlockStatus,
 * DVDGetDriveStatus), the way an interrupt would land between frames.
 * DVDCommandBlock and DVDFileInfo follow the SDK layout:
 *   block: next 0, prev 4, command 8, state 0xc, offset 0x10, length 0x14,
 *          addr 0x18, currTransferSize 0x1c, transferredSize 0x20, id 0x24,
 *          callback 0x28, userData 0x2c
 *   fileinfo: block 0..0x30, startAddr 0x30, length 0x34, callback 0x38 */
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include "ppc.h"

#define LOG() recomp_sdk_log(__func__ + 4)
#define DISC_DIR "sdmc:/custom-robo/"
#define DVD_STATE_END 0
#define DVD_STATE_BUSY 1
#define DVD_STATE_CANCELED 10
#define MAXPENDING 64

static u32 fst, nentries, strings, curdir;
static char **paths;    /* host path per entry (files only) */

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
    if (fread(ram + (fst & RAM_MASK), 1, n, f) != (size_t)n) recomp_log("# DVD: short fst.bin read\n");
    fclose(f);
    nentries = e_w2(0);
    strings = fst + 12 * nentries;
    curdir = 0;
    build_paths();
    recomp_log("# DVD: FST %08lx, %lu entries\n", (unsigned long)fst, (unsigned long)nentries);
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

/* host read of [off, off+len) of the file behind a DVDFileInfo */
static u32 entry_at(u32 start) {
    static u32 last;
    if (last < nentries && !e_isdir(last) && e_w1(last) == start) return last;
    for (u32 e = 1; e < nentries; e++)
        if (!e_isdir(e) && e_w1(e) == start) return last = e;
    return ~0u;
}

static s32 host_read(u32 fi, u32 addr, u32 len, u32 off) {
    u32 e = entry_at(rd32(fi + 0x30));
    if (e >= nentries || !paths[e] || off + len > rd32(fi + 0x34) + 0x20) {   /* SDK allows the 32-byte round-up */
        recomp_log("# DVD: bad read entry %lu off %lx len %lx\n", (unsigned long)e, (unsigned long)off, (unsigned long)len);
        return -1;
    }
    static u8 seen[4096];   /* first read of each file is logged by name and frame */
    if (e < 4096 && !seen[e]) { seen[e] = 1; recomp_log("# DVD read %s (frame %lu)\n", paths[e] + sizeof DISC_DIR + 5, (unsigned long)recomp_retrace_count()); }
    u64 t0 = recomp_ticks();
    /* hw run 15: fopen per read cost ~54 ms even for 0x80 bytes; keep the last few files open (LRU) */
    enum { NHANDLES = 8 };
    static struct { u32 e; FILE *f; u32 used; } h[NHANDLES];
    static u32 use_clock;
    int slot = -1, victim = 0;
    for (int i = 0; i < NHANDLES; i++) {
        if (h[i].f && h[i].e == e) { slot = i; break; }
        if (!h[i].f || h[i].used < h[victim].used) victim = i;
    }
    if (slot < 0) {
        slot = victim;
        if (h[slot].f) fclose(h[slot].f);
        h[slot].f = fopen(paths[e], "rb");
        h[slot].e = e;
        if (!h[slot].f) { recomp_log("# DVD: cannot open %s\n", paths[e]); return -1; }
        setvbuf(h[slot].f, NULL, _IONBF, 0);   /* reads go straight to the guest buffer */
    }
    h[slot].used = ++use_clock;
    FILE *f = h[slot].f;
    fseek(f, off, SEEK_SET);
    size_t got = fread(ram + (addr & RAM_MASK), 1, len, f);
    /* hw run 7: the frame that first reads sound/cubog39.hps took 4 s; time every read */
    static u32 frame, nreads;
    static u64 sum;
    u32 r = recomp_retrace_count();
    if (r != frame) { frame = r; nreads = 0; sum = 0; }
    u64 dt = recomp_ticks() - t0;
    sum += dt; nreads++;
    if ((nreads & (nreads - 1)) == 0 && nreads >= 8)
        recomp_log("# DVD many r%lu: %lu reads (%s off %lx len %lx)\n", (unsigned long)r, (unsigned long)nreads,
                   paths[e] + sizeof DISC_DIR + 5, (unsigned long)off, (unsigned long)len);
    if (dt > 268111856 / 20 || (sum > 268111856 / 5 && nreads % 16 == 0))
        recomp_log("# DVD slow r%lu: %s off %lx len %lx %.0f ms (frame: %lu reads, %.0f ms)\n", (unsigned long)r,
                   paths[e] + sizeof DISC_DIR + 5, (unsigned long)off, (unsigned long)len, dt / 268111.856,
                   (unsigned long)nreads, sum / 268111.856);
    if (got < len) memset(ram + ((addr + got) & RAM_MASK), 0, len - got);   /* padding past EOF */
    wr32(fi + 0x10, off); wr32(fi + 0x14, len); wr32(fi + 0x18, addr);
    wr32(fi + 0x20, len);
    return (s32)len;
}

static struct { u32 fi, cb; s32 result; } pending[MAXPENDING];
static int npending;

/* the "interrupt": completed async reads call back in issue order */
void recomp_dvd_service(void) {
    static int busy;    /* a callback that waits on DVD must not nest a guest call */
    if (busy) return;
    busy = 1;
    for (int i = 0; i < npending; i++) {
        u32 fi = pending[i].fi;
        if (rd32(fi + 0x0c) == DVD_STATE_BUSY) wr32(fi + 0x0c, pending[i].result < 0 ? -1 : DVD_STATE_END);
        if (pending[i].cb) recomp_call_guest2(pending[i].cb, pending[i].result, fi);
    }
    npending = 0;
    busy = 0;
}

void sdk_DVDReadAsyncPrio(void) {
    LOG();
    u32 fi = R[3];
    if (npending == MAXPENDING) recomp_dvd_service();
    if (npending == MAXPENDING) { recomp_log("# DVD: pending queue full\n"); R[3] = 0; return; }
    s32 r = host_read(fi, R[4], R[5], R[6]);
    wr32(fi + 0x38, R[7]);
    wr32(fi + 0x0c, DVD_STATE_BUSY);
    pending[npending].fi = fi; pending[npending].cb = R[7]; pending[npending].result = r;
    npending++;
    R[3] = 1;
}

void sdk_DVDReadPrio(void) {
    LOG();
    u32 fi = R[3];
    s32 r = host_read(fi, R[4], R[5], R[6]);
    wr32(fi + 0x0c, r < 0 ? -1 : DVD_STATE_END);
    R[3] = r;
}

void sdk_DVDGetCommandBlockStatus(void) {
    LOG();
    u32 b = R[3];
    recomp_wait_point();
    R[3] = rd32(b + 0x0c);
}

void sdk_DVDGetDriveStatus(void) { LOG(); recomp_wait_point(); R[3] = DVD_STATE_END; }
void sdk_DVDCheckDisk(void) { LOG(); R[3] = 1; }
void sdk_DVDPause(void) { LOG(); }
void sdk_DVDResume(void) { LOG(); }

/* a read is already done on the host; cancel only drops its callback */
void sdk_DVDCancel(void) {
    LOG();
    for (int i = 0; i < npending; i++)
        if (pending[i].fi == R[3]) { pending[i].cb = 0; wr32(R[3] + 0x0c, DVD_STATE_CANCELED); }
    R[3] = 0;
}
