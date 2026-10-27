#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "ppc.h"
#include "buildinfo.h"   /* RECOMP_BUILD, written by tools/buildinfo.sh every make */

void cpu_init(void);
void recomp_mtx_init(void);
void recomp_libm_init(void);
void f_80003154(void); /* __start (DOL entry) */
extern jmp_buf recomp_exit_jmp;
u32 __stacksize__ = 1024 * 1024;  /* translated code nests one host frame per guest call */

/* VFP FPSCR of the calling thread (each thread has its own). Bits: 25 DN, 24 FZ, 23-22 rounding,
 * 15/12-8 exception trap enables. */
u32 recomp_fpscr(void) { u32 v; __asm__ volatile("vmrs %0, fpscr" : "=r"(v)); return v; }
static void fpscr_set(u32 v) { __asm__ volatile("vmsr fpscr, %0" : : "r"(v)); }

/* Translator variants chosen at run time (recomp.py RECOMP_PTR_EXTENT / RECOMP_INLINE_SITES = dual, the
 * default): every function whose pointer-extent body differs tests recomp_ptr_extent at entry, the 5
 * INLINE_SITES bl sites test recomp_inline_sites. Both on unless sdmc:/cr_ptrext_off.txt /
 * cr_inline_off.txt; bottom-screen buttons "Ptr extent" / "Inline sites" flip them live (any moment is
 * safe: each body is exact on its own, a call already running keeps its body). */
int recomp_ptr_extent = 1, recomp_inline_sites = 1;
u32 recomp_codegen_flips;
static void codegen_changed(void) { recomp_codegen_flips++; }
static int flag_present(const char *name) {
    FILE *f = fopen(name, "rb");
    if (f) fclose(f);
    return f != NULL;
}
static void codegen_init(void) {
    recomp_ptr_extent = !flag_present("sdmc:/cr_ptrext_off.txt");
    recomp_inline_sites = !flag_present("sdmc:/cr_inline_off.txt");
    recomp_menu_bind("Ptr extent", &recomp_ptr_extent, codegen_changed);
    recomp_menu_bind("Inline sites", &recomp_inline_sites, codegen_changed);
    recomp_log("# codegen: pointer-extent bodies %s (sdmc:/cr_ptrext_off.txt), inline sites %s (sdmc:/cr_inline_off.txt)\n",
               recomp_ptr_extent ? "on" : "off", recomp_inline_sites ? "on" : "off");
}

/* tools/hw/agent (cr_agent.3dsx) starts the build with argv "--return <agent path>": at exit, point Luma's 3dsx
 * loader (hb:ldr SetTarget/SetArgv, as the Homebrew Launcher does) back at the agent, so unattended runs need no
 * button presses or netloader. Any other launch leaves the target alone (exit goes to the Homebrew Launcher). */
static void hw_return(int argc, char **argv) {
    if (argc < 3 || strcmp(argv[1], "--return") || argv[2][0] != '/') return;
    static char path[0x400];
    static u32 args[0x400 / 4];
    Handle h;
    if (R_FAILED(svcConnectToPort(&h, "hb:ldr"))) return;
    strncpy(path, argv[2], sizeof path - 1);
    snprintf((char *)&args[1], sizeof args - 4, "sdmc:%s", path);
    args[0] = 1;
    u32 *cmd = getThreadCommandBuffer();
    cmd[0] = IPC_MakeHeader(2, 0, 2);
    cmd[1] = IPC_Desc_StaticBuffer(strlen(path) + 1, 0);
    cmd[2] = (u32)path;
    if (R_SUCCEEDED(svcSendSyncRequest(h)) && R_SUCCEEDED(cmd[1])) {
        cmd = getThreadCommandBuffer();
        cmd[0] = IPC_MakeHeader(3, 0, 2);
        cmd[1] = IPC_Desc_StaticBuffer(sizeof args, 1);
        cmd[2] = (u32)args;
        svcSendSyncRequest(h);
    }
    svcCloseHandle(h);
}

int main(int argc, char **argv) {
    gfxInitDefault();
    recomp_menu_init();   /* bottom screen (runtime/menu.c) */
    cpu_init();
    recomp_gx_init();
    printf("boot: log, helper threads\n");   /* boot stages on the bottom screen (hw run 45a hung before the first log line) */
    recomp_log_open();
    recomp_prof_sampler_start();
    recomp_gx_start();
    recomp_log("# custom-robo-recomp phase 6: audio\n");
    /* New 3DS: 804 MHz + L2 cache; without this it runs in old-3DS mode (268 MHz) */
    osSetSpeedupEnable(true);
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    recomp_log("# system: %s, speedup requested\n", n3ds ? "New 3DS" : "old 3DS (or emulator reporting old)");
    printf("boot: loading main.dol\n");
    if (recomp_load_dol() == 0) {
        recomp_mtx_init();
        recomp_libm_init();
        codegen_init();
        recomp_perfctr_init();
        printf("main.dol loaded, running __start\n");
        recomp_log("# build %s\n", RECOMP_BUILD);
        recomp_log("# hwcheck: loads %s; stores %s\n", RECOMP_HWCHECK_MODE, RECOMP_STORE_MODE);
        recomp_log("# wrmap: %s\n", RECOMP_WRMAP_MODE);
        /* sdmc:/cr_runfast.txt: FZ+DN on the game thread (VFP11 traps to support code on
         * denormals unless FZ is set). Gekko keeps denormals (the game never sets FPSCR[NI]). */
        u32 fp0 = recomp_fpscr();
        FILE *rf = fopen("sdmc:/cr_runfast.txt", "rb");
        if (rf) { fclose(rf); fpscr_set(fp0 | (3u << 24)); }
        recomp_log("# fpscr: game thread at boot %08lx, now %08lx (sdmc:/cr_runfast.txt)\n", fp0, recomp_fpscr());
        /* sdmc flag files present at boot, so a hardware run shows what it tested */
        static const char *const flags[] = { "cr_gxthread_off", "cr_gxthread_fliptest", "cr_batch_off", "cr_dlcache_off", "cr_dlclean_off", "cr_gpukick_off",
            "cr_mtx_off", "cr_libm_off", "cr_runfast", "cr_vsync_on", "cr_dvdasync_off", "cr_dvd_stalltest", "cr_card_off", "cr_bench", "cr_fbdump", "cr_audio_dump", "cr_audio_rt_off", "cr_audio_rt_on", "cr_pad_script", "cr_pad_replay",
            "cr_vramtex_off", "cr_stereo_force", "cr_memdump", "cr_ax_stall", "cr_vtxpack_off", "cr_lazydesc_off", "cr_ptrext_off", "cr_inline_off", "cr_perfctr", "cr_widescreen" };
        char on[256] = "";
        for (unsigned i = 0; i < sizeof flags / sizeof flags[0]; i++) {
            char p[48]; snprintf(p, sizeof p, "sdmc:/%s.txt", flags[i]);
            FILE *f = fopen(p, "rb");
            size_t n = strlen(on);
            if (f) { fclose(f); snprintf(on + n, sizeof on - n, "%s%s", n ? " " : "", flags[i] + 3); }   /* "cr_" dropped */
        }
        recomp_menu_header(RECOMP_BUILD, *on ? on : "none");
        recomp_ax_pacing_init();   /* "audio real-time|per retrace" line */
        if (!setjmp(recomp_exit_jmp)) {
            f_80003154();
            recomp_done("__start returned");
        }
    } else {
        recomp_log("# DONE no DOL\n");
    }
    printf("guest run ended, see sdmc:/cr_autotest.log\n");
    recomp_gx_stop();   /* the GX thread is done with citro3d before gfxExit */
    recomp_perfctr_exit();
    recomp_log_close();
    hw_return(argc, argv);
    gfxExit();
    return 0;
}
