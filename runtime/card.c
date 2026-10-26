/* CARD module: a 59-block card in slot A backed by files on the SD card, slot B empty.
 * Emulated at the public API (names from the bodies + the game's single caller, the
 * per-channel state machine 0x800eca28): the game only uses one file,
 * "CustomRobo-BattleRevolution" (0xE000 bytes, 0x2000 per read/write), so the card
 * is a directory: sdmc:/custom-robo/save/<file name>.bin plus <file name>.stat (the
 * 0x6c-byte CARDStat the game last set). Every async call finishes at once with cb=0
 * (the game passes no callbacks and polls CARDGetResultCode).
 * sdmc:/cr_card_off.txt = no card (old behaviour: the "no memory card" dialogs). */
#include <stdio.h>
#include <sys/stat.h>
#include "ppc.h"

#define LOG() RECOMP_LOG()
#define CARD_RESULT_READY 0
#define CARD_RESULT_NOCARD (-3)
#define CARD_RESULT_NOFILE (-4)
#define CARD_RESULT_IOERROR (-5)
#define CARD_RESULT_EXIST (-7)
#define CARD_RESULT_INSSPACE (-9)
#define CARD_RESULT_FATAL_ERROR (-128)

#define SAVE_DIR "sdmc:/custom-robo/save/"
#define SECTOR 0x2000
#define CARD_BLOCKS 59      /* 4 Mbit card: 64 sectors, 5 system blocks */
#define STAT_SIZE 0x6c

static int present = -1;    /* slot A has a card (decided on first probe) */
static s32 result[2] = { CARD_RESULT_NOCARD, CARD_RESULT_NOCARD };

static int card_present(void) {
    if (present < 0) {
        FILE *f = fopen("sdmc:/cr_card_off.txt", "rb");
        present = !f;
        if (f) fclose(f);
        if (present) { mkdir("sdmc:/custom-robo", 0777); mkdir(SAVE_DIR, 0777); }
        recomp_log("# card: slot A %s\n", present ? "files in " SAVE_DIR : "empty (cr_card_off.txt)");
    }
    return present;
}

static s32 set_result(u32 chan, s32 r) { if (chan < 2) result[chan] = r; return r; }

/* guest file name (max 32 chars) -> host path with the given suffix */
static void host_path(char *out, size_t n, u32 name, const char *suffix) {
    char s[33];
    for (int i = 0; i < 32; i++) {
        char c = (char)rd8(name + i);
        s[i] = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_' || !c ? c : '_';
        if (!c) break;
    }
    s[32] = 0;
    snprintf(out, n, SAVE_DIR "%s%s", s, suffix);
}

/* one open file at a time: remember its name for fileNo 0 */
static u32 open_name;
static char open_path[96];
static long file_len(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static int chan_ok(u32 chan) { return chan == 0 && card_present(); }

/* CARDFileInfo: chan, fileNo, offset, length, iBlock */
static void fill_info(u32 fi, u32 chan, long len) {
    wr32(fi + 0, chan); wr32(fi + 4, 0); wr32(fi + 8, 0); wr32(fi + 12, (u32)len); wr16(fi + 16, 5);
}

/* (chan, s32 *memSize, s32 *sectorSize) */
void sdk_CARDProbeEx(void) {
    LOG();
    u32 chan = R[3];
    if (chan >= 2) { R[3] = (u32)CARD_RESULT_FATAL_ERROR; return; }
    if (!chan_ok(chan)) { R[3] = (u32)CARD_RESULT_NOCARD; return; }
    if (R[4]) wr32(R[4], 4);
    if (R[5]) wr32(R[5], SECTOR);
    R[3] = CARD_RESULT_READY;
}

/* (chan, workArea, detachCb, attachCb) */
void sdk_CARDMountAsync(void) { LOG(); R[3] = (u32)set_result(R[3], chan_ok(R[3]) ? CARD_RESULT_READY : CARD_RESULT_NOCARD); }
void sdk_CARDUnmount(void) { LOG(); R[3] = (u32)set_result(R[3], CARD_RESULT_NOCARD); if (R[3] == (u32)CARD_RESULT_NOCARD) R[3] = CARD_RESULT_READY; }
/* (chan, s32 *xferBytes, cb) */
void sdk_CARDCheckExAsync(void) {
    LOG();
    if (R[4]) wr32(R[4], 0);
    R[3] = (u32)set_result(R[3], chan_ok(R[3]) ? CARD_RESULT_READY : CARD_RESULT_NOCARD);
}
void sdk_CARDGetResultCode(void) { LOG(); R[3] = R[3] < 2 ? (u32)result[R[3]] : (u32)CARD_RESULT_FATAL_ERROR; }

/* (chan, cb): erases the save */
void sdk_CARDFormatAsync(void) {
    LOG();
    u32 chan = R[3];
    if (!chan_ok(chan)) { R[3] = (u32)set_result(chan, CARD_RESULT_NOCARD); return; }
    recomp_log("# card: format\n");
    remove(SAVE_DIR "CustomRobo-BattleRevolution.bin");
    remove(SAVE_DIR "CustomRobo-BattleRevolution.stat");
    R[3] = (u32)set_result(chan, CARD_RESULT_READY);
}

/* (chan, s32 *byteNotUsed, s32 *filesNotUsed) */
void sdk_CARDFreeBlocks(void) {
    LOG();
    u32 chan = R[3];
    if (!chan_ok(chan)) { R[3] = (u32)set_result(chan, CARD_RESULT_NOCARD); return; }
    long n = file_len(SAVE_DIR "CustomRobo-BattleRevolution.bin");
    int used = n > 0 ? (int)((n + SECTOR - 1) / SECTOR) : 0;
    if (R[4]) wr32(R[4], (u32)((CARD_BLOCKS - used) * SECTOR));
    if (R[5]) wr32(R[5], used ? 126 : 127);
    R[3] = (u32)set_result(chan, CARD_RESULT_READY);
}

/* (chan, name, fileInfo) */
void sdk_CARDOpen(void) {
    LOG();
    u32 chan = R[3], name = R[4], fi = R[5];
    if (!chan_ok(chan)) { R[3] = (u32)CARD_RESULT_NOCARD; return; }
    char path[96];
    host_path(path, sizeof path, name, ".bin");
    long n = file_len(path);
    recomp_log("# card: open %s -> %ld\n", path, n);
    if (n < 0) { R[3] = (u32)CARD_RESULT_NOFILE; return; }
    open_name = name;
    snprintf(open_path, sizeof open_path, "%s", path);
    fill_info(fi, chan, n);
    R[3] = CARD_RESULT_READY;
}

/* (fileInfo) */
void sdk_CARDClose(void) { LOG(); wr32(R[3], (u32)-1); R[3] = CARD_RESULT_READY; }

/* (chan, name, size, fileInfo, cb) */
void sdk_CARDCreateAsync(void) {
    LOG();
    u32 chan = R[3], name = R[4], size = R[5], fi = R[6];
    if (!chan_ok(chan)) { R[3] = (u32)set_result(chan, CARD_RESULT_NOCARD); return; }
    char path[96];
    host_path(path, sizeof path, name, ".bin");
    if (file_len(path) >= 0) { R[3] = (u32)set_result(chan, CARD_RESULT_EXIST); return; }
    if (size > CARD_BLOCKS * SECTOR) { R[3] = (u32)set_result(chan, CARD_RESULT_INSSPACE); return; }
    FILE *f = fopen(path, "wb");
    if (!f) { R[3] = (u32)set_result(chan, CARD_RESULT_IOERROR); return; }
    static u8 zero[SECTOR];
    for (u32 o = 0; o < size; o += SECTOR) fwrite(zero, 1, size - o < SECTOR ? size - o : SECTOR, f);
    fclose(f);
    recomp_log("# card: create %s 0x%lx\n", path, (unsigned long)size);
    open_name = name;
    snprintf(open_path, sizeof open_path, "%s", path);
    fill_info(fi, chan, size);
    R[3] = (u32)set_result(chan, CARD_RESULT_READY);
}

/* (chan, name, cb) */
void sdk_CARDDeleteAsync(void) {
    LOG();
    u32 chan = R[3];
    if (!chan_ok(chan)) { R[3] = (u32)set_result(chan, CARD_RESULT_NOCARD); return; }
    char path[96], spath[96];
    host_path(path, sizeof path, R[4], ".bin");
    host_path(spath, sizeof spath, R[4], ".stat");
    int r = remove(path);
    remove(spath);
    recomp_log("# card: delete %s -> %d\n", path, r);
    R[3] = (u32)set_result(chan, r ? CARD_RESULT_NOFILE : CARD_RESULT_READY);
}

/* (fileInfo, buf, len, offset, cb), write = 1 for CARDWriteAsync */
static void rw(int write) {
    u32 fi = R[3], buf = R[4], len = R[5], off = R[6];
    u32 chan = rd32(fi);
    if (!chan_ok(chan)) { R[3] = (u32)set_result(chan, CARD_RESULT_NOCARD); return; }
    FILE *f = fopen(open_path, write ? "r+b" : "rb");
    s32 r = CARD_RESULT_IOERROR;
    if (f && fseek(f, off, SEEK_SET) == 0) {
        static u8 tmp[SECTOR];
        r = CARD_RESULT_READY;
        for (u32 o = 0; o < len && r == CARD_RESULT_READY; o += SECTOR) {
            u32 n = len - o < SECTOR ? len - o : SECTOR;
            if (write) {
                for (u32 i = 0; i < n; i++) tmp[i] = rd8(buf + o + i);
                if (fwrite(tmp, 1, n, f) != n) r = CARD_RESULT_IOERROR;
            } else {
                if (fread(tmp, 1, n, f) != n) r = CARD_RESULT_IOERROR;
                else for (u32 i = 0; i < n; i++) wr8(buf + o + i, tmp[i]);
            }
        }
    }
    if (f) fclose(f);
    recomp_log("# card: %s 0x%lx at 0x%lx -> %ld\n", write ? "write" : "read", (unsigned long)len, (unsigned long)off, (long)r);
    R[3] = (u32)set_result(chan, r);
}
void sdk_CARDReadAsync(void) { LOG(); rw(0); }
void sdk_CARDWriteAsync(void) { LOG(); rw(1); }

/* CARDStat: fileName[32], length, time, gameName[4], company[2], ... (0x6c bytes) */
static void stat_path(char *out, size_t n) { host_path(out, n, open_name, ".stat"); }

/* (chan, fileNo, CARDStat *) */
void sdk_CARDGetStatus(void) {
    LOG();
    u32 chan = R[3], st = R[5];
    if (!chan_ok(chan) || !open_name) { R[3] = (u32)(chan_ok(chan) ? CARD_RESULT_NOFILE : CARD_RESULT_NOCARD); return; }
    char path[96];
    stat_path(path, sizeof path);
    u8 b[STAT_SIZE] = {0};
    FILE *f = fopen(path, "rb");
    if (f) { fread(b, 1, STAT_SIZE, f); fclose(f); }
    for (int i = 0; i < STAT_SIZE; i++) wr8(st + i, b[i]);
    for (int i = 0; i < 32; i++) wr8(st + i, rd8(open_name + i));
    wr32(st + 0x20, (u32)file_len(open_path));
    for (int i = 0; i < 4; i++) wr8(st + 0x28 + i, rd8(0x80000000 + i));   /* game code from boot info */
    for (int i = 0; i < 2; i++) wr8(st + 0x2c + i, rd8(0x80000004 + i));
    R[3] = CARD_RESULT_READY;
}

static void set_status(void) {
    u32 chan = R[3], st = R[5];
    if (!chan_ok(chan) || !open_name) { R[3] = (u32)set_result(chan, chan_ok(chan) ? CARD_RESULT_NOFILE : CARD_RESULT_NOCARD); return; }
    char path[96];
    stat_path(path, sizeof path);
    u8 b[STAT_SIZE];
    for (int i = 0; i < STAT_SIZE; i++) b[i] = rd8(st + i);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(b, 1, STAT_SIZE, f); fclose(f); }
    R[3] = (u32)set_result(chan, f ? CARD_RESULT_READY : CARD_RESULT_IOERROR);
}
void sdk_CARDSetStatus(void) { LOG(); set_status(); }
void sdk_CARDSetStatusAsync(void) { LOG(); set_status(); }

/* (chan, u64 *serial): fixed, so the game's "same card as last time" check holds */
void sdk_CARDGetSerialNo(void) {
    LOG();
    if (!chan_ok(R[3])) { R[3] = (u32)CARD_RESULT_NOCARD; return; }
    wr64(R[4], 0x4352534156453031ull);   /* "CRSAVE01" */
    R[3] = CARD_RESULT_READY;
}
