#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "ppc.h"
#include "buildinfo.h"   /* RECOMP_BUILD, written by tools/buildinfo.sh every make */

void cpu_init(void);
void recomp_mtx_init(void);
void recomp_libm_init(void);
void recomp_thp_init(void);
void recomp_suspend_init(void);
void f_80003154(void); /* __start (DOL entry) */
extern jmp_buf recomp_exit_jmp;
u32 __stacksize__ = 1024 * 1024;  /* translated code nests one host frame per guest call */

/* VFP FPSCR of the calling thread (each thread has its own). Bits: 25 DN, 24 FZ, 23-22 rounding,
 * 15/12-8 exception trap enables. */
u32 recomp_fpscr(void) { u32 v; __asm__ volatile("vmrs %0, fpscr" : "=r"(v)); return v; }

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
    recomp_suspend_init();   /* HOME / sleep trace (runtime/suspend.c), after the GX init: its APT hook runs after citro3d's */
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
        recomp_thp_init();
        recomp_perfctr_init();
        printf("main.dol loaded, running __start\n");
        recomp_log("# build %s\n", RECOMP_BUILD);
        recomp_log("# hwcheck: loads %s; stores %s\n", RECOMP_HWCHECK_MODE, RECOMP_STORE_MODE);
        recomp_log("# wrmap: %s\n", RECOMP_WRMAP_MODE);
        /* FZ+DN (RunFast) is already set at boot on Citra and hardware (03000000). Gekko keeps denormals
         * (the game never sets FPSCR[NI]). */
        recomp_log("# fpscr: game thread at boot %08lx\n", recomp_fpscr());
        /* sdmc flag files present at boot, so a hardware run shows what it tested */
        static const char *const flags[] = { "cr_gxthread_off", "cr_dlcache_off", "cr_dlclean_off", "cr_vramtex_off", "cr_vtxpack_off",
            "cr_dvdasync_off", "cr_card_off", "cr_bench", "cr_fbdump", "cr_audio_dump", "cr_audio_rt_off", "cr_audio_rt_on", "cr_pad_script", "cr_pad_replay",
            "cr_stereo_force", "cr_memdump", "cr_ax_stall", "cr_perfctr", "cr_widescreen_off" };
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
