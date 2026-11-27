/**
 * cm_wr v57: test plan stage 2 (docs/TEST_PLAN_PREVIEW.md) - IDENTICAL write of C0F38024,
 *           and stage 3a (docs/STAGE3_DESIGN.md) - 5D3 x5 window move with Canon's own values.
 *
 * Stage 2:
 * For 20 s, every 100 ms in LiveView:
 *   raw  = shamem C0F06804 (RAW_V << 16 | RAW_H)
 *   cur  = shamem C0F38024
 *   calc = 5D3:   ((RAW_V - 3) << 16) + 2 * RAW_H - 0x2F   (measured in 4 modes, cm_log + cm_prev)
 *          EOS M: ((RAW_V - 1) << 16) + RAW_H - 0x11       (Bilal crop_rec.c, measured)
 *   calc == cur -> EngDrvOutLV(C0F38024, calc)   (the value already in the register)
 *   calc != cur -> NO write, only counted (safety lock)
 * Expected: nothing changes on screen, mismatch = 0, readback always equal.
 *
 * Safety contract:
 *  - one register, written only with the value it already holds
 *  - no hooks, no sensor (CMOS/ADTG), no property/NVRAM/ROM writes
 *  - writes go through ML's EngDrvOutLV (same path crop_rec uses): skipped outside LV,
 *    with the display off, during lens jobs or shutdown
 *  - not while recording; stops at once if LV is left
 *  - only 5D3 1.1.3 / EOS M 2.0.2
 *
 * Stage 3a (5D3 1.1.3 only, x5 only: C0F06804 must be 0x056601EB):
 *   C0F383D4 (window start) / C0F383DC (window end) are switched from the Canon x5 pair the
 *   camera is using to the OTHER Canon x5 pair seen in the logs (EP5D3_01/02):
 *     a = 0x012C0177 / 0x044402A2,  b = 0x00E00047 / 0x03F80172
 *   Any other current value -> nothing is written (logged only). For 20 s, every 100 ms the pair is
 *   rewritten if Canon changed it (counted); then the original pair is written back.
 *   Write order D4 then DC, the order Canon itself uses (EP5D3_02 lines 1894/1896).
 *   No other register, no EDMAC, no sensor.
 *
 * Stage 3b (5D3 1.1.3 only, x5 only): the preview size group + window (13 registers) are set to
 *   the values Canon itself writes in 1080p x1 (logs/20261017-5D3-stage1c/EP5D3_02.LOG,
 *   docs/STAGE3_DESIGN.md). Before writing, all 13 must hold Canon's x5 values (window a or b),
 *   else nothing is written. 20 s, rewrite if changed (counted), then Canon's x5 values are
 *   written back. Order = Canon's own order (38070 group, window, 42xxx).
 *   No C0F38024 (raw size), no EDMAC, no sensor, no timing.
 *   v3: generic sets from cm_wr_tables.h: 3b-1 window only, 3b-2 size group only (= v2 3b minus
 *   window), 3b-3 all 69 C0F38/C0F42 registers that differ x1/x5 + window (3b froze the preview:
 *   x1 size with x5 intermediate stages; restore brought it back).
 *
 * Stage E1a (v4, 5D3 1.1.3 only, docs/STAGE3_DESIGN.md section 5): preview output EDMAC size only,
 *   no preview register writes. Hook on Canon's linear EDMAC helper RAM 0x13734 f(channel, bytes)
 *   (E0, logs/20261017-5D3-edmac-e0: called only on mode change, ch 0x13 from ff33e590, ch 0x14 from
 *   ff33e5a8). Started in 1080p x1 (C0F06804 = 0x0528011B); for 30 s, ONLY when Canon's x5 bytes come
 *   from the known caller, regs[1] is changed to Canon's own 1080p x1 bytes:
 *     ch 0x13 0x179740 -> 0x3DE000,  ch 0x14 0x1535D0 -> 0x376E00
 *   Anything else passes untouched. The buffers (4 fixed slots) take the x1 size in x5 too
 *   (logs/20261017-5D3-edmac/ED5D3_02.LOG). After 30 s the hook is inert (ML hooks stay installed);
 *   Canon's x5 size comes back at the next mode change (x1 -> x5).
 *   Logged: replaced/passed calls, distinct (raw, C0F2630C/10/14, C0F2640C/10/14) shadow values.
 *
 * Stage E1b (v5): E1a, and once x5 raw is seen with both EDMAC shadows at the x1 size
 *   (C0F2630C 0x3DD, C0F2640C 0x376), stage 3b-3 runs unchanged (same precondition, 20 s,
 *   Canon x5 values written back). The hook stays active until 3b-3 ends. Logged together.
 *   v6: ready also needs the hook to have replaced both channels in this run (v5 took x1 leftovers
 *   in the shadow for x5 setup: WR5D3_12); 1 s settle; second log with calls/shadows after 3b-3.
 *   v7: v6 froze anyway with EDMAC 0x13/0x14 at x1 size (WR5D3_13/14). Same writes; adds a
 *   READ-ONLY sampler of EDMAC state/pointer (ch 03 0B 13 14 18 20) every 10 ms: 2 s before
 *   3b-3, during it, 3 s after restore -> which channel stops in the freeze.
 *   v8: v7 (WR5D3_16) showed every sampled channel still moving in the freeze; adds ch 0x05 (LV VRAM)
 *   and 0x0A (HD buffer) per ML 5D3 consts.h, 4 MB page bitmaps, and the LCD display buffer
 *   pointer (YUV422_LV_BUFFER_DISPLAY_ADDR). Still read-only; same writes as v6.
 *   v9: v8 (WR5D3_18): in the freeze the display buffer stops rotating (1 change in 20 s) while
 *   ch 0x05 keeps moving. Adds ch 0x05/0x0A size shadows (base+0x0C..0x20) per phase and a
 *   histogram of the ch 0x05 pointer offset inside the LV buffer. Read-only; same writes.
 *
 * Stage E3 (v10, approved 2026-10-17): 3b-3 froze because SSLT (writer of EDMAC 0x13/0x14) stays set
 *   up for x5 (logs/20261017-5D3-3b3dm: Path Incomplete[200], SHREK1/2). Hook on the SSLT driver start
 *   ROM 0xFF33E280: only the x5 call (caller 0xFF48A2BC, p1 0xFF94F8D0, p3 0xFF94F874) gets Canon's
 *   x1 tables (p1 0xFF94BFE4, p3 0xFF94BF88, logs/20261017-5D3-e2). No EDMAC hook (SSLT computes
 *   the sizes itself). Started in 1080p x1; when x5 is entered with the x1 tables, waits (10 ms
 *   steps, max 3 s) for Canon's x5 preview values, then 3b-3 (stage3b_run) at once.
 *   v11: E3 v10 never wrote 3b-3 (pre_ok=0, WR5D3_27); now logs every register that is not at
 *   Canon's x5 value (current / expected x5 / x1 target) and the window. Still no write then.
 *   v12: also logs Timer A/B, HEAD3/HEAD4 in x5 (FPS override test; fps = 24 MHz / (A * B)).
 *
 * Stage 4 (v13, approved 2026-10-17, docs/STAGE4_SSLT_TYPE7.md): x5 raw unchanged; SSLT type 7 margin
 *   column patched (4 ROM words through patch_memory, undone at the end) and SSLT given RAM copies of the
 *   Canon x5 tables with the x1 size (W 1920, H 1280, X 1124, Y 52; p3 sizes from the x1 p3) -> SSLT
 *   processes 1948 x 1288 of the 3744 x 1380 x5 raw, Canon computes all preview registers itself.
 *   Guards: window kept inside the raw (rewritten if Canon's zoom box logic moves it out), EDMAC request
 *   above Canon's own x1 bytes is clamped and counted (slot ~5 MB). 20 s in x5, then everything off.
 *   v14: stage 4b = same with p1 +0xCC..+0xD4 from the Canon x1 table (stage 4 ran at 30 fps with no
 *   engine error, but the picture was wrong; those words land in C0F3846C/849C/84CC).
 */

#include <module.h>
#include <config.h>
#include <dryos.h>
#include <menu.h>
#include <property.h>
#include <patch.h>
#include <powersave.h>
#include <shoot.h>
#include <bmp.h>
#include <lvinfo.h>
#include "raw.h"

#define TEST_MS     20000
#define STEP_MS     100
#define REG_RAW     0xC0F06804
#define REG_TARGET  0xC0F38024
#define NSAMPLES    8

#define REG_WIN_START 0xC0F383D4
#define REG_WIN_END   0xC0F383DC
#define RAW_5D3_X5    0x056601EB

/* the two Canon x5 window pairs measured on 5D3 1.1.3 (logs/20261017-5D3-stage1b, -stage1c) */
static const uint32_t win_pairs[2][2] = {
    { 0x012C0177, 0x044402A2 },
    { 0x00E00047, 0x03F80172 },
};

/* ML's snprintf has no %c: names as strings */
static const char * const pair_name[2] = { "a", "b" };

static volatile int running = 0;
static const char * save_text(const char * text, int len);
static char last_status[64] = "";

static int is_5d3_113(void) { return is_camera("5D3", "1.1.3"); }
static int is_5d3_123(void) { return is_camera("5D3", "1.2.3"); }
static int is_5d3(void)     { return is_5d3_113() || is_5d3_123(); }
static int is_eosm(void)    { return is_camera("EOSM", "2.0.2"); }

typedef struct {
    uint32_t ss_addr, ss_x1_caller, ss_x5_caller, ss_720_caller;
    uint32_t ss_x1_p1, ss_x1_p3, ss_x5_p1, ss_x5_p3, ss_720_p1, ss_720_p3;
    uint32_t x1_clear_addr, s4_clear_addr;
    uint32_t engio_write_addr;
    uint32_t f4_tpl, f4_caller, f4_303_caller, f4_403_caller;
    uint32_t l1_5d3c_addr, l1_5f54_addr, l1_5f54_insn;
    int      l1_5f54_reg;
    uint32_t x1_sslt_a3_addr, x1_sslt_a11_addr;
    uint32_t x1_hiv_yb_addr, hfps_hiv_yb_addr, lv_state_addr;
    uint32_t x1_hiv_hook; /* LVx1_StartPreproPath: MOV R3,#0x40000000 before SetEDmac(0x18, R1 = HIV-V table, R2 = edmac_info) */
    uint32_t x5_pan_hook; /* LVx5_StartPreproPath: LDR R0,[R4,#0x50] between LDRH R1/R2 (zoom box X/Y) and BL SSLT_SetWinOffset */
    uint32_t eeko_k1, eeko_k5, eeko_m1, eeko_402, eeko_roi_y, eeko_roi_h;
    uint32_t e1_map[2][4];
    uint32_t s4_tbl[4][3];
    uint32_t lbuf_tbl[8][3];
} cm_fw_t;

static const cm_fw_t cm_fw_113 = {
    .ss_addr          = 0xFF33E280u,
    .ss_x1_caller     = 0xFF4879F8u,
    .ss_x5_caller     = 0xFF48A2BCu,
    .ss_720_caller    = 0xFF490084u,
    .ss_x1_p1         = 0xFF94BFE4u,
    .ss_x1_p3         = 0xFF94BF88u,
    .ss_x5_p1         = 0xFF94F8D0u,
    .ss_x5_p3         = 0xFF94F874u,
    .ss_720_p1        = 0xFF9564D4u,
    .ss_720_p3        = 0xFF956478u,
    .x1_clear_addr    = 0xFF94BEFCu,
    .s4_clear_addr    = 0xFF94F7E8u,
    .engio_write_addr = 0xFF28CC3Cu,
    .f4_tpl           = 0xFF92F40Cu,
    .f4_caller        = 0xFF33CC9Cu,
    .f4_303_caller    = 0xFF33CA3Cu,
    .f4_403_caller    = 0xFF508340u,
    .l1_5d3c_addr     = 0xFF165DBCu,
    .l1_5f54_addr     = 0xFF165FD0u,
    .l1_5f54_insn     = 0xE5802000u, /* STR R2, [R0] */
    .l1_5f54_reg      = 2,
    .x1_sslt_a3_addr  = 0xFF995004u,
    .x1_sslt_a11_addr = 0xFF995164u,
    .x1_hiv_yb_addr   = 0x0003268Cu,
    .x1_hiv_hook      = 0xFF4880E8u,
    .x5_pan_hook      = 0xFF48A8A0u, /* I-cache idx 5 */
    .hfps_hiv_yb_addr = 0x00033000u,
    .lv_state_addr    = 0x000563BCu,
    .eeko_k1          = 0x01E4C1A4u,
    .eeko_k5          = 0x01E4C284u,
    .eeko_m1          = 0x01E4C7B4u,
    .eeko_402         = 0x01E4D634u,
    .eeko_roi_y       = 0x01E17D02u,
    .eeko_roi_h       = 0x01E17D0Eu,
    .e1_map = {
        { 0x13u, 0xFF33E590u, 0x179740u, 0x3DE000u },
        { 0x14u, 0xFF33E5A8u, 0x1535D0u, 0x376E00u },
    },
    .s4_tbl = {
        { 0xFF994FE0u, 2576u, 1824u },
        { 0xFF99500Cu,  596u,  100u },
        { 0xFF995114u, 2548u, 1796u },
        { 0xFF99516Cu,  588u,   92u },
    },
    .lbuf_tbl = {
        { 0xFF165DACu, 0xE28C3402u, 0xE28C374Du },
        { 0xFF165DCCu, 0xE28C370Au, 0xE28C3601u },
        { 0xFF165DDCu, 0xE28C3832u, 0xE28C370Bu },
        { 0xFF165DECu, 0xE28C39D2u, 0xE28C3832u },
        { 0xFF165FC0u, 0xE2822402u, 0xE282274Du },
        { 0xFF165FE0u, 0xE282270Au, 0xE2822601u },
        { 0xFF165FF0u, 0xE2822832u, 0xE282270Bu },
        { 0xFF166000u, 0xE28229D2u, 0xE2822832u },
    },
};

static const cm_fw_t cm_fw_123 = {
    .ss_addr          = 0xFF344BD0u,
    .ss_x1_caller     = 0xFF4916ACu,
    .ss_x5_caller     = 0xFF4941D4u,
    .ss_720_caller    = 0xFF49A604u,
    .ss_x1_p1         = 0xFF95C740u,
    .ss_x1_p3         = 0xFF95C6E4u,
    .ss_x5_p1         = 0xFF9600BCu,
    .ss_x5_p3         = 0xFF960060u,
    .ss_720_p1        = 0xFF967D5Cu,
    .ss_720_p3        = 0xFF967D00u,
    .x1_clear_addr    = 0xFF95C640u,
    .s4_clear_addr    = 0xFF95FFBCu,
    .engio_write_addr = 0xFF290F98u,
    .f4_tpl           = 0xFF93F8B0u,
    .f4_caller        = 0xFF343578u,
    .f4_303_caller    = 0xFF343318u,
    .f4_403_caller    = 0xFF512E48u,
    .l1_5d3c_addr     = 0xFF165EB8u,
    .l1_5f54_addr     = 0xFF166104u,
    .l1_5f54_insn     = 0xE580C000u, /* STR IP, [R0] */
    .l1_5f54_reg      = 12,
    .x1_sslt_a3_addr  = 0xFF9A66D0u,
    .x1_sslt_a11_addr = 0xFF9A6830u,
    .x1_hiv_yb_addr   = 0x0003260Cu,
    .x1_hiv_hook      = 0xFF491D9Cu,
    .x5_pan_hook      = 0xFF4947B8u, /* I-cache idx 61 */
    .hfps_hiv_yb_addr = 0x00032F7Cu,
    .lv_state_addr    = 0x00056414u,
    .eeko_k1          = 0x01E4C2BCu,
    .eeko_k5          = 0x01E4C39Cu,
    .eeko_m1          = 0x01E4C9B4u,
    .eeko_402         = 0x01E4DBD4u,
    .eeko_roi_y       = 0x01E17E24u,
    .eeko_roi_h       = 0x01E17E30u,
    .e1_map = {
        { 0x13u, 0xFF344EE0u, 0x179740u, 0x3DE000u },
        { 0x14u, 0xFF344EF8u, 0x1535D0u, 0x376E00u },
    },
    .s4_tbl = {
        { 0xFF9A66ACu, 2576u, 1824u },
        { 0xFF9A66D8u,  596u,  100u },
        { 0xFF9A67E0u, 2548u, 1796u },
        { 0xFF9A6838u,  588u,   92u },
    },
    .lbuf_tbl = {
        { 0xFF165EA8u, 0xE28C3402u, 0xE28C374Du },
        { 0xFF165EC8u, 0xE28C370Au, 0xE28C3601u },
        { 0xFF165ED8u, 0xE28C3832u, 0xE28C370Bu },
        { 0xFF165EE8u, 0xE28C39D2u, 0xE28C3832u },
        { 0xFF1660F4u, 0xE28C2402u, 0xE28C274Du },
        { 0xFF166114u, 0xE28C270Au, 0xE28C2601u },
        { 0xFF166124u, 0xE28C2832u, 0xE28C270Bu },
        { 0xFF166134u, 0xE28C29D2u, 0xE28C2832u },
    },
};

static const cm_fw_t * cm_fw = &cm_fw_113;
#define LV_STATE_ADDR (cm_fw->lv_state_addr)

static const char * log_prefix(void)
{
    if (is_5d3())  return "ML/LOGS/W5D3";
    if (is_eosm()) return "ML/LOGS/WEOM";
    return 0;
}

/* returns 0 if the raw value does not look like a video mode */
static uint32_t calc_target(uint32_t raw)
{
    uint32_t rv = raw >> 16;
    uint32_t rh = raw & 0xFFFF;
    if (rv < 0x100 || rv > 0x1000 || rh < 0x80 || rh > 0x800) return 0;
    if (is_5d3())  return ((rv - 3) << 16) + 2 * rh - 0x2F;
    if (is_eosm()) return ((rv - 1) << 16) + rh - 0x11;
    return 0;
}

static void cm_wr_task(void)
{
    if (!log_prefix())
    {
        NotifyBox(3000, "cm_wr: camera not supported");
        return;
    }
    if (running || RECORDING || !lv || !is_movie_mode())
    {
        NotifyBox(3000, "cm_wr: need movie mode LiveView, not recording");
        return;
    }
    running = 1;

    int steps = 0, writes = 0, mismatch = 0, readback_bad = 0, skipped = 0;
    /* raw efic_temp; Celsius is computed on the Mac (cm_log formulas), no division here */
    int t0 = efic_temp;
    uint32_t s_raw[NSAMPLES], s_cur[NSAMPLES], s_calc[NSAMPLES];
    int ns = 0;
    uint32_t first_raw = 0, first_cur = 0;

    int left_s = TEST_MS / 1000;
    for (int t = 0, tick = 0; t < TEST_MS; t += STEP_MS, tick++)
    {
        if (!lv || RECORDING) break;

        uint32_t raw = shamem_read(REG_RAW);
        uint32_t cur = shamem_read(REG_TARGET);
        uint32_t calc = calc_target(raw);
        steps++;
        if (!first_raw) { first_raw = raw; first_cur = cur; }

        if (!calc || !cur)
        {
            skipped++;
        }
        else if (calc != cur)
        {
            mismatch++;
            if (ns < NSAMPLES) { s_raw[ns] = raw; s_cur[ns] = cur; s_calc[ns] = calc; ns++; }
        }
        else
        {
            EngDrvOutLV(REG_TARGET, calc);
            writes++;
            if (shamem_read(REG_TARGET) != calc) readback_bad++;
        }

        if (tick == 1000 / STEP_MS) tick = 0;
        if (tick == 0)
            NotifyBox(1500, "cm_wr: %ds, writes %d, mismatch %d", left_s--, writes, mismatch);
        msleep(STEP_MS);
    }
    int t1 = efic_temp;
    running = 0;

    char text[1024];
    int len = snprintf(text, sizeof(text),
        "# cm_wr v0 stage 2: identical write C0F38024\n"
        "zoom=%d steps=%d writes=%d mismatch=%d readback_bad=%d skipped=%d efic_temp_raw=%d->%d\n"
        "first raw C0F06804=%08x C0F38024=%08x\n",
        lv_dispsize, steps, writes, mismatch, readback_bad, skipped, t0, t1, first_raw, first_cur);
    for (int i = 0; i < ns; i++)
        len += snprintf(text + len, sizeof(text) - len, "mismatch raw=%08x cur=%08x calc=%08x\n",
            s_raw[i], s_cur[i], s_calc[i]);

    const char * n = save_text(text, len);
    snprintf(last_status, sizeof(last_status), "%s w%d m%d rb%d", n, writes, mismatch, readback_bad);
    NotifyBox(6000, "cm_wr: %s", last_status);
}

static const char * save_text(const char * text, int len)
{
    static char name[32];
    uint32_t size;
    /* v40: 4-digit counter (the 2-digit one ran out at WR5D3_99 and overwrote it, 2026-10-18) */
    int i;
    for (i = 0; i < 10000; i++)
    {
        snprintf(name, sizeof(name), "%s%04d.LOG", log_prefix(), i);
        if (FIO_GetFileSize(name, &size) != 0) break;
    }
    if (i == 10000) return "no free log name";
    FILE * f = FIO_CreateFile(name);
    if (!f) return "no log";
    FIO_WriteFile(f, text, len);
    FIO_CloseFile(f);
    return name + 8;
}

struct wr_entry { uint32_t reg, target, pre; };
#include "cm_wr_tables.h"
#include <screenshot.h>
#include "cm_wr_f1.h"
/* v39: F1 (approved 2026-10-18) = stage 4b (SSLT at x1 sizes) + the NR / second-pass stage set to Canon's
 * own 1080p x1 values (f1_tab, 83 registers, written only where the register holds Canon's x5 value),
 * so it reads the whole 1948x1288 SSLT output instead of a 1196x784 part. F1b additionally turns on
 * the C0F11 YUV stretch (x1: C0F1150C = 0x00770078 = 119:120 = out:in, C0F1154C = 0x027D0280 = 637:640)
 * with 1948 -> 1152 (288:487) and 1288 -> 768 (96:161); the C0F11 output size stays at x5 1152x768,
 * so the HD buffer is not overrun. All restored at the end (if still in x5). */
static volatile int s4_f1 = 0;   /* 0 off, 1 = F1a (83 regs), 2 = F1b (+ C0F11 stretch) */
#define F1_N   ((int) (sizeof(f1_tab) / sizeof(f1_tab[0])))
static const uint32_t f1b_tab[][3] = {   /* { reg, x5, new } */
    { 0xC0F11508, 0x80000000, 0x00002001 },   /* x1 value */
    { 0xC0F1150C, 0x00000000, 0x012001E7 },   /* 288:487 */
    { 0xC0F11510, 0x00000000, 0x00060222 },   /* x1 value */
    { 0xC0F11548, 0x80000000, 0x00000001 },   /* x1 value */
    { 0xC0F1154C, 0x00000000, 0x006000A1 },   /* 96:161 */
    { 0xC0F11550, 0x00000000, 0x00090333 },   /* x1 value */
};
#define F1B_N  ((int) (sizeof(f1b_tab) / sizeof(f1b_tab[0])))
static uint8_t f1_mine[F1_N], f1b_mine[F1B_N];
/* v41: F2 (approved 2026-10-18) = stage 4b + every register after SSLT that differs between Canon x1 and
 * Canon x5 set to Canon's x1 value (cm_wr_f2.h, 178 registers incl. the C0F43xxx routing table that made
 * F1a freeze), so the whole chain after SSLT runs as in 1080p x1 while the raw stays x5. The HD buffer
 * is a fixed 0x4A2000 (x1 size) in every mode (dispenser 0xFF165D3C type 13/14), so x1 sizes fit. */
#include "cm_wr_f2.h"
#define F2_N   ((int) (sizeof(f2_tab) / sizeof(f2_tab[0])))
static uint8_t f2_mine[F2_N];
static uint32_t f2_bad[16][2];
static int f2_nbad = 0;
/* v36: LiveView screenshots during stage 4 (ML take_screenshot, YUV only = the picture Canon sends to
 * the LCD, 720x480 PPM ~1 MB, ~1 s): at 3 s and 12 s in x5 -> ML/LOGS/CMnnnn.PPM */
static char s4_shot_pat[] = "ML/LOGS/CM%04d.PPM";
static int s4_shots = 0;

struct wr_set { const char * name; const struct wr_entry * e; int n; };
static const struct wr_set sets[3] = {
    { "3b-1", set_3b1, COUNT(set_3b1) },
    { "3b-2", set_3b2, COUNT(set_3b2) },
    { "3b-3", set_3b3, COUNT(set_3b3) },
};
#define MAX_SET 80
static volatile int set_req = 0;

/* generic stage 3b: precondition (x5 raw + every register at its Canon x5 value / known window),
 * write targets, keep them 20 s (rewrite if changed), then write the Canon values back */
/* v7: read-only EDMAC sampler phase, set by stage3b_run (0 before, 1 while written, 2 after restore) */
static volatile int smp_phase = 0;

/* same precondition as stage3b_run, without logging (stage E3 waits for it) */
static int stage3b_pre_ok(const struct wr_set * S)
{
    if (shamem_read(REG_RAW) != RAW_5D3_X5) return 0;
    uint32_t ws = shamem_read(REG_WIN_START), we = shamem_read(REG_WIN_END);
    int win = -1;
    for (int k = 0; k < 2; k++)
        if (ws == win_pairs[k][0] && we == win_pairs[k][1]) win = k;
    if (win < 0) return 0;
    for (int i = 0; i < S->n; i++)
        if (S->e[i].pre != WIN_ANY && shamem_read(S->e[i].reg) != S->e[i].pre) return 0;
    return 1;
}

/* pre: optional text put at the top of the log (stage E1b) */
static void stage3b_run(const struct wr_set * S, const char * pre)
{
    if (S->n > MAX_SET) return;

    uint32_t raw = shamem_read(REG_RAW);
    uint32_t orig[MAX_SET];
    for (int i = 0; i < S->n; i++) orig[i] = shamem_read(S->e[i].reg);

    /* window must be one Canon pair, read directly (also when the set does not write it) */
    uint32_t ws = shamem_read(REG_WIN_START), we = shamem_read(REG_WIN_END);
    int win = -1;
    for (int k = 0; k < 2; k++)
        if (ws == win_pairs[k][0] && we == win_pairs[k][1]) win = k;
    int bad = (raw != RAW_5D3_X5) || (win < 0);
    for (int i = 0; i < S->n; i++)
        if (S->e[i].pre != WIN_ANY && orig[i] != S->e[i].pre) bad = 1;

    static char text[4096];
    int len;
    if (bad)
    {
        len = snprintf(text, sizeof(text),
            "%s# cm_wr v3 stage %s: NOT run (raw=%08x win=%08x/%08x zoom=%d); nothing written\n# reg current expected_x5\n",
            pre ? pre : "", S->name, raw, ws, we, lv_dispsize);
        for (int i = 0; i < S->n; i++)
            len += snprintf(text + len, sizeof(text) - len, "%08x %08x %08x\n", S->e[i].reg, orig[i], S->e[i].pre);
        const char * n = save_text(text, len);
        snprintf(last_status, sizeof(last_status), "%s: %s not run", n, S->name);
        NotifyBox(6000, "cm_wr %s: not x5 / unknown values, nothing written", S->name);
        return;
    }

    running = 1;
    smp_phase = 1;
    int t0 = efic_temp;
    int steps = 0, writes = 0, canon_rewrites = 0, left_mode = 0;
    int left_s = TEST_MS / 1000;
    for (int t = 0, tick = 0; t < TEST_MS; t += STEP_MS, tick++)
    {
        if (!lv || RECORDING || shamem_read(REG_RAW) != RAW_5D3_X5) { left_mode = 1; break; }
        steps++;
        int diff = 0;
        for (int i = 0; i < S->n; i++)
            if (shamem_read(S->e[i].reg) != S->e[i].target) diff = 1;
        if (diff)
        {
            if (writes) canon_rewrites++;
            for (int i = 0; i < S->n; i++) EngDrvOutLV(S->e[i].reg, S->e[i].target);
            writes++;
        }
        if (tick == 1000 / STEP_MS) tick = 0;
        if (tick == 0)
            NotifyBox(1500, "cm_wr %s: %ds, writes %d", S->name, left_s--, writes);
        msleep(STEP_MS);
    }

    int restored = 0;
    if (lv && shamem_read(REG_RAW) == RAW_5D3_X5)
    {
        for (int i = 0; i < S->n; i++) EngDrvOutLV(S->e[i].reg, orig[i]);
        restored = 1;
    }
    smp_phase = 2;
    int t1 = efic_temp;
    running = 0;

    len = snprintf(text, sizeof(text),
        "%s# cm_wr v3 stage %s: %d registers (window %s)\n"
        "steps=%d writes=%d canon_rewrites=%d left_mode=%d restored=%d efic_temp_raw=%d->%d\n",
        pre ? pre : "", S->name, S->n, pair_name[win], steps, writes, canon_rewrites, left_mode, restored, t0, t1);
    const char * n = save_text(text, len);
    snprintf(last_status, sizeof(last_status), "%s %s w%d cr%d r%d", n, S->name, writes, canon_rewrites, restored);
    NotifyBox(6000, "cm_wr %s: %s", S->name, last_status);
}

static void stage3b_task(void)
{
    const struct wr_set * S = &sets[set_req];
    if (!is_5d3())
    {
        NotifyBox(3000, "cm_wr %s: 5D3 1.1.3 only", S->name);
        return;
    }
    if (running || RECORDING || !lv || !is_movie_mode())
    {
        NotifyBox(3000, "cm_wr %s: need movie mode LiveView, not recording", S->name);
        return;
    }
    stage3b_run(S, 0);
}

static void start_set(int k)
{
    if (running) return;
    set_req = k;
    run_in_separate_task(stage3b_task, 0);
}
static MENU_SELECT_FUNC(s3b1_select) { start_set(0); }
static MENU_SELECT_FUNC(s3b2_select) { start_set(1); }
static MENU_SELECT_FUNC(s3b3_select) { start_set(2); }

static void stage3a_task(void)
{
    if (!is_5d3())
    {
        NotifyBox(3000, "cm_wr 3a: 5D3 1.1.3 only");
        return;
    }
    if (running || RECORDING || !lv || !is_movie_mode())
    {
        NotifyBox(3000, "cm_wr 3a: need movie mode LiveView, not recording");
        return;
    }
    uint32_t raw = shamem_read(REG_RAW);
    uint32_t r0 = shamem_read(REG_WIN_START);
    uint32_t d0 = shamem_read(REG_WIN_END);
    int from = -1;
    for (int i = 0; i < 2; i++)
        if (r0 == win_pairs[i][0] && d0 == win_pairs[i][1]) from = i;

    char text[512];
    if (raw != RAW_5D3_X5 || from < 0)
    {
        int len = snprintf(text, sizeof(text),
            "# cm_wr v1 stage 3a: NOT run (raw=%08x start=%08x end=%08x zoom=%d)\n"
            "# needs x5 (056601eb) and a known Canon pair; nothing written\n",
            raw, r0, d0, lv_dispsize);
        const char * n = save_text(text, len);
        snprintf(last_status, sizeof(last_status), "%s: not run (see log)", n);
        NotifyBox(6000, "cm_wr 3a: not x5 or unknown window, nothing written");
        return;
    }
    int to = 1 - from;
    uint32_t tr = win_pairs[to][0], td = win_pairs[to][1];

    running = 1;
    int t0 = efic_temp;
    int steps = 0, writes = 0, canon_rewrites = 0, left_mode = 0;
    int left_s = TEST_MS / 1000;
    for (int t = 0, tick = 0; t < TEST_MS; t += STEP_MS, tick++)
    {
        if (!lv || RECORDING || shamem_read(REG_RAW) != RAW_5D3_X5) { left_mode = 1; break; }
        steps++;
        if (shamem_read(REG_WIN_START) != tr || shamem_read(REG_WIN_END) != td)
        {
            if (writes) canon_rewrites++;
            EngDrvOutLV(REG_WIN_START, tr);
            EngDrvOutLV(REG_WIN_END, td);
            writes++;
        }
        if (tick == 1000 / STEP_MS) tick = 0;
        if (tick == 0)
            NotifyBox(1500, "cm_wr 3a: %ds, writes %d", left_s--, writes);
        msleep(STEP_MS);
    }

    /* put Canon's own pair back (only while still in the same x5 mode) */
    int restored = 0;
    if (lv && shamem_read(REG_RAW) == RAW_5D3_X5)
    {
        EngDrvOutLV(REG_WIN_START, r0);
        EngDrvOutLV(REG_WIN_END, d0);
        restored = 1;
    }
    int t1 = efic_temp;
    running = 0;

    int len = snprintf(text, sizeof(text),
        "# cm_wr v2 stage 3a: window pair %s -> %s\n"
        "start %08x/%08x -> %08x/%08x\n"
        "steps=%d writes=%d canon_rewrites=%d left_mode=%d restored=%d efic_temp_raw=%d->%d\n",
        pair_name[from], pair_name[to], r0, d0, tr, td, steps, writes, canon_rewrites, left_mode, restored, t0, t1);
    const char * n = save_text(text, len);
    snprintf(last_status, sizeof(last_status), "%s %s->%s w%d cr%d", n, pair_name[from], pair_name[to], writes, canon_rewrites);
    NotifyBox(6000, "cm_wr 3a: %s", last_status);
}

/* ---- v4: stage E1a, preview output EDMAC size (5D3 1.1.3) ---- */
#define EH_ADDR     0x13734
#define EH_I0       0xE92D4030   /* PUSH {R4,R5,LR} */
#define EH_I1       0xE1A04000   /* MOV R4, R0 */
#define RAW_5D3_X1  0x0528011B
#define E1_MS       30000
#define E1_MAXCALL  32
#define E1_MAXSNAP  32

/* { channel, caller, Canon x5 bytes, Canon 1080p x1 bytes } - firmware selected via cm_fw */
#define e1_map (cm_fw->e1_map)
struct e1call { uint32_t ch, in, out, raw, caller; };
static struct e1call e1c[E1_MAXCALL];
static volatile int e1_active = 0, e1_n = 0, e1_replaced = 0, e1_passed = 0;
static volatile int e1_done_mask = 0;   /* bit k: e1_map[k] replaced at least once */
/* v34: guard limit per channel (stage 4: Canon x1 bytes; R1: twice that, the L0 slot is doubled) */
static uint32_t e1_cap[2] = { 0x3DE000, 0x376E00 };
static int e1_hooked = 0;

/* v13: e1_guard = 1 -> stage 4 memory guard: never replaces Canon's request unless it is larger than
 * Canon's own 1080p x1 bytes for that channel (slot capacity ~5 MB, logs/20261017-5D3-edmac + ROM map);
 * then it is clamped to the x1 bytes and counted as an overflow */
static volatile int e1_guard = 0, e1_overflow = 0;

static void e1_hook(uint32_t * regs, uint32_t * stack, uint32_t pc)
{
    if (!e1_active) return;
    uint32_t caller = PATCH_HOOK_CALLER();
    uint32_t in = regs[1];
    uint32_t raw = shamem_read(REG_RAW);
    uint32_t old = cli();
    for (int k = 0; k < 2; k++)
    {
        if (e1_guard)
        {
            if (regs[0] == e1_map[k][0] && in > e1_cap[k])
            {
                regs[1] = e1_cap[k];
                e1_overflow++;
            }
        }
        else if (regs[0] == e1_map[k][0] && caller == e1_map[k][1] && in == e1_map[k][2])
        {
            regs[1] = e1_map[k][3];
            e1_done_mask |= 1 << k;
        }
    }
    if (regs[1] != in) e1_replaced++; else e1_passed++;
    int i = e1_n;
    if (i < E1_MAXCALL)
    {
        e1c[i].ch = regs[0]; e1c[i].in = in; e1c[i].out = regs[1];
        e1c[i].raw = raw; e1c[i].caller = caller;
        e1_n = i + 1;
    }
    sei(old);
}

/* installs the hook once (it stays, inert while !e1_active); 0 = not hooked, nothing changed */
static int e1_install(const char * tag)
{
    if (e1_hooked) return 1;
    if (MEM(EH_ADDR) != EH_I0 || MEM(EH_ADDR + 4) != EH_I1)
    {
        NotifyBox(6000, "cm_wr %s: helper code unexpected (cm_prev loaded?), not hooking", tag);
        return 0;
    }
    int err = patch_hook_function(EH_ADDR, EH_I0, e1_hook, "cm_wr: E1 EDMAC size");
    if (err)
    {
        NotifyBox(6000, "cm_wr %s: hook failed (%x), nothing changed", tag, err);
        return 0;
    }
    e1_hooked = 1;
    return 1;
}

static const uint32_t e1_regs[6] = { 0xC0F2630C, 0xC0F26310, 0xC0F26314, 0xC0F2640C, 0xC0F26410, 0xC0F26414 };

static void e1a_task(void)
{
    if (!is_5d3())
    {
        NotifyBox(3000, "cm_wr E1a: 5D3 1.1.3 only");
        return;
    }
    if (running || RECORDING || !lv || !is_movie_mode())
    {
        NotifyBox(3000, "cm_wr E1a: need movie mode LiveView, not recording");
        return;
    }
    uint32_t raw0 = shamem_read(REG_RAW);
    if (raw0 != RAW_5D3_X1)
    {
        NotifyBox(6000, "cm_wr E1a: start in 1080p x1 (raw %x), nothing done", raw0);
        return;
    }
    if (!e1_install("E1a")) return;

    running = 1;
    static uint32_t snap[E1_MAXSNAP][7];
    int ns = 0, snap_full = 0, left_lv = 0;
    int t0 = efic_temp;
    e1_n = 0; e1_replaced = 0; e1_passed = 0;
    e1_active = 1;
    int left_s = E1_MS / 1000;
    for (int t = 0, tick = 0; t < E1_MS; t += STEP_MS, tick++)
    {
        if (!lv || RECORDING) { left_lv = 1; break; }
        uint32_t v[7];
        v[0] = shamem_read(REG_RAW);
        for (int j = 0; j < 6; j++) v[j + 1] = shamem_read(e1_regs[j]);
        int k;
        for (k = 0; k < ns; k++)
        {
            int same = 1;
            for (int j = 0; j < 7; j++) if (snap[k][j] != v[j]) same = 0;
            if (same) break;
        }
        if (k == ns)
        {
            if (ns < E1_MAXSNAP) { for (int j = 0; j < 7; j++) snap[ns][j] = v[j]; ns++; }
            else snap_full = 1;
        }
        if (tick == 1000 / STEP_MS) tick = 0;
        if (tick == 0)
            NotifyBox(1500, "cm_wr E1a: %ds, go x5 now; replaced %d", left_s--, e1_replaced);
        msleep(STEP_MS);
    }
    e1_active = 0;
    int t1 = efic_temp;
    running = 0;

    static char text[4096];
    int len = snprintf(text, sizeof(text),
        "# cm_wr v4 stage E1a: preview EDMAC ch13/14 x5 bytes -> Canon x1 bytes (hook 0x13734)\n"
        "replaced=%d passed=%d calls_logged=%d left_lv=%d efic_temp_raw=%d->%d\n"
        "# call ch bytes_in bytes_out raw caller\n",
        e1_replaced, e1_passed, e1_n, left_lv, t0, t1);
    for (int i = 0; i < e1_n; i++)
        len += snprintf(text + len, sizeof(text) - len, "call %02x %08x %08x %08x %08x\n",
            e1c[i].ch, e1c[i].in, e1c[i].out, e1c[i].raw, e1c[i].caller);
    len += snprintf(text + len, sizeof(text) - len, "# snap raw c0f2630c c0f26310 c0f26314 c0f2640c c0f26410 c0f26414%s\n",
        snap_full ? " (table full)" : "");
    for (int k = 0; k < ns; k++)
        len += snprintf(text + len, sizeof(text) - len, "snap %08x %08x %08x %08x %08x %08x %08x\n",
            snap[k][0], snap[k][1], snap[k][2], snap[k][3], snap[k][4], snap[k][5], snap[k][6]);
    const char * n = save_text(text, len);
    snprintf(last_status, sizeof(last_status), "%s E1a rep%d pass%d", n, e1_replaced, e1_passed);
    NotifyBox(8000, "cm_wr E1a: %s. Restore: x1 then x5", last_status);
}

static MENU_SELECT_FUNC(e1a_select)
{
    if (running) return;
    run_in_separate_task(e1a_task, 0);
}

/* v7: read-only sampler of EDMAC channel state/pointer (MEM(base), MEM(base+8): same reads as
 * ML edmac.c edmac_get_state / edmac_get_pointer), every 10 ms while smp_on. Question: in the 3b-3
 * freeze, which channel stops moving? Candidates from ED5D3_00..02: 0x20 (x1 0xF0 lines, x5 0xF),
 * 0x03 / 0x0B (sized in one mode only), 0x13 / 0x14 (preview out), 0x18 (HIV). */
/* v8: + 0x05 (LV VRAM, ML 5D3 REG_EDMAC_WRITE_LV_ADDR C0F04508) and 0x0A (HD buffer, C0F04A08);
 * pages = bitmap of 4 MB pages 0x08000000..0x14000000 the pointer was seen in (slot use) */
#define SMP_NCH 8
static const uint32_t smp_ch[SMP_NCH] = { 0x03, 0x05, 0x0A, 0x0B, 0x13, 0x14, 0x18, 0x20 };
struct smp { uint32_t n, moves, last, pmin, pmax, st_first, st_last, st_changes, pages[2]; };
static struct smp smp_tab[3][SMP_NCH];
/* LCD display buffer pointer: ML 5D3 consts.h YUV422_LV_BUFFER_DISPLAY_ADDR = MEM(0x246a4+0x11c) */
#define DISP_ADDR_PTR (0x246a4 + 0x11c)
#define DISP_MAX 8
struct dsp { uint32_t n, changes, last, nv, v[DISP_MAX]; };
static struct dsp dsp_tab[3];

/* v9: ch 0x05 (LV VRAM) / 0x0A (HD) size shadow words base+0x0C..0x20, distinct tuples per phase,
 * and where the ch 0x05 pointer sits inside the LV buffer (ML 5D3 consts.h YUV422_LV_BUFFER_1..3,
 * 0x410000 apart; LCD frame 720x480x2 = 0xA8C00) */
#define SZ_NW   6
#define SZ_MAX  4
static const uint32_t sz_ch[2] = { 0x05, 0x0A };
struct szt { uint32_t nv, v[SZ_MAX][SZ_NW]; int full; };
static struct szt sz_tab[3][2];
static const uint32_t lvbuf[3] = { 0x15207800, 0x15617800, 0x15A27800 };
#define OFS_BINS 17      /* 16 x 64 KB, last = >= 1 MB */
struct ofs { uint32_t below, at_frame_end, max, bins[OFS_BINS]; };
static struct ofs ofs_tab[3];
static volatile int smp_on = 0, smp_running = 0;

static uint32_t smp_base(uint32_t ch)
{
    static const uint32_t bases[3] = { 0xC0F04000, 0xC0F26000, 0xC0F30000 };
    return bases[ch >> 4] + ((ch & 0xF) << 8);
}

static void smp_task(void)
{
    smp_running = 1;
    while (smp_on)
    {
        int ph = smp_phase;
        if (ph >= 0 && ph < 3)
            for (int c = 0; c < SMP_NCH; c++)
            {
                uint32_t b = smp_base(smp_ch[c]);
                uint32_t st = MEM(b);
                uint32_t p = MEM(b + 8);
                struct smp * m = &smp_tab[ph][c];
                if (m->n == 0) { m->pmin = m->pmax = m->last = p; m->st_first = m->st_last = st; }
                else
                {
                    if (p != m->last) m->moves++;
                    if (st != m->st_last) m->st_changes++;
                    if (p < m->pmin) m->pmin = p;
                    if (p > m->pmax) m->pmax = p;
                    m->last = p; m->st_last = st;
                }
                uint32_t pg = ((p & 0x1FFFFFFF) - 0x08000000) >> 22;
                if ((p & 0x1FFFFFFF) >= 0x08000000 && pg < 48) m->pages[pg >> 5] |= 1u << (pg & 31);
                m->n++;
            }
        if (ph >= 0 && ph < 3)
        {
            for (int c = 0; c < 2; c++)
            {
                uint32_t b = smp_base(sz_ch[c]);
                uint32_t w[SZ_NW];
                for (int j = 0; j < SZ_NW; j++) w[j] = shamem_read(b + 0x0C + 4 * j);
                struct szt * z = &sz_tab[ph][c];
                int k;
                for (k = 0; k < (int)z->nv; k++)
                {
                    int same = 1;
                    for (int j = 0; j < SZ_NW; j++) if (z->v[k][j] != w[j]) same = 0;
                    if (same) break;
                }
                if (k == (int)z->nv)
                {
                    if (z->nv < SZ_MAX) { for (int j = 0; j < SZ_NW; j++) z->v[z->nv][j] = w[j]; z->nv++; }
                    else z->full = 1;
                }
            }
            uint32_t p5 = MEM(smp_base(0x05) + 8) & 0x1FFFFFFF;
            struct ofs * o = &ofs_tab[ph];
            int bi = -1;
            for (int k = 0; k < 3; k++) if (p5 >= lvbuf[k]) bi = k;
            if (bi < 0) o->below++;
            else
            {
                uint32_t off = p5 - lvbuf[bi];
                if (off == 0xA8C00) o->at_frame_end++;
                if (off > o->max) o->max = off;
                uint32_t bn = off >> 16;
                o->bins[bn < OFS_BINS - 1 ? bn : OFS_BINS - 1]++;
            }
        }
        if (ph >= 0 && ph < 3)
        {
            uint32_t d = MEM(DISP_ADDR_PTR);
            struct dsp * q = &dsp_tab[ph];
            if (q->n && d != q->last) q->changes++;
            q->last = d;
            int k;
            for (k = 0; k < (int)q->nv; k++) if (q->v[k] == d) break;
            if (k == (int)q->nv && q->nv < DISP_MAX) q->v[q->nv++] = d;
            q->n++;
        }
        msleep(10);
    }
    smp_running = 0;
}

/* stage E1b: E1a (EDMAC x1 size in x5) + stage 3b-3 (69 preview regs + window -> Canon x1)
 * in the same x5 session. 3b-3 only starts when x5 raw AND both EDMAC shadows hold the x1 size. */
#define EDMAC13_X1_XN 0x3DD
#define EDMAC14_X1_XN 0x376

static void e1b_task(void)
{
    if (!is_5d3())
    {
        NotifyBox(3000, "cm_wr E1b: 5D3 1.1.3 only");
        return;
    }
    if (running || RECORDING || !lv || !is_movie_mode())
    {
        NotifyBox(3000, "cm_wr E1b: need movie mode LiveView, not recording");
        return;
    }
    uint32_t raw0 = shamem_read(REG_RAW);
    if (raw0 != RAW_5D3_X1)
    {
        NotifyBox(6000, "cm_wr E1b: start in 1080p x1 (raw %x), nothing done", raw0);
        return;
    }
    if (!e1_install("E1b")) return;

    running = 1;
    e1_n = 0; e1_replaced = 0; e1_passed = 0; e1_done_mask = 0;
    e1_active = 1;
    int ready = 0;
    uint32_t raw = 0, x13 = 0, x14 = 0;
    int left_s = E1_MS / 1000;
    for (int t = 0, tick = 0; t < E1_MS; t += STEP_MS, tick++)
    {
        if (!lv || RECORDING) break;
        raw = shamem_read(REG_RAW);
        x13 = shamem_read(0xC0F2630C);
        x14 = shamem_read(0xC0F2640C);
        /* v6: the x1 values stay in the shadow until Canon's x5 setup runs, so the hook must
         * have replaced BOTH channels in this run (WR5D3_12: raw was x5 before the helper call) */
        if (raw == RAW_5D3_X5 && e1_done_mask == 3 && x13 == EDMAC13_X1_XN && x14 == EDMAC14_X1_XN) { ready = 1; break; }
        if (tick == 1000 / STEP_MS) tick = 0;
        if (tick == 0)
            NotifyBox(1500, "cm_wr E1b: %ds, go x5 now; replaced %d", left_s--, e1_replaced);
        msleep(STEP_MS);
    }

    static char pre[1024];
    int len = snprintf(pre, sizeof(pre),
        "# cm_wr v9 stage E1b: E1a (EDMAC x1 bytes in x5) + 3b-3\n"
        "ready=%d raw=%08x c0f2630c=%08x c0f2640c=%08x replaced=%d passed=%d mask=%d\n",
        ready, raw, x13, x14, e1_replaced, e1_passed, e1_done_mask);
    for (int i = 0; i < e1_n; i++)
        len += snprintf(pre + len, sizeof(pre) - len, "call %02x %08x %08x %08x %08x\n",
            e1c[i].ch, e1c[i].in, e1c[i].out, e1c[i].raw, e1c[i].caller);

    if (!ready)
    {
        e1_active = 0;
        running = 0;
        const char * n = save_text(pre, len);
        snprintf(last_status, sizeof(last_status), "%s E1b not ready", n);
        NotifyBox(8000, "cm_wr E1b: x5 with x1 EDMAC not seen, no preview regs written. x1 then x5");
        return;
    }

    /* hook stays active during 3b-3, in case Canon sets the EDMAC up again in x5;
     * settle so Canon's x5 setup (preview regs) is finished before 3b-3 checks them.
     * v7: the settle time is also the sampler baseline (phase 0: x5 + x1 EDMAC, Canon x5 regs) */
    memset(smp_tab, 0, sizeof(smp_tab));
    memset(dsp_tab, 0, sizeof(dsp_tab));
    memset(sz_tab, 0, sizeof(sz_tab));
    memset(ofs_tab, 0, sizeof(ofs_tab));
    smp_phase = 0;
    smp_on = 1;
    run_in_separate_task(smp_task, 0);
    msleep(2000);
    int n_before = e1_n;
    running = 0;
    stage3b_run(&sets[2], pre);
    /* phase 2 (after restore) for 3 s more, while still in x5 */
    msleep(3000);
    e1_active = 0;
    smp_on = 0;
    for (int i = 0; i < 20 && smp_running; i++) msleep(20);

    /* helper calls during/after 3b-3 + EDMAC shadows at the end (WR5D3_12 missed these) */
    static char post[8192];
    len = snprintf(post, sizeof(post),
        "# cm_wr v9 stage E1b, after 3b-3: raw=%08x c0f2630c=%08x c0f2640c=%08x replaced=%d passed=%d\n",
        shamem_read(REG_RAW), shamem_read(0xC0F2630C), shamem_read(0xC0F2640C), e1_replaced, e1_passed);
    for (int i = n_before; i < e1_n; i++)
        len += snprintf(post + len, sizeof(post) - len, "call %02x %08x %08x %08x %08x\n",
            e1c[i].ch, e1c[i].in, e1c[i].out, e1c[i].raw, e1c[i].caller);
    len += snprintf(post + len, sizeof(post) - len,
        "# EDMAC samples (10 ms; phase 0 = x5 before writes, 1 = 3b-3 written, 2 = after restore)\n"
        "# smp phase ch n moves pmin pmax state_first state_last state_changes pages4MB(hi lo, bit0 = 0x08000000)\n");
    for (int ph = 0; ph < 3; ph++)
        for (int c = 0; c < SMP_NCH; c++)
        {
            struct smp * m = &smp_tab[ph][c];
            len += snprintf(post + len, sizeof(post) - len, "smp %d %02x %d %d %08x %08x %08x %08x %d %04x%08x\n",
                ph, smp_ch[c], m->n, m->moves, m->pmin, m->pmax, m->st_first, m->st_last, m->st_changes,
                m->pages[1], m->pages[0]);
        }
    len += snprintf(post + len, sizeof(post) - len, "# disp phase n changes values (MEM 0x247c0 = LCD buffer)\n");
    for (int ph = 0; ph < 3; ph++)
    {
        struct dsp * q = &dsp_tab[ph];
        len += snprintf(post + len, sizeof(post) - len, "disp %d %d %d", ph, q->n, q->changes);
        for (int k = 0; k < (int)q->nv; k++)
            len += snprintf(post + len, sizeof(post) - len, " %08x", q->v[k]);
        len += snprintf(post + len, sizeof(post) - len, "\n");
    }
    len += snprintf(post + len, sizeof(post) - len, "# size phase ch shamem base+0C 10 14 18 1C 20\n");
    for (int ph = 0; ph < 3; ph++)
        for (int c = 0; c < 2; c++)
        {
            struct szt * z = &sz_tab[ph][c];
            for (int k = 0; k < (int)z->nv; k++)
                len += snprintf(post + len, sizeof(post) - len, "size %d %02x %08x %08x %08x %08x %08x %08x%s\n",
                    ph, sz_ch[c], z->v[k][0], z->v[k][1], z->v[k][2], z->v[k][3], z->v[k][4], z->v[k][5],
                    z->full ? " (more)" : "");
        }
    len += snprintf(post + len, sizeof(post) - len,
        "# ofs phase below at_A8C00 max bins(64KB x16, >=1MB)  (ch 0x05 pointer inside LV buffer)\n");
    for (int ph = 0; ph < 3; ph++)
    {
        struct ofs * o = &ofs_tab[ph];
        len += snprintf(post + len, sizeof(post) - len, "ofs %d %d %d %08x", ph, o->below, o->at_frame_end, o->max);
        for (int b = 0; b < OFS_BINS; b++)
            len += snprintf(post + len, sizeof(post) - len, " %d", o->bins[b]);
        len += snprintf(post + len, sizeof(post) - len, "\n");
    }
    save_text(post, len);
    NotifyBox(8000, "cm_wr E1b: %s. Restore: x1 then x5", last_status);
}

static MENU_SELECT_FUNC(e1b_select)
{
    if (running) return;
    run_in_separate_task(e1b_task, 0);
}

/* ---- v10: stage E3, SSLT set up with Canon's own x1 tables in x5, then 3b-3 (5D3 1.1.3) ----
 * SSLT driver start ROM 0xFF33E280 f(p1 0x11C, p2, p3 0x5C, r3, p5, p6), called once per zoom change
 * with ROM tables (E2, logs/20261017-5D3-e2): x1 from 0xFF4879F8 p1 0xFF94BFE4 p3 0xFF94BF88,
 * x5 from 0xFF48A2BC p1 0xFF94F8D0 p3 0xFF94F874. Only that exact x5 call gets the x1 p1/p3. */
#define SS_ADDR      (cm_fw->ss_addr)
#define SS_I0        0xE92D5FFF   /* PUSH {R0-R12,LR} */
#define SS_I1        0xE1A04000   /* MOV R4, R0 */
#define SS_X5_CALLER (cm_fw->ss_x5_caller)
#define SS_X5_P1     (cm_fw->ss_x5_p1)
#define SS_X5_P3     (cm_fw->ss_x5_p3)
#define SS_X1_P1     (cm_fw->ss_x1_p1)
#define SS_X1_P3     (cm_fw->ss_x1_p3)
static volatile int e3_active = 0, e3_replaced = 0, e3_passed = 0;
static int e3_hooked = 0;

/* v13: stage 4 gives SSLT RAM copies of the x5 tables instead (docs/STAGE4_SSLT_TYPE7.md) */
#define S4_P1_WORDS (0x11C / 4)
#define S4_P3_WORDS (0x5C / 4)
#define SS_X1_CALLER  (cm_fw->ss_x1_caller)
#define SS_720_CALLER (cm_fw->ss_720_caller)
#define SS_720_P1     (cm_fw->ss_720_p1)
#define SS_720_P3     (cm_fw->ss_720_p3)
static uint32_t s4_p1[S4_P1_WORDS] __attribute__((aligned(32)));
static uint32_t s4_p3[S4_P3_WORDS] __attribute__((aligned(32)));
static uint32_t x1_p1[S4_P1_WORDS] __attribute__((aligned(32)));
static uint32_t x1_p3[S4_P3_WORDS] __attribute__((aligned(32)));
static volatile int e3_mode = 0;   /* 0 = E3 (Canon x1 tables), 1 = stage 4 (x5 RAM), 2 = x1 tall 1x3 (RAM), 3 = 720p hfps 3x3 (RAM) */

static void e3_hook(uint32_t * regs, uint32_t * stack, uint32_t pc)
{
    if (!e3_active) return;
    uint32_t caller = PATCH_HOOK_CALLER();
    if (e3_mode == 2 && caller == SS_X1_CALLER && regs[0] == SS_X1_P1 && regs[2] == SS_X1_P3)
    {
        regs[0] = (uint32_t) x1_p1;
        regs[2] = (uint32_t) x1_p3;
        e3_replaced++;
    }
    else if (e3_mode == 3 && caller == SS_720_CALLER && regs[0] == SS_720_P1 && regs[2] == SS_720_P3)
    {
        regs[0] = (uint32_t) x1_p1;
        regs[2] = (uint32_t) x1_p3;
        e3_replaced++;
    }
    else if (e3_mode < 2 && caller == SS_X5_CALLER && regs[0] == SS_X5_P1 && regs[2] == SS_X5_P3)
    {
        if (e3_mode == 1)
        {
            regs[0] = (uint32_t) s4_p1;
            regs[2] = (uint32_t) s4_p3;
        }
        else
        {
            regs[0] = SS_X1_P1;
            regs[2] = SS_X1_P3;
        }
        e3_replaced++;
    }
    else e3_passed++;
}

static void e3_task(void)
{
    if (!is_5d3())
    {
        NotifyBox(3000, "cm_wr E3: 5D3 1.1.3 only");
        return;
    }
    if (running || RECORDING || !lv || !is_movie_mode())
    {
        NotifyBox(3000, "cm_wr E3: need movie mode LiveView, not recording");
        return;
    }
    uint32_t raw0 = shamem_read(REG_RAW);
    if (raw0 != RAW_5D3_X1)
    {
        NotifyBox(6000, "cm_wr E3: start in 1080p x1 (raw %x), nothing done", raw0);
        return;
    }
    if (!e3_hooked)
    {
        if (MEM(SS_ADDR) != SS_I0 || MEM(SS_ADDR + 4) != SS_I1)
        {
            NotifyBox(6000, "cm_wr E3: SSLT code unexpected (E2 run? restart camera), not hooking");
            return;
        }
        int err = patch_hook_function(SS_ADDR, SS_I0, e3_hook, "cm_wr: E3 SSLT x1 tables");
        if (err)
        {
            NotifyBox(6000, "cm_wr E3: hook failed (%x), nothing changed", err);
            return;
        }
        e3_hooked = 1;
    }

    running = 1;
    e3_mode = 0;
    e3_replaced = 0; e3_passed = 0;
    memset(smp_tab, 0, sizeof(smp_tab));
    memset(dsp_tab, 0, sizeof(dsp_tab));
    memset(sz_tab, 0, sizeof(sz_tab));
    memset(ofs_tab, 0, sizeof(ofs_tab));
    e3_active = 1;

    /* 1) wait (max 30 s) for x5 entered with the SSLT x1 tables */
    int entered = 0, left_s = 30;
    for (int t = 0, tick = 0; t < 30000; t += 10, tick++)
    {
        if (!lv || RECORDING) break;
        if (e3_replaced && shamem_read(REG_RAW) == RAW_5D3_X5) { entered = 1; break; }
        if (tick == 100) tick = 0;
        if (tick == 0) NotifyBox(1500, "cm_wr E3: %ds, go x5 now", left_s--);
        msleep(10);
    }
    /* 2) sampler on (phase 0 = x5 with SSLT x1, before 3b-3); wait (max 3 s) until Canon's x5
     *    preview values are all in place, then 3b-3 at once */
    int pre_ok = 0, wait_10ms = 0;
    if (entered)
    {
        smp_phase = 0;
        smp_on = 1;
        run_in_separate_task(smp_task, 0);
        for (wait_10ms = 0; wait_10ms < 300; wait_10ms++)
        {
            if (stage3b_pre_ok(&sets[2])) { pre_ok = 1; break; }
            msleep(10);
        }
    }

    /* v12: timing in x5 (FPS override test): Timer A, Timer B, HEAD3, HEAD4; fps = 24e6 / (A * B) */
    static const uint32_t tregs[4] = { 0xC0F06008, 0xC0F06014, 0xC0F0713C, 0xC0F07150 };
    uint32_t tv[2][4];
    for (int j = 0; j < 4; j++) tv[0][j] = shamem_read(tregs[j]);
    msleep(1000);
    for (int j = 0; j < 4; j++) tv[1][j] = shamem_read(tregs[j]);

    static char pre[768];
    int pre_len = snprintf(pre, sizeof(pre),
        "# cm_wr v12 stage E3: SSLT x1 tables (p1 %08x p3 %08x) in x5, then 3b-3\n"
        "entered=%d replaced=%d passed=%d pre_ok=%d wait_10ms=%d raw=%08x c0f2630c=%08x c0f2640c=%08x\n"
        "timing C0F06008 C0F06014 C0F0713C C0F07150: %08x %08x %08x %08x, 1 s later %08x %08x %08x %08x\n",
        SS_X1_P1, SS_X1_P3, entered, e3_replaced, e3_passed, pre_ok, wait_10ms,
        shamem_read(REG_RAW), shamem_read(0xC0F2630C), shamem_read(0xC0F2640C),
        tv[0][0], tv[0][1], tv[0][2], tv[0][3], tv[1][0], tv[1][1], tv[1][2], tv[1][3]);

    if (!pre_ok)
    {
        /* v11: which preview registers are not at Canon's x5 value with SSLT on the x1 tables */
        static char miss[4096];
        const struct wr_set * S = &sets[2];
        int ml = snprintf(miss, sizeof(miss), "%s", pre);
        ml += snprintf(miss + ml, sizeof(miss) - ml,
            "window %08x %08x (Canon x5 pairs a %08x/%08x, b %08x/%08x)\n"
            "# differ: reg current expected_x5 x1_target\n",
            shamem_read(REG_WIN_START), shamem_read(REG_WIN_END),
            win_pairs[0][0], win_pairs[0][1], win_pairs[1][0], win_pairs[1][1]);
        int nd = 0;
        for (int i = 0; i < S->n; i++)
        {
            uint32_t v = shamem_read(S->e[i].reg);
            if (S->e[i].pre != WIN_ANY && v != S->e[i].pre)
            {
                ml += snprintf(miss + ml, sizeof(miss) - ml, "differ %08x %08x %08x %08x%s\n",
                    S->e[i].reg, v, S->e[i].pre, S->e[i].target, v == S->e[i].target ? " (=x1)" : "");
                nd++;
            }
        }
        ml += snprintf(miss + ml, sizeof(miss) - ml, "# %d of %d differ\n", nd, S->n);
        e3_active = 0;
        smp_on = 0;
        running = 0;
        const char * n = save_text(miss, ml);
        snprintf(last_status, sizeof(last_status), "%s E3 not run", n);
        NotifyBox(8000, "cm_wr E3: no 3b-3 (see log). Restore: zoom x10 -> x1");
        return;
    }

    running = 0;
    stage3b_run(&sets[2], pre);
    msleep(3000);           /* phase 2: after restore */
    e3_active = 0;
    smp_on = 0;
    for (int i = 0; i < 20 && smp_running; i++) msleep(20);

    static char post[4096];
    int len = snprintf(post, sizeof(post),
        "# cm_wr v10 stage E3, end: raw=%08x c0f2630c=%08x c0f2640c=%08x replaced=%d passed=%d\n"
        "# EDMAC samples (phase 0 = x5 + SSLT x1 before 3b-3, 1 = 3b-3 written, 2 = after restore)\n"
        "# smp phase ch n moves pmin pmax state_first state_last state_changes pages4MB\n",
        shamem_read(REG_RAW), shamem_read(0xC0F2630C), shamem_read(0xC0F2640C), e3_replaced, e3_passed);
    for (int ph = 0; ph < 3; ph++)
        for (int c = 0; c < SMP_NCH; c++)
        {
            struct smp * m = &smp_tab[ph][c];
            len += snprintf(post + len, sizeof(post) - len, "smp %d %02x %d %d %08x %08x %08x %08x %d %04x%08x\n",
                ph, smp_ch[c], m->n, m->moves, m->pmin, m->pmax, m->st_first, m->st_last, m->st_changes,
                m->pages[1], m->pages[0]);
        }
    for (int ph = 0; ph < 3; ph++)
    {
        struct dsp * q = &dsp_tab[ph];
        len += snprintf(post + len, sizeof(post) - len, "disp %d %d %d\n", ph, q->n, q->changes);
    }
    save_text(post, len);
    NotifyBox(8000, "cm_wr E3: %s. Restore: zoom x10 -> x1", last_status);
}

static MENU_SELECT_FUNC(e3_select)
{
    if (running) return;
    run_in_separate_task(e3_task, 0);
}

/* ---- v13: stage 4, x5 raw with an x1-size 1:1 SSLT crop (docs/STAGE4_SSLT_TYPE7.md) ---- */
/* SSLT margin table (firmware selected via cm_fw) */
#define s4_tbl (cm_fw->s4_tbl)
/* v27: window = centre of the raw active area = what a centred recording contains (ML raw.c, 5D3 x5:
 * skip_left 146, skip_right 2, skip_top 60 -> active 146..3742 x 60..1380, centre 1944/720).
 * Even offsets keep the Bayer phase. (v13..v26: 1124/52 = Canon x5 "a" centre, Y 52 inside top OB.) */
#define S4_X      968             /* 1944 - 1948/2 = 970, rounded to 4 px */
#define S4_Y      76              /* 720 - 1288/2 */
#define S4_W      1948
#define S4_H      1288
#define S4_RAW_W  3744
#define S4_RAW_H  1380
#define S4_X_MIN  148
#define S4_X_MAX  1752            /* 3700 - 1948: avoids right-edge optical black border at 1796 */
#define S4_Y_MIN  60
#define S4_Y_MAX  (S4_RAW_H - S4_H)
#define S4_WIN_S  ((S4_Y << 16) | (S4_X >> 2))                         /* 0x004C00F2 */
#define S4_WIN_E  (((S4_Y + S4_H) << 16) | ((S4_X + S4_W) >> 2))       /* 0x055402D9 */
static void cm_live_disarm(void);
#define S4_MS     20000
/* v19: "4c long" = 4c for 90 s in x5 (same writes); watch when the early-boot streaks vanish and
 * compare with "Flicker Detect Complete" in cm_dmlog v7 */
#define S4_MS_LONG 90000
static volatile int s4_ms = S4_MS;
/* v20: read-only - Canon's flicker detection path (Flicker_SetPath 0xFF335618, engio list 0xFF92F1C4):
 * routing C0F37034 and the flicker block C0F08B50..B70; sampled at x5 entry, after 1 s and at the end.
 * The early-boot streaks coincide with this path being active (2026-10-18). */
static const uint32_t s4_flk_reg[] = { 0xC0F37034, 0xC0F08B50, 0xC0F08B54, 0xC0F08B58, 0xC0F08B5C,
                                       0xC0F08B60, 0xC0F08B64, 0xC0F08B68, 0xC0F08B6C, 0xC0F08B70 };
#define S4_NFLK (sizeof(s4_flk_reg) / sizeof(s4_flk_reg[0]))
static uint32_t s4_flk[3][S4_NFLK];
static void s4_flk_read(int k) { for (int i = 0; i < (int) S4_NFLK; i++) s4_flk[k][i] = shamem_read(s4_flk_reg[i]); }

/* v21: read-only dump of Canon's whole register shadow table. shamem_read (ROM 0xFF28C9C8) is
 * MEM(base | (addr & 0x3FFFF)): a plain 256 KB RAM table, so every index is a safe read.
 * File ML/LOGS/SH5D3_NN.BIN = snapshot at 1 s in x5, then snapshot at the end (2 x 256 KB,
 * word i = shamem_read(0xC0F00000 + 4*i), little endian). */
#define SH_WORDS  0x10000
#define SH_CHUNK  4096
static uint32_t sh_buf[SH_CHUNK];
static FILE * sh_file = 0;
static char sh_name[32] = "";

static void sh_open(void)
{
    uint32_t size;
    sh_name[0] = 0;
    for (int i = 0; i < 10000; i++)
    {
        snprintf(sh_name, sizeof(sh_name), "ML/LOGS/S5D3%04d.BIN", i);
        if (FIO_GetFileSize(sh_name, &size) != 0) break;
    }
    sh_file = FIO_CreateFile(sh_name);
    if (!sh_file) strcpy(sh_name, "none");
}

static void sh_snapshot(void)
{
    if (!sh_file) return;
    for (int base = 0; base < SH_WORDS; base += SH_CHUNK)
    {
        for (int i = 0; i < SH_CHUNK; i++) sh_buf[i] = shamem_read(0xC0F00000 + 4 * (base + i));
        FIO_WriteFile(sh_file, sh_buf, sizeof(sh_buf));
    }
}

static void sh_close(void)
{
    if (sh_file) { FIO_CloseFile(sh_file); sh_file = 0; }
}

/* window inside the x5 raw? (start/end words as C0F383D4/DC) */
static int s4_win_ok(uint32_t ws, uint32_t we)
{
    uint32_t x0 = (ws & 0xFFFF) << 2, y0 = ws >> 16, x1 = (we & 0xFFFF) << 2, y1 = we >> 16;
    return x0 < x1 && y0 < y1 && x1 <= S4_RAW_W && y1 <= S4_RAW_H;
}

static int s4_patched[4];
/* v14: variant 1 = stage 4b: p1 +0xCC..+0xD4 from the Canon x1 table (0x900, 0x900, 0x80) instead of
 * x5 (0xBA0, 0xBA0, 0x370); stage 4 showed they land in C0F3846C/849C/84CC (WR5D3_33) */
static volatile int s4_variant = 0;
/* v15: quiet = no NotifyBox while in x5 (stage 4b showed staircase white streaks that look like
 * NotifyBox text rows written with a wrong pitch, IMG_3606) */
static volatile int s4_quiet = 0;
/* v16: stage 4c = 4b quiet + Bilal's "Clear artifacts" for x5 LCD: the VRAM triple RAM clear width of
 * the x5 LCD row is 0 (no clear); crop_rec.c patches it to 0x5A0 (720 px * 2) on LCD output.
 * 5D3 1.1.3 address measured 2026-10-16: VRAM_PTH_StartTripleRamClearInALump[ff94f7e0] + 8 = 0xFF94F7E8. */
#define S4_CLEAR_ADDR (cm_fw->s4_clear_addr)
#define S4_CLEAR_OLD  0
#define S4_CLEAR_NEW  0x5A0
static volatile int s4_clear = 0;
/* v17: wh_x5 = keep p1 W/H at the x5 values (1168/784) and the input margins A_2/A_3 unpatched:
 * input stays 1168+2576 = 3744, 784+596 = 1380; only the crops A_9/A_11 change the processed size.
 * Needed because A_2 (0xFF994FE0) and the VRAM clear word (0xFF94F7E8) share D-cache index 63
 * (cache patch collision, stage 4c v16). */
static volatile int s4_wh_x5 = 0;
/* v18: stage 4d = 4c + C0F38154 at the Canon 1080p x1 value. Stage 4b/4c left 2 of the 71 preview regs at
 * x5: C0F38154 (x5 0x33, x1 0x133, WR5D3_37) is the candidate for the dark/high-ISO white streaks (Canon
 * x5 shows no dots at ISO 3200, DEFM runs every frame in our mode too). Written only while it reads the
 * Canon x5 value; the x5 value is written back at the end if still in x5. */
#define S4_R154      0xC0F38154
#define S4_R154_X5   0x33
#define S4_R154_X1   0x133
static volatile int s4_r154 = 0;

/* D-cache line index of a ROM address (ML cache_hacks.h: 8 KB, 32-byte lines, index bits 5..10) */
static int s4_cache_index(uint32_t a) { return (a >> 5) & 0x3F; }
static int s4_clear_patched = 0;

/* v34: R1 (approved 2026-10-18) = stage 4c + R0 move (L0 at region base + 16 MB, inside Canon's
 * unused buffer type 12) + L0 line slot 0x1000 -> 0x2000: p1 +0xCC / +0xD0 (L0 skip values that end up
 * in C0F3846C / C0F3849C, x1 0x900) + 0x1000. Same 1948x1288 picture expected. L0 then needs
 * 1280 x 0x2000 = 10 MB of the 16 MB free area. EDMAC guard limit doubled. */
#define R1_A1     0xFF165DAC
#define R1_A2     0xFF165FC0
#define R1_OLD1   0xE28C3402
#define R1_NEW1   0xE28C3401
#define R1_OLD2   0xE2822402
#define R1_NEW2   0xE2822401
static volatile int s4_r1 = 0;     /* 1 = R1, 2 = R1b */
/* v35: R1b = R1 + the two noise-reduction readers of the previous L0 frame (C0F3ABD8 block: C,
 * C0F3AC0C block: Y; same 0x34-byte layout, skip at +0x14) keep Canon's x5 skip 0xBA0 = 0x1000 - 0x460
 * (WR5D3_92, SH5D3_14) -> with 8 KB lines they read the wrong rows = the zebra overlay seen in R1
 * (IMG_3622). R1b sets both skips to 0x2000 - 0x460 = 0x1BA0 while in x5 (re-written if Canon
 * changes them). Reads 784 x 0x2000 = 6.4 MB from the 10 MB L0: inside the buffer. */
#define R1B_SKIP_C  0xC0F3ABEC
#define R1B_SKIP_Y  0xC0F3AC20
#define R1B_OLD     0xBA0
#define R1B_NEW     0x1BA0
static int s4_r1_patched = 0;
static int s4_l0cls(uint32_t a)
{
    a &= 0x3FFFFFFF;
    if ((a >= 0x0CE00000 && a < 0x0D300000) || (a >= 0x12000000 && a < 0x12500000)) return 1;
    if ((a >= 0x0BE00000 && a < 0x0CE00000) || (a >= 0x11000000 && a < 0x12000000)) return 2;
    return 0;
}

/* v42: F3 (approved 2026-10-18). Canon's LV mode table 0xFF92ED08 (8 entries x 0x98) is built from
 * parts: words [0..4] pre-processing (sensor, SSLT), [5..13] display path (VRAM path build, second pass,
 * resizers, LCD; [11] SndPas size, [12] VRAM param), [14..15] init/uninit. Canon itself mixes parts
 * (entries 5 and 7 = x1 pre-processing + other display paths). x1 [6] 0xFF4885B0 and x5 [6] 0xFF48ADE8
 * are the same code on different state structs (x1 0x325EC, x5 0x329DC) and different VRAM row tables
 * (state +0x10). F3 = stage 4b + x5 entry words [5..13] -> x1's, so Canon's own code builds the x1
 * display chain (routing, second pass, resizers) on x1's state while pre-processing stays x5.
 * Patched before x5 is entered, unpatched only after x5 was left (the display path is stopped by the
 * same x1 functions that started it). */
#define F3_X1   0xFF92ED08
#define F3_X5   0xFF92EE38
static const uint32_t f3_x1w[9] = { 0xFF4883B4, 0xFF4885B0, 0xFF488800, 0xFF488828, 0xFF488868, 0xFF4888B8, 0xFF4888E0, 0xFF488920, 0xFF4889B4 };
static const uint32_t f3_x5w[9] = { 0xFF48ABF0, 0xFF48ADE8, 0xFF48B038, 0xFF48B064, 0xFF48B0A4, 0xFF48B0F4, 0xFF48B11C, 0xFF48B15C, 0xFF48B1E8 };
static int f3_patched = 0;
static uint32_t f3_row_x1 = 0, f3_row_x5 = 0;

/* v43: F3 refusals are logged (v42 only showed a short message, nothing on the card) */
static void f3_refuse_log(const char * why, int err)
{
    static char t[1024];
    int n = snprintf(t, sizeof(t), "# cm_wr v43 F3 refused: %s (err %x)\nx1 state row %08x, x5 state row %08x\n# k x1_rom x1_expected x5_rom x5_expected\n",
        why, err, f3_row_x1, f3_row_x5);
    for (int k = 0; k < 9; k++)
        n += snprintf(t + n, sizeof(t) - n, "w%d %08x %08x %08x %08x\n", 5 + k,
            MEM(F3_X1 + 4 * (5 + k)), f3_x1w[k], MEM(F3_X5 + 4 * (5 + k)), f3_x5w[k]);
    const char * name = save_text(t, n);
    NotifyBox(10000, "cm_wr F3 refused: %s, see %s", why, name);
}

/* v52/v57/v58/v59/v60 F4: x5 second-pass message 0x302 gets Canon x1 pyramid/NR/Chb values (k1)
 * written to UNCACHED RAM (0x41F253A0), C0F11540 vertical scaling in 0x302 is kept at 1:1
 * (1, 1 -> C0F11548 = 0x80000000 passthrough) to eliminate C0F11540 scanlines, and 0x402 mode 5
 * table (0x41E4D634) is patched so C0F11300 downscales 1272 -> 480 (out_h=20, in_h=53) on the LCD path:
 * - F4A: 1904x1272 3x (box-centered, 20 s, screenshots at 3 s / 12 s; follows Canon zoom box).
 * - F4B: 1904x1272 3x (box-centered, 120 s free run until x10->x1; follows Canon zoom box).
 * - F4C: 1904x1272 3x (fixed raw-buffer centre X=968, Y=76, 20 s). */
#define F4_EEKO_K5   (cm_fw->eeko_k5 | 0x40000000u)
#define F4_EEKO_K5C  (cm_fw->eeko_k5)
#define F4_EEKO_402  (cm_fw->eeko_402 | 0x40000000u)
#define F4_EEKO_402C (cm_fw->eeko_402)
#define F4_402_STRIDE 0xE8
static const uint32_t f4_eeko_orig[14] = { 1, 1, 1, 1, 0, 0, 0, 0, 144, 107, 192, 143, 1, 1 };
static const uint32_t f4_eeko_tab[3][14] = {
    { 119, 72, 1, 1, 0, 0, 0, 0, 119, 107, 159, 143, 3, 3 },   /* F4A: 1904x1272 -> 1152x1272 + 0x402 53:20 V (box-center 20s) */
    { 119, 72, 1, 1, 0, 0, 0, 0, 119, 107, 159, 143, 3, 3 },   /* F4B: 1904x1272 -> 1152x1272 + 0x402 53:20 V (box-center 120s) */
    { 119, 72, 1, 1, 0, 0, 0, 0, 119, 107, 159, 143, 3, 3 },   /* F4C: 1904x1272 -> 1152x1272 + 0x402 53:20 V (raw-center 20s) */
};
static const uint32_t f4_hrec[3] = { 1272, 1272, 1272 };
static const char * const f4_name[3] = { "F4A", "F4B", "F4C" };
static volatile int f4_sub = 0;
static volatile int f4_eeko_patched = 0;
static volatile int f4_mod = 0;
static volatile int f4_seen = 0, f4_done = 0, f4_repl = 0, f4_kept = 0, f4_other = 0;
static uint32_t f4_msg[124];
static int f4_install(void);
/* v56 G1 (approved 2026-11-04): in 0xFF33C518 (0x302 template selector) the x5 branch
 * (S=8, Z=0x50000, 0xFF33C798 "add r1,r0,#0x9B0") jumps to Canon's own 1080p x1 branch
 * (0xFF33C600): template k1, output descriptor 0xFF92F378 (1904x1274), k = 1, NR class 0.
 * r0 (template base), r4, r6, r7, r8 = 0, fp = 1 are the same on both paths there. x10 untouched.
 * One I-cache patch (index 0x3C), undone at the end. docs: CLAUDE.md "A yolu". */
#define G1_ADDR     0xFF33C798
#define G1_OLD      0xE2801E9B      /* add r1, r0, #0x9B0 (k5) */
#define G1_NEW      0xEAFFFF98      /* b 0xFF33C600 */
static volatile int g1_patched = 0;
static void f4_set_402_m5(uint32_t h, uint32_t out_h, uint32_t in_h)
{
    static const int idx[4] = { 2, 3, 11, 47 };
    static const uint32_t v2_orig[4][2] = { { 45, 32 }, { 55, 48 }, { 97, 192 }, { 233, 384 } };
    static const uint32_t v2_1272[4][2] = { { 45, 53 }, { 110, 159 }, { 97, 318 }, { 233, 636 } };
    for (int s = 0; s < 4; s++)
    {
        uint32_t u = F4_EEKO_402 + s * F4_402_STRIDE, c = F4_EEKO_402C + s * F4_402_STRIDE;
        for (int k = 0; k < 4; k++) { MEM(u + 4 * idx[k]) = h; MEM(c + 4 * idx[k]) = h; }
        MEM(u + 16 * 4) = out_h; MEM(c + 16 * 4) = out_h;
        MEM(u + 17 * 4) = in_h;  MEM(c + 17 * 4) = in_h;
        uint32_t n2 = (h == 1272u) ? v2_1272[s][0] : v2_orig[s][0];
        uint32_t d2 = (h == 1272u) ? v2_1272[s][1] : v2_orig[s][1];
        MEM(u + 20 * 4) = n2; MEM(c + 20 * 4) = n2;
        MEM(u + 21 * 4) = d2; MEM(c + 21 * 4) = d2;
    }
}
static void s4_unpatch(void)
{
    if (g1_patched) { unpatch_memory(G1_ADDR); g1_patched = 0; }
    if (f4_eeko_patched)
    {
        for (int i = 0; i < 14; i++) { MEM(F4_EEKO_K5 + 4 * i) = f4_eeko_orig[i]; MEM(F4_EEKO_K5C + 4 * i) = f4_eeko_orig[i]; }
        f4_set_402_m5(768, 5, 8);
        f4_eeko_patched = 0;
    }
    f4_mod = 0;
    if (f3_patched) { for (int k = 0; k < 9; k++) unpatch_memory(F3_X5 + 4 * (5 + k)); f3_patched = 0; }
    for (int k = 0; k < 4; k++)
        if (s4_patched[k]) { unpatch_memory(s4_tbl[k][0]); s4_patched[k] = 0; }
    if (s4_clear_patched) { unpatch_memory(S4_CLEAR_ADDR); s4_clear_patched = 0; }
    if (s4_r1_patched) { unpatch_memory(R1_A2); unpatch_memory(R1_A1); s4_r1_patched = 0; }
}

/* v43: every stage 4 refusal also goes to the card (v42 F3 refused with only a short message) */
static char s4_refuse_buf[200];
#define S4_REFUSE(...) do { int _n = snprintf(s4_refuse_buf, sizeof(s4_refuse_buf) - 2, __VA_ARGS__); \
    if (_n < 0) _n = 0; if (_n > 196) _n = 196; NotifyBox(8000, "%s", s4_refuse_buf); \
    s4_refuse_buf[_n] = '\n'; save_text(s4_refuse_buf, _n + 1); } while (0)

/* v48: read-only. Which SSLT table pointers do Canon's live structures hold? The v47 scan
 * (W5D30010/11) found heap words that point at the ROM SSLT tables (x5: p1 0xFF94F8D0, p6 0xFF94F9EC);
 * the heap address changes between boots, so search 0x00100000..0x00200000 for the ROM tables
 * (x1/x5 p1, p3, p6) and for our RAM copies s4_p1 / s4_p3, at stage start (x1) and 1 s into x5.
 * If the x5 ROM p1 stays referenced while stage 4b runs, a consumer reads Canon's x5 sizes past
 * our copy (2nd-pass candidate). */
#define PP4_MAX 24
static uint32_t pp4_a[2][PP4_MAX], pp4_v[2][PP4_MAX];
static int pp4_n[2];
static void pp4_snap(int k)
{
    const uint32_t want[8] = { SS_X5_P1, SS_X5_P3, SS_X5_P1 + 0x11C, SS_X1_P1, SS_X1_P3, SS_X1_P1 + 0x11C,
                               (uint32_t)s4_p1, (uint32_t)s4_p3 };
    int n = 0;
    for (uint32_t a = 0x00100000; a < 0x00200000 && n < PP4_MAX; a += 4)
    {
        if (a >= (uint32_t)pp4_a && a < (uint32_t)pp4_a + sizeof(pp4_a) + sizeof(pp4_v)) continue;
        uint32_t v = MEM(a);
        for (int i = 0; i < 8; i++)
            if (v == want[i]) { pp4_a[k][n] = a; pp4_v[k][n] = v; n++; break; }
    }
    pp4_n[k] = n;
}
static const char * pp4_name(uint32_t v)
{
    if (v == SS_X5_P1) return "x5_p1(ROM)";
    if (v == SS_X5_P3) return "x5_p3(ROM)";
    if (v == SS_X5_P1 + 0x11C) return "x5_p6(ROM)";
    if (v == SS_X1_P1) return "x1_p1(ROM)";
    if (v == SS_X1_P3) return "x1_p3(ROM)";
    if (v == SS_X1_P1 + 0x11C) return "x1_p6(ROM)";
    if (v == (uint32_t)s4_p1) return "our_p1";
    if (v == (uint32_t)s4_p3) return "our_p3";
    return "?";
}

static void s4_task(void)
{
    if (!is_5d3())
    {
        S4_REFUSE("cm_wr S4: 5D3 1.1.3 only");
        return;
    }
    if (running || RECORDING || !lv || !is_movie_mode())
    {
        S4_REFUSE("cm_wr S4: need movie mode LiveView, not recording");
        return;
    }
    uint32_t raw0 = shamem_read(REG_RAW);
    if (raw0 != RAW_5D3_X1)
    {
        S4_REFUSE("cm_wr S4: start in 1080p x1 (raw %x), nothing done", raw0);
        return;
    }
    running = 1;
    cm_live_disarm();
    running = 0;

    /* templates must be exactly the Canon tables measured in E2 / read in Ghidra */
    if (MEM(SS_X1_P1 + 0xCC) != 0x900 || MEM(SS_X1_P1 + 0xD0) != 0x900 || MEM(SS_X1_P1 + 0xD4) != 0x80 ||
        MEM(SS_X5_P1) != 7 || MEM(SS_X5_P1 + 0x14) != 1168 || MEM(SS_X5_P1 + 0x1C) != 784 ||
        MEM(SS_X5_P1 + 0x28) != 1500 || MEM(SS_X5_P1 + 0x2C) != 300 ||
        MEM(SS_X5_P3 + 0x20) != 0x4AC || MEM(SS_X5_P3 + 0x24) != 0x318 ||
        MEM(SS_X5_P3 + 0x3C) != 0x4AC || MEM(SS_X5_P3 + 0x40) != 0x317 ||
        MEM(SS_X1_P3 + 0x20) != 0x79C || MEM(SS_X1_P3 + 0x24) != 0x508 ||
        MEM(SS_X1_P3 + 0x3C) != 0x79C || MEM(SS_X1_P3 + 0x40) != 0x507)
    {
        S4_REFUSE("cm_wr S4: SSLT tables unexpected, nothing done");
        return;
    }
    if (s4_clear && MEM(S4_CLEAR_ADDR) != S4_CLEAR_OLD)
    {
        S4_REFUSE("cm_wr S4c: VRAM clear value unexpected, nothing done");
        return;
    }
    for (int k = 0; k < 4; k++)
        if (MEM(s4_tbl[k][0]) != s4_tbl[k][1])
        {
            S4_REFUSE("cm_wr S4: margin table unexpected at %x, nothing done", s4_tbl[k][0]);
            return;
        }

    for (int i = 0; i < S4_P1_WORDS; i++) s4_p1[i] = MEM(SS_X5_P1 + 4 * i);
    if (!s4_wh_x5)
    {
        s4_p1[0x14 / 4] = 1920;
        s4_p1[0x1C / 4] = 1280;
    }
    uint32_t init_x = (s4_f1 == 5 && f4_sub < 2) ? 420 : S4_X;
    uint32_t init_y = S4_Y;
    s4_p1[0x28 / 4] = init_x;
    s4_p1[0x2C / 4] = init_y;
    if (s4_variant == 1)
        for (int i = 0; i < 3; i++) s4_p1[0xCC / 4 + i] = MEM(SS_X1_P1 + 0xCC + 4 * i);
    if (s4_r1)
    {
        if (MEM(R1_A1) != R1_OLD1 || MEM(R1_A2) != R1_OLD2 || s4_p1[0xCC / 4] != 0x900 || s4_p1[0xD0 / 4] != 0x900)
        {
            S4_REFUSE("cm_wr R1: dispenser code or p1 skip unexpected, nothing done");
            return;
        }
        s4_p1[0xCC / 4] += 0x1000;
        s4_p1[0xD0 / 4] += 0x1000;
    }
    for (int i = 0; i < S4_P3_WORDS; i++) s4_p3[i] = MEM(SS_X5_P3 + 4 * i);
    s4_p3[0x20 / 4] = 0x79C;
    s4_p3[0x24 / 4] = 0x508;
    s4_p3[0x3C / 4] = 0x79C;
    s4_p3[0x40 / 4] = 0x507;

    if (!e3_hooked)
    {
        if (MEM(SS_ADDR) != SS_I0 || MEM(SS_ADDR + 4) != SS_I1)
        {
            S4_REFUSE("cm_wr S4: SSLT code unexpected (E2 run? restart camera), not hooking");
            return;
        }
        int err = patch_hook_function(SS_ADDR, SS_I0, e3_hook, "cm_wr: SSLT tables");
        if (err)
        {
            S4_REFUSE("cm_wr S4: hook failed (%x), nothing changed", err);
            return;
        }
        e3_hooked = 1;
    }
    if (!e1_install("S4")) return;
    e1_cap[0] = s4_r1 ? 2 * e1_map[0][3] : e1_map[0][3];
    e1_cap[1] = s4_r1 ? 2 * e1_map[1][3] : e1_map[1][3];

    /* no two ROM patches on one D-cache line index (one lockable line per index) */
    {
        uint32_t addrs[5]; int na = 0;
        for (int k = 0; k < 4; k++) if (!(s4_wh_x5 && k < 2)) addrs[na++] = s4_tbl[k][0];
        if (s4_clear) addrs[na++] = S4_CLEAR_ADDR;
        for (int i = 0; i < na; i++)
            for (int j = i + 1; j < na; j++)
                if (s4_cache_index(addrs[i]) == s4_cache_index(addrs[j]) && (addrs[i] & ~31) != (addrs[j] & ~31))
                {
                    S4_REFUSE("cm_wr S4: cache index clash %x / %x, nothing done", addrs[i], addrs[j]);
                    return;
                }
    }
    for (int k = 0; k < 4; k++)
    {
        if (s4_wh_x5 && k < 2) continue;    /* input margins stay Canon */
        int err = patch_memory(s4_tbl[k][0], s4_tbl[k][1], s4_tbl[k][2], "cm_wr: S4 SSLT type 7 margins");
        if (err)
        {
            s4_unpatch();
            S4_REFUSE("cm_wr S4: patch failed (%x), all undone", err);
            return;
        }
        s4_patched[k] = 1;
    }
    if (s4_clear)
    {
        int err = patch_memory(S4_CLEAR_ADDR, S4_CLEAR_OLD, S4_CLEAR_NEW, "cm_wr: S4c clear VRAM x5 LCD");
        if (err)
        {
            s4_unpatch();
            S4_REFUSE("cm_wr S4c: clear patch failed (%x), all undone", err);
            return;
        }
        s4_clear_patched = 1;
    }
    if (s4_r1)
    {
        int ea = patch_instruction(R1_A1, R1_OLD1, R1_NEW1, "cm_wr: R1 L0 +16MB (A/B toggle)");
        int eb = ea ? -1 : patch_instruction(R1_A2, R1_OLD2, R1_NEW2, "cm_wr: R1 L0 +16MB");
        if (ea || eb)
        {
            if (!ea) unpatch_memory(R1_A1);
            s4_unpatch();
            S4_REFUSE("cm_wr R1: dispenser patch failed (%x %x), all undone", ea, eb);
            return;
        }
        s4_r1_patched = 1;
    }
    if (s4_f1 == 4)
    {
        int ok = 1;
        for (int k = 0; k < 9; k++)
            if (MEM(F3_X1 + 4 * (5 + k)) != f3_x1w[k] || MEM(F3_X5 + 4 * (5 + k)) != f3_x5w[k]) ok = 0;
        f3_row_x1 = MEM(0x325EC + 0x10);
        f3_row_x5 = MEM(0x329DC + 0x10);
        if (!ok || !f3_row_x1)
        {
            s4_unpatch();
            f3_refuse_log(!ok ? "mode table words unexpected" : "x1 state row is 0", 0);
            return;
        }
        for (int k = 0; k < 9; k++)
        {
            int err = patch_memory(F3_X5 + 4 * (5 + k), f3_x5w[k], f3_x1w[k], "cm_wr: F3 x1 display path in x5");
            if (err)
            {
                for (int j = 0; j < k; j++) unpatch_memory(F3_X5 + 4 * (5 + j));
                s4_unpatch();
                f3_refuse_log(k == 0 ? "patch of word 5 failed" : "patch failed after some words (undone)", err);
                return;
            }
        }
        f3_patched = 1;
    }
    if (s4_f1 == 5)
    {
        int err = f4_install();
        if (err)
        {
            s4_unpatch();
            S4_REFUSE("cm_wr F4: refused (%d: 1 templates/Eeko unexpected, 2 IPC code unexpected, 3 hook failed), all undone", err);
            return;
        }
        for (int i = 0; i < 14; i++)
        {
            MEM(F4_EEKO_K5 + 4 * i) = f4_eeko_tab[f4_sub][i];
            MEM(F4_EEKO_K5C + 4 * i) = f4_eeko_tab[f4_sub][i];
        }
        f4_set_402_m5(1272, 20, 53);
        f4_eeko_patched = 1;
        f4_seen = f4_done = f4_repl = f4_kept = f4_other = 0;
        f4_mod = 1;
    }
    if (s4_f1 == 6)
    {
        /* Canon x1 branch must be exactly as in our ROM dump, x5 branch as expected */
        static const uint32_t g1_x1[8] = { 0xE2801E1F, 0xE1A00006, 0xEB337F55, 0xE59F00FC,
                                           0xE58401F8, 0xE584B20C, 0xE586B000, 0xEAFFFFF5 };
        int ok = MEM(G1_ADDR) == G1_OLD && MEM(0xFF33C790) == 0xE3510805 && MEM(0xFF33C5F8) == 0xE1C781B0
              && ((G1_ADDR >> 5) & 0x3F) != ((SS_ADDR >> 5) & 0x3F);
        for (int i = 0; i < 8; i++) if (MEM(0xFF33C600 + 4 * i) != g1_x1[i]) ok = 0;
        if (!ok)
        {
            s4_unpatch();
            S4_REFUSE("cm_wr G1: ROM code not as expected (%08x), nothing done", MEM(G1_ADDR));
            return;
        }
        int err = patch_instruction(G1_ADDR, G1_OLD, G1_NEW, "cm_wr: G1 x5 -> x1 2nd-pass template");
        if (err)
        {
            s4_unpatch();
            S4_REFUSE("cm_wr G1: patch failed (%x), all undone", err);
            return;
        }
        g1_patched = 1;
    }

    running = 1;
    int f1_pre = 0, f1_writes = 0, f1_rewrites = 0, f1_restored = 0, f1_done = 0;
    int f1b_pre = 0, f1b_writes = 0;
    memset(f1_mine, 0, sizeof(f1_mine)); memset(f1b_mine, 0, sizeof(f1b_mine));
    memset(f2_mine, 0, sizeof(f2_mine)); f2_nbad = 0;
    int f2_pre = 0, f2_writes = 0, f2_rewrites = 0, f2_restored = 0, f2_done = 0;
    uint32_t r1c13[3] = {0}, r1c1b[3] = {0};
    int r1b_writes = 0, r1b_other = 0;
    uint32_t r1b_seen = 0;
    static uint32_t r1regs[16];
    /* v38: the NR / recombination stage (C0F39xxx..C0F3Bxxx, 83 registers) stayed at Canon x5 sizes in
     * 4c (SH5D3_15 vs x1 SH5D3_17): C0F3A04C x1 0x04FF079B / x5 0x030F04AB, C0F3ABE0 x1 0x04FF0700 / x5 0x030F0460 */
    static const uint32_t dsreg[4] = { 0xC0F3A04C, 0xC0F3ABE0, 0xC0F39F18, 0xC0F3ADB4 };
    static uint32_t dsval[4];
    static const uint32_t r1reg[16] = { 0xC0F3844C, 0xC0F3845C, 0xC0F38464, 0xC0F3846C, 0xC0F3847C, 0xC0F38484,
        0xC0F38494, 0xC0F3849C, 0xC0F384C4, 0xC0F384CC, 0xC0F3ABD8, 0xC0F3ABE0, 0xC0F3ABEC, 0xC0F3AC0C,
        0xC0F3AC14, 0xC0F3AC20 };
    /* v19: read-only context for the intermittent dark/high-ISO streaks: raw efic_temp and uptime */
    int s4_t_start = efic_temp, s4_ms_start = get_ms_clock();
    pp4_n[0] = pp4_n[1] = 0;
    pp4_snap(0);
    int s4_t_x5 = -1, s4_ms_x5 = -1, s4_t_end = -1;
    sh_name[0] = 0;
    s4_shots = 0;
    e1_n = 0; e1_replaced = 0; e1_passed = 0; e1_overflow = 0;
    e1_guard = 1;
    e1_active = 1;
    e3_replaced = 0; e3_passed = 0;
    e3_mode = 1;
    e3_active = 1;
    memset(dsp_tab, 0, sizeof(dsp_tab));
    memset(smp_tab, 0, sizeof(smp_tab));
    memset(sz_tab, 0, sizeof(sz_tab));
    memset(ofs_tab, 0, sizeof(ofs_tab));

    /* 1) wait (max 30 s) for x5 entered with the stage 4 tables */
    int entered = 0, left_s = 30;
    for (int t = 0, tick = 0; t < 30000; t += 10, tick++)
    {
        if (!lv || RECORDING) break;
        if (e3_replaced && shamem_read(REG_RAW) == RAW_5D3_X5) { entered = 1; break; }
        if (tick == 100) tick = 0;
        if (tick == 0) NotifyBox(1500, "cm_wr S4: %ds, go x5 now", left_s--);
        msleep(10);
    }

    static uint32_t wins[8][2];
    int nwin = 0, win_writes = 0, left_mode = 0;
    /* v26: Canon window events in order (ms in x5, start, end), to see where each window comes from */
    static uint32_t wev[48][3];
    int nwev = 0;
    uint32_t wev_ls = 0, wev_le = 0;
    int r154_writes = 0, r154_other = 0, r154_restored = 0;
    uint32_t r154_seen = 0, r154_end = 0;
    uint32_t regs_at_1s[MAX_SET];
    int nregs = sets[2].n;
    static char text[16384];
    int len;

    uint32_t cur_win_s = (init_y << 16) | (init_x >> 2);
    uint32_t cur_win_e = ((init_y + S4_H) << 16) | ((init_x + S4_W) >> 2);

    if (entered)
    {
        s4_t_x5 = efic_temp;
        s4_ms_x5 = get_ms_clock();
        s4_flk_read(0);
        smp_phase = 0;
        smp_on = 1;
        run_in_separate_task(smp_task, 0);
        int left = s4_ms / 1000;
        for (int t = 0, tick = 0; t < s4_ms; t += STEP_MS, tick++)
        {
            if (!lv || RECORDING || shamem_read(REG_RAW) != RAW_5D3_X5) { left_mode = 1; break; }
            uint32_t ws = shamem_read(REG_WIN_START), we = shamem_read(REG_WIN_END);
            int k;
            for (k = 0; k < nwin; k++) if (wins[k][0] == ws && wins[k][1] == we) break;
            if (k == nwin && nwin < 8) { wins[nwin][0] = ws; wins[nwin][1] = we; nwin++; }
            if ((ws != wev_ls || we != wev_le) && nwev < 48)
            {
                wev[nwev][0] = t; wev[nwev][1] = ws; wev[nwev][2] = we; nwev++;
                wev_ls = ws; wev_le = we;
            }
            if (ws != cur_win_s || we != cur_win_e)
            {
                if (s4_f1 == 5 && f4_sub < 2 && ((we & 0xFFFF) - (ws & 0xFFFF)) == (S4_W >> 2) && (ws >> 16) >= 140)
                {
                    int x0 = ((int)((ws & 0xFFFF) << 2) - 376) & ~3;
                    if (x0 < S4_X_MIN) x0 = S4_X_MIN;
                    if (x0 > S4_X_MAX) x0 = S4_X_MAX;
                    int y0 = ((int)(ws >> 16) - 248) & ~1;
                    if (y0 < S4_Y_MIN) y0 = S4_Y_MIN;
                    if (y0 > S4_Y_MAX) y0 = S4_Y_MAX;
                    cur_win_s = ((uint32_t)y0 << 16) | ((uint32_t)x0 >> 2);
                    cur_win_e = ((uint32_t)(y0 + S4_H) << 16) | ((uint32_t)(x0 + S4_W) >> 2);
                }
                EngDrvOutLV(REG_WIN_START, cur_win_s);
                EngDrvOutLV(REG_WIN_END, cur_win_e);
                win_writes++;
            }
            if (s4_r154)
            {
                uint32_t v = shamem_read(S4_R154);
                if (v == S4_R154_X5) { EngDrvOutLV(S4_R154, S4_R154_X1); r154_writes++; }
                else if (v != S4_R154_X1) { r154_other++; r154_seen = v; }
            }
            if (s4_f1 == 3 && t == 1000)
            {
                for (int i = 0; i < F2_N; i++)
                {
                    uint32_t v = shamem_read(f2_tab[i][0]);
                    if (v == f2_tab[i][1]) f2_pre++;
                    else if (f2_nbad < 16) { f2_bad[f2_nbad][0] = f2_tab[i][0]; f2_bad[f2_nbad][1] = v; f2_nbad++; }
                }
                if (f2_pre >= F2_N - 8)
                {
                    for (int i = 0; i < F2_N; i++)
                        if (shamem_read(f2_tab[i][0]) == f2_tab[i][1]) { EngDrvOutLV(f2_tab[i][0], f2_tab[i][2]); f2_mine[i] = 1; f2_writes++; }
                    f2_done = 1;
                }
            }
            else if (f2_done)
            {
                for (int i = 0; i < F2_N; i++)
                    if (f2_mine[i] && shamem_read(f2_tab[i][0]) == f2_tab[i][1]) { EngDrvOutLV(f2_tab[i][0], f2_tab[i][2]); f2_rewrites++; }
            }
            if (s4_f1 && s4_f1 < 3 && t == 1000)
            {
                for (int i = 0; i < F1_N; i++) if (shamem_read(f1_tab[i][0]) == f1_tab[i][1]) f1_pre++;
                for (int i = 0; i < F1B_N; i++) if (shamem_read(f1b_tab[i][0]) == f1b_tab[i][1]) f1b_pre++;
                if (f1_pre >= F1_N - 8 && (s4_f1 == 1 || f1b_pre == F1B_N))
                {
                    for (int i = 0; i < F1_N; i++)
                        if (shamem_read(f1_tab[i][0]) == f1_tab[i][1]) { EngDrvOutLV(f1_tab[i][0], f1_tab[i][2]); f1_mine[i] = 1; f1_writes++; }
                    if (s4_f1 == 2)
                        for (int i = 0; i < F1B_N; i++) { EngDrvOutLV(f1b_tab[i][0], f1b_tab[i][2]); f1b_mine[i] = 1; f1b_writes++; }
                    f1_done = 1;
                }
            }
            else if (f1_done)
            {
                for (int i = 0; i < F1_N; i++)
                    if (f1_mine[i] && shamem_read(f1_tab[i][0]) == f1_tab[i][1]) { EngDrvOutLV(f1_tab[i][0], f1_tab[i][2]); f1_rewrites++; }
            }
            if (t == 3000 || t == 12000) { if (take_screenshot(s4_shot_pat, SCREENSHOT_YUV)) s4_shots++; }
            if (t == 1000) { s4_flk_read(1); sh_open(); sh_snapshot(); for (int i = 0; i < 16; i++) r1regs[i] = shamem_read(r1reg[i]); for (int i = 0; i < 4; i++) dsval[i] = shamem_read(dsreg[i]); pp4_snap(1); }
            for (int i = 0; i < 16; i++) { r1c13[s4_l0cls(MEM(0xC0F26308))]++; r1c1b[s4_l0cls(MEM(0xC0F26B08))]++; }
            if (s4_r1 == 2)
            {
                uint32_t sc = shamem_read(R1B_SKIP_C), sy = shamem_read(R1B_SKIP_Y);
                if (sc == R1B_OLD) { EngDrvOutLV(R1B_SKIP_C, R1B_NEW); r1b_writes++; }
                else if (sc != R1B_NEW) { r1b_other++; r1b_seen = sc; }
                if (sy == R1B_OLD) { EngDrvOutLV(R1B_SKIP_Y, R1B_NEW); r1b_writes++; }
                else if (sy != R1B_NEW) { r1b_other++; r1b_seen = sy; }
            }
            if (t == 1000)
                for (int i = 0; i < nregs; i++) regs_at_1s[i] = shamem_read(sets[2].e[i].reg);
            if (tick == 1000 / STEP_MS) tick = 0;
            if (tick == 0 && !s4_quiet) NotifyBox(1500, "cm_wr S4: %ds in x5, win writes %d", left--, win_writes);
            msleep(STEP_MS);
        }
        if (!left_mode) sh_snapshot();
        sh_close();
        if (f2_done && !left_mode && lv && !RECORDING && shamem_read(REG_RAW) == RAW_5D3_X5)
            for (int i = F2_N - 1; i >= 0; i--)
                if (f2_mine[i] && shamem_read(f2_tab[i][0]) == f2_tab[i][2]) { EngDrvOutLV(f2_tab[i][0], f2_tab[i][1]); f2_restored++; }
        if (f1_done && !left_mode && lv && !RECORDING && shamem_read(REG_RAW) == RAW_5D3_X5)
        {
            for (int i = 0; i < F1B_N; i++) if (f1b_mine[i]) EngDrvOutLV(f1b_tab[i][0], f1b_tab[i][1]);
            for (int i = 0; i < F1_N; i++)
                if (f1_mine[i] && shamem_read(f1_tab[i][0]) == f1_tab[i][2]) { EngDrvOutLV(f1_tab[i][0], f1_tab[i][1]); f1_restored++; }
        }
        smp_on = 0;
        for (int i = 0; i < 20 && smp_running; i++) msleep(20);
        if (s4_r154)
        {
            r154_end = shamem_read(S4_R154);
            if (!left_mode && lv && !RECORDING && shamem_read(REG_RAW) == RAW_5D3_X5 && r154_end == S4_R154_X1)
            {
                EngDrvOutLV(S4_R154, S4_R154_X5);
                r154_restored = 1;
            }
        }
    }

    s4_t_end = efic_temp;
    int f3_wait_s = 0;
    if (f3_patched && entered)
    {
        /* the x1 display path must be stopped by the same functions: unpatch only after x5 is left */
        for (int t = 0; t < 300 && shamem_read(REG_RAW) == RAW_5D3_X5; t++)
        {
            NotifyBox(1500, "cm_wr F3: done. Zoom x10 -> x1 now");
            msleep(1000);
            f3_wait_s++;
        }
        msleep(1000);
    }
    sh_close();
    if (entered) s4_flk_read(2); else memset(s4_flk, 0, sizeof(s4_flk));
    e3_active = 0;
    e1_active = 0;
    e1_guard = 0;
    s4_unpatch();
    running = 0;

    len = snprintf(text, sizeof(text),
        "# cm_wr v60 stage 4%s%s%s%s: x5 raw, SSLT type 7 margins %d/%d/%d/%d, p1 W %d H %d X %d Y %d, +0xCC %x %x %x\n"
        "entered=%d replaced=%d passed=%d left_mode=%d raw=%08x\n"
        "edmac calls=%d overflow=%d c0f2630c=%08x c0f2640c=%08x\n"
        "expected window %08x %08x, window writes %d\n",
        s4_r154 ? "d" : s4_variant ? "b" : "", s4_quiet ? " quiet" : "", s4_clear ? " +clear 0x5A0" : "",
        s4_r154 ? " +C0F38154=0x133" : "",
        s4_wh_x5 ? s4_tbl[0][1] : s4_tbl[0][2], s4_wh_x5 ? s4_tbl[1][1] : s4_tbl[1][2], s4_tbl[2][2], s4_tbl[3][2],
        s4_p1[0x14 / 4], s4_p1[0x1C / 4], init_x, init_y,
        s4_p1[0xCC / 4], s4_p1[0xD0 / 4], s4_p1[0xD4 / 4],
        entered, e3_replaced, e3_passed, left_mode, shamem_read(REG_RAW),
        e1_n, e1_overflow, shamem_read(0xC0F2630C), shamem_read(0xC0F2640C),
        cur_win_s, cur_win_e, win_writes);
    for (int i = 0; i < e1_n; i++)
        len += snprintf(text + len, sizeof(text) - len, "call %02x %08x %08x %08x %08x\n",
            e1c[i].ch, e1c[i].in, e1c[i].out, e1c[i].raw, e1c[i].caller);
    for (int k = 0; k < nwin; k++)
        len += snprintf(text + len, sizeof(text) - len, "window %08x %08x%s\n", wins[k][0], wins[k][1],
            s4_win_ok(wins[k][0], wins[k][1]) ? "" : " (outside raw)");
    len += snprintf(text + len, sizeof(text) - len,
        "efic_temp_raw start=%d x5=%d end=%d, uptime_s start=%d x5=%d end=%d, x5 time %d s\n",
        s4_t_start, s4_t_x5, s4_t_end, s4_ms_start / 1000, s4_ms_x5 < 0 ? -1 : s4_ms_x5 / 1000,
        get_ms_clock() / 1000, s4_ms / 1000);
    len += snprintf(text + len, sizeof(text) - len,
        "R1 %d: p1 skip %x %x, guard %x %x; ch13 L0 old/new/other %d %d %d, ch1B %d %d %d; "
        "R1b reader skip writes %d other %d (last %x)\n",
        s4_r1, s4_p1[0xCC / 4], s4_p1[0xD0 / 4], e1_cap[0], e1_cap[1],
        r1c13[1], r1c13[2], r1c13[0], r1c1b[1], r1c1b[2], r1c1b[0], r1b_writes, r1b_other, r1b_seen);
    for (int i = 0; i < 16; i++)
        len += snprintf(text + len, sizeof(text) - len, "r1reg %08x %08x\n", r1reg[i], r1regs[i]);
    len += snprintf(text + len, sizeof(text) - len, "downstream C0F3A04C %08x C0F3ABE0 %08x C0F39F18 %08x C0F3ADB4 %08x (x1 04ff079b 04ff0700 027f0700 028003ce, x5 030f04ab 030f0460 01870460 01880256)\n",
        dsval[0], dsval[1], dsval[2], dsval[3]);
    len += snprintf(text + len, sizeof(text) - len, "sslt-ptr our_p1 %08x our_p3 %08x; hits start(x1) %d, x5+1s %d\n",
        (uint32_t)s4_p1, (uint32_t)s4_p3, pp4_n[0], pp4_n[1]);
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < pp4_n[k]; i++)
            len += snprintf(text + len, sizeof(text) - len, "sslt-ptr %s %08x %08x %s\n",
                k ? "x5" : "x1", pp4_a[k][i], pp4_v[k][i], pp4_name(pp4_v[k][i]));
    if (s4_f1 == 4)
        len += snprintf(text + len, sizeof(text) - len,
            "F3: x5 mode table [5..13] -> x1 display path, x1 state row %08x, x5 state row %08x, waited %d s for leaving x5\n",
            f3_row_x1, f3_row_x5, f3_wait_s);
    if (s4_f1 == 6)
        len += snprintf(text + len, sizeof(text) - len,
            "G1: 0xFF33C798 %08x -> %08x (x5 uses Canon 1080p x1 2nd-pass setup: template k1, 1904x1274, NR class 0), patched %d\n",
            G1_OLD, G1_NEW, g1_patched);
    if (s4_f1 == 5)
    {
        len += snprintf(text + len, sizeof(text) - len,
            "%s v60 (uncached 0x302 + Eeko C0F11 1904x1272->1152x1272, C0F11540 1:1 + 0x402 53:20 V, %s): 0x302 seen %d, modified %d, words replaced %d, kept %d, other %d\n",
            f4_name[f4_sub], f4_sub < 2 ? "box-centered" : "raw-centered",
            f4_seen, f4_done, f4_repl, f4_kept, f4_other);
        if (f4_done)
        {
            len += snprintf(text + len, sizeof(text) - len, "f4msg");
            for (int i = 0; i < 124; i++) len += snprintf(text + len, sizeof(text) - len, " %08x", f4_msg[i]);
            len += snprintf(text + len, sizeof(text) - len, "\n");
        }
    }
    if (s4_f1 == 3)
    {
        len += snprintf(text + len, sizeof(text) - len,
            "F2: prereq %d/%d, done %d, writes %d, rewrites %d, restored %d\n", f2_pre, F2_N, f2_done, f2_writes, f2_rewrites, f2_restored);
        for (int k = 0; k < f2_nbad; k++)
            len += snprintf(text + len, sizeof(text) - len, "f2 not x5 at start: %08x = %08x\n", f2_bad[k][0], f2_bad[k][1]);
    }
    if (s4_f1 && s4_f1 < 3)
        len += snprintf(text + len, sizeof(text) - len,
            "F1 mode %d: prereq %d/%d, C0F11 prereq %d/%d, done %d, writes %d, C0F11 writes %d, rewrites %d, restored %d\n",
            s4_f1, f1_pre, F1_N, f1b_pre, F1B_N, f1_done, f1_writes, f1b_writes, f1_rewrites, f1_restored);
    len += snprintf(text + len, sizeof(text) - len, "screenshots %d (ML/LOGS/CMnnnn.PPM, 3 s and 12 s)\n", s4_shots);
    len += snprintf(text + len, sizeof(text) - len, "shadow dump %s (%s)\n", sh_name[0] ? sh_name + 8 : "-",
        left_mode ? "1 s only or none" : "1 s + end");
    len += snprintf(text + len, sizeof(text) - len, "# flicker path regs: reg x5_entry 1s end\n");
    for (int i = 0; i < (int) S4_NFLK; i++)
        len += snprintf(text + len, sizeof(text) - len, "flk %08x %08x %08x %08x\n",
            s4_flk_reg[i], s4_flk[0][i], s4_flk[1][i], s4_flk[2][i]);
    if (s4_r154)
        len += snprintf(text + len, sizeof(text) - len,
            "c0f38154 writes=%d other=%d (last %08x) end=%08x restored=%d\n",
            r154_writes, r154_other, r154_seen, r154_end, r154_restored);
    len += snprintf(text + len, sizeof(text) - len, "# Canon window events: ms_in_x5 start end\n");
    for (int k = 0; k < nwev; k++)
        len += snprintf(text + len, sizeof(text) - len, "wev %5d %08x %08x\n", wev[k][0], wev[k][1], wev[k][2]);
    struct dsp * q = &dsp_tab[0];
    len += snprintf(text + len, sizeof(text) - len, "disp n=%d changes=%d\n", q->n, q->changes);
    if (entered && !left_mode)
    {
        len += snprintf(text + len, sizeof(text) - len, "# preview regs 1 s after x5: reg value x5 x1\n");
        for (int i = 0; i < nregs; i++)
            len += snprintf(text + len, sizeof(text) - len, "reg %08x %08x %08x %08x%s\n",
                sets[2].e[i].reg, regs_at_1s[i], sets[2].e[i].pre, sets[2].e[i].target,
                regs_at_1s[i] == sets[2].e[i].target ? " =x1" : regs_at_1s[i] == sets[2].e[i].pre ? " =x5" : "");
    }
    const char * n = save_text(text, len);
    snprintf(last_status, sizeof(last_status), "%s S4 e%d ov%d w%d", n, entered, e1_overflow, win_writes);
    NotifyBox(8000, "cm_wr S4: %s. Restore: zoom x10 -> x1", last_status);
}

static MENU_SELECT_FUNC(s4_select)
{
    if (running) return;
    s4_clear = 0;
    s4_wh_x5 = 0;
    s4_variant = 0;
    s4_quiet = 0;
    s4_r154 = 0;
    s4_ms = S4_MS;
    s4_r1 = 0;
    s4_f1 = 0;
    run_in_separate_task(s4_task, 0);
}

static MENU_SELECT_FUNC(s4b_select)
{
    if (running) return;
    s4_clear = 0;
    s4_wh_x5 = 0;
    s4_variant = 1;
    s4_quiet = 0;
    s4_r154 = 0;
    s4_ms = S4_MS;
    s4_r1 = 0;
    s4_f1 = 0;
    run_in_separate_task(s4_task, 0);
}

static MENU_SELECT_FUNC(s4c_select)
{
    if (running) return;
    s4_clear = 1;
    s4_wh_x5 = 1;
    s4_variant = 1;
    s4_quiet = 1;
    s4_r154 = 0;
    s4_ms = S4_MS;
    s4_r1 = 0;
    s4_f1 = 0;
    run_in_separate_task(s4_task, 0);
}

static MENU_SELECT_FUNC(s4bq_select)
{
    if (running) return;
    s4_clear = 0;
    s4_wh_x5 = 0;
    s4_variant = 1;
    s4_quiet = 1;
    s4_r154 = 0;
    s4_ms = S4_MS;
    s4_r1 = 0;
    s4_f1 = 0;
    run_in_separate_task(s4_task, 0);
}

/* v28: display source test (full-frame preview research). Canon's LCD path gets, every frame, a
 * struct with the 5 SSLT pyramid level addresses (FrameParam 0xFF32C654 r1: id 0x006A1D60,
 * w[1..5] = L0 L1 L2 L3 L4, w[6..10] the same again; logs/20261018-5D3-fp). Here, for 3 s, only w[1]
 * is replaced by w[2] (L1 = half size copy of the same frame) when the struct looks exactly like the
 * logged one. Only the display's read address changes; nothing writes there, all addresses are the
 * LV YUV buffers. Expected if the LCD reads w[1]: the scene at half size in the top-left quarter. */
#define DS_ADDR  0xFF32C654
#define DS_I0    0xE92D403E
#define DS_I1    0xE51F2040
#define DS_ID    0x006A1D60
#define DS_MS    3000
static volatile int ds_active = 0, ds_seen = 0, ds_replaced = 0, ds_other = 0;
/* v29: which copy of the L0 address is replaced: 1 = w[1] (v28: picture unchanged, zebra-like
 * streaks on top = the stage reading w[1] blends it in), 2 = w[6] only, 3 = w[1] and w[6] */
static volatile int ds_mode = 1;
static int ds_hooked = 0;
static uint32_t ds_first[12];

static int ds_l0(uint32_t a) { return a == 0x52000000 || a == 0x4CE00000; }
static int ds_l1(uint32_t a) { return a == 0x50000000 || a == 0x4AE00000; }
static volatile int x1_fp_active = 0, x1_fp_cnt = 0, x1_fp_seen = 0;
static uint32_t x1_fp_w[2];

static void ds_hook(uint32_t * regs, uint32_t * stack, uint32_t pc)
{
    if (!ds_active && !x1_fp_active) return;
    uint32_t p = regs[1];
    if (!p || (p & 3) || p < 0x1000 || p >= 0x20000000) return;
    uint32_t * w = (uint32_t *) p;
    if (w[0] != DS_ID) return;
    if (x1_fp_active)
    {
        x1_fp_seen++;
        if (x1_fp_seen == 1) { x1_fp_w[0] = w[1]; x1_fp_w[1] = w[2]; }
        uint32_t a1 = w[1] & ~0x40000000u;
        uint32_t a2 = w[2] & ~0x40000000u;
        /* Relocated L0 = 0x11000000 / 0x0BE00000, L1 = 0x11800000 / 0x0C600000 */
        if ((a1 == 0x11000000u && a2 == 0x11800000u) ||
            (a1 == 0x0BE00000u && a2 == 0x0C600000u))
        {
            uint32_t l1 = w[2], l2 = w[3], l3 = w[4], l4 = w[5];
            w[1] = l1;
            w[2] = l2;
            w[3] = l3;
            w[4] = l4;
            if ((w[6] & ~0x40000000u) == a1)
            {
                w[6] = l1;
                w[7] = l2;
                w[8] = l3;
                w[9] = l4;
            }
            x1_fp_cnt++;
        }
        return;
    }
    ds_seen++;
    if (ds_seen == 1) for (int i = 0; i < 12; i++) ds_first[i] = w[i];
    if (ds_l0(w[1]) && w[6] == w[1] && ds_l1(w[2]) && w[7] == w[2] &&
        (w[1] == 0x52000000) == (w[2] == 0x50000000))
    {
        uint32_t l1 = w[2];
        if (ds_mode & 1) w[1] = l1;
        if (ds_mode & 2) w[6] = l1;
        ds_replaced++;
    }
    else ds_other++;
}

static void ds_task(void)
{
    if (!is_5d3() || running || !lv || RECORDING)
    {
        NotifyBox(3000, "cm_wr: 5D3, LiveView, not recording, no test running");
        return;
    }
    if (!ds_hooked)
    {
        if (MEM(DS_ADDR) != DS_I0 || MEM(DS_ADDR + 4) != DS_I1)
        {
            NotifyBox(5000, "cm_wr: FrameParam code unexpected (cm_prev observe used?), nothing done");
            return;
        }
        int err = patch_hook_function(DS_ADDR, DS_I0, ds_hook, "cm_wr: display source test");
        if (err)
        {
            NotifyBox(5000, "cm_wr: hook failed (%x), nothing done", err);
            return;
        }
        ds_hooked = 1;
    }
    running = 1;
    uint32_t raw = shamem_read(REG_RAW);
    int zoom = lv_dispsize;
    ds_seen = 0; ds_replaced = 0; ds_other = 0;
    ds_active = 1;
    msleep(DS_MS);
    ds_active = 0;
    msleep(100);
    running = 0;

    static char text[512];
    int len = snprintf(text, sizeof(text),
        "# cm_wr v29 display source test: FrameParam %s (L0) -> L1 for %d ms\n"
        "raw %08x zoom %d, frames seen %d, replaced %d, other %d\nfirst:",
        ds_mode == 1 ? "w[1]" : ds_mode == 2 ? "w[6]" : "w[1]+w[6]", DS_MS, raw, zoom, ds_seen, ds_replaced, ds_other);
    for (int i = 0; i < 12; i++) len += snprintf(text + len, sizeof(text) - len, " %08x", ds_first[i]);
    len += snprintf(text + len, sizeof(text) - len, "\n");
    const char * n = save_text(text, len);
    snprintf(last_status, sizeof(last_status), "%s ds seen %d repl %d", n, ds_seen, ds_replaced);
    NotifyBox(5000, "cm_wr: display test %s", last_status);
}

static MENU_SELECT_FUNC(ds_select)
{
    if (running) return;
    ds_mode = 1;
    run_in_separate_task(ds_task, 0);
}

static MENU_SELECT_FUNC(ds6_select)
{
    if (running) return;
    ds_mode = 2;
    run_in_separate_task(ds_task, 0);
}

static MENU_SELECT_FUNC(ds16_select)
{
    if (running) return;
    ds_mode = 3;
    run_in_separate_task(ds_task, 0);
}

/* v38: one LiveView screenshot (YUV) anytime, e.g. of plain Canon x5 for comparison */
static void shot_task(void)
{
    int ok = take_screenshot(s4_shot_pat, SCREENSHOT_YUV);
    NotifyBox(3000, "cm_wr: screenshot %s", ok ? "saved (ML/LOGS/CMnnnn.PPM)" : "failed");
}
static MENU_SELECT_FUNC(shot_select) { run_in_separate_task(shot_task, 0); }

/* v44: read-only value scan. F3 (x1 display-path functions) ran without freezing but the second-pass
 * sizes stayed x5 (C0F3A04C 0x030F04AB): they are not built by the display path. They are written
 * outside EngDrvOut/engio, so look for the RAM structure that holds them: scan cached RAM
 * 0x1000..0x02000000 for the second-pass size words of x5 and x1. */
/* v46: the v44 hits were our own data (2026-11-04, M5D30000/01): 0x00A36000.. = this module (code,
 * strings, cm_wr_f1.h/f2.h {reg, x5, x1} tables), 0x001AE4xx = this task's stack. v46 scans cached RAM
 * 0x1000..0x08000000 and drops hits inside this module (lo..hi from own symbol addresses, +-32 KB) and
 * near this task's stack (+-16 KB). Patterns: (1) packed sizes (H<<16|W and W<<16|H, also -1) of
 * x5 1196x784/792 and x1 1948x1280/1288; (2) a word 1196/1195/1948/1947 followed within 8 words by
 * the matching height (784/783/792/791 or 1280/1279/1288/1287). Each hit with the 8 words after it.
 * Reads only. */
#define VS_NV 16
static uint32_t vs_val[VS_NV];
#define VS_MAX 600
static uint32_t vs_hit_a[VS_MAX];
static uint8_t  vs_hit_k[VS_MAX];
static char vs_txt[96 * 1024];
static volatile int vs_on = 0;
static volatile int pp_on;
static int vs_h_ok(uint32_t w, uint32_t h)
{
    if (w == 1196 || w == 1195) return h == 784 || h == 783 || h == 792 || h == 791;
    if (w == 1948 || w == 1947) return h == 1280 || h == 1279 || h == 1288 || h == 1287;
    return 0;
}
static void vs_task(void)
{
    if (vs_on || pp_on) return;
    vs_on = 1;
    static const uint16_t W[2] = { 1196, 1948 }, H[2][2] = { { 784, 792 }, { 1280, 1288 } };
    int nv = 0;
    for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) for (int d = 0; d < 2; d++)
    {
        uint32_t w = W[i] - d, h = H[i][j] - d;
        vs_val[nv++] = (h << 16) | w;
        vs_val[nv++] = (w << 16) | h;
    }
    /* own module block: code, rodata, data and bss of this .mo are one allocation */
    uint32_t sym[6] = { (uint32_t)&vs_task, (uint32_t)&save_text, (uint32_t)vs_txt, (uint32_t)vs_hit_a,
                        (uint32_t)&vs_on, (uint32_t)sh_buf };
    uint32_t lo = sym[0], hi = sym[0];
    for (int i = 1; i < 6; i++) { if (sym[i] < lo) lo = sym[i]; if (sym[i] > hi) hi = sym[i]; }
    lo = (lo & ~0xFFF) - 0x8000;
    hi = hi + sizeof(vs_txt) + 0x8000;
    uint32_t stk = (uint32_t)&nv;
    uint32_t slo = stk - 0x4000, shi = stk + 0x4000;
    int n = 0, more = 0, self = 0, stack = 0, packed = 0, pairs = 0;
    uint32_t raw = shamem_read(REG_RAW);
    int zoom = lv_dispsize;
    int t0 = get_ms_clock();
    for (uint32_t a = 0x1000; a < 0x08000000; a += 4)
    {
        if ((a & 0x3FFFF) == 0) msleep(1);
        uint32_t v = MEM(a);
        int k = -1;
        if ((v & 0xF800F800) == 0)
        {
            if (v < 2048)
            {
                if (v == 1196 || v == 1195 || v == 1948 || v == 1947)
                    for (int j = 1; j <= 8; j++)
                        if (vs_h_ok(v, MEM(a + 4 * j))) { k = VS_NV; break; }
            }
            else
                for (int i = 0; i < nv; i++) if (v == vs_val[i]) { k = i; break; }
        }
        if (k < 0) continue;
        if (a >= lo && a < hi) { self++; continue; }
        if (a >= slo && a < shi) { stack++; continue; }
        if (k == VS_NV) pairs++; else packed++;
        if (n < VS_MAX) { vs_hit_a[n] = a; vs_hit_k[n] = k; n++; } else more++;
    }
    int t1 = get_ms_clock();
    int len = snprintf(vs_txt, sizeof(vs_txt),
        "# cm_wr v46 value scan (read-only): RAM 0x1000..0x08000000, raw %08x zoom %d, %d ms\n"
        "# excluded: module %08x..%08x (%d hits), stack %08x..%08x (%d hits)\n"
        "# kept %d (+%d not listed): packed %d, W/H pairs %d\n"
        "# line: vs addr kind | 8 words after (kind: p<idx> packed, see order below; pr = W then H)\n"
        "# packed order: (1196x784) HW WH, -1 HW WH, (1196x792) .., (1948x1280) .., (1948x1288) ..\n",
        raw, zoom, t1 - t0, lo, hi, self, slo, shi, stack, n, more, packed, pairs);
    for (int i = 0; i < n && len < (int)sizeof(vs_txt) - 160; i++)
    {
        uint32_t a = vs_hit_a[i];
        if (vs_hit_k[i] == VS_NV) len += snprintf(vs_txt + len, sizeof(vs_txt) - len, "vs %08x pr  %08x |", a, MEM(a));
        else len += snprintf(vs_txt + len, sizeof(vs_txt) - len, "vs %08x p%02d %08x |", a, vs_hit_k[i], MEM(a));
        for (int j = 1; j <= 8; j++) len += snprintf(vs_txt + len, sizeof(vs_txt) - len, " %08x", MEM(a + 4 * j));
        len += snprintf(vs_txt + len, sizeof(vs_txt) - len, "\n");
    }
    const char * name = save_text(vs_txt, len);
    vs_on = 0;
    NotifyBox(5000, "cm_wr: value scan %s, %d hits (+%d)", name, n, more);
}
static MENU_SELECT_FUNC(vs_select) { run_in_separate_task(vs_task, 0); }

/* v47: read-only pointer scan (2026-11-04). The second-pass block address table 0xFF90B7E0..0xFF90B990
 * (C0F3AD80.., C0F43000.. routing, C0F3A000.., C0F39000.., C0F3ABD8..) and the x5 path row 0xFFA11A80
 * (link lists 0xFF94F630/6A0/6B0) have no code reference in ROM, so they are used through a RAM
 * descriptor. Scan cached RAM 0x1000..0x08000000 for words pointing into
 *   B 0xFF90B700..0xFF90B9A0 (block table and the counters before it)
 *   R 0xFFA11600..0xFFA12100 (LCD path rows, x1/x5/x10/720p)
 *   T 0xFF94B000..0xFF950000 (x1 and x5 path tables: engio, link lists, SSLT p1/p3)
 * same module/stack exclusion as v46; each hit with 4 words before and 8 after. Reads only. */
static volatile int pp_on = 0;
static char pp_class(uint32_t v)
{
    if (v >= 0xFF90B700 && v < 0xFF90B9A0) return 'B';
    if (v >= 0xFFA11600 && v < 0xFFA12100) return 'R';
    if (v >= 0xFF94B000 && v < 0xFF950000) return 'T';
    return 0;
}
static void pp_task(void)
{
    if (pp_on || vs_on) return;
    pp_on = 1;
    uint32_t sym[6] = { (uint32_t)&pp_task, (uint32_t)&save_text, (uint32_t)vs_txt, (uint32_t)vs_hit_a,
                        (uint32_t)&pp_on, (uint32_t)sh_buf };
    uint32_t lo = sym[0], hi = sym[0];
    for (int i = 1; i < 6; i++) { if (sym[i] < lo) lo = sym[i]; if (sym[i] > hi) hi = sym[i]; }
    lo = (lo & ~0xFFF) - 0x8000;
    hi = hi + sizeof(vs_txt) + 0x8000;
    int nb = 0;
    uint32_t stk = (uint32_t)&nb;
    uint32_t slo = stk - 0x4000, shi = stk + 0x4000;
    int n = 0, more = 0, self = 0, stack = 0, cb = 0, cr = 0, ct = 0;
    uint32_t raw = shamem_read(REG_RAW);
    int zoom = lv_dispsize;
    int t0 = get_ms_clock();
    for (uint32_t a = 0x1000; a < 0x08000000; a += 4)
    {
        if ((a & 0x3FFFF) == 0) msleep(1);
        uint32_t v = MEM(a);
        if (v < 0xFF900000) continue;
        char c = pp_class(v);
        if (!c) continue;
        if (a >= lo && a < hi) { self++; continue; }
        if (a >= slo && a < shi) { stack++; continue; }
        if (c == 'B') cb++; else if (c == 'R') cr++; else ct++;
        if (n < VS_MAX) { vs_hit_a[n] = a; vs_hit_k[n] = c; n++; } else more++;
    }
    int t1 = get_ms_clock();
    int len = snprintf(vs_txt, sizeof(vs_txt),
        "# cm_wr v47 pointer scan (read-only): RAM 0x1000..0x08000000, raw %08x zoom %d, %d ms\n"
        "# excluded: module %08x..%08x (%d hits), stack %08x..%08x (%d hits)\n"
        "# kept %d (+%d not listed): B %d (2nd-pass block table), R %d (LCD path rows), T %d (path tables)\n"
        "# line: ps addr class value | 4 words before | 8 words after\n",
        raw, zoom, t1 - t0, lo, hi, self, slo, shi, stack, n, more, cb, cr, ct);
    for (int i = 0; i < n && len < (int)sizeof(vs_txt) - 200; i++)
    {
        uint32_t a = vs_hit_a[i];
        len += snprintf(vs_txt + len, sizeof(vs_txt) - len, "ps %08x %s %08x |", a,
            vs_hit_k[i] == 'B' ? "B" : vs_hit_k[i] == 'R' ? "R" : "T", MEM(a));
        for (int j = -4; j < 0; j++) len += snprintf(vs_txt + len, sizeof(vs_txt) - len, " %08x", MEM(a + 4 * j));
        len += snprintf(vs_txt + len, sizeof(vs_txt) - len, " |");
        for (int j = 1; j <= 8; j++) len += snprintf(vs_txt + len, sizeof(vs_txt) - len, " %08x", MEM(a + 4 * j));
        len += snprintf(vs_txt + len, sizeof(vs_txt) - len, "\n");
    }
    const char * name = save_text(vs_txt, len);
    pp_on = 0;
    NotifyBox(5000, "cm_wr: pointer scan %s, %d hits (+%d)", name, n, more);
}
static MENU_SELECT_FUNC(pp_select) { run_in_separate_task(pp_task, 0); }

/* v49: read-only write-timing probe (2026-11-04). The second-pass sizes (C0F3A04C ...) are written by
 * a direct path (reg + shadow, no EngDrvOut/engio) and no RAM/ROM structure holding them was found
 * (v46/v47/v48). Sample shadow words of each LV stage every tick (msleep(1)) for 15 s while the user
 * goes x1 -> x5, and log every change with its time: the stage whose words change together with the
 * second pass narrows down which Canon setup step writes it. Reads only (shamem_read and MEM). */
#define TW_N 18
static const uint32_t tw_reg[TW_N] = {
    0xC0F06804,             /* raw size (sensor readout switch) */
    0x000568B8,             /* RAM: SSLT p3 copy W (memcpy in SSLT driver 0xFF33E280, mode op [0]) */
    0xC0F2630C,             /* EDMAC ch13 size (helper 0x13734 inside SSLT driver) */
    0xC0F38070,             /* SSLT output size (SSLT parser) */
    0xC0F383D4,             /* SSLT window */
    0xC0F26808,             /* HIV ch 0x18 address (mode op [1] StartPreproPath) */
    0xC0F3A04C,             /* second pass input size */
    0xC0F3ABE0,             /* second pass NR reader size */
    0xC0F39F18,             /* second pass L1 reader size */
    0xC0F3ADB4,             /* second pass half size */
    0xC0F43038,             /* routing */
    0xC0F43060,             /* routing */
    0xC0F430A4,             /* routing */
    0xC0F11110,             /* C0F11 resizer mode */
    0xC0F11508,             /* C0F11 H stretch */
    0xC0F04A10,             /* EDMAC ch 0x0A (HD buffer) size */
    0xC0F26510,             /* EDMAC ch 0x15 size */
    0xC0F3000C,             /* EDMAC ch 0x20 size */
};
#define TW_MAXEV 320
#define TW_PERREG 16
static uint32_t tw_ev_t[TW_MAXEV], tw_ev_v[TW_MAXEV];
static uint8_t tw_ev_r[TW_MAXEV];
static volatile int tw_on = 0;
static uint32_t tw_read(int i) { return tw_reg[i] < 0xC0000000 ? MEM(tw_reg[i]) : shamem_read(tw_reg[i]); }
static void tw_task(void)
{
    if (tw_on || vs_on || pp_on) return;
    tw_on = 1;
    uint32_t cur[TW_N], first[TW_N];
    int nper[TW_N], nev = 0, lost = 0, samples = 0, maxgap = 0;
    for (int i = 0; i < TW_N; i++) { cur[i] = first[i] = tw_read(i); nper[i] = 0; }
    int zoom0 = lv_dispsize, zoomev = 0;
    uint32_t zt[8]; int zv[8];
    NotifyBox(3000, "cm_wr: timing probe 15 s, switch x1 -> x5 now");
    int t0 = get_ms_clock(), tl = t0, t = t0;
    while ((t = get_ms_clock()) - t0 < 15000)
    {
        if (t - tl > maxgap) maxgap = t - tl;
        tl = t;
        samples++;
        for (int i = 0; i < TW_N; i++)
        {
            uint32_t v = tw_read(i);
            if (v == cur[i]) continue;
            cur[i] = v;
            if (nper[i] < TW_PERREG && nev < TW_MAXEV)
            { tw_ev_t[nev] = t - t0; tw_ev_r[nev] = i; tw_ev_v[nev] = v; nev++; nper[i]++; }
            else lost++;
        }
        if (lv_dispsize != zoom0 && zoomev < 8) { zt[zoomev] = t - t0; zv[zoomev] = lv_dispsize; zoomev++; zoom0 = lv_dispsize; }
        msleep(1);
    }
    int len = snprintf(vs_txt, sizeof(vs_txt),
        "# cm_wr v49 write-timing probe (read-only): 15 s, %d samples, max gap %d ms, %d events (+%d not listed)\n"
        "# start values:\n", samples, maxgap, nev, lost);
    for (int i = 0; i < TW_N; i++)
        len += snprintf(vs_txt + len, sizeof(vs_txt) - len, "start %08x %08x\n", tw_reg[i], first[i]);
    for (int i = 0; i < zoomev; i++)
        len += snprintf(vs_txt + len, sizeof(vs_txt) - len, "zoom %5d ms -> %d\n", zt[i], zv[i]);
    len += snprintf(vs_txt + len, sizeof(vs_txt) - len, "# events: ms reg new_value\n");
    for (int i = 0; i < nev && len < (int)sizeof(vs_txt) - 64; i++)
        len += snprintf(vs_txt + len, sizeof(vs_txt) - len, "ev %5d %08x %08x\n", tw_ev_t[i], tw_reg[tw_ev_r[i]], tw_ev_v[i]);
    const char * name = save_text(vs_txt, len);
    tw_on = 0;
    NotifyBox(5000, "cm_wr: timing probe %s, %d events", name, nev);
}
static MENU_SELECT_FUNC(tw_select) { run_in_separate_task(tw_task, 0); }

/* v50: read-only IPC observer (2026-11-04, last planned experiment). The display path setup [6]
 * (0xFF48ADE8) does not write the second pass itself: it sends command 0x400 with channel ids to
 * another core through the mailbox function RAM 0x18E90 (ROM 0xFF9F7DD8:
 * f(core < 8, msg < 10, cmd, ptr)) and waits. Observe every call of 0x18E90 for 15 s (x1 -> x5):
 * the hook only copies raw words (core, msg, cmd, ptr, caller, raw size, 24 payload words if ptr is
 * RAM) into a static table, no formatting, interrupts off for the copy; identical calls are counted.
 * Nothing is changed: regs[] are never written. The hook stays installed but inert afterwards. */
#define IPC_ADDR  0x18E90
#define IPC_I0    0xE92D41FC   /* PUSH {R2-R8,LR} */
#define IPC_I1    0xE351000A   /* CMP R1, #10 */
#define IPC_PW    64
#define IPC_MAX   96
/* v51: v50's table filled in x1 before the zoom (per-frame messages carry new buffer addresses every
 * frame and were counted as new records). Now one record per (core, msg, cmd, caller, raw size):
 * first payload kept, last payload kept, count of payload changes; 64 payload words. */
struct ipc_rec { uint32_t n, nchg, t0, t1, core, msg, cmd, ptr, caller, raw, w[IPC_PW], wl[IPC_PW]; };
static struct ipc_rec ipc_r[IPC_MAX];
static volatile int ipc_active = 0, ipc_n = 0, ipc_lost = 0, ipc_calls = 0, ipc_hooked = 0;
static volatile uint32_t ipc_tick = 0;   /* ~ms since start, advanced by the logging task */
/* v52 F4 variant A (approved 2026-11-04). The second pass, C0F11 and the HD output are set up by
 * another core from message 0x302 (built in 0xFF33C830, ptr 0x01F253A0), a copy of ROM template
 * 0xFF92F40C + k*0x1F0 chosen by PATH_SelectPathDriveMode (x5 k5, 1080p x1 k1; docs/sndpass_templates.txt).
 * During stage 4b in x5, the hook on RAM 0x18E90 replaces in that message every word where the k5 and
 * k1 templates differ, but only if the message still holds the k5 value and the k5 value is not a
 * buffer address (0x40000000..0x5FFFFFFF); word 0 (template id 5) is kept (variant A). Only RAM of the
 * message is changed, no register write; Canon's own x1 template values. */
#define F4_TPL      (cm_fw->f4_tpl)
#define F4_CALLER   (cm_fw->f4_caller)
static int f4_tpl_ok(void)
{
    uint32_t k1 = F4_TPL + 1 * 0x1F0, k5 = F4_TPL + 5 * 0x1F0;
    for (int i = 0; i < 14; i++) if (MEM(F4_EEKO_K5 + 4 * i) != f4_eeko_orig[i]) return 0;
    for (int s = 0; s < 4; s++)
    {
        uint32_t u = F4_EEKO_402 + s * F4_402_STRIDE;
        if (MEM(u + 2 * 4) != 768 || MEM(u + 3 * 4) != 768 || MEM(u + 11 * 4) != 768
            || MEM(u + 16 * 4) != 5 || MEM(u + 17 * 4) != 8 || MEM(u + 47 * 4) != 768) return 0;
    }
    return MEM(k1) == 1 && MEM(k5) == 5 && MEM(k1 + 16 * 4) == 0x07700000 && MEM(k5 + 16 * 4) == 0x04800000
        && MEM(k1 + 17 * 4) == 0x05000000 && MEM(k5 + 17 * 4) == 0x03100000
        && MEM(k1 + 19 * 4) == 0x900 && MEM(k5 + 19 * 4) == 0xBA0;
}
static void f4_modify(uint32_t * regs, uint32_t caller)
{
    if (regs[2] != 0x302) return;
    f4_seen++;
    uint32_t a = regs[3] & ~0x40000000u;
    uint32_t u = a | 0x40000000u;   /* FUN_ff5064b0 allocates/writes uncached 0x41F253A0, passes cached 0x01F253A0 */
    /* MEM(u) == 5 uniquely identifies Canon's x5 k5 template (k0..k4=x1, k5=x5, k6=x10).
     * Do NOT gate on shamem_read(REG_RAW) == RAW_5D3_X5: during x1->x5 transitions (boot or
     * PLAY->LV), Canon can send 0x302(k5) 1 frame before REG_RAW flips, causing an unpatched
     * k5 (0xBA0/768) vs patched SSLT/402 (0x900/1272) mismatch -> pink stripes + Eeko freeze. */
    if (caller != F4_CALLER || a < 0x1000 || a >= 0x20000000 || (a & 3) || MEM(u) != 5)
    { f4_other++; return; }
    uint32_t k1 = F4_TPL + 1 * 0x1F0, k5 = F4_TPL + 5 * 0x1F0;
    /* Replace pyramid (12..76) and NR/Chb (94..113) with k1 (1904x1280 -> 1904x1272),
     * keeping word 11 at k5's native 0 (L0 C-plane chroma phase matching x5 C0F38458=0x8DD80000);
     * also raise main output height words (81, 89, 122) from 768 to 1272 for 0x402 53:20 downscale. */
    for (int i = 12; i <= 122; i++)
    {
        if (i >= 79 && i <= 92 && i != 81 && i != 89) continue;
        if (i >= 114 && i != 122) continue;
        uint32_t v1 = MEM(k1 + 4 * i), v5 = MEM(k5 + 4 * i);
        if (i == 98 || i == 81 || i == 89 || i == 122) v1 = f4_hrec[f4_sub];
        if (v1 == v5 || (v5 >= 0x40000000 && v5 < 0x60000000)) continue;
        if (MEM(u + 4 * i) != v5) { f4_kept++; continue; }
        MEM(u + 4 * i) = v1;
        MEM(a + 4 * i) = v1;
        f4_repl++;
    }
    for (int i = 0; i < 124; i++) f4_msg[i] = MEM(u + 4 * i);
    f4_done++;
}
static void cm_x1_403_modify(uint32_t * regs, uint32_t caller);
static void ipc_hook(uint32_t * regs, uint32_t * stack, uint32_t pc);
static int f4_install(void)
{
    if (ipc_hooked) return 0;
    if (!f4_tpl_ok()) return 1;
    if (MEM(IPC_ADDR) != IPC_I0 || MEM(IPC_ADDR + 4) != IPC_I1) return 2;
    if (patch_hook_function(IPC_ADDR, IPC_I0, ipc_hook, "cm_wr: IPC observe / F4")) return 3;
    ipc_hooked = 1;
    return 0;
}
static void ipc_hook(uint32_t * regs, uint32_t * stack, uint32_t pc)
{
    if (f4_mod) { uint32_t old = cli(); f4_modify(regs, PATCH_HOOK_CALLER()); sei(old); }
    cm_x1_403_modify(regs, PATCH_HOOK_CALLER());
    if (!ipc_active) return;
    uint32_t old = cli();
    ipc_calls++;
    uint32_t ptr = regs[3], a = ptr & ~0x40000000u;
    int ram = (a >= 0x1000 && a < 0x20000000 && !(a & 3));
    uint32_t caller = PATCH_HOOK_CALLER();
    uint32_t raw = shamem_read(REG_RAW);
    int k;
    for (k = 0; k < ipc_n; k++)
    {
        struct ipc_rec * r = &ipc_r[k];
        if (r->core == regs[0] && r->msg == regs[1] && r->cmd == regs[2] && r->caller == caller && r->raw == raw) break;
    }
    if (k < ipc_n)
    {
        struct ipc_rec * r = &ipc_r[k];
        r->n++; r->t1 = ipc_tick;
        int chg = 0;
        for (int i = 0; i < IPC_PW; i++) { uint32_t v = ram ? MEM(a + 4 * i) : 0; if (v != r->wl[i]) { r->wl[i] = v; chg = 1; } }
        if (chg) r->nchg++;
    }
    else if (ipc_n < IPC_MAX)
    {
        struct ipc_rec * r = &ipc_r[ipc_n];
        r->n = 1; r->nchg = 0; r->t0 = r->t1 = ipc_tick; r->core = regs[0]; r->msg = regs[1]; r->cmd = regs[2];
        r->ptr = ptr; r->caller = caller; r->raw = raw;
        for (int i = 0; i < IPC_PW; i++) r->w[i] = r->wl[i] = ram ? MEM(a + 4 * i) : 0;
        ipc_n++;
    }
    else ipc_lost++;
    sei(old);
}
static volatile int ipc_on = 0;
static void ipc_task(void)
{
    if (ipc_on || tw_on || vs_on || pp_on) return;
    if (!is_5d3()) { NotifyBox(3000, "cm_wr: 5D3 1.1.3 only"); return; }
    ipc_on = 1;
    if (!ipc_hooked)
    {
        if (MEM(IPC_ADDR) != IPC_I0 || MEM(IPC_ADDR + 4) != IPC_I1)
        {
            NotifyBox(6000, "cm_wr: IPC code unexpected (%08x %08x), not hooking", MEM(IPC_ADDR), MEM(IPC_ADDR + 4));
            ipc_on = 0;
            return;
        }
        int err = patch_hook_function(IPC_ADDR, IPC_I0, ipc_hook, "cm_wr: IPC observe");
        if (err) { NotifyBox(6000, "cm_wr: IPC hook failed (%x), nothing changed", err); ipc_on = 0; return; }
        ipc_hooked = 1;
    }
    ipc_n = 0; ipc_lost = 0; ipc_calls = 0; ipc_tick = 0;
    int zoom0 = lv_dispsize, zoomev = 0;
    uint32_t zt[8]; int zv[8];
    NotifyBox(3000, "cm_wr: IPC observe 15 s, switch x1 -> x5 now");
    int t0 = get_ms_clock(), t;
    ipc_active = 1;
    while ((t = get_ms_clock()) - t0 < 15000)
    {
        ipc_tick = t - t0;
        if (lv_dispsize != zoom0 && zoomev < 8) { zt[zoomev] = t - t0; zv[zoomev] = lv_dispsize; zoomev++; zoom0 = lv_dispsize; }
        msleep(1);
    }
    ipc_active = 0;
    msleep(50);
    int len = snprintf(vs_txt, sizeof(vs_txt),
        "# cm_wr v51 IPC observe (read-only, hook RAM 0x18E90): 15 s, calls %d, keys %d, lost %d\n"
        "# line: ipc n n_payload_changes t_first t_last core msg cmd ptr caller raw | 64 first payload words (0 if ptr not RAM)\n"
        "# last: index:value of words that differ in the last payload\n",
        ipc_calls, ipc_n, ipc_lost);
    for (int i = 0; i < zoomev; i++)
        len += snprintf(vs_txt + len, sizeof(vs_txt) - len, "zoom %5d ms -> %d\n", zt[i], zv[i]);
    for (int k = 0; k < ipc_n && len < (int)sizeof(vs_txt) - 1200; k++)
    {
        struct ipc_rec * r = &ipc_r[k];
        len += snprintf(vs_txt + len, sizeof(vs_txt) - len, "ipc %d %d %d %d %d %d %x %08x %08x %08x |",
            r->n, r->nchg, r->t0, r->t1, r->core, r->msg, r->cmd, r->ptr, r->caller, r->raw);
        for (int i = 0; i < IPC_PW; i++) len += snprintf(vs_txt + len, sizeof(vs_txt) - len, " %08x", r->w[i]);
        len += snprintf(vs_txt + len, sizeof(vs_txt) - len, "\n");
        if (r->nchg)
        {
            len += snprintf(vs_txt + len, sizeof(vs_txt) - len, "last");
            for (int i = 0; i < IPC_PW; i++)
                if (r->wl[i] != r->w[i]) len += snprintf(vs_txt + len, sizeof(vs_txt) - len, " %d:%08x", i, r->wl[i]);
            len += snprintf(vs_txt + len, sizeof(vs_txt) - len, "\n");
        }
    }
    const char * name = save_text(vs_txt, len);
    ipc_on = 0;
    NotifyBox(5000, "cm_wr: IPC observe %s, %d calls, %d unique", name, ipc_calls, ipc_n);
}
static MENU_SELECT_FUNC(ipc_select) { run_in_separate_task(ipc_task, 0); }

/* v37: read-only memory dump. The stage 4 staircase streaks look like ASCII text read as luma (bright
 * letters, dark spaces, lines shifted by a wrong pitch; CM0000.PPM). Dumps 4 x 256 KB around the
 * small pyramid levels and the start of Canon's buffer type 12 (A and B regions, uncached), plus
 * 0x4C800000 / 0x51800000 (inside type 12, never used by R0/R1), 6 x 256 KB, to ML/LOGS/MD5D3_NN.BIN, to see whether text sits next to the buffers the NR readers use. */
static const uint32_t md_addr[6] = { 0x4B100000, 0x4B140000, 0x50300000, 0x50340000, 0x4C800000, 0x51800000 };
/* v45: RAM structures found by the value scan (W5D30005/06): second-pass size words of x5 and x1 side by
 * side at 0x00A36E54..0x00A51xxx, live entries at 0x001AE4AC..0x001AE548, 0x00A50F64, 0x00A5106C.
 * Dumped as 0x001AE000 + 0x1000 bytes, then 0x00A36000 + 0x20000 bytes (cached RAM, read only). */
static const uint32_t md2_addr[2] = { 0x001AE000, 0x00A36000 };
static const uint32_t md2_len[2]  = { 0x1000, 0x20000 };
static volatile int md_on = 0;
static void md_task(void)
{
    if (md_on) return;
    md_on = 1;
    char name[32];
    uint32_t size;
    for (int i = 0; i < 10000; i++)
    {
        snprintf(name, sizeof(name), "ML/LOGS/M5D3%04d.BIN", i);
        if (FIO_GetFileSize(name, &size) != 0) break;
    }
    FILE * f = FIO_CreateFile(name);
    if (f)
    {
        for (int k = 0; k < 2; k++)
            for (uint32_t o = 0; o < md2_len[k]; o += 0x1000)
            {
                for (int i = 0; i < 0x400; i++) sh_buf[i] = MEM(md2_addr[k] + o + 4 * i);
                FIO_WriteFile(f, sh_buf, 0x1000);
            }
        FIO_CloseFile(f);
    }
    snprintf(last_status, sizeof(last_status), "%s", f ? name + 8 : "no file");
    NotifyBox(4000, "cm_wr: memory dump %s", last_status);
    md_on = 0;
}

static MENU_SELECT_FUNC(md_select)
{
    run_in_separate_task(md_task, 0);
}

/* v33: R0 (approved 2026-10-18) - move L0 inside its own LV region, same size. Canon's LV buffer
 * dispenser (0xFF165D3C toggling A/B, 0xFF165F54 not toggling; ops table 0xFF7F5644) returns type 7
 * (L0) as region base + 0x2000000. Both ADD #0x2000000 instructions become ADD #0x1000000: L0 moves to
 * base + 16 MB, inside Canon's buffer type 12 (+0x352000, 0x1CAE000), which never changed in movie LV
 * (memory watch WR5D3_83..87; photo LV used only its first ~8.7 MB). Size unchanged (5 MB).
 * The addresses are taken when a path is set up, so a zoom change is needed after patching and after
 * unpatching. While patched, the live EDMAC address of ch 0x13 (SSLT writer) and ch 0x1B (L0 reader)
 * is sampled: old L0 / new L0 / other. 30 s, then unpatched. The 2 sister dispensers (0xFF166498,
 * 0xFF166A68) are not touched. */
#define R0_A1     0xFF165DAC
#define R0_A2     0xFF165FC0
#define R0_OLD1   0xE28C3402      /* ADD R3, R12, #0x2000000 */
#define R0_NEW1   0xE28C3401      /* ADD R3, R12, #0x1000000 */
#define R0_OLD2   0xE2822402      /* ADD R2, R2, #0x2000000 */
#define R0_NEW2   0xE2822401      /* ADD R2, R2, #0x1000000 */
#define R0_MS     30000
static volatile int r0_on = 0;

static int r0_cls(uint32_t a)
{
    a &= 0x3FFFFFFF;
    if ((a >= 0x0CE00000 && a < 0x0D300000) || (a >= 0x12000000 && a < 0x12500000)) return 1;   /* old L0 */
    if ((a >= 0x0BE00000 && a < 0x0C300000) || (a >= 0x11000000 && a < 0x11500000)) return 2;   /* new L0 */
    return 0;
}

static void r0_task(void)
{
    if (!is_5d3() || running || r0_on || !lv || RECORDING || !is_movie_mode())
    {
        NotifyBox(3000, "cm_wr R0: movie LiveView, not recording, no other test");
        return;
    }
    uint32_t raw0 = shamem_read(REG_RAW);
    if (raw0 != RAW_5D3_X1)
    {
        NotifyBox(5000, "cm_wr R0: start in 1080p x1 (raw %x), nothing done", raw0);
        return;
    }
    if (MEM(R0_A1) != R0_OLD1 || MEM(R0_A2) != R0_OLD2)
    {
        NotifyBox(5000, "cm_wr R0: dispenser code unexpected, nothing done");
        return;
    }
    running = 1; r0_on = 1;
    int e1 = patch_instruction(R0_A1, R0_OLD1, R0_NEW1, "cm_wr: R0 L0 +16MB (A/B toggle)");
    int e2 = e1 ? -1 : patch_instruction(R0_A2, R0_OLD2, R0_NEW2, "cm_wr: R0 L0 +16MB");
    if (e1 || e2)
    {
        if (!e1) unpatch_memory(R0_A1);
        running = 0; r0_on = 0;
        NotifyBox(6000, "cm_wr R0: patch failed (%x %x), all undone", e1, e2);
        return;
    }
    static uint32_t modes[24][3];
    int nmodes = 0;
    uint32_t c13[3] = {0}, c1b[3] = {0};
    uint32_t c13_t[24][3], c1b_t[24][3];
    int t0 = get_ms_clock(), left = R0_MS / 1000, tick = 0;
    for (int k = 0; k < 24; k++) for (int j = 0; j < 3; j++) c13_t[k][j] = c1b_t[k][j] = 0;
    while (get_ms_clock() - t0 < R0_MS)
    {
        int t = get_ms_clock() - t0;
        uint32_t raw = shamem_read(REG_RAW);
        if (nmodes < 24 && (nmodes == 0 || modes[nmodes - 1][1] != raw || modes[nmodes - 1][2] != (uint32_t) lv_dispsize))
        { modes[nmodes][0] = t; modes[nmodes][1] = raw; modes[nmodes][2] = lv_dispsize; nmodes++; }
        int m = nmodes ? nmodes - 1 : 0;
        for (int i = 0; i < 64; i++)
        {
            int a = r0_cls(MEM(0xC0F26308)), b = r0_cls(MEM(0xC0F26B08));
            c13[a]++; c1b[b]++; c13_t[m][a]++; c1b_t[m][b]++;
        }
        if (++tick == 100) { tick = 0; NotifyBox(1500, "cm_wr R0: %ds, zoom x5 then x1 now", left--); }
        msleep(10);
    }
    unpatch_memory(R0_A2);
    unpatch_memory(R0_A1);
    running = 0; r0_on = 0;

    static char text[4096];
    int len = snprintf(text, sizeof(text),
        "# cm_wr v33 R0: L0 dispenser +0x2000000 -> +0x1000000 for %d ms (same size), then unpatched\n"
        "# EDMAC live address class: old L0 / new L0 / other\n"
        "ch13 total %d %d %d, ch1B total %d %d %d\n",
        R0_MS, c13[1], c13[2], c13[0], c1b[1], c1b[2], c1b[0]);
    for (int k = 0; k < nmodes; k++)
        len += snprintf(text + len, sizeof(text) - len, "mode %6d raw %08x zoom %d  ch13 %d %d %d  ch1B %d %d %d\n",
            modes[k][0], modes[k][1], modes[k][2], c13_t[k][1], c13_t[k][2], c13_t[k][0], c1b_t[k][1], c1b_t[k][2], c1b_t[k][0]);
    const char * n = save_text(text, len);
    snprintf(last_status, sizeof(last_status), "%s R0 new %d/%d", n, c13[2], c1b[2]);
    NotifyBox(8000, "cm_wr R0 done %s. Now zoom x5 -> x10 -> x1", last_status);
}

static MENU_SELECT_FUNC(r0_select)
{
    if (running) return;
    run_in_separate_task(r0_task, 0);
}

/* v32: read-only RAM pointer scan. Who holds the L0 / L1 buffer addresses (needed before L0 could be
 * moved into the unused LV memory found by the memory watch)? Scans cached RAM 0x1000..0x01000000
 * twice, 1 s apart, for the slot base addresses (cached / uncached, Y and C = +0x800); logs each hit
 * with both values (ping-pong entries flip between A and B). Nothing is written. */
#define PS_END   0x01000000
#define PS_MAX   192
static uint32_t ps_addr[PS_MAX], ps_v0[PS_MAX], ps_v1[PS_MAX];
static int ps_n = 0, ps_more = 0;
static volatile int ps_on = 0;
static int ps_match(uint32_t v)
{
    if ((v >> 30) > 1) return 0;            /* only cached 0x0/0x1... or uncached 0x4/0x5... */
    uint32_t base = (v & 0x3FFFFFFF) & ~0x800u;
    return base == 0x0CE00000 || base == 0x12000000 || base == 0x0AE00000 || base == 0x10000000;
}

static void ps_task(void)
{
    if (!is_5d3() || ps_on || !lv)
    {
        NotifyBox(3000, "cm_wr: 5D3, LiveView, scan not already running");
        return;
    }
    ps_on = 1;
    uint32_t raw = shamem_read(REG_RAW);
    int zoom = lv_dispsize;
    ps_n = 0; ps_more = 0;
    for (uint32_t a = 0x1000; a < PS_END; a += 4)
    {
        uint32_t v = MEM(a);
        if (ps_match(v))
        {
            if (ps_n < PS_MAX) { ps_addr[ps_n] = a; ps_v0[ps_n] = v; ps_n++; }
            else ps_more++;
        }
        if ((a & 0x3FFFF) == 0) msleep(1);
    }
    msleep(1000);
    for (int i = 0; i < ps_n; i++) ps_v1[i] = MEM(ps_addr[i]);
    ps_on = 0;

    static char text[8192];
    int len = snprintf(text, sizeof(text),
        "# cm_wr v32 RAM pointer scan (read-only): 0x1000..%08x, L0 (0CE00000/12000000) and L1 (0AE00000/10000000) "
        "bases, cached or uncached, +0x800; raw %08x zoom %d; hits %d (+%d not listed)\n# addr value value_after_1s\n",
        PS_END, raw, zoom, ps_n, ps_more);
    for (int i = 0; i < ps_n; i++)
        len += snprintf(text + len, sizeof(text) - len, "ps %08x %08x %08x\n", ps_addr[i], ps_v0[i], ps_v1[i]);
    const char * n = save_text(text, len);
    snprintf(last_status, sizeof(last_status), "%s ps %d hits", n, ps_n);
    NotifyBox(5000, "cm_wr: pointer scan %s", last_status);
}

static MENU_SELECT_FUNC(ps_select)
{
    if (ps_on) return;
    run_in_separate_task(ps_task, 0);
}

/* v31: read-only memory watch of Canon's two LV YUV regions (ROM table 0xFF8BED30: 0x4AE00000 and
 * 0x50000000, 0x2800000 each). Question: is the space between the SSLT slots (A/B +3.3 MB .. +32 MB)
 * really unused in every LV state, so a larger L0 buffer could live there? One uncached word per
 * 1 KB, every 64 KB block hashed, a sweep every 250 ms for 60 s; per block: changes, first change.
 * Nothing is written. Runs without the test lock, so it can watch Stage 4c too. */
#define MW_A      0x4AE00000
#define MW_B      0x50000000
#define MW_SIZE   0x02800000
#define MW_BLK    0x10000
#define MW_NBLK   (2 * (MW_SIZE / MW_BLK))
#define MW_MS     60000
struct mw_blk { uint32_t h0, h, changes, first_ms; };
static struct mw_blk mw[MW_NBLK];
static volatile int mw_on = 0;
#define MW_HALF   (MW_NBLK / 2)
/* no '%' / '/' on variables: the module may not link __aeabi_idivmod */
static uint32_t mw_addr(int b) { return b < MW_HALF ? MW_A + b * MW_BLK : MW_B + (b - MW_HALF) * MW_BLK; }
static uint32_t mw_hash_zero(void)
{
    uint32_t h = 2166136261u;
    for (uint32_t o = 0; o < MW_BLK; o += 1024) h = (h ^ 0) * 16777619u;
    return h;
}
static uint32_t mw_hash(uint32_t a)
{
    uint32_t h = 2166136261u;
    for (uint32_t o = 0; o < MW_BLK; o += 1024) h = (h ^ MEM(a + o)) * 16777619u;
    return h;
}

static void mw_task(void)
{
    if (!is_5d3() || mw_on || !lv)
    {
        NotifyBox(3000, "cm_wr: 5D3, LiveView, watch not already running");
        return;
    }
    mw_on = 1;
    static uint32_t modes[32][3];
    int nmodes = 0;
    int t0 = get_ms_clock(), sweeps = 0;
    for (int b = 0; b < MW_NBLK; b++) { mw[b].h0 = mw[b].h = mw_hash(mw_addr(b)); mw[b].changes = 0; mw[b].first_ms = 0; }
    while (get_ms_clock() - t0 < MW_MS)
    {
        int t = get_ms_clock() - t0;
        uint32_t raw = shamem_read(REG_RAW);
        if (nmodes < 32 && (nmodes == 0 || modes[nmodes - 1][1] != raw || modes[nmodes - 1][2] != (uint32_t) lv_dispsize))
        { modes[nmodes][0] = t; modes[nmodes][1] = raw; modes[nmodes][2] = lv_dispsize; nmodes++; }
        for (int b = 0; b < MW_NBLK; b++)
        {
            uint32_t h = mw_hash(mw_addr(b));
            if (h != mw[b].h) { if (!mw[b].changes) mw[b].first_ms = t; mw[b].changes++; mw[b].h = h; }
            if ((b & 63) == 63) msleep(1);
        }
        sweeps++;
        if ((sweeps & 3) == 0) NotifyBox(1500, "cm_wr: memory watch %ds", (MW_MS - t) / 1000);
        msleep(250);
    }
    mw_on = 0;

    static char text[12288];
    int len = snprintf(text, sizeof(text),
        "# cm_wr v31 memory watch (read-only): LV YUV regions A %08x B %08x, %d x 64 KB blocks, 1 word/KB, %d sweeps\n",
        MW_A, MW_B, MW_NBLK, sweeps);
    for (int k = 0; k < nmodes; k++)
        len += snprintf(text + len, sizeof(text) - len, "mode %6d raw %08x zoom %d\n", modes[k][0], modes[k][1], modes[k][2]);
    len += snprintf(text + len, sizeof(text) - len, "# runs: start end class (zero = all sampled words 0 at start; still = no change; changed n first_ms)\n");
    int b = 0;
    while (b < MW_NBLK && len < (int) sizeof(text) - 128)
    {
        int cls = mw[b].changes ? 2 : (mw[b].h0 == mw_hash_zero() ? 0 : 1);
        int e = b, nmax = 0, fmin = 0x7FFFFFFF;
        while (e < MW_NBLK && (e == b || e != MW_HALF))
        {
            int c2 = mw[e].changes ? 2 : (mw[e].h0 == mw_hash_zero() ? 0 : 1);
            if (c2 != cls) break;
            if ((int) mw[e].changes > nmax) nmax = mw[e].changes;
            if (mw[e].changes && (int) mw[e].first_ms < fmin) fmin = mw[e].first_ms;
            e++;
        }
        len += snprintf(text + len, sizeof(text) - len, "%08x %08x %s", mw_addr(b), mw_addr(e - 1) + MW_BLK,
            cls == 0 ? "zero" : cls == 1 ? "still" : "changed");
        if (cls == 2) len += snprintf(text + len, sizeof(text) - len, " max %d first %d", nmax, fmin);
        len += snprintf(text + len, sizeof(text) - len, "\n");
        b = e;
    }
    const char * n = save_text(text, len);
    snprintf(last_status, sizeof(last_status), "%s mw %d sweeps", n, sweeps);
    NotifyBox(5000, "cm_wr: memory watch %s", last_status);
}

static MENU_SELECT_FUNC(mw_select)
{
    if (mw_on) return;
    run_in_separate_task(mw_task, 0);
}

/* v30: read-only EDMAC scan. The v28/v29 display tests showed FrameParam w[1]/w[6] feed a blending
 * stage, not the main LCD picture. Which channel reads the SSLT output (L0) for the LCD / HD resizer?
 * For 2 s, all 48 channels' live address registers (MEM(base + 8), as ML edmac.c / "Show EDMAC
 * channels" read them) are sampled; per channel: samples in L0 / L1 / L2-4 / LCD VRAM, min, max. */
#define ES_NCH 48
static uint32_t es_base(int ch) { static const uint32_t b[3] = { 0xC0F04000, 0xC0F26000, 0xC0F30000 }; return b[ch >> 4] + ((ch & 15) << 8); }
struct es_ch { uint32_t n, l0, l1, lo, vram, mn, mx, moves, last; };
static struct es_ch es[ES_NCH];

static void es_task(void)
{
    /* v31: read-only, so it may run while Stage 4c runs (no test lock) */
    static volatile int es_on = 0;
    if (!is_5d3() || es_on || !lv || RECORDING)
    {
        NotifyBox(3000, "cm_wr: 5D3, LiveView, not recording, scan not already running");
        return;
    }
    es_on = 1;
    uint32_t raw = shamem_read(REG_RAW);
    int zoom = lv_dispsize;
    for (int c = 0; c < ES_NCH; c++) { es[c].n = es[c].l0 = es[c].l1 = es[c].lo = es[c].vram = es[c].moves = 0; es[c].mn = 0xFFFFFFFF; es[c].mx = 0; es[c].last = 0; }
    int t0 = get_ms_clock(), rounds = 0;
    while (get_ms_clock() - t0 < 2000)
    {
        for (int c = 0; c < ES_NCH; c++)
        {
            uint32_t a = MEM(es_base(c) + 8) & 0x3FFFFFFF;
            struct es_ch * e = &es[c];
            e->n++;
            if (a != e->last) { e->moves++; e->last = a; }
            if (a < e->mn) e->mn = a;
            if (a > e->mx) e->mx = a;
            if ((a >= 0x0CE00000 && a < 0x0D300000) || (a >= 0x12000000 && a < 0x12500000)) e->l0++;
            else if ((a >= 0x0AE00000 && a < 0x0B080000) || (a >= 0x10000000 && a < 0x10280000)) e->l1++;
            else if ((a >= 0x0B080000 && a < 0x0B200000) || (a >= 0x10280000 && a < 0x10400000)) e->lo++;
            else if (a >= 0x15000000 && a < 0x16000000) e->vram++;
        }
        if ((++rounds & 63) == 0) msleep(1);
    }
    es_on = 0;

    static char text[6144];
    int len = snprintf(text, sizeof(text),
        "# cm_wr v30 EDMAC scan (read-only): live MEM(base+8), 2 s, %d rounds, raw %08x zoom %d\n"
        "# ch base n moves L0 L1 L2-4 VRAM min max\n", rounds, raw, zoom);
    for (int c = 0; c < ES_NCH; c++)
    {
        struct es_ch * e = &es[c];
        if (e->moves <= 1 && !e->l0 && !e->l1 && !e->lo && !e->vram) continue;
        len += snprintf(text + len, sizeof(text) - len, "ch %02x %08x %d %d %d %d %d %d %08x %08x\n",
            c, es_base(c), e->n, e->moves, e->l0, e->l1, e->lo, e->vram, e->mn, e->mx);
    }
    const char * n = save_text(text, len);
    snprintf(last_status, sizeof(last_status), "%s es %d rounds", n, rounds);
    NotifyBox(5000, "cm_wr: EDMAC scan %s", last_status);
}

static MENU_SELECT_FUNC(es_select)
{
    run_in_separate_task(es_task, 0);
}

/* v22: Canon reference for the shadow comparison - nothing written, no hook, no patch:
 * two shadow snapshots 1 s apart in whatever mode Canon is in, plus a short WR log line */
static void shdump_task(void)
{
    if (!is_5d3())
    {
        NotifyBox(3000, "cm_wr: 5D3 1.1.3 only");
        return;
    }
    if (running || !lv)
    {
        NotifyBox(3000, "cm_wr: need LiveView, no test running");
        return;
    }
    running = 1;
    int ms0 = get_ms_clock(), t0 = efic_temp;
    uint32_t raw0 = shamem_read(REG_RAW), zoom0 = lv_dispsize;
    sh_name[0] = 0;
    sh_open();
    sh_snapshot();
    msleep(1000);
    sh_snapshot();
    sh_close();
    uint32_t raw1 = shamem_read(REG_RAW);
    running = 0;

    static char text[256];
    int len = snprintf(text, sizeof(text),
        "# cm_wr v22 shadow dump (read-only, Canon state, nothing written)\n"
        "shadow dump %s, raw %08x -> %08x, zoom %d, efic_temp_raw %d, uptime_s %d\n",
        sh_name[0] ? sh_name + 8 : "-", raw0, raw1, zoom0, t0, ms0 / 1000);
    const char * n = save_text(text, len);
    snprintf(last_status, sizeof(last_status), "%s %s", n, sh_name[0] ? sh_name + 8 : "-");
    NotifyBox(5000, "cm_wr: shadow dump %s", last_status);
}

static MENU_SELECT_FUNC(shdump_select)
{
    if (running) return;
    run_in_separate_task(shdump_task, 0);
}

static MENU_SELECT_FUNC(s4r1_select)
{
    if (running) return;
    s4_clear = 1;
    s4_wh_x5 = 1;
    s4_variant = 1;
    s4_quiet = 1;
    s4_r154 = 0;
    s4_ms = S4_MS;
    s4_r1 = 1;
    s4_f1 = 0;
    run_in_separate_task(s4_task, 0);
}

static MENU_SELECT_FUNC(s4r1b_select)
{
    if (running) return;
    s4_clear = 1;
    s4_wh_x5 = 1;
    s4_variant = 1;
    s4_quiet = 1;
    s4_r154 = 0;
    s4_ms = S4_MS;
    s4_r1 = 2;
    s4_f1 = 0;
    run_in_separate_task(s4_task, 0);
}

static void f1_start(int mode)
{
    if (running) return;
    s4_clear = 0;
    s4_wh_x5 = 0;      /* stage 4b: SSLT W/H 1920x1280 */
    s4_variant = 1;
    s4_quiet = 1;
    s4_r154 = 0;
    s4_ms = (mode == 5 && f4_sub == 1) ? 120000 : S4_MS;
    s4_r1 = 0;
    s4_f1 = mode;
    run_in_separate_task(s4_task, 0);
}
static MENU_SELECT_FUNC(f1a_select) { f1_start(1); }
static MENU_SELECT_FUNC(f1b_select) { f1_start(2); }
static MENU_SELECT_FUNC(f2_select) { f1_start(3); }
static MENU_SELECT_FUNC(f3_select) { f1_start(4); }
static MENU_SELECT_FUNC(f4a_select) { f4_sub = 0; f1_start(5); }
static MENU_SELECT_FUNC(f4b_select) { f4_sub = 1; f1_start(5); }
static MENU_SELECT_FUNC(f4c_select) { f4_sub = 2; f1_start(5); }
static MENU_SELECT_FUNC(g1_select) { f1_start(6); }

static MENU_SELECT_FUNC(s4cl_select)
{
    if (running) return;
    s4_clear = 1;
    s4_wh_x5 = 1;
    s4_variant = 1;
    s4_quiet = 1;
    s4_r154 = 0;
    s4_ms = S4_MS_LONG;
    s4_r1 = 0;
    s4_f1 = 0;
    run_in_separate_task(s4_task, 0);
}

static MENU_SELECT_FUNC(s4d_select)
{
    if (running) return;
    s4_clear = 1;
    s4_wh_x5 = 1;
    s4_variant = 1;
    s4_quiet = 1;
    s4_r154 = 1;
    s4_ms = S4_MS;
    s4_r1 = 0;
    s4_f1 = 0;
    run_in_separate_task(s4_task, 0);
}

static MENU_SELECT_FUNC(stage3a_select)
{
    run_in_separate_task(stage3a_task, 0);
}

static MENU_UPDATE_FUNC(stage3a_update)
{
    if (!is_5d3())
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "5D3 1.1.3 only (values measured on 5D3).");
    else if (last_status[0])
        MENU_SET_HELP("last: %s", last_status);   /* v19: not as value - it hid the item names */
}

static MENU_SELECT_FUNC(cm_wr_select)
{
    run_in_separate_task(cm_wr_task, 0);
}

static MENU_UPDATE_FUNC(cm_wr_update)
{
    if (!log_prefix())
    {
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Only 5D3 1.1.3 and EOS M 2.0.2.");
        return;
    }
    if (last_status[0]) MENU_SET_HELP("last: %s", last_status);
}

/* v20: only the stage 4c entries in the menu (the user picked neighbouring stages by mistake);
 * the older stages stay in the source and can be put back from this list */
static struct menu_entry cm_wr_menu_old[] __attribute__((unused)) =
{
    {
        .name   = "Stage 2: same-value write",
        .select = cm_wr_select,
        .update = cm_wr_update,
        .icon_type = IT_ACTION,
        .help   = "20 s: rewrites C0F38024 with the value it already has.",
        .help2  = "Never writes a different value. Screen should not change.",
    },
    {
        .name   = "Stage 3a: x5 window move",
        .select = stage3a_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "5D3 x5, 20 s: switches C0F383D4/DC to Canon's other x5 window pair.",
        .help2  = "Only Canon-measured values; restores the original pair at the end.",
    },
    {
        .name   = "Stage 3b-1: window top-left",
        .select = s3b1_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "5D3 x5, 20 s: only C0F383D4/DC -> top-left window, x5 size.",
        .help2  = "Checks Canon x5 values first; restores them at the end.",
    },
    {
        .name   = "Stage 3b-2: size group only",
        .select = s3b2_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "5D3 x5, 20 s: 11 size registers -> Canon x1 values, window stays.",
        .help2  = "Checks Canon x5 values first; restores them at the end.",
    },
    {
        .name   = "Stage 3b-3: full x1 block",
        .select = s3b3_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "5D3 x5, 20 s: 69 preview regs + window -> Canon x1 values.",
        .help2  = "Checks Canon x5 values first; restores them at the end.",
    },
    {
        .name   = "Stage E1a: x5 EDMAC x1 size",
        .select = e1a_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "5D3: start in 1080p x1, then go x5 within 30 s. Preview EDMAC only.",
        .help2  = "Canon x5 bytes -> Canon x1 bytes. Restore: x1 then x5 again.",
    },
    {
        .name   = "Stage E1b: E1a + 3b-3",
        .select = e1b_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "5D3: start in 1080p x1, go x5. EDMAC x1 size, then 71 preview regs x1.",
        .help2  = "Regs restored after 20 s. EDMAC restore: x1 then x5 again.",
    },
    {
        .name   = "Stage E3: SSLT x1 + 3b-3",
        .select = e3_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "5D3: start in 1080p x1, go x5. SSLT gets Canon x1 tables, then 3b-3.",
        .help2  = "Regs restored after 20 s. SSLT restore: zoom x10 -> x1.",
    },
    {
        .name   = "Stage 4: x5 1:1 x1-size",
        .select = s4_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "5D3: start in 1080p x1, go x5. SSLT crops an x1-size area of the x5 raw.",
        .help2  = "20 s in x5, then tables restored. Restore preview: zoom x10 -> x1.",
    },
    {
        .name   = "Stage 4b: as 4, x1 +0xCC",
        .select = s4b_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "Stage 4 with SSLT +0xCC..+0xD4 from the Canon x1 table (0x900/0x900/0x80).",
        .help2  = "20 s in x5, then tables restored. Restore preview: zoom x10 -> x1.",
    },
    {
        .name   = "Stage 4b quiet",
        .select = s4bq_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "Stage 4b without on-screen messages while in x5 (overlay test).",
        .help2  = "20 s in x5 (silent), then tables restored. Restore: zoom x10 -> x1.",
    },
    {
        .name   = "Stage 4d: 4c + C0F38154 x1",
        .select = s4d_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "4c + C0F38154 0x33 -> 0x133 (Canon x1 value) while in x5.",
        .help2  = "20 s in x5 (silent), then all restored. Restore: zoom x10 -> x1.",
    },
    {
        .name   = "R1b: R1 + NR reader fix",
        .select = s4r1b_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "R1 plus the 2 noise-reduction readers set to 8 KB lines (zebra fix).",
        .help2  = "Start in 1080p x1, go x5. 20 s, then all undone. Restore: zoom x10 -> x1.",
    },
    {
        .name   = "R1: 4c + L0 8 KB lines",
        .select = s4r1_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "Stage 4c with the full-size buffer moved +16 MB and 8 KB lines (same picture).",
        .help2  = "Start in 1080p x1, go x5. 20 s, then all undone. Restore: zoom x10 -> x1.",
    },
    {
        .name   = "R0: move L0 buffer (30 s)",
        .select = r0_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "Start in 1080p x1. 30 s: full-size preview buffer moved +16 MB (same size).",
        .help2  = "During the 30 s: zoom x5, then back x1. After: x5 -> x10 -> x1.",
    },
    {
        .name   = "Pointer scan (read-only)",
        .select = ps_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "Writes nothing: finds where Canon keeps the preview buffer addresses in RAM.",
        .help2  = "Run in Canon x1, Canon x5, and during Stage 4c. File WR5D3_NN.LOG.",
    },
    {
        .name   = "Memory watch (read-only)",
        .select = mw_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "Writes nothing: 60 s, which parts of Canon's LV image memory change.",
        .help2  = "Change zoom / start Stage 4c while it runs. File WR5D3_NN.LOG.",
    },
    {
        .name   = "EDMAC scan (read-only)",
        .select = es_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "Writes nothing: 2 s of live EDMAC addresses, which channel reads which buffer.",
        .help2  = "Run in Canon x1, then in Canon x5, then during Stage 4c.",
    },
    {
        .name   = "Display L1 test A: w6 (3 s)",
        .select = ds6_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "3 s: second copy of the full-size address -> half-size copy.",
        .help2  = "Use in plain Canon x1 (no other test).",
    },
    {
        .name   = "Display L1 test B: w1+w6 (3 s)",
        .select = ds16_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "3 s: both copies of the full-size address -> half-size copy.",
        .help2  = "Use in plain Canon x1 (no other test).",
    },
    {
        .name   = "Display L1 test (3 s)",
        .select = ds_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "3 s: LCD reads the half-size SSLT copy instead of the full one.",
        .help2  = "Use in plain Canon x1 (no other test). Expect a small picture top-left.",
    },
    {
        .name   = "Stage 4c long (90 s)",
        .select = s4cl_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "Same as 4c, but 90 s in x5 (watch when the early-boot streaks vanish).",
        .help2  = "90 s in x5 (silent), then all restored. Restore: zoom x10 -> x1.",
    },
    {
        .name   = "Memory dump (read-only)",
        .select = md_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "Writes nothing to the camera: 2nd-pass tables in RAM to ML/LOGS/M5D3nnnn.BIN.",
        .help2  = "Run once in Canon x1 and once in Canon x5.",
    },
    {
        .name   = "Value scan (read-only)",
        .select = vs_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "Writes nothing: finds where Canon keeps the 2nd-pass sizes in RAM.",
        .help2  = "Run once in Canon x1 and once in Canon x5 (no test running).",
    },
    {
        .name   = "2nd-pass pointer scan (read-only)",
        .select = pp_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "Writes nothing: finds RAM pointers to the 2nd-pass block table / path rows.",
        .help2  = "Run once in Canon x1 and once in Canon x5; wait ~10 s for the message.",
    },
    {
        .name   = "2nd-pass write timing (read-only)",
        .select = tw_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "Writes nothing: 15 s, logs when each LV stage's registers change.",
        .help2  = "Start in Canon 1080p x1, then switch to x5 within ~3 s and stay there.",
    },
    {
        .name   = "F3: x1 display path (Canon code)",
        .select = f3_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "4b + x5 uses Canon's own x1 display-path functions (mode table words 5..13).",
        .help2  = "Start in 1080p x1, go x5. 20 s, then zoom x10 -> x1 (patch undone after that).",
    },
    {
        .name   = "F2: x1 chain on x5 raw",
        .select = f2_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "4b + 178 registers after SSLT at Canon's own 1080p x1 values (routing incl.).",
        .help2  = "Start in 1080p x1, go x5. 20 s, screenshots 3 s / 12 s. If frozen: camera off/on.",
    },
    {
        .name   = "F1a: 4b + 2nd pass x1",
        .select = f1a_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "4b + noise-reduction/2nd-pass stage at Canon x1 sizes (83 regs).",
        .help2  = "Start in 1080p x1, go x5. 20 s, screenshots at 3 s / 12 s. Restore: x10 -> x1.",
    },
    {
        .name   = "F1b: F1a + YUV stretch",
        .select = f1b_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "F1a + C0F11 stretch 1948->1152, 1288->768 (output size unchanged).",
        .help2  = "Start in 1080p x1, go x5. 20 s, screenshots at 3 s / 12 s. Restore: x10 -> x1.",
    },
    {
        .name   = "G1: x5 with Canon x1 2nd-pass setup",
        .select = g1_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "4b + x5 branch of 0x302 template selector -> Canon's own 1080p x1 branch.",
        .help2  = "Start in 1080p x1, go x5. 20 s. If frozen/Err: battery out.",
    },
    {
        .name   = "Stage 4c: 4b + VRAM clear",
        .select = s4c_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "4b quiet, W/H x5 + 2 crop words only, + x5 LCD VRAM clear 0 -> 0x5A0.",
        .help2  = "20 s in x5 (silent), then all restored. Restore: zoom x10 -> x1.",
    },
};

/* v65: persistent multi-mode Crop Mood for 5D3 1.1.3:
 *  Mode 1: 3x Full (x5)        - 1920x1280 (3:2) 1:1 sensor pixel WYSIWYG in x5
 *  Mode 2: 3x 1080p (x5)       - 1920x1080 (16:9) 1:1 sensor pixel WYSIWYG in x5
 *  Mode 3: 3x Scope (x5)       - 1920x816  (2.35:1) 1:1 sensor pixel WYSIWYG in x5
 *  Mode 4: 3.5K Anam 16:9 (x1) - 1136x1920 (24p) / 1024x1728 (25p) / 912x1536 (30p) 1x3 Anamorphic -> 16:9
 *  Mode 5: 4.5K Anam Scope(x1) - 1504x1920 (24p) / 1360x1728 (25p) / 1200x1536 (30p) 1x3 Anamorphic -> 2.35:1
 *  Mode 6: 5.7K Anam Max (x1)  - 1920x1920 (24p) / 1920x1728 (25p) / 1920x1536 (30p) 1x3 Anamorphic -> 3:1 Max
 *  Mode 0: OFF */
#define F4_EEKO_402_M1    (cm_fw->eeko_m1 | 0x40000000u)
#define F4_EEKO_402_M1C   (cm_fw->eeko_m1)
#define F4_EEKO_DIRTY_U   0x41E80564
#define F4_EEKO_DIRTY_C   0x01E80564
#define X1_CLEAR_ADDR     (cm_fw->x1_clear_addr)
#define CMOS_WRITE_ADDR   0x119CC
#define CMOS_WRITE_I0     0xE92D47F0
#define CMOS_WRITE_I1     0xE51F8400
#define ADTG_WRITE_ADDR   0x11640
#define ADTG_WRITE_I0     0xE92D47F0
#define ADTG_WRITE_I1     0xE51F6074
#define ENGIO_WRITE_ADDR  (cm_fw->engio_write_addr)
#define ENGIO_WRITE_I0    0xE51FC15C
#define ENGIO_WRITE_I1    0xE8900006

static volatile int cm_live_mode = 1;
static volatile int cm_live_armed = 0;

int cm_wr_get_mode(void)
{
    return cm_live_mode;
}
static volatile int cm_live_wins = 0;
static volatile int cm_live_err = 0;
static volatile int cm_1x3_active = 0;
static volatile int cm_engio_vidmode_ok = 0;
static volatile int cm_cmos_vidmode_ok = 0;
static volatile int cm_1x3_cmos_cnt = 0;
static volatile int cm_1x3_adtg_cnt = 0;
static volatile int cm_1x3_302_cnt = 0;
static volatile int cm_1x3_303_cnt = 0;
static volatile int cm_1x3_l1_swaps = 0;
static volatile int cm_1x3_403_cnt = 0;
static volatile int x1_l1_active = 0;
static uint32_t cm_1x3_dbg_l0 = 0;
static uint32_t cm_1x3_dbg_l1 = 0;
static int cm_1x3_hooked = 0;
static int f4_m1_patched = 0;
static uint16_t cm_cmos_copy[64];
static uint32_t cm_adtg_copy[128];
static uint32_t cm_engio_copy[1024];

extern int gui_menu_shown(void);
extern int liveview_display_idle(void);
extern void fake_simple_button(int bgmt_code);
#define BGMT_PRESS_ZOOM_IN_5D3 0x12

static void cm_apply_x1_vscale(int anam);
static void cm_apply_hfps_vscale(int enable);
static void cm_x1_tall_unpatch(void);
static int  cm_x1_tall_patch(void);
/* prepro / HIV / SSLT yres_delta chosen by cm_x1_tall_patch() (0 = not set: clamp to 662).
 * > 662 (mode 13, 2070 lines) only when the HIV yb could be set to match (KI-3). */
static volatile uint32_t cm_x1_prepro_yd = 0;
static void cm_hfps_tall_unpatch(void);
static int  cm_hfps_tall_patch(void);
static int  cm_mode_is_hfps(int m);
static void cm_recompute_mode(void);

static void cm_sync_shared_mode(void)
{
    int shared = (cm_live_armed > 0) ? cm_live_armed : cm_live_mode;
    if (MEM(LV_STATE_ADDR + 0x08) == 1 && !cm_mode_is_hfps(shared))
        shared = 0;
    else if (MEM(LV_STATE_ADDR + 0x08) != 1 && cm_mode_is_hfps(shared))
        shared = 0;
    raw_capture_info.reserved = 0x5D30u | (uint16_t)(shared & 0x0Fu);
}

static int cm_raw_is_x1(uint32_t raw)
{
    return (raw & 0xFFFF) == 0x011B && (raw >> 16) >= 0x0500u;
}

static int cm_raw_is_720p(uint32_t raw)
{
    return (raw & 0xFFFF) == 0x011B && (raw >> 16) >= 0x0280u && (raw >> 16) < 0x0500u;
}

#define CM_HFPS_LOG_MAX 4096
static char cm_hfps_buf[CM_HFPS_LOG_MAX];
static int  cm_hfps_len = 0;
static int  cm_hfps_dirty = 0;

#define cm_hfps_logf(fmt, ...) do { \
    if (cm_hfps_len < CM_HFPS_LOG_MAX - 640) { \
        int _n = snprintf(cm_hfps_buf + cm_hfps_len, CM_HFPS_LOG_MAX - cm_hfps_len, fmt, ##__VA_ARGS__); \
        if (_n > 0) { \
            cm_hfps_len += _n; \
            if (cm_hfps_len >= CM_HFPS_LOG_MAX) cm_hfps_len = CM_HFPS_LOG_MAX - 1; \
            cm_hfps_dirty = 1; \
        } \
    } \
} while (0)

#define cm_hfps_flush_logf(fmt, ...) do { \
    if (cm_hfps_len < CM_HFPS_LOG_MAX - 32) { \
        int _n = snprintf(cm_hfps_buf + cm_hfps_len, CM_HFPS_LOG_MAX - cm_hfps_len, fmt, ##__VA_ARGS__); \
        if (_n > 0) { \
            cm_hfps_len += _n; \
            if (cm_hfps_len >= CM_HFPS_LOG_MAX) cm_hfps_len = CM_HFPS_LOG_MAX - 1; \
            cm_hfps_dirty = 1; \
        } \
    } \
} while (0)

static void cm_hfps_flush(const char *tag)
{
    if (tag && tag[0] == 'P' && tag[1] == 'R' && tag[2] == 'E')
        cm_hfps_len = 0; /* keep latest transition so POST_RESUME is never truncated */
    cm_hfps_flush_logf("[%s] m=%d arm=%d lv=%d z=%d S=%x fm=%x res=%x Z=%x raw=%08X 6800=%08X 6010=%08X 6014=%08X 713C=%08X 7150=%08X 8024=%08X ws=%08X we=%08X dsp=%08X cmos=%d adtg=%d e3=%d\n",
        tag, cm_live_mode, cm_live_armed, lv, lv_dispsize,
        MEM(LV_STATE_ADDR + 0x08), MEM(LV_STATE_ADDR + 0x04),
        MEM(LV_STATE_ADDR + 0x0C), MEM(LV_STATE_ADDR + 0x10),
        lv ? shamem_read(0xC0F06804) : 0,
        lv ? shamem_read(0xC0F06800) : 0,
        lv ? shamem_read(0xC0F06010) : 0,
        lv ? shamem_read(0xC0F06014) : 0,
        lv ? shamem_read(0xC0F0713C) : 0,
        lv ? shamem_read(0xC0F07150) : 0,
        lv ? shamem_read(0xC0F38024) : 0,
        lv ? shamem_read(REG_WIN_START) : 0,
        lv ? shamem_read(REG_WIN_END) : 0,
        MEM(DISP_ADDR_PTR),
        cm_1x3_cmos_cnt, cm_1x3_adtg_cnt, e3_replaced);
    if (lv)
    {
        uint32_t k2u = (cm_fw->eeko_k1 + 0x38u) | 0x40000000u;
        cm_hfps_flush_logf("EEKO st=%x/%x/%x/%x/%x/%x 508=%08X 50C=%08X 548=%08X 54C=%08X 558=%08X 55C=%08X 308=%08X a04c=%08X 8070=%08X k2=%u/%u/%u/%u\n",
            MEM(0x01E8053C + 0x00), MEM(0x01E8053C + 0x04), MEM(0x01E8053C + 0x08),
            MEM(0x01E8053C + 0x0C), MEM(0x01E8053C + 0x10), MEM(0x01E8053C + 0x14),
            shamem_read(0xC0F11508), shamem_read(0xC0F1150C),
            shamem_read(0xC0F11548), shamem_read(0xC0F1154C),
            shamem_read(0xC0F11558), shamem_read(0xC0F1155C),
            shamem_read(0xC0F11308),
            shamem_read(0xC0F3A04C), shamem_read(0xC0F38070),
            MEM(k2u + 0), MEM(k2u + 4), MEM(k2u + 8), MEM(k2u + 12));
    }
    FILE *f = FIO_CreateFile("ML/LOGS/CMHFPS.LOG");
    if (f)
    {
        FIO_WriteFile(f, cm_hfps_buf, cm_hfps_len);
        FIO_CloseFile(f);
        cm_hfps_dirty = 0;
    }
}

static int cm_mode_is_x5(int m)
{
    return (m >= 1 && m <= 3) || (m >= 7 && m <= 9);
}

static int cm_mode_is_x1_anam(int m)
{
    return (m >= 4 && m <= 6) || (m == 13);
}

static int cm_mode_is_hfps(int m)
{
    return (m >= 10 && m <= 12);
}

#define RAW_5D3_720 0x02B6011Bu

static int cm_x1_is_1080p_path(void)
{
    return (MEM(LV_STATE_ADDR) == 1 && (MEM(LV_STATE_ADDR + 0x10) >> 16) == 1);
}

static int cm_x1_is_720p_path(void)
{
    return (MEM(LV_STATE_ADDR) == 1 &&
            MEM(LV_STATE_ADDR + 0x08) == 1 &&
            MEM(LV_STATE_ADDR + 0x0C) == 1 &&
            (MEM(LV_STATE_ADDR + 0x10) >> 16) == 1);
}

static int nrzi_encode(int in_val)
{
    int out_val = 0;
    int old_bit = 0;
    for (int num = 0; num < 31; num++)
    {
        int bit = in_val & (1 << (30 - num)) ? 1 : 0;
        if (bit != old_bit)
            out_val |= (1 << (30 - num));
        old_bit = bit;
    }
    return out_val;
}

static void cm_get_tall_params(int mode, int * yres_delta_out, uint16_t * cmos1_out,
                               int * timer_a_out, int * timer_b_out,
                               int * orig_timer_a_out, int * orig_timer_b_out)
{
    uint32_t fm = MEM(LV_STATE_ADDR + 0x04);
    if (cm_mode_is_hfps(mode))
    {
        /* Family 4: High FPS 3x3 (modes 10..12) on Canon 720p 50/60fps path (S=1, fm=0/1).
         * Matches proven crop_rec.c 5D3 3x3 vertical blanking headroom (171..220 lines):
         *   Mode 10 (50/60p Native):
         *     50p (fm=1): 1920x960  @ 50.00p (yres_delta=296, CMOS[1]=0x384, TimerA=400, TimerB=1200)
         *     60p (fm=0): 1920x800  @ 59.94p (yres_delta=136, CMOS[1]=0x386, TimerA=400, TimerB=1001)
         *   Mode 11 (45/48p Tall):
         *     45p (fm=1): 1920x1080 @ 45.00p (yres_delta=416, CMOS[1]=0x3C3, TimerA=401, TimerB=1330)
         *     48p (fm=0): 1920x1040 @ 48.00p (yres_delta=376, CMOS[1]=0x3C3, TimerA=400, TimerB=1250)
         *   Mode 12 (2.40 Scope 50/60p):
         *     50p (fm=1): 1920x800  @ 50.00p (yres_delta=136, CMOS[1]=0x386, TimerA=400, TimerB=1200)
         *     60p (fm=0): 1920x800  @ 59.94p (yres_delta=136, CMOS[1]=0x386, TimerA=400, TimerB=1001) */
        int is_720 = (MEM(LV_STATE_ADDR + 0x08) == 1 && (fm == 0 || fm == 1));
        int orig_ta = (fm == 1) ? 480 : 440;
        int orig_tb = (fm == 1) ? 1000 : (fm == 0) ? 910 : 0;
        if (orig_timer_a_out) *orig_timer_a_out = orig_ta;
        if (orig_timer_b_out) *orig_timer_b_out = orig_tb;
        int yd = 0, ta = 0, tb = 0;
        uint16_t c1 = 0x386;
        if (is_720)
        {
            if (mode == 10)
            {
                yd = (fm == 1) ? 296 : 136;
                c1 = (fm == 1) ? 0x384 : 0x386;
                ta = 400;
                tb = (fm == 1) ? 1200 : 1001;
            }
            else if (mode == 11)
            {
                yd = (fm == 1) ? 416 : 376;
                c1 = 0x3C3;
                ta = (fm == 1) ? 401 : 400;
                tb = (fm == 1) ? 1330 : 1250;
            }
            else /* mode == 12 */
            {
                yd = 136;
                c1 = 0x386;
                ta = 400;
                tb = (fm == 1) ? 1200 : 1001;
            }
        }
        if (yres_delta_out) *yres_delta_out = yd;
        if (cmos1_out)      *cmos1_out      = c1;
        if (timer_a_out)    *timer_a_out    = ta;
        if (timer_b_out)    *timer_b_out    = tb;
        return;
    }

    int orig_ta = (fm == 3) ? 480 : 440;
    int orig_tb = (fm == 4) ? 2275 : (fm == 3) ? 2000 : (fm == 2) ? 1820 : 0;
    if (orig_timer_a_out) *orig_timer_a_out = orig_ta;
    if (orig_timer_b_out) *orig_timer_b_out = orig_tb;

    if (mode == 13)
    {
        /* Mode 13 (5.4K 2040p 24p): TimerA=418, TimerB=2395 (full 293-line OB clamp window @ exact 23.973 fps), CMOS[1]=0x348 */
        if (yres_delta_out) *yres_delta_out = (fm == 4) ? 782   : (fm == 3) ? 510   : (fm == 2) ? 278   : 0;
        if (cmos1_out)      *cmos1_out      = (fm == 4) ? 0x348 : (fm == 3) ? 0x308 : (fm == 2) ? 0x2C9 : 0x2CB;
        if (timer_a_out)    *timer_a_out    = (fm == 4) ? 418 : 0;
        if (timer_b_out)    *timer_b_out    = (fm == 4) ? 2395 : (fm == 3) ? 2040 : 0;
        return;
    }

    /* 1x3 tall with STOCK Canon FPS timers (zero TimerA overclock -> zero vertical banding and zero OB clamp black-level droop!):
     *   24p (fm=4): stock TimerA=440, TimerB=2275, yres_delta=662 -> 1920 active lines, CMOS[1]=0x348 PACK12(8,13)
     *   25p (fm=3): stock TimerA=480, TimerB=2000, yres_delta=470 -> 1728 active lines, CMOS[1]=0x308 PACK12(8,12)
     *   30p (fm=2): stock TimerA=440, TimerB=1820, yres_delta=278 -> 1536 active lines, CMOS[1]=0x2C9 PACK12(9,11) */
    if (yres_delta_out) *yres_delta_out = (fm == 4) ? 662   : (fm == 3) ? 470   : (fm == 2) ? 278   : 0;
    if (cmos1_out)      *cmos1_out      = (fm == 4) ? 0x348 : (fm == 3) ? 0x308 : (fm == 2) ? 0x2C9 : 0x2CB;
    if (timer_a_out)    *timer_a_out    = 0;
    if (timer_b_out)    *timer_b_out    = 0;
}

static void cm_clear_lv_vram(void)
{
    for (int k = 0; k < 3; k++)
    {
        uint32_t unc = lvbuf[k] | 0x40000000u;
        memset((void *)unc, 0, 720 * 480 * 2);
    }
}

/* Pack 16 identical 14-bit LE samples into 7 uint32_t words (28 bytes). */
static void cm_pack14(uint32_t v, uint32_t out[7])
{
    for (int i = 0; i < 7; i++) out[i] = 0;
    v &= 0x3FFFu;
    for (int s = 0; s < 16; s++)
    {
        int bit = s * 14;
        int w = bit >> 5;
        int sh = bit & 31;
        out[w] |= v << sh;
        if (sh > 18 && w + 1 < 7)
            out[w + 1] |= v >> (32 - sh);
    }
}

/* Fill a 0x1000-byte-stride L0/L1 region with 14-bit packed Y (+0x000) and C (+0x800). */
static void cm_fill_l0_val(uint32_t addr, uint32_t bytes, uint32_t y_val, uint32_t c_val)
{
    uint32_t y14[7], c14[7];
    cm_pack14(y_val, y14);
    cm_pack14(c_val, c14);
    for (uint32_t off = 0; off < bytes; off += 0x1000u)
    {
        uint32_t *yp = (uint32_t *)(addr + off);
        uint32_t *cp = (uint32_t *)(addr + off + 0x800u);
        for (int g = 0; g < 69; g++) /* 69 * 28 = 1932 bytes (covers full 0x700..0x780 active width) */
        {
            for (int j = 0; j < 7; j++)
            {
                yp[j] = y14[j];
                cp[j] = c14[j];
            }
            yp += 7;
            cp += 7;
        }
    }
}

/* Fill top/bottom pads of ALL 5 pyramid levels (L0..L4) with exact Canon hardware black
 * measured from dark-frame L0BLK2.LOG:
 *   L0 (+0x000 / +0x800): 14-bit 0x0200 / 0x0200
 *   L1 (+0x000 / +0x800): 14-bit 0x0200 / 0x1100
 *   L2..L4 (32-bit word): 0x02001100 */
static void cm_clear_1x3_buffers(void)
{
    static const uint32_t bases[2] = { 0x50000000u, 0x4AE00000u };
    for (int k = 0; k < 2; k++)
    {
        /* L0 top pad (+0x1000000..+0x1340000) & L0 bottom pad (+0x18C0000..+0x1D60000) */
        cm_fill_l0_val(bases[k] + 0x1000000u, 0x340000u, 0x0200u, 0x0200u);
        cm_fill_l0_val(bases[k] + 0x18C0000u, 0x4A0000u, 0x0200u, 0x0200u);
        /* L1 top pad (+0x1D60000..+0x1F00000) & L1 bottom pad (+0x2200000..+0x2500000) */
        cm_fill_l0_val(bases[k] + 0x1D60000u, 0x1A0000u, 0x0200u, 0x1100u);
        cm_fill_l0_val(bases[k] + 0x2200000u, 0x300000u, 0x0200u, 0x1100u);
        /* L2..L4 pads (+0x0090000..+0x0340000): 32-bit 0x02001100 */
        uint32_t *p = (uint32_t *)(bases[k] + 0x0090000u);
        for (uint32_t i = 0; i < (0x2B0000u >> 2); i++)
            p[i] = 0x02001100u;
    }
}

static void cm_restore_stock_x1_sensor(void)
{
    void (*cmos_fn)(uint16_t *) = (void (*)(uint16_t *))CMOS_WRITE_ADDR;
    void (*adtg_fn)(int, uint32_t *) = (void (*)(int, uint32_t *))ADTG_WRITE_ADDR;
    int is_720 = (MEM(LV_STATE_ADDR + 0x08) == 1);
    uint32_t fm = MEM(LV_STATE_ADDR + 0x04);
    uint16_t cmos_restore[4];
    cmos_restore[0] = 0x1800;
    cmos_restore[1] = 0x200A;
    cmos_restore[2] = 0x60B0;
    cmos_restore[3] = 0xFFFF;
    uint32_t adtg_restore[2];
    adtg_restore[0] = is_720 ? 0x800C0004u : 0x800C0002u;
    adtg_restore[1] = 0xFFFFFFFFu;
    int orig_ta = 0, orig_tb = 0;
    cm_get_tall_params(cm_live_mode, 0, 0, 0, 0, &orig_ta, &orig_tb);
    cmos_fn(cmos_restore);
    adtg_fn(6, adtg_restore);
    uint32_t stock_raw = is_720 ? RAW_5D3_720 : RAW_5D3_X1;
    uint32_t cur_raw = shamem_read(0xC0F06804);
    if (cur_raw != stock_raw && (cm_raw_is_x1(cur_raw) || cm_raw_is_720p(cur_raw)))
    {
        EngDrvOutLV(0xC0F06804, stock_raw);
        EngDrvOutLV(0xC0F0713C, is_720 ? ((fm == 1) ? 0x02E6u : 0x02B4u) : 0x055Eu);
        EngDrvOutLV(0xC0F07150, is_720 ? ((fm == 1) ? 0x02B4u : 0x026Du) : 0x0527u);
        EngDrvOutLV(0xC0F06800, 0x00010017u);
        if (orig_ta > 0 && orig_tb > 0)
        {
            uint32_t ta = (uint32_t)(orig_ta - 1);
            uint32_t ta2 = ta | (ta << 16);
            uint32_t tb = (uint32_t)(orig_tb - 1);
            EngDrvOutLV(0xC0F06824, ta);
            EngDrvOutLV(0xC0F06828, ta);
            EngDrvOutLV(0xC0F0682C, ta);
            EngDrvOutLV(0xC0F06830, ta);
            EngDrvOutLV(0xC0F06010, ta);
            EngDrvOutLV(0xC0F06008, ta2);
            EngDrvOutLV(0xC0F0600C, ta2);
            EngDrvOutLV(0xC0F06014, tb);
        }
    }
}

#define BGMT_MENU_5D3 6
/* After the x10 -> x1 zoom path Canon sends 0x403 with the display sub-window off (FUN_ff33db80: w7 = 0)
 * -> squashed ~54-line strip. Forcing w7 = 1 in the message stalls Eeko (2026-11-08 test J);
 * a Canon MENU open/close re-runs the LV display setup like the user's manual MENU in/out did. */
/* 2026-11-08 stage 1: instead of the Canon MENU press, restart LV with ML PauseLiveView/ResumeLiveView
 * (PROP_LV_ACTION stop/start). Root cause: w7 comes from FUN_ff33be18, which reads Canon's 8-phase
 * one-hot counter MEM(MEM(0x2D38C)) (1..8 -> w7=0, 0x10..0x80 -> w7=1). After x10->x1 it stays stuck
 * in the low group. A full LV start goes through FUN_ff18253c -> FUN_ff33bb7c, which resets it (as MENU does).
 * Called only from cm_live_task (ML menu closed, not recording, LV running). Counter is read only, for the log. */
static void cm_menu_cycle(void)
{
    if (!lv || lv_paused || RECORDING || gui_menu_shown() || lv_dispsize != 1) return;
    gui_uilock(UILOCK_EVERYTHING);
    PauseLiveView();
    msleep(200);
    ResumeLiveView();
    gui_uilock(UILOCK_NONE);
    for (int w = 0; w < 100 && !lv; w++) msleep(20);
    /* Wait for Canon's ResumeLiveView() x1 SSLT/Eeko pipeline to finish settling before
     * any caller fires BGMT_PRESS_ZOOM_IN_5D3 (prevents x1/x5 Eeko overlap -> pink freeze). */
    for (int w = 0; w < 50 && lv &&
         ((!cm_raw_is_x1(shamem_read(REG_RAW)) && !cm_raw_is_720p(shamem_read(REG_RAW))) || MEM(0x01E8053C + 0x10) != 1); w++)
        msleep(20);
    for (int w = 0; w < 10 && lv && is_movie_mode() && !gui_menu_shown(); w++)
        msleep(50);
}

static uint32_t cm_hfps_y_shift(int mode)
{
    uint32_t fm = MEM(LV_STATE_ADDR + 0x04);
    if (mode == 11) return 168u;                  /* 45/48p (1080/1040): center 642-line LV window vertically */
    if (mode == 10 && fm == 1) return 112u;       /* 50p (960): center 642-line LV window vertically */
    return 0u;                                    /* 60p / Scope (800): already centered */
}

static void cm_zoom_to_x1(void)
{
    if (!lv || !is_movie_mode() || RECORDING) return;
    int was_1x3 = cm_1x3_active;
    int is_hfps = cm_mode_is_hfps(cm_live_mode);
    if (was_1x3)
    {
        cm_1x3_active = 0;
        cm_engio_vidmode_ok = 0;
        cm_cmos_vidmode_ok = 0;
        if (!is_hfps)
            cm_apply_x1_vscale(0);
    }

    /* Stage 2: drop straight from x5/x10 to x1 via set_lv_zoom(1) — never cycles through x10! */
    if (lv_dispsize != 1)
    {
        set_lv_zoom(1);
        for (int w = 0; w < 50 && lv &&
             (lv_dispsize != 1 || (!cm_raw_is_x1(shamem_read(REG_RAW)) && !cm_raw_is_720p(shamem_read(REG_RAW))) || MEM(0x01E8053C + 0x10) != 1); w++)
            msleep(20);
    }

    /* Cleanly rebuild sensor/SSLT/Eeko in x1: pause LiveView FIRST so all 1x3 L0..L4/SSLT
     * patches and buffer clears happen while LV is off (no pink/green flash before blink). */
    if (was_1x3 && is_hfps)
    {
        if (cm_x1_is_720p_path() && lv && lv_dispsize == 1 && !lv_paused && !gui_menu_shown())
        {
            cm_hfps_flush("PRE_PAUSE");
            gui_uilock(UILOCK_EVERYTHING);
            PauseLiveView();
            msleep(200);
            cm_1x3_active = 1;
            cm_engio_vidmode_ok = 1;
            cm_cmos_vidmode_ok = 1;
            cm_hfps_tall_patch();
            cm_apply_hfps_vscale(1);
            cm_clear_lv_vram();
            ResumeLiveView();
            gui_uilock(UILOCK_NONE);
            for (int w = 0; w < 100 && !lv; w++) msleep(20);
            for (int w = 0; w < 50 && lv && (!cm_raw_is_720p(shamem_read(REG_RAW)) || MEM(0x01E8053C + 0x10) != 1); w++)
                msleep(20);
            for (int w = 0; w < 10 && lv && is_movie_mode() && !gui_menu_shown(); w++)
                msleep(50);
            cm_hfps_flush("POST_RESUME");
        }
        else
        {
            cm_1x3_active = 1;
        }
    }
    else if (was_1x3 && lv && lv_dispsize == 1 && !lv_paused && !gui_menu_shown())
    {
        gui_uilock(UILOCK_EVERYTHING);
        PauseLiveView();
        msleep(200);
        cm_1x3_active = 1;
        cm_engio_vidmode_ok = 1;
        cm_cmos_vidmode_ok = 1;
        cm_x1_tall_patch();
        cm_apply_x1_vscale(1);
        /* Do NOT write raw 0x4AE00000/0x50000000 while LV is paused: Canon's RscMgr frees/rebuilds
         * SRM PackHeap (FreeMemoryToShootMemoryObject) during PauseLiveView -> Memory.c:568 assert! */
        cm_clear_lv_vram();
        ResumeLiveView();
        gui_uilock(UILOCK_NONE);
        for (int w = 0; w < 100 && !lv; w++) msleep(20);
        for (int w = 0; w < 50 && lv && (!cm_raw_is_x1(shamem_read(REG_RAW)) || MEM(0x01E8053C + 0x10) != 1); w++)
            msleep(20);
        for (int w = 0; w < 10 && lv && is_movie_mode() && !gui_menu_shown(); w++)
            msleep(50);
    }
    else if (!was_1x3 && lv && lv_dispsize == 1)
    {
        cm_menu_cycle();
    }
}

static void cm_zoom_to_x5(void)
{
    if (!lv || !is_movie_mode() || RECORDING) return;
    for (int p = 0; p < 4 && lv && !RECORDING && !gui_menu_shown(); p++)
    {
        if (shamem_read(REG_RAW) == RAW_5D3_X5 && lv_dispsize == 5 && (MEM(LV_STATE_ADDR + 0x10) >> 16) == 5)
            break;
        int prev_z = lv_dispsize;
        uint32_t prev_r = shamem_read(REG_RAW);
        fake_simple_button(BGMT_PRESS_ZOOM_IN_5D3);
        for (int w = 0; w < 50 && lv && shamem_read(REG_RAW) == prev_r && lv_dispsize == prev_z; w++)
            msleep(10);
        if (shamem_read(REG_RAW) == RAW_5D3_X5)
        {
            uint32_t ws = shamem_read(REG_WIN_START), we = shamem_read(REG_WIN_END);
            if (!s4_win_ok(ws, we))
            {
                EngDrvOutLV(REG_WIN_START, S4_WIN_S);
                EngDrvOutLV(REG_WIN_END, S4_WIN_E);
            }
            if (lv_dispsize == 5 && (MEM(LV_STATE_ADDR + 0x10) >> 16) == 5)
                break;
        }
        msleep(100);
    }
}

static uint32_t cm_x1_ss_h(void)
{
    int yd = 0;
    cm_get_tall_params(cm_live_mode, &yd, 0, 0, 0, 0, 0);
    if (yd <= 0) return 1280;
    int prepro_yd = (yd > 662) ? 662 : yd;
    uint32_t raw_v = 0x528u + (uint32_t)prepro_yd;
    uint32_t ss_h = (raw_v - 48u) & ~31u; /* 24p: 1888, 25p: 1696, 30p: 1504 (multiple of 32 for L0..L4) */
    if (ss_h > 1888u) ss_h = 1888u;
    return ss_h;
}

#define F4_303_CALLER (cm_fw->f4_303_caller)

static void cm_x1_403_modify(uint32_t * regs, uint32_t caller)
{
    if (!cm_1x3_active) return;
    if (regs[2] == 0x302 && caller == F4_CALLER)
    {
        cm_1x3_302_cnt++;
        if (!x1_l1_active) return;
        uint32_t a = regs[3] & ~0x40000000u;
        if (a < 0x1000u || a >= 0x20000000u || (a & 3u)) return;
        uint32_t u = a | 0x40000000u;
        uint32_t old = cli();
        if (MEM(u) == 1u)
        {
            uint32_t s0 = MEM(u + 19 * 4);
            uint32_t s1 = MEM(u + 32 * 4);
            cm_1x3_dbg_l0 = s0;
            cm_1x3_dbg_l1 = s1;
            /* 3:1 (ODD!) vertical line-skip on ALL 5 pyramid levels (L0..L4):
             *   L0/L1: w[19]=w[32]=0x2900 (+2*0x1000), L2: w[45]=0x1020 (+2*0x800),
             *   L3:    w[58]=0x0800 (+2*0x400),        L4: w[71]=0x0400 (+2*0x200).
             * Aligning L2..L4 with L0/L1 eliminates multi-scale NR shadow ghosting/cloning!
             * Shifting L0 back by -0x330000 reads strictly inside our zeroed black pad (+0x1010000). */
            if (s0 == 0x900u && s1 == 0x900u)
            {
                uint32_t l0c = MEM(u + 14 * 4);
                uint32_t l0y = MEM(u + 15 * 4);
                uint32_t l1c = MEM(u + 27 * 4);
                uint32_t l1y = MEM(u + 28 * 4);
                uint32_t l2c = MEM(u + 40 * 4);
                uint32_t l2y = MEM(u + 41 * 4);
                uint32_t l3c = MEM(u + 53 * 4);
                uint32_t l3y = MEM(u + 54 * 4);
                uint32_t l4c = MEM(u + 66 * 4);
                uint32_t l4y = MEM(u + 67 * 4);
                uint32_t a0 = l0c & ~0x40000000u;
                if ((a0 == 0x11340000u || a0 == 0x0C140000u) && l0y == l0c + 0x800u && l1y == l1c + 0x800u)
                {
                    MEM(u + 14 * 4) = l0c - 0x330000u; MEM(a + 14 * 4) = l0c - 0x330000u;
                    MEM(u + 15 * 4) = l0y - 0x330000u; MEM(a + 15 * 4) = l0y - 0x330000u;
                    MEM(u + 27 * 4) = l1c - 0x198000u; MEM(a + 27 * 4) = l1c - 0x198000u;
                    MEM(u + 28 * 4) = l1y - 0x198000u; MEM(a + 28 * 4) = l1y - 0x198000u;
                    if ((l2c & ~0x40000000u) == a0 - 0x1240000u && l2y == l2c)
                    {
                        MEM(u + 40 * 4) = l2c - 0x066000u; MEM(a + 40 * 4) = l2c - 0x066000u;
                        MEM(u + 41 * 4) = l2y - 0x066000u; MEM(a + 41 * 4) = l2y - 0x066000u;
                    }
                    if ((l3c & ~0x40000000u) == a0 - 0x1080000u && l3y == l3c)
                    {
                        MEM(u + 53 * 4) = l3c - 0x019800u; MEM(a + 53 * 4) = l3c - 0x019800u;
                        MEM(u + 54 * 4) = l3y - 0x019800u; MEM(a + 54 * 4) = l3y - 0x019800u;
                    }
                    if ((l4c & ~0x40000000u) == a0 - 0x1020000u && l4y == l4c)
                    {
                        MEM(u + 66 * 4) = l4c - 0x006600u; MEM(a + 66 * 4) = l4c - 0x006600u;
                        MEM(u + 67 * 4) = l4y - 0x006600u; MEM(a + 67 * 4) = l4y - 0x006600u;
                    }
                }
                MEM(u + 19 * 4) = 0x00002900u; MEM(a + 19 * 4) = 0x00002900u;
                MEM(u + 32 * 4) = 0x00002900u; MEM(a + 32 * 4) = 0x00002900u;
                MEM(u + 45 * 4) = 0x00001020u; MEM(a + 45 * 4) = 0x00001020u;
                MEM(u + 58 * 4) = 0x00000800u; MEM(a + 58 * 4) = 0x00000800u;
                MEM(u + 71 * 4) = 0x00000400u; MEM(a + 71 * 4) = 0x00000400u;
                cm_1x3_l1_swaps++;
            }
        }
        sei(old);
        return;
    }
    if (regs[2] == 0x303 && caller == F4_303_CALLER)
    {
        cm_1x3_303_cnt++;
        if (!x1_l1_active) return;
        uint32_t a = regs[3] & ~0x40000000u;
        if (a < 0x1000u || a >= 0x20000000u || (a & 3u)) return;
        uint32_t u = a | 0x40000000u;
        uint32_t old = cli();
        uint32_t l0c = MEM(u + 7 * 4);
        uint32_t l0y = MEM(u + 8 * 4);
        uint32_t l1c = MEM(u + 9 * 4);
        uint32_t l1y = MEM(u + 10 * 4);
        uint32_t l2c = MEM(u + 11 * 4);
        uint32_t l2y = MEM(u + 12 * 4);
        uint32_t l3c = MEM(u + 13 * 4);
        uint32_t l3y = MEM(u + 14 * 4);
        uint32_t l4c = MEM(u + 15 * 4);
        uint32_t l4y = MEM(u + 16 * 4);
        uint32_t a0 = l0c & ~0x40000000u;
        if ((a0 == 0x11340000u || a0 == 0x0C140000u) && l0y == l0c + 0x800u && l1y == l1c + 0x800u)
        {
            MEM(u +  7 * 4) = l0c - 0x330000u; MEM(a +  7 * 4) = l0c - 0x330000u;
            MEM(u +  8 * 4) = l0y - 0x330000u; MEM(a +  8 * 4) = l0y - 0x330000u;
            MEM(u +  9 * 4) = l1c - 0x198000u; MEM(a +  9 * 4) = l1c - 0x198000u;
            MEM(u + 10 * 4) = l1y - 0x198000u; MEM(a + 10 * 4) = l1y - 0x198000u;
            if ((l2c & ~0x40000000u) == a0 - 0x1240000u && l2y == l2c)
            {
                MEM(u + 11 * 4) = l2c - 0x066000u; MEM(a + 11 * 4) = l2c - 0x066000u;
                MEM(u + 12 * 4) = l2y - 0x066000u; MEM(a + 12 * 4) = l2y - 0x066000u;
            }
            if ((l3c & ~0x40000000u) == a0 - 0x1080000u && l3y == l3c)
            {
                MEM(u + 13 * 4) = l3c - 0x019800u; MEM(a + 13 * 4) = l3c - 0x019800u;
                MEM(u + 14 * 4) = l3y - 0x019800u; MEM(a + 14 * 4) = l3y - 0x019800u;
            }
            if ((l4c & ~0x40000000u) == a0 - 0x1020000u && l4y == l4c)
            {
                MEM(u + 15 * 4) = l4c - 0x006600u; MEM(a + 15 * 4) = l4c - 0x006600u;
                MEM(u + 16 * 4) = l4y - 0x006600u; MEM(a + 16 * 4) = l4y - 0x006600u;
            }
        }
        sei(old);
        return;
    }
    if (regs[2] != 0x403 || caller != cm_fw->f4_403_caller) return;
    if (!cm_raw_is_x1(shamem_read(REG_RAW))) return;
    cm_1x3_403_cnt++;
}

static void cm_engio_hook(uint32_t * regs, uint32_t * stack, uint32_t pc)
{
    if (!cm_1x3_active || !is_movie_mode()) return;
    uint32_t * reg_list = (uint32_t *)regs[0];
    if (!reg_list) return;
    int mode = cm_live_mode;
    int is_hfps = cm_mode_is_hfps(mode);
    int yres_delta = 0, timer_a = 0, timer_b = 0, orig_ta = 0, orig_tb = 0;
    uint16_t cmos1 = 0x2CB;
    cm_get_tall_params(mode, &yres_delta, &cmos1, &timer_a, &timer_b, &orig_ta, &orig_tb);
    if (yres_delta <= 0) return;

    int has_target = 0;
    int has_fps = 0;
    int has_prepro = 0;
    for (int k = 0; k < 510 && reg_list[2 * k] != 0xFFFFFFFF; k++)
    {
        uint32_t reg = reg_list[2 * k];
        uint32_t val = reg_list[2 * k + 1];
        if (reg == 0xC0F06804)
            cm_engio_vidmode_ok = is_hfps ? (val == RAW_5D3_720) : (val == RAW_5D3_X1);
        if (reg == 0xC0F06804 || reg == 0xC0F0713C || reg == 0xC0F07150)
            has_target = 1;
        if (reg == 0xC0F06008 || reg == 0xC0F0600C || reg == 0xC0F06010 || reg == 0xC0F06014 ||
            reg == 0xC0F06824 || reg == 0xC0F06828 || reg == 0xC0F0682C || reg == 0xC0F06830)
            has_fps = 1;
        if ((reg == 0xC0F08184 && (val == 0x00000525u || (is_hfps && val == 0x000002B3u))) ||
            ((reg == 0xC0F37A70 || reg == 0xC0F37B8C || reg == 0xC0F37AD0) &&
             (val == 0x05250207u || (is_hfps && val == 0x02B30207u))))
            has_prepro = 1;
    }
    int path_ok = is_hfps ? cm_x1_is_720p_path() : (cm_x1_is_1080p_path() && MEM(LV_STATE_ADDR + 0x08) == 0);
    int fps_ok = (has_fps && path_ok);
    int prepro_ok = (has_prepro && path_ok);
    if ((!cm_engio_vidmode_ok || !has_target) && !fps_ok && !prepro_ok) return;

    uint32_t old = cli();
    uint32_t exp_ta = (orig_ta > 0) ? (uint32_t)(orig_ta - 1) : 0;
    uint32_t exp_ta2 = exp_ta | (exp_ta << 16);
    uint32_t exp_tb = (orig_tb > 0) ? (uint32_t)(orig_tb - 1) : 0;
    uint32_t exp_tb2 = exp_tb | (exp_tb << 16);
    uint32_t new_ta = (timer_a > 0) ? (uint32_t)(timer_a - 1) : 0;
    uint32_t new_ta2 = new_ta | (new_ta << 16);
    uint32_t new_tb = (timer_b > 0) ? (uint32_t)(timer_b - 1) : 0;
    int head_adj_720 = (MEM(LV_STATE_ADDR + 0x04) == 1) ? -10 : -20;
    int i = 0;
    while (i < 1020 && reg_list[i] != 0xFFFFFFFF)
    {
        uint32_t reg = reg_list[i];
        uint32_t val = reg_list[i + 1];
        uint32_t old_val = val;
        if (is_hfps)
        {
            if (reg == 0xC0F06804 && val == RAW_5D3_720)
                val = ((0x02B6u + (uint32_t)yres_delta) << 16) | 0x011Bu;
            else if (reg == 0xC0F06800 && val == 0x00010017u)
                val = 0x001D0017u; /* 720p skip_top=0: top_adj=28 (0x1D = 29) */
            else if (reg == 0xC0F0713C && (val == 0x02B4u || val == 0x02E6u))
                val = (mode == 11) ? (0x02B4u + (uint32_t)yres_delta)
                                   : (uint32_t)((int)val + yres_delta + head_adj_720);
            else if (reg == 0xC0F07150 && (val == 0x026Du || val == 0x02B4u))
                val = (mode == 11) ? (0x026Du + (uint32_t)yres_delta)
                                   : (uint32_t)((int)val + yres_delta + head_adj_720);
            else if (prepro_ok && reg == 0xC0F08184 && val == 0x000002B3u)
                val = 0x000002B3u + cm_hfps_y_shift(mode);
            else if (prepro_ok && (reg == 0xC0F37A70 || reg == 0xC0F37B8C || reg == 0xC0F37AD0) && val == 0x02B30207u)
                val = ((0x02B3u + cm_hfps_y_shift(mode)) << 16) | 0x0207u;
        }
        else
        {
            uint32_t prepro_yd = (yres_delta > 662) ? 662u : (uint32_t)yres_delta;
            if (cm_x1_prepro_yd && cm_x1_prepro_yd <= (uint32_t)yres_delta)
                prepro_yd = cm_x1_prepro_yd;
            if (reg == 0xC0F06804 && val == RAW_5D3_X1)
                val = ((0x0528u + (uint32_t)yres_delta) << 16) | 0x011Bu;
            else if (reg == 0xC0F06800 && val == 0x00010017u)
                val = 0x001F0017u;  /* crop_rec reg_override_top_bar: first raw line 30 px lower */
            else if (reg == 0xC0F0713C && (val == 0x055Eu || val == 0x0515u || val == 0x0528u))
                val += (uint32_t)yres_delta;
            else if (reg == 0xC0F07150 && (val == 0x0527u || val == 0x04DEu || val == 0x04EEu))
                val += (uint32_t)yres_delta;
            else if (prepro_ok && reg == 0xC0F08184 && val == 0x00000525u)
                val = 0x00000525u + prepro_yd - 30u; /* = HIV ch 0x18 lines: 1950, mode 13: 2070 */
            else if (prepro_ok && (reg == 0xC0F37A70 || reg == 0xC0F37B8C || reg == 0xC0F37AD0) && val == 0x05250207u)
                val = ((0x0525u + prepro_yd - 30u) << 16) | 0x0207u;
        }
        if (timer_a > 0 && (reg == 0xC0F06824 || reg == 0xC0F06828 || reg == 0xC0F0682C || reg == 0xC0F06830 || reg == 0xC0F06010) &&
            (val == exp_ta || val == exp_ta + 1))
            val = new_ta;
        else if (timer_a > 0 && (reg == 0xC0F06008 || reg == 0xC0F0600C) &&
                 (val == exp_ta2 || val == exp_ta2 + 0x00010001u))
            val = new_ta2;
        else if (timer_b > 0 && reg == 0xC0F06014 &&
                 (val == exp_tb || val == exp_tb + 1 || val == exp_tb2 || val == exp_tb2 + 0x00010001u))
            val = new_tb;
        if (is_hfps && val != old_val)
            cm_hfps_logf("ENG %08X:%08X->%08X\n", reg, old_val, val);
        cm_engio_copy[i]     = reg;
        cm_engio_copy[i + 1] = val;
        i += 2;
    }
    if (reg_list[i] == 0xFFFFFFFF)
    {
        cm_engio_copy[i] = 0xFFFFFFFF;
        regs[0] = (uint32_t)cm_engio_copy;
    }
    sei(old);
}

static void cm_cmos_hook(uint32_t * regs, uint32_t * stack, uint32_t pc)
{
    if (!cm_1x3_active || !is_movie_mode()) return;
    uint16_t * data_buf = (uint16_t *)regs[0];
    if (!data_buf) return;
    int mode = cm_live_mode;
    int is_hfps = cm_mode_is_hfps(mode);
    int has_mode_regs = 0;
    for (int k = 0; data_buf[k] != 0xFFFF && k < 61; k++)
    {
        uint16_t v = data_buf[k];
        int reg = v >> 12;
        if (reg == 1)
        {
            cm_cmos_vidmode_ok = ((v & 0xFFF) == 0x800);
            has_mode_regs = 1;
        }
        if (reg == 6)
            has_mode_regs = 1;
    }
    if (is_hfps && cm_x1_is_720p_path() && has_mode_regs)
        cm_cmos_vidmode_ok = 1;
    if (!cm_cmos_vidmode_ok || !has_mode_regs) return;

    uint32_t old = cli();
    int yres_delta = 0, timer_a = 0, timer_b = 0, orig_ta = 0, orig_tb = 0;
    uint16_t cmos1 = 0x2CB;
    cm_get_tall_params(mode, &yres_delta, &cmos1, &timer_a, &timer_b, &orig_ta, &orig_tb);
    if (yres_delta <= 0) { sei(old); return; }
    uint16_t cmos6 = is_hfps ? 0x370 : 0x170;
    int i = 0;
    int seen_reg6 = 0;
    while (data_buf[i] != 0xFFFF && i < 62)
    {
        uint16_t v = data_buf[i];
        uint16_t old_v = v;
        int reg = v >> 12;
        if (reg == 1) v = (1 << 12) | cmos1;
        else if (reg == 6) { v = (6 << 12) | cmos6; seen_reg6 = 1; }
        if (is_hfps) cm_hfps_logf("CMOS[%d]:%04X->%04X ", i, old_v, v);
        cm_cmos_copy[i++] = v;
    }
    if (is_hfps && !seen_reg6 && i < 63)
    {
        cm_cmos_copy[i++] = (6 << 12) | cmos6;
        cm_hfps_logf("+CMOS6:%04X ", (6 << 12) | cmos6);
    }
    if (is_hfps) cm_hfps_logf("\n");
    cm_cmos_copy[i] = 0xFFFF;
    regs[0] = (uint32_t)cm_cmos_copy;
    cm_1x3_cmos_cnt++;
    sei(old);
}

static void cm_adtg_hook(uint32_t * regs, uint32_t * stack, uint32_t pc)
{
    if (!cm_1x3_active || !is_movie_mode()) return;
    int mode = cm_live_mode;
    int is_hfps = cm_mode_is_hfps(mode);
    if (!is_hfps && !cm_cmos_vidmode_ok) return;
    if (is_hfps && !cm_x1_is_720p_path()) return;
    int dst = regs[0] & 0xF;
    if (!(dst & 6)) return;
    uint32_t * data_buf = (uint32_t *)regs[1];
    if (!data_buf) return;
    int yres_delta = 0, timer_a = 0, timer_b = 0, orig_ta = 0, orig_tb = 0;
    uint16_t cmos1 = 0x2CB;
    cm_get_tall_params(mode, &yres_delta, &cmos1, &timer_a, &timer_b, &orig_ta, &orig_tb);
    if (yres_delta <= 0) return;
    int has_mode_regs = 0;
    for (int k = 0; data_buf[k] != 0xFFFFFFFF && k < 126; k++)
    {
        int reg = data_buf[k] >> 16;
        if (reg == 0x800C || reg == 0x8172 || reg == 0x8178 ||
            reg == 0x8196 || reg == 0x82B6 || reg == 0x82F8 ||
            reg == 0x8173 || reg == 0x8179 || reg == 0x8197 || reg == 0x82F9)
        {
            has_mode_regs = 1;
            break;
        }
    }
    if (!has_mode_regs) return;

    uint32_t old = cli();
    int readout_end = (is_hfps ? 0x02B6 : 0x0528) + yres_delta;
    uint32_t adtg_800c = is_hfps ? 2u : 0u;
    int fps_tb = (timer_b > 0) ? timer_b : orig_tb;
    if (fps_tb <= 1) fps_tb = (((int)(shamem_read(0xC0F06014) & 0xFFFF)) + 1);
    int i = 0;
    while (data_buf[i] != 0xFFFFFFFF && i < 127)
    {
        uint32_t v = data_buf[i];
        uint32_t old_v = v;
        int reg = v >> 16;
        if (reg == 0x800C)
        {
            v = (0x800C << 16) | adtg_800c;
        }
        else if (yres_delta > 0)
        {
            if (is_hfps)
            {
                /* Match crop_rec.c 3x3 720p PowerSaveTiming & ReadOutTiming */
                if (reg == 0x8172 || reg == 0x8178 || reg == 0x8196 || reg == 0x82F8)
                    v = (reg << 16) | (nrzi_encode(readout_end + 1) & 0xFFFF);
                else if (reg == 0x82B6)
                    v = (reg << 16) | (nrzi_encode(readout_end - 1) & 0xFFFF);
                else if (fps_tb > 1 && (reg == 0x8173 || reg == 0x8179 || reg == 0x8197 || reg == 0x82F9))
                    v = (reg << 16) | (nrzi_encode(fps_tb - 1) & 0xFFFF);
            }
            else
            {
                /* Keep ADTG PowerSaveTiming disabled (0): in 1920-line tall mode the short
                 * end-of-frame power-save window caused OB black level to jump +350 ADU after ~12s
                 * (pink/magenta cast in LV & RAW). Keeping ReadOutTiming (82F8/82F9) keeps OB rock-solid at 2048. */
                if (reg == 0x8172 || reg == 0x8178 || reg == 0x8196 ||
                    reg == 0x8173 || reg == 0x8179 || reg == 0x8197 || reg == 0x82B6)
                    v = (reg << 16) | 0u;
                else if (reg == 0x82F8)
                    v = (reg << 16) | (nrzi_encode(readout_end + 1) & 0xFFFF);
                else if (fps_tb > 1 && reg == 0x82F9)
                    v = (reg << 16) | (nrzi_encode(fps_tb - 1) & 0xFFFF);
            }
        }
        if (is_hfps && v != old_v)
            cm_hfps_logf("ADTG %08X->%08X\n", old_v, v);
        cm_adtg_copy[i++] = v;
    }
    cm_adtg_copy[i] = 0xFFFFFFFF;
    regs[1] = (uint32_t)cm_adtg_copy;
    cm_1x3_adtg_cnt++;
    sei(old);
}

#define EEKO_K1_C     (cm_fw->eeko_k1)
#define EEKO_K1_U     (cm_fw->eeko_k1 | 0x40000000u)
#define EEKO_M1_C     (cm_fw->eeko_m1)
#define EEKO_M1_U     (cm_fw->eeko_m1 | 0x40000000u)
#define EEKO_ROI_H_C  (cm_fw->eeko_roi_h)
#define EEKO_ROI_H_U  (cm_fw->eeko_roi_h | 0x40000000u)
#define EEKO_ROI_Y_C  (cm_fw->eeko_roi_y)
#define EEKO_ROI_Y_U  (cm_fw->eeko_roi_y | 0x40000000u)

/* Relocate ALL 5 pyramid levels + 3.25 MB black top-padding strictly inside the proven safe
 * +0x1000000..+0x2000000 (16 MB) unused region:
 *   Pad (+0x1000000..+0x1340000, 3.25 MB zeroed YUV black -> L0 - 0x330000 never reads outside!)
 *   L0  (+0x1340000, 7.50 MB slot, max 7.375 MB @ 24p)
 *   L1  (+0x1AC0000, 3.75 MB slot, max 3.688 MB @ 24p — via case 8 hooks)
 *   L2  (+0x1E80000, 1.00 MB slot, max 0.922 MB @ 24p)
 *   L3  (+0x1F80000, 0.25 MB slot, max 0.230 MB @ 24p)
 *   L4  (+0x1FC0000, 0.25 MB slot, max 0.058 MB @ 24p) */
#define L1_5D3C_ADDR  (cm_fw->l1_5d3c_addr)
#define L1_5D3C_INSN  0xE580C000u  /* STR IP, [R0] */
#define L1_5F54_ADDR  (cm_fw->l1_5f54_addr)
#define L1_5F54_INSN  (cm_fw->l1_5f54_insn)
static int x1_l1_hooked = 0;

static void x1_l1_5d3c_hook(uint32_t * regs, uint32_t * stack, uint32_t pc)
{
    if (x1_l1_active) regs[12] += 0x1F00000u;
}

static void x1_l1_5f54_hook(uint32_t * regs, uint32_t * stack, uint32_t pc)
{
    if (x1_l1_active) regs[cm_fw->l1_5f54_reg] += 0x1F00000u;
}

#define x1_lbuf_tbl (cm_fw->lbuf_tbl)
static int x1_lbuf_patched[8];
static int x1_sslt_patched[2];
static uint32_t x1_sslt_val[2];
#define X1_SSLT_A3_ADDR    (cm_fw->x1_sslt_a3_addr)
#define X1_SSLT_A11_ADDR   (cm_fw->x1_sslt_a11_addr)
#define HFPS_SSLT_A3_ADDR  (cm_fw->x1_sslt_a3_addr + 4u)
#define HFPS_SSLT_A10_ADDR (cm_fw->x1_sslt_a11_addr - 0x2Cu + 4u) /* A_10 top H crop margin for type 6 (stock = 29) */
#define X1_HIV_YB_ADDR     (cm_fw->x1_hiv_yb_addr)
#define HFPS_HIV_YB_ADDR   (cm_fw->hfps_hiv_yb_addr)
#define EEKO_K2_C          (cm_fw->eeko_k1 + 0x38u)
#define EEKO_K2_U          ((cm_fw->eeko_k1 + 0x38u) | 0x40000000u)
#define EEKO_M2_C          (cm_fw->eeko_m1 + 4u * F4_402_STRIDE)
#define EEKO_M2_U          ((cm_fw->eeko_m1 + 4u * F4_402_STRIDE) | 0x40000000u)
#define EEKO_M3_C          (cm_fw->eeko_m1 + 8u * F4_402_STRIDE)
#define EEKO_M3_U          ((cm_fw->eeko_m1 + 8u * F4_402_STRIDE) | 0x40000000u)
static int x1_hiv_patched = 0;
static int hfps_sslt_patched[2];
static uint32_t hfps_sslt_val[2];
static int hfps_hiv_patched = 0;
static int hfps_k2_patched = 0;
static int hfps_m2_patched = 0;

/* HIV-V darkframe table (EDMAC ch 0x18, 16 B per line, read every frame; SetEDmac(0x18, R1 = table,
 * R2 = edmac_info) in LVx1_StartPreproPath runs once per frame, ~24.6 calls/s).
 * KI-3 (2026-11-26): mode 13 needs 2070 lines; Canon's table area is valid that far (camera logs 2026-11-27).
 * KI-4 (2026-11-27): Canon's x1 table ends at line 1318 (stock 0x525 + 1); entry 1318 is not table
 * data (0105003A 00005268 ...), so every 1x3 tall mode got a dark half line at raw row 1318 in
 * 8..12-bit lossless (raw type DEFCORRE is taken after HIV; 14-bit CCD is not affected).
 * Fix: the cm_live task copies Canon's table ONCE per LV start into our buffer with entry 1318
 * replaced by entry 1316 (same row parity); the hook then only swaps R1 for our buffer.
 * The first attempt copied 33 KB inside the hook every frame and broke DEFCORRE (M26-2356).
 * Canon memory is only read. */
#define HIV_ROW_BYTES   16u
#define HIV_BAD_ROW     1318u
#define HIV_MAX_ROWS    2112u
#define HIV_HOOK_INSN   0xE3A03101u   /* MOV R3, #0x40000000 (same in 1.1.3 and 1.2.3) */
#define X1_HIV_HOOK     (cm_fw->x1_hiv_hook)
#define X1_HIV_INFO     (cm_fw->x1_hiv_yb_addr - 0x20u)
static uint32_t cm_hiv_raw[(HIV_MAX_ROWS * HIV_ROW_BYTES + 64u) / 4u];
static int cm_hiv_hooked = 0;
static volatile uint32_t cm_hiv_rows = 0;      /* > 0: hook armed for this many lines */
static volatile uint32_t cm_hiv_src = 0;       /* Canon table address last seen by the hook */
static volatile uint32_t cm_hiv_ready_src = 0; /* our buffer holds a copy of this Canon table ... */
static volatile uint32_t cm_hiv_ready_rows = 0;/* ... for this many lines (0 = no valid copy) */
static int cm_hiv_t_poll = 0;

static uint32_t cm_hiv_base(void)
{
    return ((uint32_t)cm_hiv_raw + 63u) & ~63u & ~0x40000000u;
}

static inline void cm_hiv_put(uint32_t off, uint32_t v)
{
    uint32_t c = cm_hiv_base() + off;
    MEM(c) = v;
    MEM(c | 0x40000000u) = v;
}

static inline uint32_t cm_hiv_src_row(uint32_t r)
{
    return (r == HIV_BAD_ROW) ? HIV_BAD_ROW - 2u : r;
}

static void cm_hiv_hook(uint32_t * regs, uint32_t * stack, uint32_t pc)
{
    uint32_t rows = cm_hiv_rows;
    uint32_t src = regs[1];
    if (!rows || !cm_1x3_active || regs[2] != X1_HIV_INFO || MEM(X1_HIV_YB_ADDR) + 1u != rows ||
        src < 0x1000u || (src & 3u) || ((src & ~0x40000000u) >= 0x20000000u))
        return;                      /* not our tall x1 path: Canon's table */
    cm_hiv_src = src;
    if (cm_hiv_ready_src != src || cm_hiv_ready_rows != rows)
        return;                      /* copy not ready yet (cm_hiv_poll): Canon's table this frame */
    regs[1] = cm_hiv_base() | 0x40000000u;
}

/* rows > HIV_BAD_ROW + 1: arm the hook. Otherwise disarm (Canon table). */
static int cm_hiv_arm(uint32_t rows)
{
    if (rows <= HIV_BAD_ROW + 1u || rows > HIV_MAX_ROWS)
    {
        cm_hiv_rows = 0;
        cm_hiv_ready_rows = 0;
        if (cm_hiv_hooked) { unpatch_memory(X1_HIV_HOOK); cm_hiv_hooked = 0; }
        return 0;
    }
    if (!cm_hiv_hooked)
    {
        if (MEM(X1_HIV_HOOK) != HIV_HOOK_INSN) return 0;
        if (patch_hook_function(X1_HIV_HOOK, HIV_HOOK_INSN, cm_hiv_hook, "crop_rec: x1 HIV-V table")) return 0;
        cm_hiv_hooked = 1;
    }
    if (cm_hiv_rows != rows) cm_hiv_ready_rows = 0;
    cm_hiv_rows = rows;
    return 1;
}

/* cm_live task: make the copy as soon as the hook has seen Canon's table; then every ~1 s
 * re-check it against Canon's (verified static on camera, changed = 0 over 60 s). */
static void cm_hiv_poll(void)
{
    if (!cm_hiv_rows || !lv || lv_paused || !cm_1x3_active) return;
    uint32_t rows = cm_hiv_rows;
    uint32_t src = cm_hiv_src;
    if (!src) return;
    uint32_t su = src | 0x40000000u;
    if (cm_hiv_ready_src != src || cm_hiv_ready_rows != rows)
    {
        cm_hiv_ready_rows = 0;                     /* hook keeps Canon's table while we copy */
        for (uint32_t r = 0; r < rows; r++)
            for (uint32_t w = 0; w < HIV_ROW_BYTES; w += 4u)
                cm_hiv_put(r * HIV_ROW_BYTES + w, MEM(su + cm_hiv_src_row(r) * HIV_ROW_BYTES + w));
        cm_hiv_ready_src = src;
        cm_hiv_ready_rows = rows;
        cm_hiv_t_poll = get_ms_clock();
        return;
    }
    int t = get_ms_clock();
    if (t - cm_hiv_t_poll < 1000) return;
    cm_hiv_t_poll = t;
    uint32_t du = cm_hiv_base() | 0x40000000u;
    for (uint32_t r = 0; r < rows; r++)
        for (uint32_t w = 0; w < HIV_ROW_BYTES; w += 4u)
        {
            uint32_t v = MEM(su + cm_hiv_src_row(r) * HIV_ROW_BYTES + w);
            if (MEM(du + r * HIV_ROW_BYTES + w) != v)
                cm_hiv_put(r * HIV_ROW_BYTES + w, v);
        }
}


static void cm_apply_hfps_vscale(int enable)
{
    uint32_t out_h = enable ? 61u : 101u;
    uint16_t roi_y_insn = enable ? 0x2200u : 0x68DAu; /* MOVS R2, #0   vs LDR R2, [R3, #12] */
    uint16_t roi_h_insn = enable ? 0x21F4u : 0x6959u; /* MOVS R1, #244 vs LDR R1, [R3, #20] (720*61/180 = 244) */
    if (MEM(EEKO_M2_U + 2 * 4) == 720u && MEM(EEKO_M2_U + 15 * 4) == 16u && MEM(EEKO_M2_U + 17 * 4) == 180u)
    {
        MEM(EEKO_M2_C + 14 * 4) = 9u;
        MEM(EEKO_M2_U + 14 * 4) = 9u;
        MEM(EEKO_M2_C + 16 * 4) = out_h;
        MEM(EEKO_M2_U + 16 * 4) = out_h;
    }
    if (is_5d3_123() && MEM(EEKO_M3_U + 2 * 4) == 720u && MEM(EEKO_M3_U + 15 * 4) == 16u && MEM(EEKO_M3_U + 17 * 4) == 180u)
    {
        MEM(EEKO_M3_C + 14 * 4) = 9u;
        MEM(EEKO_M3_U + 14 * 4) = 9u;
        MEM(EEKO_M3_C + 16 * 4) = out_h;
        MEM(EEKO_M3_U + 16 * 4) = out_h;
    }
    *(volatile uint16_t *)EEKO_ROI_Y_C = roi_y_insn;
    *(volatile uint16_t *)EEKO_ROI_Y_U = roi_y_insn;
    *(volatile uint16_t *)EEKO_ROI_H_C = roi_h_insn;
    *(volatile uint16_t *)EEKO_ROI_H_U = roi_h_insn;
    MEM(F4_EEKO_DIRTY_C) = 0u;
    MEM(F4_EEKO_DIRTY_U) = 0u;
    hfps_m2_patched = enable;
}

static void cm_hfps_tall_unpatch(void)
{
    if (hfps_m2_patched)
        cm_apply_hfps_vscale(0);
    if (hfps_hiv_patched) { MEM(HFPS_HIV_YB_ADDR) = 0x2B3u; hfps_hiv_patched = 0; }
    if (hfps_sslt_patched[1]) { unpatch_memory(HFPS_SSLT_A10_ADDR); hfps_sslt_patched[1] = 0; hfps_sslt_val[1] = 0; }
    if (hfps_sslt_patched[0]) { unpatch_memory(HFPS_SSLT_A3_ADDR);  hfps_sslt_patched[0] = 0; hfps_sslt_val[0] = 0; }
}

static void cm_x1_tall_unpatch(void)
{
    x1_fp_active = 0;
    x1_l1_active = 0;
    cm_x1_prepro_yd = 0;
    cm_hiv_arm(0);
    if (x1_hiv_patched) { MEM(X1_HIV_YB_ADDR) = 0x525u; x1_hiv_patched = 0; }
    if (x1_sslt_patched[1]) { unpatch_memory(X1_SSLT_A11_ADDR); x1_sslt_patched[1] = 0; x1_sslt_val[1] = 0; }
    if (x1_sslt_patched[0]) { unpatch_memory(X1_SSLT_A3_ADDR); x1_sslt_patched[0] = 0; x1_sslt_val[0] = 0; }
    for (int k = 7; k >= 0; k--)
        if (x1_lbuf_patched[k]) { unpatch_memory(x1_lbuf_tbl[k][0]); x1_lbuf_patched[k] = 0; }
}

static int cm_x1_lbuf_patch(void)
{
    if (!x1_l1_hooked)
    {
        if (MEM(L1_5D3C_ADDR) != L1_5D3C_INSN || MEM(L1_5F54_ADDR) != L1_5F54_INSN)
            return 0;
        if (patch_hook_function(L1_5D3C_ADDR, L1_5D3C_INSN, x1_l1_5d3c_hook, "cm_wr: x1 L1 5D3C"))
            return 0;
        if (patch_hook_function(L1_5F54_ADDR, L1_5F54_INSN, x1_l1_5f54_hook, "cm_wr: x1 L1 5F54"))
        {
            unpatch_memory(L1_5D3C_ADDR);
            return 0;
        }
        x1_l1_hooked = 1;
    }
    for (int k = 0; k < 8; k++)
        if (!x1_lbuf_patched[k] && MEM(x1_lbuf_tbl[k][0]) != x1_lbuf_tbl[k][1])
            return 0;
    for (int k = 0; k < 8; k++)
    {
        if (!x1_lbuf_patched[k])
        {
            if (patch_instruction(x1_lbuf_tbl[k][0], x1_lbuf_tbl[k][1], x1_lbuf_tbl[k][2], "cm_wr: x1 L0..L4"))
            {
                cm_x1_tall_unpatch();
                return 0;
            }
            x1_lbuf_patched[k] = 1;
        }
    }
    cm_1x3_303_cnt = 0;
    cm_1x3_l1_swaps = 0;
    cm_1x3_dbg_l0 = 0;
    cm_1x3_dbg_l1 = 0;
    x1_l1_active = 1;
    x1_fp_active = 0;
    return 1;
}

static int cm_x1_tall_patch(void)
{
    int yd = 0;
    cm_get_tall_params(cm_live_mode, &yd, 0, 0, 0, 0, 0);
    if (yd <= 0) return 0;
    uint32_t prepro_yd = (yd > 662) ? 662u : (uint32_t)yd;
    /* SSLT, prepro regs and HIV ch 0x18 must all see the same height (1950 lines, mode 13: 2070). */
    int yb_ok = x1_hiv_patched || MEM(X1_HIV_YB_ADDR) == 0x525u; /* same condition as the HIV yb write below */
    if ((uint32_t)yd > prepro_yd && yb_ok)
        prepro_yd = (uint32_t)yd;    /* KI-3: Canon's HIV table area is valid for 2070 lines */
    /* KI-4: our copy of the HIV table without Canon's bad entry 1318 (falls back to Canon's table) */
    cm_hiv_arm(yb_ok ? 0x525u + prepro_yd - 30u + 1u : 0u);
    cm_x1_prepro_yd = prepro_yd;
    uint32_t raw_v = 0x528u + prepro_yd - 30u; /* SSLT input = prepro output = HIV ch 0x18 lines (1950 / 2070) */
    uint32_t ss_h  = (raw_v - 48u) & ~31u;
    if (ss_h > 1888u) ss_h = 1888u;
    uint32_t new_a3  = raw_v - ss_h;
    uint32_t new_a11 = new_a3 - 37u;

    if (!cm_x1_lbuf_patch()) return 0;

    for (int i = 0; i < S4_P1_WORDS; i++) x1_p1[i] = MEM(SS_X1_P1 + 4 * i);
    x1_p1[0x1C / 4] = ss_h;
    for (int i = 0; i < S4_P3_WORDS; i++) x1_p3[i] = MEM(SS_X1_P3 + 4 * i);
    x1_p3[0x24 / 4] = ss_h + 8u;
    x1_p3[0x40 / 4] = ss_h + 7u;

    if (!e3_hooked)
    {
        if (MEM(SS_ADDR) != SS_I0 || MEM(SS_ADDR + 4) != SS_I1) return 0;
        if (patch_hook_function(SS_ADDR, SS_I0, e3_hook, "cm_wr: SSLT tables")) return 0;
        e3_hooked = 1;
    }
    if (x1_sslt_patched[0] && x1_sslt_val[0] != new_a3)
    {
        unpatch_memory(X1_SSLT_A3_ADDR);
        x1_sslt_patched[0] = 0;
        x1_sslt_val[0] = 0;
    }
    if (x1_sslt_patched[1] && x1_sslt_val[1] != new_a11)
    {
        unpatch_memory(X1_SSLT_A11_ADDR);
        x1_sslt_patched[1] = 0;
        x1_sslt_val[1] = 0;
    }
    if (!x1_sslt_patched[0])
    {
        if (MEM(X1_SSLT_A3_ADDR) != 38u) return 0;
        if (patch_memory(X1_SSLT_A3_ADDR, 38u, new_a3, "cm_wr: x1 tall SSLT A3"))
        {
            cm_x1_tall_unpatch();
            return 0;
        }
        x1_sslt_patched[0] = 1;
        x1_sslt_val[0] = new_a3;
    }
    if (!x1_sslt_patched[1])
    {
        if (MEM(X1_SSLT_A11_ADDR) != 1u) return 0;
        if (patch_memory(X1_SSLT_A11_ADDR, 1u, new_a11, "cm_wr: x1 tall SSLT A11"))
        {
            cm_x1_tall_unpatch();
            return 0;
        }
        x1_sslt_patched[1] = 1;
        x1_sslt_val[1] = new_a11;
    }
    if (x1_hiv_patched || MEM(X1_HIV_YB_ADDR) == 0x525u)
    {
        MEM(X1_HIV_YB_ADDR) = 0x525u + prepro_yd - 30u;
        x1_hiv_patched = 1;
    }
    e3_mode = 2;
    e3_active = 1;
    return 1;
}

static int cm_hfps_tall_patch(void)
{
    uint32_t y_shift = cm_hfps_y_shift(cm_live_mode);
    if (y_shift == 0u)
    {
        if (hfps_hiv_patched) { MEM(HFPS_HIV_YB_ADDR) = 0x2B3u; hfps_hiv_patched = 0; }
        if (hfps_sslt_patched[1]) { unpatch_memory(HFPS_SSLT_A10_ADDR); hfps_sslt_patched[1] = 0; hfps_sslt_val[1] = 0; }
        if (hfps_sslt_patched[0]) { unpatch_memory(HFPS_SSLT_A3_ADDR);  hfps_sslt_patched[0] = 0; hfps_sslt_val[0] = 0; }
        e3_active = 0;
        return 1;
    }
    uint32_t new_a3  = 36u + y_shift;
    uint32_t new_a10 = 29u + y_shift;

    /* Keep SSLT output height at 100% stock 656 lines (and p3 at stock 663 lines) so L0..L4,
     * 0x302(k=2), and EEKO_K2 stay 100% stock; shift top crop margin A_10 by +y_shift so the
     * 642-line 1:1 preview window sits vertically centered inside 1040p/1080p/960p! */
    for (int i = 0; i < S4_P1_WORDS; i++) x1_p1[i] = MEM(SS_720_P1 + 4 * i);
    for (int i = 0; i < S4_P3_WORDS; i++) x1_p3[i] = MEM(SS_720_P3 + 4 * i);

    if (!e3_hooked)
    {
        if (MEM(SS_ADDR) != SS_I0 || MEM(SS_ADDR + 4) != SS_I1) return 0;
        if (patch_hook_function(SS_ADDR, SS_I0, e3_hook, "cm_wr: SSLT tables")) return 0;
        e3_hooked = 1;
    }
    if (hfps_sslt_patched[0] && hfps_sslt_val[0] != new_a3)
    {
        unpatch_memory(HFPS_SSLT_A3_ADDR);
        hfps_sslt_patched[0] = 0;
        hfps_sslt_val[0] = 0;
    }
    if (hfps_sslt_patched[1] && hfps_sslt_val[1] != new_a10)
    {
        unpatch_memory(HFPS_SSLT_A10_ADDR);
        hfps_sslt_patched[1] = 0;
        hfps_sslt_val[1] = 0;
    }
    if (!hfps_sslt_patched[0])
    {
        if (MEM(HFPS_SSLT_A3_ADDR) != 36u) return 0;
        if (patch_memory(HFPS_SSLT_A3_ADDR, 36u, new_a3, "cm_wr: hfps SSLT A3"))
        {
            cm_hfps_tall_unpatch();
            return 0;
        }
        hfps_sslt_patched[0] = 1;
        hfps_sslt_val[0] = new_a3;
    }
    if (!hfps_sslt_patched[1])
    {
        if (MEM(HFPS_SSLT_A10_ADDR) != 29u) return 0;
        if (patch_memory(HFPS_SSLT_A10_ADDR, 29u, new_a10, "cm_wr: hfps SSLT A10"))
        {
            cm_hfps_tall_unpatch();
            return 0;
        }
        hfps_sslt_patched[1] = 1;
        hfps_sslt_val[1] = new_a10;
    }
    if (hfps_hiv_patched || MEM(HFPS_HIV_YB_ADDR) == 0x2B3u)
    {
        MEM(HFPS_HIV_YB_ADDR) = 0x2B3u + y_shift;
        hfps_hiv_patched = 1;
    }
    e3_mode = 3;
    e3_active = 1;
    return 1;
}

static void cm_apply_x1_vscale(int anam)
{
    /* Keep EEKO_K1 100% stock (C0F11 OFF) and EEKO_M1 header stock (1904x1274);
     * Step 1 tests FrameParam L1 (952x960 = full 1920 lines) on proven {3,4,1,2} 0x402. */
    MEM(EEKO_K1_C +  0 * 4) = 1u;  MEM(EEKO_K1_U +  0 * 4) = 1u;
    MEM(EEKO_K1_C +  1 * 4) = 1u;  MEM(EEKO_K1_U +  1 * 4) = 1u;
    MEM(EEKO_K1_C +  2 * 4) = 1u;  MEM(EEKO_K1_U +  2 * 4) = 1u;
    MEM(EEKO_K1_C +  3 * 4) = 1u;  MEM(EEKO_K1_U +  3 * 4) = 1u;
    MEM(EEKO_K1_C + 10 * 4) = 49u; MEM(EEKO_K1_U + 10 * 4) = 49u;
    MEM(EEKO_K1_C + 11 * 4) = 44u; MEM(EEKO_K1_U + 11 * 4) = 44u;

    static const uint32_t m1_hdr_stock[8] = { 1904, 1920, 1274, 1280, 120, 119, 640, 637 };
    volatile uint32_t * hdst_c = (volatile uint32_t *)EEKO_M1_C;
    volatile uint32_t * hdst_u = (volatile uint32_t *)EEKO_M1_U;
    for (int i = 0; i < 8; i++)
    {
        hdst_c[i] = m1_hdr_stock[i];
        hdst_u[i] = m1_hdr_stock[i];
    }

    /* Keep 0x402 mode 1 & ROI 100% stock (720x480): with 3:1 L0 skip, 1920/3 = 640 rows
     * scale by stock 3/8 -> exact 720x240 (3:1) at LCD y=0..239 with full RGB colors. */
    static const uint32_t m1_stock[10] = { 0, 0, 0, 0, 1, 1, 3, 4, 3, 4 };
    volatile uint32_t * dst_c = (volatile uint32_t *)(EEKO_M1_C + 0x20u);
    volatile uint32_t * dst_u = (volatile uint32_t *)(EEKO_M1_U + 0x20u);
    for (int i = 0; i < 10; i++)
    {
        dst_c[i] = m1_stock[i];
        dst_u[i] = m1_stock[i];
    }

    *(volatile uint16_t *)EEKO_ROI_H_C = 0x6959u;
    *(volatile uint16_t *)EEKO_ROI_H_U = 0x6959u;
    *(volatile uint16_t *)EEKO_ROI_Y_C = 0x68DAu;
    *(volatile uint16_t *)EEKO_ROI_Y_U = 0x68DAu;
    (void)anam;
    if (lv && lv_dispsize == 1 && cm_raw_is_x1(shamem_read(REG_RAW)) && MEM(0x01E8053C + 0x10) == 1)
    {
        if (shamem_read(0xC0F38458) != 0xD88D0000u)
            EngDrvOutLV(0xC0F38458, 0xD88D0000u);
    }
}

/* Unpatch all hooks, ROM patches, and Eeko RAM tables without writing live sensor registers.
 * When LiveView is active, callers use cm_live_disarm() which pauses LV around this function
 * so SSLT and Eeko never see mismatched tables mid-frame. */
/* KI-1 / KI-2 (2026-11-27): x5 1:1 pan. Canon's LVx5_StartPreproPath (1.1.3 0xFF48A330, 1.2.3 0xFF494248)
 * loads the zoom box position X = LDRH [R4,#0x2C], Y = LDRH [R4,#0x2E] and calls SSLT_SetWinOffset
 * (1.1.3 0xFF33E6C0, 1.2.3 0xFF345010; r0 = table, r1 = X, r2 = Y). When X/Y change, SetWinOffset re-runs
 * the SSLT parser, which writes C0F383D4/DC = (Y << 16 | X >> 2, (Y + 1288) << 16 | (X + 1948) >> 2)
 * (type 7 margins A_8 = A_10 = 0). Canon's box logic is for its own 1196x792 window, so with our
 * 1948x1288 SSLT the window lands outside the raw; cm_live_task used to rewrite it <= 10 ms later, not
 * synced to the frame (race: pink/frozen LV, compress ASSERT, Canon VramStage.c:891 crash).
 * Fix: hook the instruction between the LDRHs and the BL and give SetWinOffset our X/Y, so Canon
 * writes our window itself, at its own time. Mapping (same as the old cm_live_task code): our
 * x0 = (Canon X - 376) & ~7 (376 = (1948 - 1196) / 2, keeps Canon's box centre), clamped, y0 = S4_Y;
 * locked presets (7..9) always S4_X. Before the first pan the window stays at S4_X. */
#define X5_PAN_HOOK      (cm_fw->x5_pan_hook)
#define X5_PAN_INSN      0xE5940050u   /* LDR R0, [R4, #0x50] (same in 1.1.3 and 1.2.3) */
static int cm_pan_hooked = 0;
static volatile int cm_pan_on = 0;          /* hook may change X/Y */
static volatile int cm_pan_cx = -1;         /* Canon X seen last (-1 = none since arm) */
static volatile uint32_t cm_pan_x0 = S4_X;  /* our window X */
static volatile uint32_t cm_pan_calls = 0;

static void cm_pan_hook(uint32_t * regs, uint32_t * stack, uint32_t pc)
{
    int m = cm_live_armed;
    if (!cm_pan_on || !e3_active || !cm_mode_is_x5(m)) return;
    int cx = (int)(regs[1] & 0xFFFFu);
    uint32_t x0 = cm_pan_x0;
    if (m <= 3)
    {
        if (cm_pan_cx >= 0 && cx != cm_pan_cx)
        {
            int x = (cx - 376) & ~7;
            if (x < 152) x = 152;
            if (x > S4_X_MAX) x = S4_X_MAX;
            x0 = (uint32_t)x;
        }
    }
    else
        x0 = S4_X;
    cm_pan_cx = cx;
    cm_pan_x0 = x0;
    regs[1] = x0;
    regs[2] = S4_Y;
    cm_pan_calls++;
}

static void cm_pan_unhook(void)
{
    cm_pan_on = 0;
    if (cm_pan_hooked) { unpatch_memory(X5_PAN_HOOK); cm_pan_hooked = 0; }
}

/* 1 = hook installed; 0 = not (cm_live_task then keeps the old rewrite) */
static int cm_pan_hook_arm(void)
{
    cm_pan_on = 0;
    cm_pan_cx = -1;
    cm_pan_x0 = S4_X;
    if (!cm_pan_hooked)
    {
        if (MEM(X5_PAN_HOOK) != X5_PAN_INSN) return 0;
        if (patch_hook_function(X5_PAN_HOOK, X5_PAN_INSN, cm_pan_hook, "crop_rec: x5 pan window")) return 0;
        cm_pan_hooked = 1;
    }
    cm_pan_on = 1;
    return 1;
}

static void cm_live_unpatch_all(int restore_sensor_direct)
{
    int was_x1_anam = cm_mode_is_x1_anam(cm_live_armed);
    int was_hfps    = cm_mode_is_hfps(cm_live_armed);
    cm_1x3_active = 0;
    cm_engio_vidmode_ok = 0;
    cm_cmos_vidmode_ok = 0;
    if (cm_1x3_hooked)
    {
        unpatch_memory(ENGIO_WRITE_ADDR);
        unpatch_memory(ADTG_WRITE_ADDR);
        unpatch_memory(CMOS_WRITE_ADDR);
        cm_1x3_hooked = 0;
    }
    if (was_x1_anam)
    {
        cm_x1_tall_unpatch();
        if (restore_sensor_direct && lv && is_movie_mode())
            cm_restore_stock_x1_sensor();
    }
    else if (was_hfps)
    {
        cm_hfps_tall_unpatch();
        if (restore_sensor_direct && lv && is_movie_mode() && MEM(LV_STATE_ADDR + 0x08) == 1)
            cm_restore_stock_x1_sensor();
    }
    cm_pan_unhook();
    e3_active = 0;
    e1_active = 0;
    e1_guard = 0;
    f4_mod = 0;
    s4_unpatch();
    if (f4_m1_patched)
    {
        cm_apply_x1_vscale(0);
        f4_m1_patched = 0;
    }
    if (f4_eeko_patched)
    {
        for (int i = 0; i < 14; i++)
        {
            MEM(F4_EEKO_K5 + 4 * i) = f4_eeko_orig[i];
            MEM(F4_EEKO_K5C + 4 * i) = f4_eeko_orig[i];
        }
        f4_set_402_m5(768, 5, 8);
        f4_eeko_patched = 0;
    }
    cm_live_armed = 0;
}

static void cm_live_disarm(void)
{
    if (!cm_live_armed) return;
    /* Safe disarm: if LiveView is actively running in movie mode, pause LV FIRST (dropping from x5/x10
     * to x1 beforehand if needed) so SSLT/Eeko DMA is completely stopped while we unpatch L0..L4,
     * SSLT margins, and Eeko tables, then resume LV cleanly with 100% stock Canon state. */
    if (lv && !lv_paused && is_movie_mode() && !RECORDING && !gui_menu_shown())
    {
        cm_1x3_active = 0;
        cm_engio_vidmode_ok = 0;
        cm_cmos_vidmode_ok = 0;
        if (lv_dispsize != 1)
        {
            set_lv_zoom(1);
            for (int w = 0; w < 50 && lv &&
                 (lv_dispsize != 1 || (!cm_raw_is_x1(shamem_read(REG_RAW)) && !cm_raw_is_720p(shamem_read(REG_RAW))) || MEM(0x01E8053C + 0x10) != 1); w++)
                msleep(20);
        }
        if (lv && !lv_paused && !RECORDING && !gui_menu_shown())
        {
            gui_uilock(UILOCK_EVERYTHING);
            PauseLiveView();
            msleep(200);
            cm_live_unpatch_all(0);
            cm_clear_lv_vram();
            ResumeLiveView();
            gui_uilock(UILOCK_NONE);
            for (int w = 0; w < 100 && !lv; w++) msleep(20);
            for (int w = 0; w < 50 && lv &&
                 ((!cm_raw_is_x1(shamem_read(REG_RAW)) && !cm_raw_is_720p(shamem_read(REG_RAW))) || MEM(0x01E8053C + 0x10) != 1); w++)
                msleep(20);
            for (int w = 0; w < 10 && lv && is_movie_mode() && !gui_menu_shown(); w++)
                msleep(50);
            return;
        }
    }
    cm_live_unpatch_all(1);
}

static int cm_live_arm(int mode)
{
    if (!is_5d3()) { cm_live_err = 1; return 0; }
    if (running) { cm_live_err = 2; return 0; }

    if (cm_mode_is_x1_anam(mode) || cm_mode_is_hfps(mode))
    {
        if (f4_install()) { cm_live_err = 8; return 0; }
        if (!cm_1x3_hooked)
        {
            if (MEM(CMOS_WRITE_ADDR) != CMOS_WRITE_I0 || MEM(CMOS_WRITE_ADDR + 4) != CMOS_WRITE_I1 ||
                MEM(ADTG_WRITE_ADDR) != ADTG_WRITE_I0 || MEM(ADTG_WRITE_ADDR + 4) != ADTG_WRITE_I1 ||
                MEM(ENGIO_WRITE_ADDR) != ENGIO_WRITE_I0 || MEM(ENGIO_WRITE_ADDR + 4) != ENGIO_WRITE_I1)
            { cm_live_err = 10; return 0; }
            if (patch_hook_function(CMOS_WRITE_ADDR, CMOS_WRITE_I0, cm_cmos_hook, "cm_wr: CMOS 1x3"))
            { cm_live_err = 11; return 0; }
            if (patch_hook_function(ADTG_WRITE_ADDR, ADTG_WRITE_I0, cm_adtg_hook, "cm_wr: ADTG 1x3"))
            { cm_live_err = 12; return 0; }
            if (patch_hook_function(ENGIO_WRITE_ADDR, ENGIO_WRITE_I0, cm_engio_hook, "cm_wr: ENGIO tall 1x3"))
            { cm_live_err = 13; return 0; }
            cm_1x3_hooked = 1;
        }
        cm_1x3_cmos_cnt = 0;
        cm_1x3_adtg_cnt = 0;
        cm_1x3_active = 1;
        /* Defer L0..L4 & tall SSLT patching to cm_zoom_to_x1() while LV is paused so
         * LiveView never flashes garbled pink/green frames before PauseLiveView(). */
        if (cm_mode_is_x1_anam(mode))
            f4_m1_patched = 1;
        cm_live_err = 0;
        cm_live_armed = mode;
        return 1;
    }

    if (lv && shamem_read(REG_RAW) == RAW_5D3_X5) { cm_live_err = 2; return 0; }
    if (MEM(SS_X1_P1 + 0xCC) != 0x900 || MEM(SS_X1_P1 + 0xD0) != 0x900 || MEM(SS_X1_P1 + 0xD4) != 0x80 ||
        MEM(SS_X5_P1) != 7 || MEM(SS_X5_P1 + 0x14) != 1168 || MEM(SS_X5_P1 + 0x1C) != 784 ||
        MEM(SS_X5_P1 + 0x28) != 1500 || MEM(SS_X5_P1 + 0x2C) != 300 ||
        MEM(SS_X5_P3 + 0x20) != 0x4AC || MEM(SS_X5_P3 + 0x24) != 0x318 ||
        MEM(SS_X5_P3 + 0x3C) != 0x4AC || MEM(SS_X5_P3 + 0x40) != 0x317 ||
        MEM(SS_X1_P3 + 0x20) != 0x79C || MEM(SS_X1_P3 + 0x24) != 0x508 ||
        MEM(SS_X1_P3 + 0x3C) != 0x79C || MEM(SS_X1_P3 + 0x40) != 0x507)
    { cm_live_err = 3; return 0; }
    for (int k = 0; k < 4; k++)
        if (!s4_patched[k] && MEM(s4_tbl[k][0]) != s4_tbl[k][1])
        { cm_live_err = 4; return 0; }

    for (int i = 0; i < S4_P1_WORDS; i++) s4_p1[i] = MEM(SS_X5_P1 + 4 * i);
    s4_p1[0x14 / 4] = 1920;
    s4_p1[0x1C / 4] = 1280;
    s4_p1[0x28 / 4] = S4_X;
    s4_p1[0x2C / 4] = S4_Y;
    for (int i = 0; i < 3; i++) s4_p1[0xCC / 4 + i] = MEM(SS_X1_P1 + 0xCC + 4 * i);
    for (int i = 0; i < S4_P3_WORDS; i++) s4_p3[i] = MEM(SS_X5_P3 + 4 * i);
    s4_p3[0x20 / 4] = 0x79C;
    s4_p3[0x24 / 4] = 0x508;
    s4_p3[0x3C / 4] = 0x79C;
    s4_p3[0x40 / 4] = 0x507;

    if (!e3_hooked)
    {
        if (MEM(SS_ADDR) != SS_I0 || MEM(SS_ADDR + 4) != SS_I1) { cm_live_err = 5; return 0; }
        if (patch_hook_function(SS_ADDR, SS_I0, e3_hook, "cm_wr: SSLT tables")) { cm_live_err = 6; return 0; }
        e3_hooked = 1;
    }
    if (!e1_install("3x")) { cm_live_err = 7; return 0; }
    e1_cap[0] = e1_map[0][3];
    e1_cap[1] = e1_map[1][3];
    if (f4_install()) { cm_live_err = 8; return 0; }

    for (int k = 0; k < 4; k++)
    {
        if (!s4_patched[k])
        {
            if (patch_memory(s4_tbl[k][0], s4_tbl[k][1], s4_tbl[k][2], "cm_wr: S4 SSLT type 7 margins"))
            {
                s4_unpatch();
                cm_live_err = 9;
                return 0;
            }
            s4_patched[k] = 1;
        }
    }

    f4_sub = 0;
    for (int i = 0; i < 14; i++)
    {
        MEM(F4_EEKO_K5 + 4 * i) = f4_eeko_tab[0][i];
        MEM(F4_EEKO_K5C + 4 * i) = f4_eeko_tab[0][i];
    }
    f4_set_402_m5(1272, 20, 53);
    f4_eeko_patched = 1;
    e3_replaced = 0;
    f4_seen = f4_done = f4_repl = f4_kept = f4_other = 0;
    f4_mod = 1;
    e1_guard = 1;
    e1_active = 1;
    e3_mode = 1;
    e3_active = 1;
    cm_live_err = 0;
    cm_live_armed = mode;
    cm_pan_hook_arm();
    return 1;
}

static void cm_live_task(void * unused)
{
    uint32_t cur_win_s = S4_WIN_S, cur_win_e = S4_WIN_E;
    uint32_t pan_seen = 0;
    int pan_stale = 1000;
    uint32_t prev_raw = 0;
    int target_zoom_done = 0;
    int was_lv = 0;
    int first_boot = 1;
    uint32_t prev_fm = 0xFFFFFFFFu;
    cm_recompute_mode();
    msleep(500);
    cm_recompute_mode();
    while (1)
    {
        cm_sync_shared_mode();
        cm_hiv_poll();
        if (running)
        {
            msleep(50);
            continue;
        }
        if (!lv || !is_movie_mode())
        {
            was_lv = 0;
            target_zoom_done = 0;
            msleep(100);
            continue;
        }
        if (gui_menu_shown())
        {
            target_zoom_done = 0;
            msleep(50);
            continue;
        }
        if (!was_lv)
        {
            /* Wait for Canon LV x1 display state to come up cleanly before arming/zooming
             * (prevents cold-boot x5 Eeko freeze when ML + Canon are still initializing). */
            for (int w = 0; w < 50 && lv && is_movie_mode() && !gui_menu_shown() &&
                 ((!cm_raw_is_x1(shamem_read(REG_RAW)) && !cm_raw_is_720p(shamem_read(REG_RAW))) || MEM(0x01E8053C + 0x10) != 1); w++)
                msleep(20);
            int settle_steps = first_boot ? 22 : 12; /* 1100 ms on cold boot, 600 ms after PLAY */
            for (int w = 0; w < settle_steps && lv && is_movie_mode() && !gui_menu_shown(); w++)
                msleep(50);
            if (!lv || !is_movie_mode() || gui_menu_shown())
                continue;
            was_lv = 1;
            first_boot = 0;
            target_zoom_done = 0;
        }
        uint32_t raw = lv ? shamem_read(REG_RAW) : 0;
        cm_recompute_mode();
        int is_720_lv = (MEM(LV_STATE_ADDR + 0x08) == 1);
        int want = cm_live_mode;
        if (cm_mode_is_hfps(want) && !is_720_lv) want = 0;
        else if (!cm_mode_is_hfps(want) && want > 0 && is_720_lv) want = 0;
        if (cm_live_armed != want)
        {
            if (RECORDING)
            {
                msleep(50);
                continue;
            }
            msleep(250);
            if (RECORDING || gui_menu_shown() || !lv || !is_movie_mode())
                continue;
            cm_recompute_mode();
            raw = lv ? shamem_read(REG_RAW) : 0;
            is_720_lv = (MEM(LV_STATE_ADDR + 0x08) == 1);
            want = cm_live_mode;
            if (cm_mode_is_hfps(want) && !is_720_lv) want = 0;
            else if (!cm_mode_is_hfps(want) && want > 0 && is_720_lv) want = 0;
            target_zoom_done = 0;
            if (cm_mode_is_x5(cm_live_armed) && cm_mode_is_x5(want))
            {
                cm_live_armed = want;
                continue;
            }
            if (cm_mode_is_x1_anam(cm_live_armed) && cm_mode_is_x1_anam(want))
            {
                int old_grp = (cm_live_armed >= 13) ? cm_live_armed : 0;
                int new_grp = (want >= 13) ? want : 0;
                int need_rebuild = (cm_1x3_cmos_cnt == 0 || cm_1x3_adtg_cnt == 0 ||
                                    old_grp != new_grp);
                cm_live_armed = want;
                if (lv && is_movie_mode())
                {
                    if (need_rebuild)
                    {
                        cm_1x3_cmos_cnt = 0;
                        cm_1x3_adtg_cnt = 0;
                        cm_zoom_to_x1();
                    }
                    cm_apply_x1_vscale(1);
                    prev_raw = lv ? shamem_read(REG_RAW) : 0;
                }
                continue;
            }
            if (cm_mode_is_hfps(cm_live_armed) && cm_mode_is_hfps(want))
            {
                cm_live_armed = want;
                if (lv && is_movie_mode() && cm_x1_is_720p_path())
                {
                    cm_1x3_cmos_cnt = 0;
                    cm_1x3_adtg_cnt = 0;
                    cm_zoom_to_x1();
                    prev_raw = lv ? shamem_read(REG_RAW) : 0;
                }
                continue;
            }
            if (cm_live_armed)
                cm_live_disarm();
            if (!want)
            {
                target_zoom_done = 1;
                prev_raw = lv ? shamem_read(REG_RAW) : 0;
                msleep(100);
                continue;
            }
            if (cm_mode_is_x1_anam(want) || cm_mode_is_hfps(want))
            {
                if (cm_live_arm(want))
                {
                    if (lv && is_movie_mode())
                    {
                        cm_zoom_to_x1();
                        if (cm_mode_is_x1_anam(want))
                            cm_apply_x1_vscale(1);
                    }
                    prev_fm = MEM(LV_STATE_ADDR + 0x04);
                    prev_raw = lv ? shamem_read(REG_RAW) : 0;
                }
                msleep(100);
                continue;
            }
            /* want == 1..3 or 7..9 (x5 1:1 modes): cm_live_disarm() already restored clean x1 if needed */
            if (lv && shamem_read(REG_RAW) == RAW_5D3_X5)
                cm_zoom_to_x1();
            cur_win_s = S4_WIN_S;
            cur_win_e = S4_WIN_E;
            if (cm_live_arm(want))
            {
                if (lv && is_movie_mode())
                    cm_zoom_to_x5();
                prev_raw = lv ? shamem_read(REG_RAW) : 0;
            }
            msleep(10);
            continue;
        }

        if (lv && is_movie_mode() && !RECORDING && !gui_menu_shown() && !target_zoom_done)
        {
            if (cm_mode_is_x5(cm_live_armed))
            {
                if (raw == RAW_5D3_X5 && lv_dispsize == 5 && (MEM(LV_STATE_ADDR + 0x10) >> 16) == 5)
                    target_zoom_done = 1;
                else
                {
                    if (f4_eeko_patched)
                    {
                        for (int i = 0; i < 14; i++)
                        {
                            MEM(F4_EEKO_K5 + 4 * i) = f4_eeko_tab[0][i];
                            MEM(F4_EEKO_K5C + 4 * i) = f4_eeko_tab[0][i];
                        }
                        f4_set_402_m5(1272, 20, 53);
                    }
                    cm_zoom_to_x5();
                }
                raw = lv ? shamem_read(REG_RAW) : 0;
            }
            else if (cm_mode_is_x1_anam(cm_live_armed))
            {
                if (cm_raw_is_x1(raw) && cm_1x3_cmos_cnt > 0 && cm_1x3_adtg_cnt > 0 && lv_dispsize == 1)
                {
                    cm_apply_x1_vscale(1);
                    target_zoom_done = 1;
                }
                else
                {
                    cm_zoom_to_x1();
                    cm_apply_x1_vscale(1);
                    target_zoom_done = 1;
                }
                raw = lv ? shamem_read(REG_RAW) : 0;
            }
            else if (cm_mode_is_hfps(cm_live_armed))
            {
                if (!cm_x1_is_720p_path() || (cm_raw_is_720p(raw) && cm_1x3_cmos_cnt > 0 && cm_1x3_adtg_cnt > 0 && lv_dispsize == 1))
                {
                    if (cm_x1_is_720p_path())
                        cm_apply_hfps_vscale(1);
                    target_zoom_done = 1;
                }
                else
                {
                    cm_zoom_to_x1();
                    target_zoom_done = 1;
                }
                raw = lv ? shamem_read(REG_RAW) : 0;
            }
            else
            {
                target_zoom_done = 1;
            }
        }
        else if (lv && is_movie_mode() && !RECORDING && !gui_menu_shown() &&
                 (cm_mode_is_x1_anam(cm_live_armed) || cm_mode_is_hfps(cm_live_armed)) &&
                 (cm_raw_is_x1(raw) || cm_raw_is_720p(raw)) && lv_dispsize == 1)
        {
            uint32_t cur_fm = MEM(LV_STATE_ADDR + 0x04);
            if (prev_fm != 0xFFFFFFFFu && cur_fm != prev_fm)
            {
                /* Wait until Canon Q menu / any Canon dialog is fully closed before calling
                 * PauseLiveView/ResumeLiveView (calling it while Q menu is open deadlocks GUI). */
                if (liveview_display_idle())
                {
                    msleep(250);
                    if (lv && is_movie_mode() && !RECORDING && !gui_menu_shown() &&
                        liveview_display_idle() && (cm_mode_is_x1_anam(cm_live_armed) || cm_mode_is_hfps(cm_live_armed)))
                    {
                        cm_zoom_to_x1();
                        if (cm_mode_is_x1_anam(cm_live_armed))
                            cm_apply_x1_vscale(1);
                        prev_fm = MEM(LV_STATE_ADDR + 0x04);
                    }
                }
            }
            else
            {
                prev_fm = cur_fm;
                if (cm_mode_is_x1_anam(cm_live_armed))
                    cm_apply_x1_vscale(1);
            }
        }
        if (cm_hfps_dirty && !RECORDING && !gui_menu_shown())
            cm_hfps_flush("TICK");

        prev_raw = raw;
        if (lv && cm_mode_is_x5(cm_live_armed) && raw == RAW_5D3_X5 &&
            lv_dispsize == 5 && (MEM(LV_STATE_ADDR + 0x10) >> 16) != 10)
        {
            uint32_t ws = shamem_read(REG_WIN_START), we = shamem_read(REG_WIN_END);
            /* pan hook live (called within the last ~200 ms): Canon writes our window itself, in
             * frame. Only a window outside the raw (another Canon writer) is still rewritten here. */
            uint32_t pc = cm_pan_calls;
            if (pc != pan_seen) { pan_seen = pc; pan_stale = 0; }
            else if (pan_stale < 1000) pan_stale++;
            if (cm_pan_on && pan_stale < 20)
            {
                uint32_t x0 = cm_pan_x0;
                cur_win_s = ((uint32_t)S4_Y << 16) | (x0 >> 2);
                cur_win_e = ((uint32_t)(S4_Y + S4_H) << 16) | ((x0 + S4_W) >> 2);
                if (!s4_win_ok(ws, we))
                {
                    EngDrvOutLV(REG_WIN_START, cur_win_s);
                    EngDrvOutLV(REG_WIN_END, cur_win_e);
                    cm_live_wins++;
                }
                msleep(10);
                continue;
            }
            if (cm_live_armed >= 7 && cm_live_armed <= 9)
            {
                cur_win_s = S4_WIN_S;
                cur_win_e = S4_WIN_E;
            }
            if (ws != cur_win_s || we != cur_win_e)
            {
                if (cm_live_armed <= 3 && ((we & 0xFFFF) - (ws & 0xFFFF)) == (S4_W >> 2) && (ws >> 16) >= 140)
                {
                    int x0 = ((int)((ws & 0xFFFF) << 2) - 376) & ~7;
                    if (x0 < 152) x0 = 152;
                    if (x0 > S4_X_MAX) x0 = S4_X_MAX;
                    int y0 = S4_Y;
                    cur_win_s = ((uint32_t)y0 << 16) | ((uint32_t)x0 >> 2);
                    cur_win_e = ((uint32_t)(y0 + S4_H) << 16) | ((uint32_t)(x0 + S4_W) >> 2);
                }
                EngDrvOutLV(REG_WIN_START, cur_win_s);
                EngDrvOutLV(REG_WIN_END, cur_win_e);
                cm_live_wins++;
            }
            msleep(10);
        }
        else if (lv && cm_mode_is_x1_anam(cm_live_armed) && cm_raw_is_x1(raw) && lv_dispsize == 1)
        {
            cm_apply_x1_vscale(1);
            msleep(20);
        }
        else
        {
            msleep(20);
        }
    }
}

#define CM_WR_DEBUG_MENU 0

static struct menu_entry cm_live_movie_menu[];

static CONFIG_INT("cm.family",       cm_family,       1); /* 0: OFF, 1: 1:1 crop, 2: 1x3, 3: 3x3 */
static CONFIG_INT("cm.preset_x5",    cm_preset_x5,    0); /* 0: 3:2 Full, 1: 1080p, 2: 2.35 Scope, 3: 2.3K, 4: 3.1K, 5: 3.5K */
static CONFIG_INT("cm.preset_anam",  cm_preset_anam,  0); /* 0: 3.4K (16:9), 1: 4.5K (2.35:1), 2: 5.4K (2040p), 3: 5.7K (3:1) */
static CONFIG_INT("cm.preset_hires", cm_preset_hires, 0); /* legacy config slot */
static CONFIG_INT("cm.preset_hfps",  cm_preset_hfps,  0); /* 0: 50/60p (HFR), 1: 45/48p Tall, 2: 2.40:1 Scope */

static int cm_saved_1080_family = 1; /* remembers last 1080p family (1:1 crop or 1x3) when Canon switches to 720p */

static void cm_recompute_mode(void)
{
    if (cm_family < 0 || cm_family > 3) cm_family = 0;
    int in_720 = (MEM(LV_STATE_ADDR + 0x08) == 1);
    uint32_t fm = MEM(LV_STATE_ADDR + 0x04);

    if (cm_family == 1 || cm_family == 2)
        cm_saved_1080_family = cm_family;

    /* Auto-adapt family to Canon's active video mode (1080p vs 720p 50/60) so the user
     * can never select or stay on an incompatible family for the current Canon mode. */
    if (cm_family != 0)
    {
        if (in_720 && cm_family != 3)
            cm_family = 3;
        else if (!in_720 && cm_family == 3)
            cm_family = (cm_saved_1080_family == 2) ? 2 : 1;
    }

    int m = 0;
    if (cm_family == 1)
    {
        int p = (cm_preset_x5 >= 0 && cm_preset_x5 <= 5) ? cm_preset_x5 : 0;
        m = (p <= 2) ? (1 + p) : (4 + p); /* 0..2 -> 1..3, 3..5 -> 7..9 */
    }
    else if (cm_family == 2)
    {
        int p = (cm_preset_anam >= 0 && cm_preset_anam <= 3) ? cm_preset_anam : 0;
        /* Preset 2 (5.4K 2040p, mode 13) requires 24p (fm == 4); on 25p/30p fall back to 5.7K */
        if (p == 2 && fm != 4)
        {
            p = 3;
            cm_preset_anam = 3;
        }
        static const int anam_modes[4] = { 4, 5, 13, 6 };
        m = anam_modes[p];
    }
    else if (cm_family == 3)
    {
        int p = (cm_preset_hfps >= 0 && cm_preset_hfps <= 2) ? cm_preset_hfps : 0;
        m = 10 + p;
    }
    cm_live_mode = m;
    cm_sync_shared_mode();
}

static int cm_current_fps_num(void)
{
    uint32_t fm = MEM(LV_STATE_ADDR + 0x04);
    if (fm == 4) return 24;
    if (fm == 3) return 25;
    if (fm == 2) return 30;
    if (fm == 1) return 50;
    if (fm == 0) return 60;
    return 24;
}

static void cm_current_tall_wh(int preset, int *w_out, int *h_out)
{
    uint32_t fm = MEM(LV_STATE_ADDR + 0x04);
    if (preset == 2) /* 5.4K (2040p) -> mode 13 */
    {
        int ty = (fm == 4) ? 2040 : (fm == 3) ? 1768 : 1536;
        *h_out = ty;
        *w_out = (fm == 4) ? 1808 : (fm == 3) ? 1568 : 1360;
        return;
    }
    /* fm: 4=24p(1920), 3=25p(1728), 2=30p(1536), other(1260) */
    int idx = (fm == 4) ? 0 : (fm == 3) ? 1 : (fm == 2) ? 2 : 3;
    static const int ty_tbl[4]   = { 1920, 1728, 1536, 1260 };
    static const int w169_tbl[4] = { 1136, 1024,  912,  752 };
    static const int w235_tbl[4] = { 1504, 1360, 1200,  992 };
    *h_out = ty_tbl[idx];
    *w_out = (preset == 0) ? w169_tbl[idx] : (preset == 1) ? w235_tbl[idx] : 1920;
}

static void cm_current_hfps_wh_fps(int preset, int *w_out, int *h_out, int *fps_out)
{
    uint32_t fm = MEM(LV_STATE_ADDR + 0x04);
    int is_50 = (fm == 1);
    *w_out = 1920;
    if (preset == 0)
    {
        *h_out   = is_50 ? 960 : 800;
        *fps_out = is_50 ? 50  : 60;
    }
    else if (preset == 1)
    {
        *h_out   = is_50 ? 1080 : 1040;
        *fps_out = is_50 ? 45   : 48;
    }
    else
    {
        *h_out   = 800;
        *fps_out = is_50 ? 50 : 60;
    }
}

static MENU_SELECT_FUNC(cm_live_select)
{
    int in_720 = (MEM(LV_STATE_ADDR + 0x08) == 1);
    int f = cm_family;
    if (in_720)
    {
        /* In Canon 720p 50/60fps mode, only OFF (0) and 3x3 High FPS (3) are valid */
        f = (f == 0) ? 3 : 0;
    }
    else
    {
        /* In Canon 1080p 24/25/30fps mode, only OFF (0), 1:1 crop (1), and 1x3 (2) are valid */
        if (f > 2) f = 0;
        f += (delta >= 0) ? 1 : -1;
        if (f < 0) f = 2;
        if (f > 2) f = 0;
        if (f == 1 || f == 2)
            cm_saved_1080_family = f;
    }
    cm_family = f;
    cm_recompute_mode();
}

static MENU_SELECT_FUNC(cm_preset_x5_select)
{
    int p = cm_preset_x5 + delta;
    if (p < 0) p = 5;
    if (p > 5) p = 0;
    cm_preset_x5 = p;
    cm_recompute_mode();
}

static MENU_SELECT_FUNC(cm_preset_anam_select)
{
    uint32_t fm = MEM(LV_STATE_ADDR + 0x04);
    int step = (delta >= 0) ? 1 : -1;
    int p = cm_preset_anam + step;
    if (p < 0) p = 3;
    if (p > 3) p = 0;
    /* Preset 2 (5.4K 2040p) is only valid in 24p (fm == 4); skip it in 25p/30p */
    if (p == 2 && fm != 4)
    {
        p += step;
        if (p < 0) p = 3;
        if (p > 3) p = 0;
    }
    cm_preset_anam = p;
    cm_recompute_mode();
}

static MENU_SELECT_FUNC(cm_preset_hfps_select)
{
    int p = cm_preset_hfps + delta;
    if (p < 0) p = 2;
    if (p > 2) p = 0;
    cm_preset_hfps = p;
    cm_recompute_mode();
}

static MENU_UPDATE_FUNC(cm_preset_x5_update)
{
    static const char *names[6] = {
        "1280p (3:2)",
        "1080p (16:9)",
        "816p (2.35:1)",
        "2.3K (16:9)",
        "3.1K (2.35:1)",
        "3.5K (2.71:1)"
    };
    static const char *res[6] = {
        "1920x1280",
        "1920x1080",
        "1920x816",
        "2336x1312",
        "3104x1320",
        "3584x1320"
    };
    int p = (cm_preset_x5 >= 0 && cm_preset_x5 <= 5) ? cm_preset_x5 : 0;
    MENU_SET_VALUE("%s", names[p]);
    MENU_SET_RINFO("%s", res[p]);
    if (p <= 2)
        MENU_SET_HELP("1:1 center crop (3x), true WYSIWYG LiveView.");
    else
        MENU_SET_HELP("1:1 Hi-Res center crop (fast CF recommended). Centered LiveView.");
}

static MENU_UPDATE_FUNC(cm_preset_anam_update)
{
    int p = (cm_preset_anam >= 0 && cm_preset_anam <= 3) ? cm_preset_anam : 0;
    int rw = 1920, ty = 1920;
    cm_current_tall_wh(p, &rw, &ty);
    static const char *names[4] = {
        "3.4K (16:9)",
        "4.5K (2.35:1)",
        "5.4K (2040p)",
        "5.7K (3:1 Max)"
    };
    MENU_SET_VALUE("%s", names[p]);
    MENU_SET_RINFO("%dx%d", rw, ty);
    MENU_SET_HELP("1x3 anamorphic -> %dx%d desqueezed @ %dp, WYSIWYG.", rw * 3, ty, cm_current_fps_num());
}

static MENU_UPDATE_FUNC(cm_preset_hfps_update)
{
    static const char *names[3] = { "50/60p (HFR)", "45/48p Tall", "2.40:1 Scope" };
    int p = (cm_preset_hfps >= 0 && cm_preset_hfps <= 2) ? cm_preset_hfps : 0;
    int rw = 1920, rh = 800, rfps = 60;
    cm_current_hfps_wh_fps(p, &rw, &rh, &rfps);
    MENU_SET_VALUE("%s", names[p]);
    MENU_SET_RINFO("%dx%d %dp", rw, rh, rfps);
    if (MEM(LV_STATE_ADDR + 0x08) != 1)
        MENU_SET_WARNING(MENU_WARN_ADVICE, "Switch Canon video mode to 720p 50/60fps first.");
    else
        MENU_SET_HELP("3x3 full-frame High FPS (%dx%d @ %dp).", rw, rh, rfps);
}

static MENU_UPDATE_FUNC(cm_ar_info_update)
{
    int m = cm_live_mode;
    const char *ar = "16:9";
    if (m == 1) ar = "3:2";
    else if (m == 2 || m == 4 || m == 7) ar = "16:9";
    else if (m == 3 || m == 5 || m == 8) ar = "2.35:1";
    else if (m == 6) ar = "3:1";
    else if (m == 9) ar = "2.71:1";
    else if (m == 13) ar = "2.66:1";
    else if (m == 10) ar = (MEM(LV_STATE_ADDR + 0x04) == 1) ? "2:1" : "2.40:1";
    else if (m == 11) ar = (MEM(LV_STATE_ADDR + 0x04) == 1) ? "16:9" : "1.85:1";
    else if (m == 12) ar = "2.40:1";
    MENU_SET_VALUE("%s", ar);
    MENU_SET_HELP("Aspect ratio of the current preset (use Preset / Framing to change).");
}

static MENU_UPDATE_FUNC(cm_output_info_update)
{
    int m = cm_live_mode;
    int fps = cm_current_fps_num();
    if (m == 0)
    {
        int is_720 = (MEM(LV_STATE_ADDR + 0x08) == 1);
        MENU_SET_VALUE(is_720 ? "Canon stock 720p" : "Canon stock 1080p");
        MENU_SET_RINFO("%dp", fps);
        return;
    }
    if (m >= 1 && m <= 3)
    {
        static const int h[3] = { 1280, 1080, 816 };
        MENU_SET_VALUE("1920x%d @ %dp", h[m - 1], fps);
        MENU_SET_RINFO("1:1 WYSIWYG");
    }
    else if ((m >= 4 && m <= 6) || m == 13)
    {
        int rw = 1920, ty = 1920;
        int p = (m == 4) ? 0 : (m == 5) ? 1 : (m == 13) ? 2 : 3;
        cm_current_tall_wh(p, &rw, &ty);
        MENU_SET_VALUE("%dx%d @ %dp", rw * 3, ty, fps);
        MENU_SET_RINFO("1x3 WYSIWYG");
    }
    else if (m >= 7 && m <= 9)
    {
        static const int w[3] = { 2336, 3104, 3584 };
        static const int h[3] = { 1312, 1320, 1320 };
        MENU_SET_VALUE("%dx%d @ %dp", w[m - 7], h[m - 7], fps);
        MENU_SET_RINFO("1:1 Center");
    }
    else
    {
        int rw = 1920, rh = 800, rfps = 60;
        cm_current_hfps_wh_fps(m - 10, &rw, &rh, &rfps);
        if (MEM(LV_STATE_ADDR + 0x08) != 1)
        {
            MENU_SET_VALUE("Requires 720p 50/60");
            MENU_SET_RINFO("%dx%d", rw, rh);
        }
        else
        {
            MENU_SET_VALUE("%dx%d @ %dp", rw, rh, rfps);
            MENU_SET_RINFO("3x3 FF");
        }
    }
}

static MENU_SELECT_FUNC(cm_framing_select)
{
    if (cm_family == 1)      cm_preset_x5_select(priv, delta);
    else if (cm_family == 2) cm_preset_anam_select(priv, delta);
    else if (cm_family == 3) cm_preset_hfps_select(priv, delta);
}

static MENU_UPDATE_FUNC(cm_framing_update)
{
    if (cm_family == 1)      cm_preset_x5_update(entry, info);
    else if (cm_family == 2) cm_preset_anam_update(entry, info);
    else if (cm_family == 3) cm_preset_hfps_update(entry, info);
    else                     MENU_SET_VALUE("N/A");
}

static MENU_UPDATE_FUNC(cm_live_update)
{
    if (!is_5d3())
    {
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "5D3 1.1.3 / 1.2.3 only.");
        MENU_SET_VALUE("N/A");
        return;
    }

    cm_recompute_mode();

    cm_live_movie_menu[0].children[0].shidden = (cm_family == 0);
    cm_live_movie_menu[0].children[1].shidden = (cm_family == 0);
    cm_live_movie_menu[0].children[2].shidden = (cm_family == 0);

    int m = cm_live_mode;
    int in_720 = (MEM(LV_STATE_ADDR + 0x08) == 1);
    MENU_SET_ENABLED(m != 0);
    MENU_SET_ICON(m ? MNI_ON : MNI_OFF, 0);
    if (m == 0)
    {
        MENU_SET_VALUE("OFF");
        if (in_720)
            MENU_SET_HELP("Canon 720p mode: select 3x3 High FPS (switch Canon to 1080p for 1:1 / 1x3).");
        else
            MENU_SET_HELP("Canon 1080p mode: select 1:1 crop or 1x3 (switch Canon to 720p for 3x3 HFR).");
        return;
    }

    if (m >= 1 && m <= 3)
    {
        static const char *tag[3] = { "1280p", "1080p", "816p" };
        int act = (cm_live_armed == m && lv && shamem_read(REG_RAW) == RAW_5D3_X5);
        MENU_SET_VALUE("1:1 crop%s", act ? " *" : "");
        MENU_SET_RINFO("%s", tag[m - 1]);
        MENU_SET_HELP("Center crop on sensor, no pixel binning/skipping. Press Q for Preset.");
    }
    else if (m >= 7 && m <= 9)
    {
        static const char *tag[3] = { "2.3K", "3.1K", "3.5K" };
        int act = (cm_live_armed == m && lv && shamem_read(REG_RAW) == RAW_5D3_X5);
        MENU_SET_VALUE("1:1 crop%s", act ? " *" : "");
        MENU_SET_RINFO("%s", tag[m - 7]);
        MENU_SET_HELP("Center crop on sensor, no pixel binning/skipping. Press Q for Preset.");
    }
    else if ((m >= 4 && m <= 6) || m == 13)
    {
        int rw = 1920, ty = 1920;
        int p = (m == 4) ? 0 : (m == 5) ? 1 : (m == 13) ? 2 : 3;
        cm_current_tall_wh(p, &rw, &ty);
        static const char *tag[4] = { "3.4K", "4.5K", "5.4K", "5.7K" };
        int act = (cm_live_armed == m && lv && cm_raw_is_x1(shamem_read(REG_RAW)) && cm_1x3_cmos_cnt > 0);
        MENU_SET_VALUE("1x3%s", act ? " *" : "");
        MENU_SET_RINFO("%s", tag[p]);
        MENU_SET_HELP("a.k.a Anamorphic (%dx%d -> %dx%d). Press Q for Preset.", rw, ty, rw * 3, ty);
    }
    else
    {
        int rw = 1920, rh = 800, rfps = 60;
        cm_current_hfps_wh_fps(m - 10, &rw, &rh, &rfps);
        int in_720 = (MEM(LV_STATE_ADDR + 0x08) == 1);
        int act = (cm_live_armed == m && lv && in_720 && cm_raw_is_720p(shamem_read(REG_RAW)) && cm_1x3_cmos_cnt > 0);
        MENU_SET_VALUE("3x3%s", act ? " *" : "");
        MENU_SET_RINFO("%dp %dfps", rh, rfps);
        if (!in_720)
            MENU_SET_WARNING(MENU_WARN_ADVICE, "Switch Canon video mode to 720p 50/60fps first.");
        else
            MENU_SET_HELP("Full-frame 3x3 High Framerate (%dx%d @ %dp). Press Q for Preset.", rw, rh, rfps);
    }
}

static struct menu_entry cm_live_movie_menu[] =
{
    {
        .name   = "Crop Mor3",
        .select = cm_live_select,
        .update = cm_live_update,
        .icon_type = IT_BOOL,
        .help   = "Turn your camera into Crop Mor3 modes (select one): 1:1 crop | 1x3 | 3x3.",
        .help2  = "Press Q to select Preset and inspect Aspect ratio / Output resolution.",
        .children = (struct menu_entry[]) {
            {
                .name   = "Preset:",
                .select = cm_framing_select,
                .update = cm_framing_update,
                .icon_type = IT_DICE,
                .shidden = 0,
            },
            {
                .name   = "Aspect ratio:",
                .select = cm_framing_select,
                .update = cm_ar_info_update,
                .icon_type = IT_DICE,
                .shidden = 0,
            },
            {
                .name   = "Output:",
                .update = cm_output_info_update,
                .icon_type = IT_ALWAYS_ON,
                .shidden = 0,
            },
            MENU_EOL,
        },
    },
};

static struct menu_entry cm_wr_menu[] =
{
    {
        .name   = "Crop Mor3",
        .select = cm_live_select,
        .update = cm_live_update,
        .icon_type = IT_DICE,
        .help   = "1:1 crop | 1x3 | 3x3.",
        .help2  = "WYSIWYG LiveView framing + MLV raw recording.",
    },
    {
        .name   = "Screenshot (read-only)",
        .select = shot_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "Saves the current LiveView picture to ML/LOGS/CMnnnn.PPM.",
        .help2  = "Use in plain Canon x5 for comparison with Stage 4b / 4c.",
    },
    {
        .name   = "F4A: 3x 1904x1272 (box-center 20s)",
        .select = f4a_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "3x 1904x1272 (3:2, no lines) centered on Canon zoom box. 20 s + screenshots.",
        .help2  = "Start in 1080p x1, go x5. Move zoom box to pan! Restore: x10 -> x1.",
    },
    {
        .name   = "F4B: 3x 1904x1272 (free pan 120s)",
        .select = f4b_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "3x 1904x1272 (3:2, no lines) centered on Canon zoom box. Up to 120 s in x5.",
        .help2  = "Start in 1080p x1, go x5. Exit anytime via x10 -> x1.",
    },
    {
        .name   = "F4C: 3x 1904x1272 (raw-center 20s)",
        .select = f4c_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "3x 1904x1272 (3:2, no lines) at fixed raw buffer center (968, 76). 20 s.",
        .help2  = "Start in 1080p x1, go x5. 20 s. Restore: x10 -> x1.",
    },
    {
        .name   = "Stage 4b quiet (x1 sizes)",
        .select = s4bq_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "Stage 4 with SSLT W/H = x1 (1920x1280): the whole pipeline at x1 sizes.",
        .help2  = "Start in 1080p x1, go x5. 20 s, screenshots at 3 s / 12 s. Restore: x10 -> x1.",
    },
    {
        .name   = "IPC observe (hook, read-only)",
        .select = ipc_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "Observes Canon's inter-core mailbox (RAM 0x18E90) for 15 s, changes nothing.",
        .help2  = "Start in Canon 1080p x1, then switch to x5 within ~3 s and stay there.",
    },
    {
        .name   = "Shadow dump (read-only)",
        .select = shdump_select,
        .update = stage3a_update,
        .icon_type = IT_ACTION,
        .help   = "Writes nothing: Canon register shadow table, 2 snapshots 1 s apart.",
        .help2  = "File ML/LOGS/SH5D3_NN.BIN + a short WR5D3 log line.",
    },
};

static LVINFO_UPDATE_FUNC(cm_lvinfo_update)
{
    LVINFO_BUFFER(16);
    if (!is_5d3() || !lv || !is_movie_mode())
        return;

    int m = cm_live_mode;
    if (m <= 0)
        return;

    int in_720 = (MEM(LV_STATE_ADDR + 0x08) == 1);
    uint32_t raw = shamem_read(REG_RAW);

    if (m >= 1 && m <= 3)
    {
        static const char *lbl[3] = { "3x 1:1", "1080p 1:1", "2.35 1:1" };
        int act = (cm_live_armed == m && !in_720 && raw == RAW_5D3_X5);
        snprintf(buffer, sizeof(buffer), "%s%s", lbl[m - 1], act ? "" : " " SYM_WARNING);
        item->color_fg = act ? COLOR_WHITE : COLOR_YELLOW;
    }
    else if ((m >= 4 && m <= 6) || m == 13)
    {
        int p = (m == 4) ? 0 : (m == 5) ? 1 : (m == 13) ? 2 : 3;
        static const char *lbl[4] = { "3.4K 1x3", "4.5K 1x3", "5.4K 1x3", "5.7K 1x3" };
        int act = (cm_live_armed == m && !in_720 && cm_raw_is_x1(raw) && cm_1x3_cmos_cnt > 0);
        snprintf(buffer, sizeof(buffer), "%s%s", lbl[p], act ? "" : " " SYM_WARNING);
        item->color_fg = act ? COLOR_WHITE : COLOR_YELLOW;
    }
    else if (m >= 7 && m <= 9)
    {
        static const char *lbl[3] = { "2.3K 1:1", "3.1K 1:1", "3.5K 1:1" };
        int act = (cm_live_armed == m && !in_720 && raw == RAW_5D3_X5);
        snprintf(buffer, sizeof(buffer), "%s%s", lbl[m - 7], act ? "" : " " SYM_WARNING);
        item->color_fg = act ? COLOR_WHITE : COLOR_YELLOW;
    }
    else if (m >= 10 && m <= 12)
    {
        int rw = 1920, rh = 800, rfps = 60;
        cm_current_hfps_wh_fps(m - 10, &rw, &rh, &rfps);
        int act = (cm_live_armed == m && in_720 && cm_raw_is_720p(raw) && cm_1x3_cmos_cnt > 0);
        if (m == 12)
            snprintf(buffer, sizeof(buffer), "2.40 %dp 3x3%s", rfps, act ? "" : " " SYM_WARNING);
        else
            snprintf(buffer, sizeof(buffer), "%dp%d 3x3%s", rh, rfps, act ? "" : " " SYM_WARNING);
        item->color_fg = act ? COLOR_WHITE : COLOR_YELLOW;
    }
}

static struct lvinfo_item cm_lvinfo_items[] = {
    {
        .name = "Crop info",
        .which_bar = LV_BOTTOM_BAR_ONLY,
        .update = cm_lvinfo_update,
        .preferred_position = -50,
        .priority = 1,
    }
};

static unsigned int cm_wr_init()
{
    if (!is_5d3())
    {
        raw_capture_info.reserved = 0;
        return 0;
    }
    cm_fw = is_5d3_123() ? &cm_fw_123 : &cm_fw_113;
    cm_recompute_mode();
    menu_add("Movie", cm_live_movie_menu, COUNT(cm_live_movie_menu));
    lvinfo_add_items(cm_lvinfo_items, COUNT(cm_lvinfo_items));
#if CM_WR_DEBUG_MENU
    menu_add("Debug", cm_wr_menu, COUNT(cm_wr_menu));
#endif
    task_create("cm_live", 0x1e, 0x1000, cm_live_task, 0);
    return 0;
}

static unsigned int cm_wr_deinit()
{
    raw_capture_info.reserved = 0;
    return 0;
}

MODULE_INFO_START()
    MODULE_INIT(cm_wr_init)
    MODULE_DEINIT(cm_wr_deinit)
MODULE_INFO_END()

MODULE_CONFIGS_START()
    MODULE_CONFIG(cm_family)
    MODULE_CONFIG(cm_preset_x5)
    MODULE_CONFIG(cm_preset_anam)
    MODULE_CONFIG(cm_preset_hires)
    MODULE_CONFIG(cm_preset_hfps)
MODULE_CONFIGS_END()
