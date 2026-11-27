/**
 
 */

/*
 * Copyright (C) 2013 Magic Lantern Team
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include <module.h>
#include <dryos.h>
#include <property.h>
#include <bmp.h>
#include <beep.h>
#include <menu.h>
#include <config.h>
#include <cropmarks.h>
#include <edmac.h>
#include <raw.h>
#include <zebra.h>
#include <util.h>
#include <timer.h>
#include <shoot.h>
#include <vram.h>
#include <imgconv.h>

/* crop-mood build switches:
 * MLV_PLAY_PERF:  on-screen timings, automatic benchmark (ML/LOGS/MLVBENCH.TXT), RATE/MENU test keys
 * MLV_PLAY_PROXY: build / play .PRX proxies (not yet tested on camera) */
#ifndef MLV_PLAY_PERF
#define MLV_PLAY_PERF  0
#endif
#ifndef MLV_PLAY_PROXY
#define MLV_PLAY_PROXY 1
#endif

#include <string.h>

#include "../ime_base/ime_base.h"
#include "../trace/trace.h"
#include "../mlv_rec/mlv.h"
#include "../file_man/file_man.h"
#include "../lv_rec/lv_rec.h"
#include "../raw_twk/raw_twk.h"
#include "../silent/lossless.h"
#include "console.h"

/* uncomment for live debug messages */
//~ #define trace_write(trace, fmt, ...) { printf(fmt, ## __VA_ARGS__); printf("\n"); msleep(500); }

#define MAX_PATH                 100

/* declare the video.bmp we have added */
/* icon from: http://www.softicons.com/free-icons/system-icons/touch-of-gold-icons-by-skingcito/video-black-icon */
EXTLD(video_bmp);


/* only works if the raw_rec/mlv_rec module has its config variable non-static */
static int video_enabled_dummy = 0;
extern int WEAK_FUNC(video_enabled_dummy) raw_video_enabled;
extern int WEAK_FUNC(video_enabled_dummy) mlv_video_enabled;

static char *movie_filename_dummy = "";
extern char *WEAK_FUNC(movie_filename_dummy) raw_movie_filename;
extern char *WEAK_FUNC(movie_filename_dummy) mlv_movie_filename;

static int raw_exposure_adjust_dummy = 0;
extern int WEAK_FUNC(raw_exposure_adjust_dummy) raw_exposure_adjust;

static int res_x = 0;
static int res_y = 0;
static int frame_count = 0;
static int frame_size = 0;
static int bits_per_pixel = 0;
static unsigned int fps1000 = 0; /* used for RAW playback */

static volatile uint32_t mlv_play_render_abort = 0;
static volatile uint32_t mlv_play_rendering = 0;
static volatile uint32_t mlv_play_render_alive = 0;
static volatile uint32_t mlv_play_osd_alive = 0;
static volatile uint32_t mlv_play_stopfile = 0;
static volatile uint32_t mlv_play_in_gallery = 0;
static volatile uint32_t mlv_play_return_to_gallery = 0;
static volatile uint32_t mlv_play_render_in_gallery_ack = 0;
static volatile uint32_t mlv_play_osd_in_gallery_ack = 0;

static CONFIG_INT("play.quality", mlv_play_quality, 2); /* 0: stock color, 1: stock gray fast, 2: superpixel (crop-mood) */
#define MLV_PLAY_QUALITY_SUPER 2
#define MLV_PLAY_QUALITY_COUNT 3
static CONFIG_INT("play.exact_fps", mlv_play_exact_fps, 0);
static CONFIG_INT("play.lut", mlv_play_lut, 0); /* 0: Orig (bypass), 1..5: presets, 6+: ML/LUTS/*.cube */
static CONFIG_INT("play.audio", mlv_play_audio, 5); /* 0: Mute, 1..5: ASIF hardware volume (2..10), 6: +6dB, 7: +12dB */
static volatile uint32_t mlv_play_lut_dirty = 0;

/* ASIF hardware DMA DAC interface (present on 5D3.113, 5D3.123, EOSM.202) */
extern void StartASIFDMADAC(void *buf1, int N1, void *buf2, int N2, void (*continue_cbr)(void *), void *cbr_arg);
extern void StopASIFDMADAC(void (*stop_cbr)(void *), int cbr_arg);
extern void SetNextASIFDACBuffer(void *buf, int N);
extern void SetSamplingRate(int sample_rate, int unknown);
extern void PowerAudioOutput(void);
extern void SetAudioVolumeOut(int volume);
extern void audio_configure(int force);

/* forward declarations for 3D LUT and audio helpers used by OSD */
static int sp_lut_count(void);
static const char *sp_lut_name(int idx);
static void mlv_play_lut_cycle(int show_toast);
static void aud_stop(void);
static void aud_free_buffers(void);
static void aud_set_volume(int vol);
static int32_t mlv_play_read_small(FILE *f, void *dst, uint32_t size);

static int mlv_play_zoom = 0;
static int mlv_play_zoom_x_pct = 0;
static int mlv_play_zoom_y_pct = 0;

static uint32_t mlv_play_trace_ctx = TRACE_ERROR;

/* OSD menu items */
#define MLV_PLAY_MENU_IDLE    0
#define MLV_PLAY_MENU_FADEIN  1
#define MLV_PLAY_MENU_FADEOUT 2
#define MLV_PLAY_MENU_SHOWN   3
#define MLV_PLAY_MENU_HIDDEN  4


static uint32_t mlv_play_osd_state = MLV_PLAY_MENU_IDLE;
static int32_t mlv_play_osd_x = 360;
static int32_t mlv_play_osd_y = 0;
static uint32_t mlv_play_render_timestep = 10;
static uint32_t mlv_play_idle_timestep = 1000;
static uint32_t mlv_play_osd_force_redraw = 0;
static uint32_t mlv_play_osd_idle = 1000;
static volatile uint32_t mlv_play_osd_item = 2;
static volatile uint32_t mlv_play_paused = 0;
static uint32_t mlv_play_info = 1;
static uint32_t mlv_play_timer_stop = 1;
static uint32_t mlv_play_frames_skipped = 0;

/* this structure is used to build the mlv_xref_t table */
typedef struct 
{
    uint64_t    frameTime;
    uint64_t    frameOffset;
    uint16_t    fileNumber;
    uint16_t    frameType;
} frame_xref_t;

typedef struct
{
    char fullPath[MAX_PATH];
    uint32_t fileSize;
    uint32_t timestamp;
} playlist_entry_t;


#define SCREEN_MSG_LEN  64

typedef struct 
{
    char topLeft[SCREEN_MSG_LEN];
    char topRight[SCREEN_MSG_LEN];
    char botLeft[SCREEN_MSG_LEN];
    char botRight[SCREEN_MSG_LEN];
} screen_msg_t;

typedef struct 
{
    uint32_t frameSize;
    void *frameBuffer;
    void *frameBufferAligned;
    void *frameData;       /* raw data start: frameBufferAligned + offset of a sector-aligned card read */
    screen_msg_t messages;
    uint16_t xRes;
    uint16_t yRes;
    uint8_t  yBinSkip;
    uint8_t  xBinSkip;
    uint16_t bitDepth;     /* container bits per pixel of frameBufferAligned */
    uint16_t valueBits;    /* bits actually used by the pixel values (black/white scale) */
    uint16_t blackLevel;
    uint16_t whiteLevel;
    uint32_t allocSize;
    uint32_t filledSize;   /* bench rn: frame_size this buffer already holds data for */
    uint32_t frameIndex;   /* VIDF frameNumber */
    uint32_t totalFrames;  /* total frames in clip for progress bar */
    uint32_t isProxy;      /* frameBufferAligned holds a PRX frame, not raw data */
} frame_buf_t;

/* --- perf instrumentation (crop-mood 5D3) ---
 * per-frame timings in microseconds, summed over a 1 s window and shown on line 3 */
typedef struct
{
    uint32_t read_us;      /* FIO_ReadFile of VIDF payload */
    uint32_t read_bytes;
    uint32_t dec_us;       /* lossless_decompress_raw */
    uint32_t read_frames;
    uint32_t read_calls;
    uint32_t render_us;    /* mlv_play_render_frame (incl. LUT build) */
    uint32_t render_frames;
} perf_acc_t;

static perf_acc_t perf_acc;           /* accumulating window */
static char perf_line[SCREEN_MSG_LEN];
static char perf_line2[SCREEN_MSG_LEN];
static uint64_t perf_win_start = 0;
static uint32_t sp_auto = 0;
static uint32_t mlv_play_read_mode = 0;   /* 0: full frame, 1: rows needed by super (2x lines), 2: rows4 (4x lines, half the rows) */
static uint32_t mlv_play_bench = 0;       /* 0: off, 1: read only (no render), 2: render only (no card read) */
static const char *mlv_play_read_names[] = { "full", "rows", "rows4", "full-u" };   /* full-u: old unaligned read */
static const char *mlv_play_bench_names[] = { "off", "rd", "rn", "seq" };   /* 1 if super was forced because stock can't render the format */
static uint32_t perf_lut_us = 0;      /* render overhead with zero lines (LUT + malloc) */
static char prx_status[16] = "-";
static uint32_t perf_render_max = 0;
static uint64_t perf_last_frame_us = 0;
static uint32_t perf_gap_max = 0;      /* longest pause between rendered frames in this window */

/* --- automatic benchmark: runs once per opened file, no keys needed ---
 * 4 phases x 4 one-second windows (first window of each phase discarded),
 * results shown on screen and appended to ML/LOGS/MLVBENCH.TXT */
#define AB_PHASES 4
#define AB_WINDOWS 4
static const struct { uint8_t read_mode; uint8_t bench; const char *name; } ab_phases[AB_PHASES] =
{
    { 0, 0, "normal     " },
    { 0, 1, "read only  " },
    { 0, 2, "render only" },
    { 0, 3, "seq test   " },
};
static int ab_phase = -1;          /* -1: idle, 0..AB_PHASES-1: running, AB_PHASES: done */
static int ab_window = 0;
static perf_acc_t ab_sum;
static uint32_t ab_sum_dt = 0;
static char ab_file[SCREEN_MSG_LEN];
static char ab_status[SCREEN_MSG_LEN];
static char ab_result[AB_PHASES][SCREEN_MSG_LEN];

/* phase 4: one-shot read pattern test done by the reader task (bench mode 3) */
static volatile int seq_state = 0;          /* 0: pending, 1: running, 2: done */
static char seq_result[SCREEN_MSG_LEN];

static void ab_set_phase(int phase)
{
    seq_state = 0;
    ab_phase = phase;
    ab_window = 0;
    ab_sum_dt = 0;
    memset(&ab_sum, 0, sizeof(ab_sum));
    if (phase >= 0 && phase < AB_PHASES)
    {
        mlv_play_read_mode = ab_phases[phase].read_mode;
        mlv_play_bench = ab_phases[phase].bench;
        snprintf(ab_status, SCREEN_MSG_LEN, "AUTO TEST %d/%d: %s ...", phase + 1, AB_PHASES, ab_phases[phase].name);
    }
    else
    {
        mlv_play_read_mode = 0;
        mlv_play_bench = 0;
    }
}

static void ab_start(const char *file)
{
    strncpy(ab_file, file, SCREEN_MSG_LEN - 1);
    ab_file[SCREEN_MSG_LEN - 1] = 0;
    for (int i = 0; i < AB_PHASES; i++)
    {
        ab_result[i][0] = 0;
    }
    ab_set_phase(0);
}

static void ab_write_log()
{
    static char buf[6 * SCREEN_MSG_LEN];
    snprintf(buf, sizeof(buf), "%s\n", ab_file);
    for (int i = 0; i < AB_PHASES; i++)
    {
        int n = strlen(buf);
        snprintf(buf + n, sizeof(buf) - n, "  %s\n", ab_result[i]);
    }
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/MLVBENCH.TXT");
    if (f)
    {
        FIO_WriteFile(f, buf, strlen(buf));
        FIO_CloseFile(f);
        snprintf(ab_status, SCREEN_MSG_LEN, "TEST DONE - saved to ML/LOGS/MLVBENCH.TXT");
    }
    else
    {
        snprintf(ab_status, SCREEN_MSG_LEN, "TEST DONE (log write failed)");
    }
}

static void ab_window_done(perf_acc_t *a, uint32_t dt, uint32_t gap_max)
{
    if (ab_phase < 0 || ab_phase >= AB_PHASES)
    {
        return;
    }
    if (ab_phases[ab_phase].bench == 3)
    {
        if (seq_state == 2)
        {
            snprintf(ab_result[ab_phase], SCREEN_MSG_LEN, "%s", seq_result);
            if (ab_phase + 1 < AB_PHASES)
            {
                ab_set_phase(ab_phase + 1);
            }
            else
            {
                ab_set_phase(AB_PHASES);
                ab_write_log();
            }
        }
        return;
    }
    if (gap_max > 500000)
    {
        /* clip restarted / file reopened in this window: ignore it, phase gets one more window */
        return;
    }
    if (ab_window > 0)
    {
        ab_sum.read_us += a->read_us;
        ab_sum.read_bytes += a->read_bytes;
        ab_sum.dec_us += a->dec_us;
        ab_sum.read_frames += a->read_frames;
        ab_sum.read_calls += a->read_calls;
        ab_sum.render_us += a->render_us;
        ab_sum.render_frames += a->render_frames;
        ab_sum_dt += dt;
    }
    ab_window++;
    if (ab_window < AB_WINDOWS)
    {
        return;
    }

    uint32_t rf = MAX(ab_sum.read_frames, 1);
    uint32_t nf = MAX(ab_sum.render_frames, 1);
    uint32_t fps10 = ab_sum_dt ? (uint32_t)((uint64_t)ab_sum.render_frames * 10000000ULL / ab_sum_dt) : 0;
    snprintf(ab_result[ab_phase], SCREEN_MSG_LEN, "%s %d.%dfps rd %dms %dK x%d dec %d rn %dms",
        ab_phases[ab_phase].name, fps10 / 10, fps10 % 10,
        ab_sum.read_us / rf / 1000, ab_sum.read_bytes / rf / 1024, ab_sum.read_calls / rf,
        ab_sum.dec_us / rf / 1000, ab_sum.render_us / nf / 1000);

    if (ab_phase + 1 < AB_PHASES)
    {
        ab_set_phase(ab_phase + 1);
    }
    else
    {
        ab_set_phase(AB_PHASES);
        ab_write_log();
    }
}

static void perf_window_tick()
{
    uint64_t now = get_us_clock();
    if (!perf_win_start)
    {
        perf_win_start = now;
        return;
    }
    uint32_t dt = (uint32_t)(now - perf_win_start);
    if (dt < 1000000)
    {
        return;
    }

    perf_acc_t a = perf_acc;
    memset(&perf_acc, 0, sizeof(perf_acc));
    perf_win_start = now;

    uint32_t rf = MAX(a.read_frames, 1);
    uint32_t nf = MAX(a.render_frames, 1);
    uint32_t fps10 = (uint32_t)((uint64_t)a.render_frames * 10000000ULL / dt);
    /* MB/s of the read itself (bytes / read time) */
    uint32_t mbs = a.read_us ? (uint32_t)((uint64_t)a.read_bytes / a.read_us) : 0;

    snprintf(perf_line, SCREEN_MSG_LEN, "%d.%dfps rd %dms %dK x%d %dMB/s dec %dms",
        fps10 / 10, fps10 % 10, a.read_us / rf / 1000, a.read_bytes / rf / 1024, a.read_calls / rf, mbs, a.dec_us / rf / 1000);
    snprintf(perf_line2, SCREEN_MSG_LEN, "rn %dms max %d %s%s %s b:%s prx:%s",
        a.render_us / nf / 1000, perf_render_max / 1000, sp_auto ? "auto-" : "",
        mlv_play_quality == MLV_PLAY_QUALITY_SUPER ? "super" : mlv_play_quality ? "fast" : "color",
        mlv_play_read_names[mlv_play_read_mode], mlv_play_bench_names[mlv_play_bench], prx_status);
    perf_render_max = 0;
    ab_window_done(&a, dt, perf_gap_max);
    perf_gap_max = 0;
    if (mlv_play_quality != MLV_PLAY_QUALITY_SUPER && !sp_auto)
    {
        perf_lut_us = 0;    /* stock: re-measure next frame */
    }
}

/* set up two queues - one with empty buffers and one with buffers to render */
static struct msg_queue *mlv_play_queue_empty;
static struct msg_queue *mlv_play_queue_render;
static struct msg_queue *mlv_play_queue_osd;
static struct msg_queue *mlv_play_queue_fps;

/* queue for playlist item submits by scanner tasks */
static struct msg_queue *mlv_playlist_queue;
static struct msg_queue *mlv_playlist_scan_queue;
static playlist_entry_t *mlv_playlist = NULL;
static uint32_t mlv_playlist_entries = 0;

static char mlv_play_next_filename[MAX_PATH];
static char mlv_play_current_filename[MAX_PATH];
static playlist_entry_t mlv_playlist_next(playlist_entry_t current);
static playlist_entry_t mlv_playlist_prev(playlist_entry_t current);
static void mlv_playlist_delete(playlist_entry_t current);
static void mlv_playlist_build(uint32_t priv);
static uint32_t mlv_play_should_stop();

/* microsecond durations for one frame */
static uint32_t mlv_play_frame_div_pos = 0;
static uint32_t mlv_play_frame_dividers[3];


static void mlv_play_flush_queue(struct msg_queue *queue)
{
    uint32_t messages = 0;

    msg_queue_count(queue, &messages);
    while(messages > 0)
    {
        uint32_t tmp_buf = 0;
        msg_queue_receive(queue, &tmp_buf, 0);
        msg_queue_count(queue, &messages);
    }
}

static void mlv_play_next()
{
    playlist_entry_t current;
    playlist_entry_t next;
    
    strncpy(current.fullPath, mlv_play_current_filename, sizeof(current.fullPath));
    
    next = mlv_playlist_next(current);
    
    if(strlen(next.fullPath))
    {
        strncpy(mlv_play_next_filename, next.fullPath, sizeof(mlv_play_next_filename));
        mlv_play_stopfile = 1;
        mlv_play_paused = 0;
    }
}

static void mlv_play_prev()
{
    playlist_entry_t current;
    playlist_entry_t next;
    
    strncpy(current.fullPath, mlv_play_current_filename, sizeof(current.fullPath));
    
    next = mlv_playlist_prev(current);
    
    if(strlen(next.fullPath))
    {
        strncpy(mlv_play_next_filename, next.fullPath, sizeof(mlv_play_next_filename));
        mlv_play_stopfile = 1;
        mlv_play_paused = 0;
    }
}

static void mlv_play_progressbar(int pct, char *msg)
{
    static int last_pct = -1;
    int border = 4;
    int height = 40;
    int width = 720 - 100;

    int x = (720 - width) / 2;
    int y = (480 - height) / 2;

    if(pct == 0)
    {
        bmp_fill(COLOR_BLACK, x, y - font_med.height - border, width, font_med.height);
        bmp_fill(COLOR_WHITE, x, y, width, height);
        bmp_fill(COLOR_BLACK, x + border - 1, y + border - 1, width - 2 * (border - 1), height - 2 * (border - 1));
        last_pct = -1;
    }
    
    if(last_pct != pct)
    {
        bmp_fill(COLOR_BLUE, x + border, y + border, ((width - 2 * border) * pct) / 100, height - 2 * border);
        bmp_printf(FONT_MED, x, y - font_med.height - border, msg);
        last_pct = pct;
    }
}

/* call with positive duration and some string => print it for a while, then clear it */
/* call with 0 duration and some string => print it without clearing */
/* call with 0 duration and null string => just clear it */
static void mlv_play_show_dlg(uint32_t duration, char *string)
{
    uint32_t width = 500;
    uint32_t height = 128 + 20;
    uint32_t pos_y = (480 - height) / 2;
    uint32_t pos_x = (720 - width) / 2;
    struct bmp_file_t *icon = NULL;
    
    if (string)
    {
        icon = bmp_load_ram((uint8_t *)LDVAR(video_bmp), LDLEN(video_bmp), 0);
        
        bmp_fill(COLOR_BG, pos_x, pos_y, width, height);
        
        /* redraw 4 times in case it gets overdrawn */
        for(int loop = 0; loop < 4; loop++)
        {
            if(icon)
            {
                bmp_draw_scaled_ex(icon, pos_x + 10, pos_y + 10, 128, 128, 0);
            }
            else
            {
                bmp_printf(FONT_CANON, pos_x + 10, pos_y + 10 - font_med.height / 2, "[X]");
            }
            
            bmp_printf(FONT_CANON, pos_x + 128 + 20, pos_y + (128 / 2) - 16, string);
            msleep(duration / 10);
        }
        
        free(icon);
    }
    
    if (duration || !string)
    {
        bmp_fill(COLOR_EMPTY, pos_x, pos_y, width, height);
    }
}

/* run deletion in the same task as playback, to sync with file closing etc */
static int mlv_play_delete_requested = 0;

static void mlv_play_delete_request()
{
    mlv_play_delete_requested = 1;
    mlv_play_show_dlg(0, "Stopping...");
}

/* returns 1 on success, 0 on failure */
static int mlv_play_delete_work(char *filename)
{
    uint32_t size = 0;
    uint32_t state = 0;
    uint32_t seq_number = 0;
    char current_ext[4];
    char current_file[128];
    
    /* original extension, to be used as read-only */
    const char* main_ext = filename + strlen(filename) - 3;
    
    trace_write(mlv_play_trace_ctx, "[Delete] Delete request for: '%s'", filename);
    
    strncpy(current_file, filename, sizeof(current_file));
    
    /* file does not exist. the specified base file must exist, so abort */
    if(FIO_GetFileSize(filename, &size))
    {
        trace_write(mlv_play_trace_ctx, "  Does not exist!");
        return 0;
    }
    
    int is_mlv = streq(main_ext, "MLV") || streq(main_ext, "mlv");
    
    while (1)
    {
        switch(state)
        {
            case 0:
                /* try to delete main file */
                strcpy(current_ext, main_ext);
                break;

            case 1:
                /* try to delete index file */
                if (is_mlv)
                {
                    strcpy(current_ext, "IDX");
                }
                else
                {
                    /* index files are only used on MLVs, skip for other files */
                    state++;
                    continue;
                }
                break;

            case 2:
                /* try to delete gallery thumbnail cache */
                strcpy(current_ext, "THM");
                break;

            case 3:
                /* try to delete unified .PRX sidecar (holds THM + IDX + Proxy) */
                strcpy(current_ext, "PRX");
                break;
                
            case 4:
                /* try to delete chunk files */
                snprintf(current_ext, 4, "X%02d", seq_number);
                current_ext[0] = main_ext[0];
                break;
                
            case 5:
                /* next chunk file */
                if(seq_number >= 99)
                {
                    /* that would be the hundreth chunk, so lets say that was it */
                    state++;
                }
                else
                {
                    /* increase chunk counter and delete next file */
                    seq_number++;
                    state--;
                }
                continue;
                
            case 6:
                /* we are done */
                return 1;
        }

        for(int drive = 0; drive < 2; drive++)
        {
            /* set extension */
            strcpy(&current_file[strlen(current_file) - 3], current_ext);
            
            /* set drive letter */
            current_file[0] = drive ? 'A' : 'B';
            
            if (!is_mlv)
            {
                if (current_file[0] != filename[0])
                {
                    /* non-MLV files can't be spanned, do not delete the file with the same name from the other card */
                    trace_write(mlv_play_trace_ctx, "[Delete] Skip drive: '%s'", current_file);
                    continue;
                }
            }
            
            trace_write(mlv_play_trace_ctx, "[Delete] Next file: '%s'", current_file);
            
            /* check if file exists */
            if(FIO_GetFileSize(current_file, &size))
            {
                if (state == 0 && current_file[0] == filename[0])
                {
                    trace_write(mlv_play_trace_ctx, "[Delete] main file does not exist '%s'", current_file);
                    return 0;
                }
                continue;
            }

            trace_write(mlv_play_trace_ctx, "  existing, deleting");
            
            /* if existing, try to delete and set fail flag if that doesnt work */
            if(FIO_RemoveFile(current_file))
            {
                trace_write(mlv_play_trace_ctx, "  FAILED");
                return 0;
            }
            else
            {
                trace_write(mlv_play_trace_ctx, "  deleted");
            }
        }
        
        /* if there was no failure in deleting the file, next state */
        state++;
    }
    
    return 1;
}

static int mlv_play_delete()
{
    playlist_entry_t current;
    playlist_entry_t next;
    playlist_entry_t prev;
    
    /* save the currently played file */
    strncpy(current.fullPath, mlv_play_current_filename, sizeof(current.fullPath));
    
    /* check next/prev */
    next = mlv_playlist_next(current);
    prev = mlv_playlist_prev(current);
    
    /* remove the currently played from the playlist */
    mlv_playlist_delete(current);

    /* delete the files */
    int ok = mlv_play_delete_work(current.fullPath);
    trace_write(mlv_play_trace_ctx, "[Delete] returned %s", ok ? "success" : "FAILURE");

    /* check which of the files is available and play it */
    if(strlen(next.fullPath))
    {
        strncpy(mlv_play_next_filename, next.fullPath, sizeof(mlv_play_next_filename));
    }
    else if(strlen(prev.fullPath))
    {
        strncpy(mlv_play_next_filename, prev.fullPath, sizeof(mlv_play_next_filename));
    }
    else
    {
        /* if none is available, just stop playback */
        mlv_play_render_abort = 1;
    }
    
    return ok;
}

static void mlv_play_delete_if_requested()
{
    if (mlv_play_delete_requested)
    {
        mlv_play_show_dlg(0, "Deleting...");
        int ok = mlv_play_delete();
        mlv_play_show_dlg(3000, ok ? "Deleted." : "Delete failed.");
        mlv_play_delete_requested = 0;
    }
}

static void mlv_play_osd_quality(char *msg, uint32_t msg_len, uint32_t selected)
{
    if(selected)
    {
        mlv_play_quality = MOD(mlv_play_quality + 1, MLV_PLAY_QUALITY_COUNT);
    }
    
    if(msg)
    {
        static const char *names[] = { "color", "fast", "super" };
        snprintf(msg, msg_len, "%s", names[MOD(mlv_play_quality, MLV_PLAY_QUALITY_COUNT)]);
    }
}

static void mlv_play_osd_lut(char *msg, uint32_t msg_len, uint32_t selected)
{
    if (selected)
    {
        mlv_play_lut_cycle(0);
    }

    if (msg)
    {
        snprintf(msg, msg_len, "%s", sp_lut_name(mlv_play_lut));
    }
}

static void mlv_play_osd_audio(char *msg, uint32_t msg_len, uint32_t selected)
{
    if (selected)
    {
        mlv_play_audio = MOD(mlv_play_audio + 1, 8); /* 0=Mute, 1..5=Vol(2..10), 6=+6dB, 7=+12dB */
        if (mlv_play_audio == 0)
        {
            aud_stop();
        }
        else
        {
            aud_set_volume(mlv_play_audio);
        }
    }

    if (msg)
    {
        if (mlv_play_audio == 0)
        {
            snprintf(msg, msg_len, "Mute");
        }
        else if (mlv_play_audio == 6)
        {
            snprintf(msg, msg_len, "Vol:5+6dB");
        }
        else if (mlv_play_audio == 7)
        {
            snprintf(msg, msg_len, "Vol:5+12dB");
        }
        else
        {
            snprintf(msg, msg_len, "Vol:%d", mlv_play_audio);
        }
    }
}

static void mlv_play_osd_readmode(char *msg, uint32_t msg_len, uint32_t selected)
{
    if(selected)
    {
        mlv_play_read_mode = MOD(mlv_play_read_mode + 1, 4);
    }
    if(msg)
    {
        snprintf(msg, msg_len, "rd:%s", mlv_play_read_names[mlv_play_read_mode]);
    }
}

static void mlv_play_osd_bench(char *msg, uint32_t msg_len, uint32_t selected)
{
    if(selected)
    {
        mlv_play_bench = MOD(mlv_play_bench + 1, 4);
    }
    if(msg)
    {
        snprintf(msg, msg_len, "bench:%s", mlv_play_bench_names[mlv_play_bench]);
    }
}

static void mlv_play_osd_exact(char *msg, uint32_t msg_len, uint32_t selected)
{
    if(selected)
    {
        mlv_play_exact_fps = MOD(mlv_play_exact_fps + 1, 2);
    }
    
    if(msg)
    {
        /* any idea for a meaningful text here? feature description in long: 
                if enabled: 
                  play at exact fps with dropping if necessary
                  grayscale might keep up with video rate, but color requires dropping for sure
                if disabled:
                  play frame by frame as fast as possible
       */
        snprintf(msg, msg_len, mlv_play_exact_fps?"exact":"all");
    }
}


static void mlv_play_osd_prev(char *msg, uint32_t msg_len, uint32_t selected)
{
    if(selected)
    {
        mlv_play_prev();
    }
    
    if(msg)
    {
        snprintf(msg, msg_len, "<<");
    }
}

static void mlv_play_osd_next(char *msg, uint32_t msg_len, uint32_t selected)
{
    if(selected)
    {
        mlv_play_next();
    }
    
    if(msg)
    {
        snprintf(msg, msg_len, ">>");
    }
}

static void mlv_play_osd_pause(char *msg, uint32_t msg_len, uint32_t selected)
{
    if(selected)
    {
        mlv_play_paused = MOD(mlv_play_paused + 1, 2);
        if (!mlv_play_paused)
        {
            mlv_play_flush_queue(mlv_play_queue_fps);
        }
    }
    
    if(msg)
    {
        snprintf(msg, msg_len, mlv_play_paused?"|>":"||");
    }
}

static void mlv_play_osd_grid(char *msg, uint32_t msg_len, uint32_t selected)
{
    if (selected)
    {
        mlv_play_osd_state = MLV_PLAY_MENU_HIDDEN;
        mlv_play_return_to_gallery = 1;
        mlv_play_stopfile = 1;
    }

    if (msg)
    {
        snprintf(msg, msg_len, "Grid");
    }
}

static void mlv_play_osd_quit(char *msg, uint32_t msg_len, uint32_t selected)
{
    if(selected)
    {
        mlv_play_render_abort = 1;
    }
    
    if(msg)
    {
        snprintf(msg, msg_len, "Exit");
    }
}

/* --- safe pixel-drawn UI helpers (bmp_fill only, zero extra symbols) --- */
static void ui_draw_rect(int color, int x, int y, int w, int h, int t)
{
    if (w <= 0 || h <= 0 || t <= 0) return;
    bmp_fill(color, x, y, w, t);
    bmp_fill(color, x, y + h - t, w, t);
    bmp_fill(color, x, y, t, h);
    bmp_fill(color, x + w - t, y, t, h);
}

static void ui_icon_film(int x, int y, int color)
{
    ui_draw_rect(color, x, y, 18, 14, 2);
    bmp_fill(color, x + 5, y + 2, 2, 10);
    bmp_fill(color, x + 11, y + 2, 2, 10);
    bmp_fill(color, x + 2, y + 5, 3, 2);
    bmp_fill(color, x + 2, y + 9, 3, 2);
    bmp_fill(color, x + 13, y + 5, 3, 2);
    bmp_fill(color, x + 13, y + 9, 3, 2);
}

static void ui_icon_play(int x, int y, int color)
{
    /* 10x12 right-pointing triangle */
    for (int i = 0; i < 6; i++)
    {
        bmp_fill(color, x + i * 2, y + i, 2, 12 - i * 2);
    }
}

static void ui_icon_pause(int x, int y, int color)
{
    bmp_fill(color, x + 1, y, 4, 12);
    bmp_fill(color, x + 7, y, 4, 12);
}

static void ui_icon_prev(int x, int y, int color)
{
    bmp_fill(color, x, y, 2, 12);
    for (int i = 0; i < 5; i++)
    {
        bmp_fill(color, x + 10 - i * 2, y + i + 1, 2, 10 - i * 2);
    }
}

static void ui_icon_next(int x, int y, int color)
{
    for (int i = 0; i < 5; i++)
    {
        bmp_fill(color, x + i * 2, y + i + 1, 2, 10 - i * 2);
    }
    bmp_fill(color, x + 10, y, 2, 12);
}

static void ui_icon_grid(int x, int y, int color)
{
    bmp_fill(color, x,     y,     5, 5);
    bmp_fill(color, x + 7, y,     5, 5);
    bmp_fill(color, x,     y + 7, 5, 5);
    bmp_fill(color, x + 7, y + 7, 5, 5);
}

static void ui_icon_trash(int x, int y, int color)
{
    bmp_fill(color, x + 3, y,     6, 2);
    bmp_fill(color, x,     y + 2, 12, 2);
    ui_draw_rect(color, x + 1, y + 4, 10, 9, 2);
    bmp_fill(color, x + 5, y + 6, 2, 5);
}

static void ui_icon_exit(int x, int y, int color)
{
    for (int i = 0; i < 10; i++)
    {
        bmp_fill(color, x + i,     y + i + 1, 2, 2);
        bmp_fill(color, x + 9 - i, y + i + 1, 2, 2);
    }
}

static void ui_icon_info(int x, int y, int color)
{
    ui_draw_rect(color, x, y, 12, 12, 1);
    bmp_fill(color, x + 5, y + 2, 2, 2);
    bmp_fill(color, x + 5, y + 5, 2, 5);
}

static uint32_t mlv_play_osd_delete_selected = 0;

static void mlv_play_osd_delete(char *msg, uint32_t msg_len, uint32_t selected)
{
    uint32_t max_time = 5000;

    if(selected)
    {
        if(!mlv_play_osd_delete_selected || selected == 2)
        {
            mlv_play_osd_force_redraw = 1;
            mlv_play_osd_delete_selected = get_ms_clock();
        }
        else
        {
            mlv_play_osd_force_redraw = 0;
            mlv_play_osd_delete_selected = 0;
            mlv_play_delete_request();
        }
    }
    
    if(msg)
    {
        uint32_t time_passed = get_ms_clock() - mlv_play_osd_delete_selected;
        uint32_t seconds = (max_time - time_passed) / 1000;
        
        if(mlv_play_osd_delete_selected && seconds > 0)
        {
            snprintf(msg, msg_len, "[delete? %ds]", seconds);
        }
        else
        {
            mlv_play_osd_delete_selected = 0;
            mlv_play_osd_force_redraw = 0;
            snprintf(msg, msg_len, "Del");
        }
    }
}

static void(*mlv_play_osd_items[])(char *, uint32_t,  uint32_t) = {
    &mlv_play_osd_grid,
    &mlv_play_osd_prev,
    &mlv_play_osd_pause,
    &mlv_play_osd_next,
    &mlv_play_osd_quality,
    &mlv_play_osd_lut,
    &mlv_play_osd_audio,
    &mlv_play_osd_exact,
    &mlv_play_osd_delete,
    &mlv_play_osd_quit
};

static uint32_t mlv_play_osd_handle(uint32_t msg)
{
    switch(msg)
    {
        case MODULE_KEY_PRESS_SET:
        case MODULE_KEY_JOY_CENTER:
        {
            /* execute menu item */
            mlv_play_osd_items[mlv_play_osd_item](NULL, 0, 1);
            break;
        }
        
        case MODULE_KEY_WHEEL_UP:
        //case MODULE_KEY_WHEEL_LEFT:
        case MODULE_KEY_PRESS_LEFT:
        {
            if(mlv_play_osd_item > 0)
            {
                mlv_play_osd_item--;
            }
            break;
        }
        
        case MODULE_KEY_WHEEL_DOWN:
        //case MODULE_KEY_WHEEL_RIGHT:
        case MODULE_KEY_PRESS_RIGHT:
        {
            if(mlv_play_osd_item < COUNT(mlv_play_osd_items) - 1)
            {
                mlv_play_osd_item++;
            }
            break;
        }
    }
    
    return 0;
}

static uint32_t mlv_play_osd_draw()
{
    uint32_t redraw = 0;
    int border = 6;
    int bar_h = font_med.height + 14;
    int y_top = os.y_max - bar_h - 18;

    /* clear previous bar area */
    static int prev_x = 0, prev_y = 0, prev_w = 0, prev_h = 0;
    if (prev_w > 0 && prev_h > 0)
    {
        bmp_fill(COLOR_EMPTY, prev_x, prev_y, prev_w, prev_h);
        prev_w = 0;
    }

    /* handle animation */
    switch(mlv_play_osd_state)
    {
        case MLV_PLAY_MENU_SHOWN:
            redraw = 0;
            break;

        case MLV_PLAY_MENU_HIDDEN:
        case MLV_PLAY_MENU_IDLE:
            mlv_play_osd_y = os.y_max + 2;
            return 0;

        case MLV_PLAY_MENU_FADEIN:
            mlv_play_osd_y = MAX(mlv_play_osd_y - 8, y_top);
            if (mlv_play_osd_y <= y_top)
            {
                mlv_play_osd_state = MLV_PLAY_MENU_SHOWN;
            }
            redraw = 1;
            break;

        case MLV_PLAY_MENU_FADEOUT:
            mlv_play_osd_y = MIN(mlv_play_osd_y + 8, os.y_max + 2);
            if (mlv_play_osd_y >= os.y_max + 2)
            {
                mlv_play_osd_state = MLV_PLAY_MENU_HIDDEN;
                return 0;
            }
            redraw = 1;
            break;
    }

    int count = (int)COUNT(mlv_play_osd_items);
    char labels[12][32];
    int item_w[12];
    int has_icon[12];
    int total_w = border * 2;
    int gap = 4;

    for (int i = 0; i < count; i++)
    {
        mlv_play_osd_items[i](labels[i], sizeof(labels[i]), 0);
        has_icon[i] = (mlv_play_osd_items[i] == &mlv_play_osd_grid  ||
                       mlv_play_osd_items[i] == &mlv_play_osd_prev  ||
                       mlv_play_osd_items[i] == &mlv_play_osd_pause ||
                       mlv_play_osd_items[i] == &mlv_play_osd_next  ||
                       mlv_play_osd_items[i] == &mlv_play_osd_delete ||
                       mlv_play_osd_items[i] == &mlv_play_osd_quit);
        int is_icon_only = (mlv_play_osd_items[i] == &mlv_play_osd_prev  ||
                            mlv_play_osd_items[i] == &mlv_play_osd_pause ||
                            mlv_play_osd_items[i] == &mlv_play_osd_next);
        int tw = is_icon_only ? 0 : (int)bmp_string_width(FONT_MED, labels[i]);
        item_w[i] = 14 + (has_icon[i] ? 16 : 0) + (tw > 0 && has_icon[i] ? 4 : 0) + tw;
        total_w += item_w[i] + (i ? gap : 0);
    }

    int x0 = mlv_play_osd_x - total_w / 2;
    if (x0 < 4) x0 = 4;
    if (x0 + total_w > 716) total_w = 716 - x0;
    int y0 = mlv_play_osd_y;

    bmp_fill(COLOR_ALMOST_BLACK, x0, y0, total_w, bar_h);
    ui_draw_rect(COLOR_GRAY(40), x0, y0, total_w, bar_h, 1);

    int cx = x0 + border;
    int iy = y0 + (bar_h - 12) / 2;
    int ty = y0 + (bar_h - (int)font_med.height) / 2;

    for (int i = 0; i < count; i++)
    {
        int sel = ((uint32_t)i == mlv_play_osd_item);
        int is_del_confirm = (mlv_play_osd_items[i] == &mlv_play_osd_delete && mlv_play_osd_delete_selected);
        int bg = is_del_confirm ? COLOR_RED : (sel ? COLOR_ORANGE : COLOR_ALMOST_BLACK);
        int fg = sel ? COLOR_BLACK : COLOR_WHITE;
        if (is_del_confirm) fg = COLOR_WHITE;

        bmp_fill(bg, cx, y0 + 3, item_w[i], bar_h - 6);
        int px = cx + 7;
        if (has_icon[i])
        {
            if (mlv_play_osd_items[i] == &mlv_play_osd_grid)        ui_icon_grid(px, iy, fg);
            else if (mlv_play_osd_items[i] == &mlv_play_osd_prev)   ui_icon_prev(px, iy, fg);
            else if (mlv_play_osd_items[i] == &mlv_play_osd_pause)  { if (mlv_play_paused) ui_icon_play(px, iy, fg); else ui_icon_pause(px, iy, fg); }
            else if (mlv_play_osd_items[i] == &mlv_play_osd_next)   ui_icon_next(px, iy, fg);
            else if (mlv_play_osd_items[i] == &mlv_play_osd_delete) ui_icon_trash(px, iy, fg);
            else if (mlv_play_osd_items[i] == &mlv_play_osd_quit)   ui_icon_exit(px, iy, fg);
            px += 18;
        }
        int is_icon_only = (mlv_play_osd_items[i] == &mlv_play_osd_prev  ||
                            mlv_play_osd_items[i] == &mlv_play_osd_pause ||
                            mlv_play_osd_items[i] == &mlv_play_osd_next);
        if (!is_icon_only)
        {
            bmp_printf(FONT(FONT_MED, fg, bg), px, ty, "%s", labels[i]);
        }
        cx += item_w[i] + gap;
    }

    prev_x = x0;
    prev_y = y0;
    prev_w = total_w;
    prev_h = bar_h;
    return redraw;
}

static void mlv_play_osd_act(void *handler)
{
    int entry = 0;
    
    for(entry = 0; entry < COUNT(mlv_play_osd_items); entry++)
    {
        if(mlv_play_osd_items[entry] == handler)
        {
            mlv_play_osd_item = entry;
            mlv_play_osd_state = MLV_PLAY_MENU_FADEIN;
            return;
        }
    }
}

static void mlv_play_osd_step(uint32_t *next_render_time, uint32_t *last_keypress_time, uint32_t timeout)
{
    uint32_t key;

    if(!msg_queue_receive(mlv_play_queue_osd, &key, timeout))
    {
        /* there was a keypress */
        *last_keypress_time = get_ms_clock();
        
        /* no matter which state - these are handled */
        switch(key)
        {
            case MODULE_KEY_WHEEL_LEFT:
                mlv_play_prev();
                if(mlv_play_osd_state != MLV_PLAY_MENU_SHOWN)
                {
                    mlv_play_osd_state = MLV_PLAY_MENU_FADEIN;
                }
                break;
                
            case MODULE_KEY_WHEEL_RIGHT:
                mlv_play_next();
                if(mlv_play_osd_state != MLV_PLAY_MENU_SHOWN)
                {
                    mlv_play_osd_state = MLV_PLAY_MENU_FADEIN;
                }
                break;
                
#if MLV_PLAY_PERF
            /* crop-mood perf tests, kept off the OSD line (it has no room) */
            case MODULE_KEY_RATE:
                mlv_play_osd_readmode(NULL, 0, 1);
                break;

            case MODULE_KEY_MENU:
                mlv_play_osd_bench(NULL, 0, 1);
                break;
#else
            case MODULE_KEY_RATE:
                mlv_play_osd_act(&mlv_play_osd_lut);
                mlv_play_osd_lut(NULL, 0, 1);
                break;

            case MODULE_KEY_MENU:
                mlv_play_osd_state = MLV_PLAY_MENU_HIDDEN;
                mlv_play_return_to_gallery = 1;
                mlv_play_stopfile = 1;
                break;
#endif

            case MODULE_KEY_TRASH:
                mlv_play_osd_act(&mlv_play_osd_delete);
                mlv_play_osd_delete(NULL, 0, 2);
                break;

            case MODULE_KEY_PLAY:
                mlv_play_osd_act(&mlv_play_osd_pause);
                mlv_play_osd_pause(NULL, 0, 1);
                break;

            case MODULE_KEY_PRESS_ZOOMIN:
                mlv_play_osd_state = MLV_PLAY_MENU_HIDDEN;
                mlv_play_zoom = (mlv_play_zoom + 1) % 4;
                raw_twk_set_zoom(mlv_play_zoom, mlv_play_zoom_x_pct, mlv_play_zoom_y_pct);
                break;
                
            case MODULE_KEY_PRESS_UP:
                mlv_play_zoom_y_pct = MAX(0, mlv_play_zoom_y_pct - 10);
                raw_twk_set_zoom(mlv_play_zoom, mlv_play_zoom_x_pct, mlv_play_zoom_y_pct);
                break;

            case MODULE_KEY_PRESS_DOWN:
                mlv_play_zoom_y_pct = MIN(100, mlv_play_zoom_y_pct + 10);
                raw_twk_set_zoom(mlv_play_zoom, mlv_play_zoom_x_pct, mlv_play_zoom_y_pct);
                break;

            case MODULE_KEY_PRESS_LEFT:
                mlv_play_zoom_x_pct = MAX(0, mlv_play_zoom_x_pct - 10);
                raw_twk_set_zoom(mlv_play_zoom, mlv_play_zoom_x_pct, mlv_play_zoom_y_pct);
                break;

            case MODULE_KEY_PRESS_RIGHT:
                mlv_play_zoom_x_pct = MIN(100, mlv_play_zoom_x_pct + 10);
                raw_twk_set_zoom(mlv_play_zoom, mlv_play_zoom_x_pct, mlv_play_zoom_y_pct);
                break;
        }
        
        switch(mlv_play_osd_state)
        {
            case MLV_PLAY_MENU_SHOWN:
            case MLV_PLAY_MENU_FADEIN:
            {
                if(key == MODULE_KEY_Q || key == MODULE_KEY_PICSTYLE)
                {
                    if (!mlv_play_osd_force_redraw)
                    {
                        mlv_play_osd_state = MLV_PLAY_MENU_FADEOUT;
                    }
                }
                else
                {
                    mlv_play_osd_handle(key);
                }
                break;
            }

            /* when fading out, still handle and fade back in */
            case MLV_PLAY_MENU_FADEOUT:
            {
                if(!mlv_play_zoom)
                {
                    mlv_play_osd_state = MLV_PLAY_MENU_FADEIN;
                    mlv_play_osd_handle(key);
                }
                break;
            }
            
            case MLV_PLAY_MENU_IDLE:
            case MLV_PLAY_MENU_HIDDEN:
            {
                if(!mlv_play_zoom)
                {
                    if (key == MODULE_KEY_PRESS_SET || key == MODULE_KEY_Q || key == MODULE_KEY_PICSTYLE)
                    {
                        mlv_play_osd_state = MLV_PLAY_MENU_FADEIN;
                    }
                    if (key == MODULE_KEY_INFO)
                    {
                        clrscr();
                        mlv_play_info = MOD(mlv_play_info + 1, 2) ? 2 : 0;
                    }
                }
                break;
            }
        }
    }

    if (mlv_play_render_abort || mlv_play_in_gallery)
    {
        return;
    }

    uint32_t idle_time = get_ms_clock() - *last_keypress_time;

    if(mlv_play_osd_draw())
    {
        *next_render_time = get_ms_clock() + mlv_play_render_timestep;
    }
    else
    {
        *next_render_time = get_ms_clock() + mlv_play_idle_timestep;
        
        /* when redrawing is forced, keep OSD shown */
        if(mlv_play_osd_force_redraw)
        {
            mlv_play_osd_state = MLV_PLAY_MENU_SHOWN;
        }
        else if(idle_time > mlv_play_osd_idle && mlv_play_osd_state == MLV_PLAY_MENU_SHOWN)
        {
            mlv_play_osd_state = MLV_PLAY_MENU_FADEOUT;
        }
    }
}

static uint32_t mlv_play_inline_osd_next = 0;
static uint32_t mlv_play_inline_osd_last = 0;

static void mlv_play_inline_osd_poll(uint32_t wait_ms)
{
    if (mlv_play_osd_alive) return;
    if (!mlv_play_inline_osd_last) mlv_play_inline_osd_last = get_ms_clock();
    mlv_play_osd_step(&mlv_play_inline_osd_next, &mlv_play_inline_osd_last, wait_ms);
    if (mlv_play_osd_state == MLV_PLAY_MENU_FADEIN || mlv_play_osd_state == MLV_PLAY_MENU_FADEOUT)
    {
        mlv_play_osd_draw();
    }
}

static void mlv_play_osd_task(void *priv)
{
    uint32_t next_render_time = get_ms_clock() + mlv_play_render_timestep;
    mlv_play_osd_alive = 1;
 
    mlv_play_osd_state = MLV_PLAY_MENU_IDLE;
    mlv_play_osd_item = 2; /* pause/play */
    mlv_play_paused = 0;   
    
    uint32_t last_keypress_time = get_ms_clock();
    TASK_LOOP
    {
        if (mlv_play_render_abort)
        {
            break;
        }
        if (mlv_play_in_gallery)
        {
            mlv_play_osd_state = MLV_PLAY_MENU_HIDDEN;
            mlv_play_osd_in_gallery_ack = 1;
            msleep(40);
            continue;
        }
        mlv_play_osd_in_gallery_ack = 0;

        int32_t dt = (int32_t)(next_render_time - get_ms_clock());
        uint32_t timeout = (uint32_t)COERCE(dt, 10, 100);
        mlv_play_osd_step(&next_render_time, &last_keypress_time, timeout);
    }
    mlv_play_osd_alive = 0;
}


static void mlv_play_xref_resize(frame_xref_t **table, uint32_t entries, uint32_t *allocated)
{
    /* make sure there is no crappy pointer before using */
    if(*allocated == 0)
    {
        *table = NULL;
    }
    
    /* only resize if the buffer is too small */
    if(entries * sizeof(frame_xref_t) > *allocated)
    {
        *allocated += (entries + 1) * sizeof(frame_xref_t);
        *table = realloc(*table, *allocated);
    }
}

static void mlv_play_xref_sort(frame_xref_t *table, uint32_t entries)
{
    if (!entries) return;
    
    uint32_t n = entries;
    do
    {
        uint32_t newn = 1;
        for (uint32_t i = 0; i < n-1; ++i)
        {
            if (table[i].frameTime > table[i+1].frameTime)
            {
                frame_xref_t tmp = table[i+1];
                table[i+1] = table[i];
                table[i] = tmp;
                newn = i + 1;
            }
        }
        n = newn;
    } while (n > 1);
}

#define GAL_COLS       3
#define GAL_ROWS       2
#define GAL_PER_PAGE   (GAL_COLS * GAL_ROWS)
#define GAL_CELL_W     220
#define GAL_CELL_H     160
#define GAL_GAP_X      14
#define GAL_GAP_Y      14
#define GAL_MARGIN_X   16
#define GAL_TOP_Y      48
#define GAL_TW         212   /* even, inside 4px cell border */
#define GAL_TH         132
#define GAL_THM_PIXELS (GAL_TW * GAL_TH * 2)

/* Unified single-sidecar .PRX layout (holds Thumbnail + XREF Index + Proxy frames):
 *   [0               .. 56480)   : Thumbnail slot (512-byte mlv_thm_hdr_t + 55968-byte UYVY)
 *   [56480           .. 154784)  : XREF index slot (up to 98304 bytes = ~8190 frames)
 *   [154784          .. 155296)  : Proxy header (512-byte prx_hdr_t)
 *   [155296          .. EOF)     : Proxy video frames (frame_count * frame_bytes)
 */
#define PRX_THM_OFFSET    0
#define PRX_THM_SLOT_SIZE (512 + GAL_THM_PIXELS)          /* 56,480 bytes */
#define PRX_IDX_OFFSET    PRX_THM_SLOT_SIZE               /* 56,480 */
#define PRX_IDX_SLOT_SIZE 98304                           /* 98,304 bytes (~8190 xref entries) */
#define PRX_HDR_OFFSET    (PRX_IDX_OFFSET + PRX_IDX_SLOT_SIZE) /* 154,784 */
#define PRX_HDR_SIZE      512
#define PRX_FRAMES_OFFSET (PRX_HDR_OFFSET + PRX_HDR_SIZE) /* 155,296 */

/* Open existing .PRX for in-place update or create & pre-pad up to min_size so seeks never fail on FAT32 */
static FILE *prx_open_or_create_slot(const char *prx_filename, uint32_t min_size)
{
    uint32_t cur_size = 0;
    int exists = (FIO_GetFileSize(prx_filename, &cur_size) == 0);
    FILE *f = NULL;

    if (exists)
    {
        f = FIO_OpenFile(prx_filename, O_RDWR | O_SYNC);
    }
    if (!f)
    {
        f = FIO_CreateFile(prx_filename);
        cur_size = 0;
    }
    if (!f)
    {
        return NULL;
    }

    if (cur_size < min_size)
    {
        uint32_t pad_chunk = 8192;
        uint8_t *zbuf = fio_malloc(pad_chunk);
        if (zbuf)
        {
            memset(zbuf, 0, pad_chunk);
            FIO_SeekSkipFile(f, cur_size, SEEK_SET);
            uint32_t rem = min_size - cur_size;
            while (rem > 0)
            {
                uint32_t n = MIN(rem, pad_chunk);
                if (FIO_WriteFile(f, zbuf, n) != (int32_t)n)
                {
                    break;
                }
                rem -= n;
            }
            fio_free(zbuf);
        }
    }
    return f;
}

static mlv_xref_hdr_t *mlv_play_load_index(char *base_filename)
{
    mlv_xref_hdr_t *block_hdr = NULL;
    char filename[128];
    FILE *in_file = NULL;

    strncpy(filename, base_filename, sizeof(filename));
    filename[sizeof(filename) - 1] = '\0';
    strcpy(&filename[strlen(filename) - 3], "PRX");

    uint32_t fsize = 0;
    if (FIO_GetFileSize(filename, &fsize) == 0 && fsize >= PRX_IDX_OFFSET + sizeof(mlv_file_hdr_t) + sizeof(mlv_xref_hdr_t))
    {
        in_file = FIO_OpenFile(filename, O_RDONLY | O_SYNC);
        if (in_file)
        {
            FIO_SeekSkipFile(in_file, PRX_IDX_OFFSET, SEEK_SET);
            mlv_file_hdr_t fhdr;
            mlv_xref_hdr_t xhdr;
            if (mlv_play_read_small(in_file, &fhdr, sizeof(fhdr)) == (int32_t)sizeof(fhdr) &&
                !memcmp(fhdr.fileMagic, "MLVI", 4) &&
                mlv_play_read_small(in_file, &xhdr, sizeof(xhdr)) == (int32_t)sizeof(xhdr) &&
                !memcmp(xhdr.blockType, "XREF", 4) &&
                xhdr.blockSize >= sizeof(mlv_xref_hdr_t) &&
                xhdr.blockSize <= PRX_IDX_SLOT_SIZE - sizeof(mlv_file_hdr_t))
            {
                block_hdr = malloc(xhdr.blockSize);
                if (block_hdr)
                {
                    memcpy(block_hdr, &xhdr, sizeof(xhdr));
                    uint32_t payload = xhdr.blockSize - sizeof(mlv_xref_hdr_t);
                    if (mlv_play_read_small(in_file, (uint8_t *)block_hdr + sizeof(mlv_xref_hdr_t), payload) != (int32_t)payload)
                    {
                        free(block_hdr);
                        block_hdr = NULL;
                    }
                }
            }
            FIO_CloseFile(in_file);
            if (block_hdr)
            {
                return block_hdr;
            }
        }
    }

    /* Fallback for legacy standalone .IDX files if present */
    strcpy(&filename[strlen(filename) - 3], "IDX");
    in_file = FIO_OpenFile(filename, O_RDONLY | O_SYNC);
    if (!in_file)
    {
        return NULL;
    }

    TASK_LOOP
    {
        mlv_hdr_t buf;
        int64_t position = FIO_SeekSkipFile(in_file, 0, SEEK_CUR);
        if (mlv_play_read_small(in_file, &buf, sizeof(mlv_hdr_t)) != sizeof(mlv_hdr_t))
        {
            break;
        }
        FIO_SeekSkipFile(in_file, position, SEEK_SET);
        if (!memcmp(buf.blockType, "XREF", 4))
        {
            block_hdr = malloc(buf.blockSize);
            if (block_hdr && mlv_play_read_small(in_file, block_hdr, buf.blockSize) != (int32_t)buf.blockSize)
            {
                free(block_hdr);
                block_hdr = NULL;
            }
        }
        else
        {
            FIO_SeekSkipFile(in_file, position + buf.blockSize, SEEK_SET);
        }
        if (position == FIO_SeekSkipFile(in_file, 0, SEEK_CUR))
        {
            break;
        }
    }
    FIO_CloseFile(in_file);
    return block_hdr;
}

static void mlv_play_save_index(char *base_filename, mlv_file_hdr_t *ref_file_hdr, int fileCount, frame_xref_t *index, int entries)
{
    char filename[128];
    FILE *out_file = NULL;

    uint32_t total_idx_bytes = sizeof(mlv_file_hdr_t) + sizeof(mlv_xref_hdr_t) + (uint32_t)entries * sizeof(mlv_xref_t);
    if (total_idx_bytes > PRX_IDX_SLOT_SIZE)
    {
        return;
    }

    strncpy(filename, base_filename, sizeof(filename));
    filename[sizeof(filename) - 1] = '\0';
    strcpy(&filename[strlen(filename) - 3], "PRX");

    out_file = prx_open_or_create_slot(filename, PRX_IDX_OFFSET);
    if (!out_file)
    {
        return;
    }
    FIO_SeekSkipFile(out_file, PRX_IDX_OFFSET, SEEK_SET);

    /* first write MLVI header */
    mlv_file_hdr_t file_hdr = *ref_file_hdr;
    file_hdr.blockSize = sizeof(mlv_file_hdr_t);
    file_hdr.videoFrameCount = 0;
    file_hdr.audioFrameCount = 0;
    file_hdr.fileNum = fileCount + 1;

    FIO_WriteFile(out_file, &file_hdr, sizeof(mlv_file_hdr_t));

    /* now write XREF block */
    mlv_xref_hdr_t hdr;
    memset(&hdr, 0x00, sizeof(mlv_xref_hdr_t));
    memcpy(hdr.blockType, "XREF", 4);
    hdr.blockSize = sizeof(mlv_xref_hdr_t) + entries * sizeof(mlv_xref_t);
    hdr.entryCount = entries;

    if (FIO_WriteFile(out_file, &hdr, sizeof(mlv_xref_hdr_t)) != sizeof(mlv_xref_hdr_t))
    {
        FIO_CloseFile(out_file);
        return;
    }

    uint32_t last_pct = 0;
    mlv_play_progressbar(0, "");

    /* allocate at least 512 byte */
    uint32_t entry_count = (512 / sizeof(mlv_xref_t)) + 1;
    mlv_xref_t *buffer = malloc(sizeof(mlv_xref_t) * entry_count);
    if (!buffer)
    {
        FIO_CloseFile(out_file);
        return;
    }

    /* and then the single entries */
    for (int entry = 0; entry < entries; entry++)
    {
        uint32_t buffer_pos = entry % entry_count;
        mlv_xref_t *field = &buffer[buffer_pos];
        uint32_t pct = (entry * 100) / entries;

        if (last_pct != pct)
        {
            char msg[36];
            snprintf(msg, sizeof(msg), "Saving index (%d entries)...", entries);
            mlv_play_progressbar(pct, msg);
            last_pct = pct;
        }
        memset(field, 0x00, sizeof(mlv_xref_t));
        field->frameOffset = index[entry].frameOffset;
        field->fileNumber = index[entry].fileNumber;
        field->frameType = index[entry].frameType;

        if ((buffer_pos + 1) == entry_count || ((entry + 1) == entries))
        {
            int32_t write_size = (buffer_pos + 1) * sizeof(mlv_xref_t);
            if (FIO_WriteFile(out_file, buffer, write_size) != write_size)
            {
                free(buffer);
                FIO_CloseFile(out_file);
                return;
            }
        }
    }

    free(buffer);
    FIO_CloseFile(out_file);
}

static void mlv_play_build_index(char *filename, FILE **chunk_files, uint32_t chunk_count)
{
    frame_xref_t *frame_xref_table = NULL;
    uint32_t frame_xref_entries = 0;
    uint32_t frame_xref_allocated = 0;
    mlv_file_hdr_t main_header;
    
    for(uint32_t chunk = 0; chunk < chunk_count; chunk++)
    {
        uint32_t last_pct = 0;
        int64_t size = 0;
        int64_t position = 0;
        
        size = FIO_SeekSkipFile(chunk_files[chunk], 0, SEEK_END);
        FIO_SeekSkipFile(chunk_files[chunk], 0, SEEK_SET);
        
        mlv_play_progressbar(0, "");
        
        while(1)
        {
            if(ml_shutdown_requested)
            {
                break;
            }
            
            mlv_hdr_t buf;
            uint64_t timestamp = 0;
            
            uint32_t pct = ((position / 10) / (size / 1000));
            
            if(last_pct != pct)
            {
                char msg[100];
                
                snprintf(msg, sizeof(msg), "Building index... (%d/%d)", chunk + 1, chunk_count);
                mlv_play_progressbar(pct, msg);
                last_pct = pct;
            }
            
            int read = FIO_ReadFile(chunk_files[chunk], &buf, sizeof(mlv_hdr_t));
            
            if(read != sizeof(mlv_hdr_t))
            {
                if(read <= 0)
                {
                    break;
                }
                else
                {
                    bmp_printf(FONT_MED, 30, 190, "File #%d ends prematurely, %d bytes read", chunk, read);
                    beep();
                    msleep(2000);
                    return;
                }
            }
            
            /* unexpected block header size? */
            if(buf.blockSize < sizeof(mlv_hdr_t) || buf.blockSize > 50 * 1024 * 1024)
            {
                bmp_printf(FONT_MED, 30, 190, "Invalid header size: %d bytes at 0x%08X", buf.blockSize, position);
                beep();
                msleep(2000);
                return;
            }

            /* file header */
            if(!memcmp(buf.blockType, "MLVI", 4))
            {
                mlv_file_hdr_t file_hdr;
                uint32_t hdr_size = MIN(sizeof(mlv_file_hdr_t), buf.blockSize);
                
                FIO_SeekSkipFile(chunk_files[chunk], position, SEEK_SET);
                
                /* read the whole header block, but limit size to either our local type size or the written block size */
                if(FIO_ReadFile(chunk_files[chunk], &file_hdr, hdr_size) != (int32_t)hdr_size)
                {
                    bmp_printf(FONT_MED, 30, 190, "File ends prematurely during MLVI");
                    beep();
                    msleep(2000);
                    return;
                }

                /* is this the first file? */
                if(file_hdr.fileNum == 0)
                {
                    memcpy(&main_header, &file_hdr, sizeof(mlv_file_hdr_t));
                }
                else
                {
                    /* no, its another chunk */
                    if(main_header.fileGuid != file_hdr.fileGuid)
                    {
                        bmp_printf(FONT_MED, 30, 190, "Error: GUID within the file chunks mismatch!");
                        beep();
                        msleep(2000);
                        return;
                    }
                }
                
                /* emulate timestamp zero (will overwrite version string) */
                timestamp = 0;
            }
            else
            {
                /* all other blocks have a timestamp */
                timestamp = buf.timestamp;
            }
            
            /* dont index NULL blocks */
            if(memcmp(buf.blockType, "NULL", 4))
            {
                mlv_play_xref_resize(&frame_xref_table, frame_xref_entries + 1, &frame_xref_allocated);
                
                /* add xref data */
                frame_xref_table[frame_xref_entries].frameTime = timestamp;
                frame_xref_table[frame_xref_entries].frameOffset = position;
                frame_xref_table[frame_xref_entries].fileNumber = chunk;
                frame_xref_table[frame_xref_entries].frameType =
                    !memcmp(buf.blockType, "VIDF", 4) ? MLV_FRAME_VIDF :
                    !memcmp(buf.blockType, "AUDF", 4) ? MLV_FRAME_AUDF :
                    MLV_FRAME_UNSPECIFIED;
                
                frame_xref_entries++;
            }
            
            position += buf.blockSize;
            FIO_SeekSkipFile(chunk_files[chunk], position, SEEK_SET);
        }
    }
    
    mlv_play_xref_sort(frame_xref_table, frame_xref_entries);
    mlv_play_save_index(filename, &main_header, chunk_count, frame_xref_table, frame_xref_entries);
}

static mlv_xref_hdr_t *mlv_play_get_index(char *filename, FILE **chunk_files, uint32_t chunk_count)
{
    mlv_xref_hdr_t *table = NULL;
    
    table = mlv_play_load_index(filename);
    if(table)
    {
        return table;
    }
    
    bmp_printf(FONT_LARGE, 30, 100, "Preparing:", filename);
    bmp_printf(FONT_MED, 40, 100 + font_large.height + 1, filename);
    mlv_play_build_index(filename, chunk_files, chunk_count);
    
    return mlv_play_load_index(filename);
}

static unsigned int mlv_play_is_raw(FILE *f)
{
    lv_rec_file_footer_t footer;

    /* get current position in file, seek to footer, read and go back where we were */
    int64_t old_pos = FIO_SeekSkipFile(f, 0, SEEK_CUR);
    FIO_SeekSkipFile(f, -(int64_t)sizeof(lv_rec_file_footer_t), SEEK_END);
    int read = FIO_ReadFile(f, &footer, sizeof(lv_rec_file_footer_t));
    FIO_SeekSkipFile(f, old_pos, SEEK_SET);

    /* check if the footer was read */
    if(read != sizeof(lv_rec_file_footer_t))
    {
        return 0;
    }
    
    /* check if the footer is in the right format */
    if(strncmp((char *)footer.magic, "RAWM", 4))
    {
        return 0;
    }
    
    return 1;
}

static unsigned int mlv_play_is_mlv(FILE *f)
{
    mlv_file_hdr_t header;

    /* get current position in file, seek to footer, read and go back where we were */
    int64_t old_pos = FIO_SeekSkipFile(f, 0, SEEK_CUR);
    FIO_SeekSkipFile(f, 0, SEEK_SET);
    int read = FIO_ReadFile(f, &header, sizeof(mlv_file_hdr_t));
    FIO_SeekSkipFile(f, old_pos, SEEK_SET);

    /* check if the footer was read */
    if(read != sizeof(mlv_file_hdr_t))
    {
        return 0;
    }
    
    /* check if the footer is in the right format */
    if(strncmp((char *)header.fileMagic, "MLVI", 4))
    {
        return 0;
    }
    
    return 1;
}

static unsigned int mlv_play_read_footer(FILE *f)
{
    lv_rec_file_footer_t footer;

    /* get current position in file, seek to footer, read and go back where we were */
    int64_t old_pos = FIO_SeekSkipFile(f, 0, SEEK_CUR);
    FIO_SeekSkipFile(f, - (int64_t)sizeof(lv_rec_file_footer_t), SEEK_END);
    int read = FIO_ReadFile(f, &footer, sizeof(lv_rec_file_footer_t));
    FIO_SeekSkipFile(f, old_pos, SEEK_SET);

    /* check if the footer was read */
    if(read != sizeof(lv_rec_file_footer_t))
    {
        bmp_printf(FONT_MED, 30, 190, "File position mismatch. Read %d", read);
        beep();
        msleep(1000);
    }
    
    /* check if the footer is in the right format */
    if(strncmp((char *)footer.magic, "RAWM", 4))
    {
        bmp_printf(FONT_MED, 30, 190, "Footer format mismatch");
        beep();
        msleep(1000);
        return 0;
    }
        
    /* update global variables with data from footer */
    res_x = footer.xRes;
    res_y = footer.yRes;
    frame_count = footer.frameCount + 1;
    frame_size = footer.frameSize;
    raw_info.white_level = footer.raw_info.white_level;
    raw_info.black_level = footer.raw_info.black_level;
    fps1000 = footer.sourceFpsx1000;
    
    return 1;
}


static FILE **mlv_play_load_chunks(char *base_filename, uint32_t *entries)
{
    uint32_t seq_number = 0;
    char filename[128];
    
    *entries = 0;
    
    strncpy(filename, base_filename, sizeof(filename));
    FILE **files = malloc(sizeof(FILE*));
    
    files[0] = FIO_OpenFile(filename, O_RDONLY | O_SYNC);
    if (!files[0])
    {
        return NULL;
    }
    
    (*entries)++;
    while(seq_number < 99)
    {
        files = realloc(files, (*entries + 1) * sizeof(FILE*));
        
        /* check for the next file M00, M01 etc */
        char seq_name[3];

        snprintf(seq_name, 3, "%02d", seq_number);
        seq_number++;

        strcpy(&filename[strlen(filename) - 2], seq_name);

        /* try to open from A: first*/
        filename[0] = 'A';
        files[*entries] = FIO_OpenFile(filename, O_RDONLY | O_SYNC);
        
        /* if failed, try B */
        if (!files[*entries])
        {
            filename[0] = 'B';
            files[*entries] = FIO_OpenFile(filename, O_RDONLY | O_SYNC);
        }
        
        /* when succeeded, check for next chunk, else abort */
        if (files[*entries])
        {
            (*entries)++;
        }
        else
        {
            break;
        }
    }
    return files;
}

static void mlv_play_close_chunks(FILE **chunk_files, uint32_t chunk_count)
{
    if(!chunk_files || !chunk_count || chunk_count > 100)
    {
        bmp_printf(FONT_MED, 30, 400, "mlv_play_close_chunks(): faulty parameters");
        beep();
        return;
    }
    
    for(uint32_t pos = 0; pos < chunk_count; pos++)
    {
        FIO_CloseFile(chunk_files[pos]);
    }
}

/* just don't run it on overexposed footage :) */
static void check_dup_frame(frame_buf_t *buffer)
{
    /* extremely unlikely to get identical values
     * in subsequent frames, because of the noise
     * exception: overexposed areas
     * checking a small number of pixels should be
     * enough for non-overexposed footage, and very fast */

    struct sample
    {
        int pixels[5][5];
    } __attribute__((packed));

    /* store a small sample from current frame and also last 3 frames */
    static struct sample prev_samples[4] = {
        { { { -1 } } },
        { { { -2 } } },
        { { { -3 } } },
        { { { -4 } } },
    };

    prev_samples[3] = prev_samples[2];
    prev_samples[2] = prev_samples[1];
    prev_samples[1] = prev_samples[0];

    /* sample the current frame (all channels) */
    for (int i = -2; i <= 2; i++)
    {
        for (int j = -2; j <= 2; j++)
        {
            prev_samples[0].pixels[i+2][j+2] = 
                raw_get_pixel_ex(
                    buffer->frameData,
                    buffer->xRes / 2 + j * 49,
                    buffer->yRes / 2 + i * 49
                );
        }
    }

    /* compare current frame with past samples */
    for (int i = 1; i <= 3; i++)
    {
        if (memcmp(&prev_samples[0], &prev_samples[i], sizeof(prev_samples[0])) == 0)
        {
            printf("Duplicate frame (%d)\n", i);
        }
    }
}

/* ---- superpixel renderer + optional 16x16x16 3D LUT (crop-mood) ----
 * One RGGB 2x2 cell -> one RGB sample -> 1D log/WB LUT (3 KB, fits 8 KB D-cache).
 * Default 'Orig' (0) bypasses 3D LUT completely for maximum playback speed.
 * Presets (Rec709, Kodak, Warm, Teal, B&W) and user ML/LUTS/*.cube files use
 * a 16x16x16 YUV 3D LUT with integer tetrahedral interpolation. */

/* 11-bit index (2048 levels of the raw range, was 10-bit); R and B share one curve (WB 2,1,2),
 * so the two tables are 4 KB and still fit the 8 KB D-cache. */
#define SP_IDX_BITS 11
#define SP_IDX_SIZE (1 << SP_IDX_BITS)
static uint8_t sp_lut_rb[SP_IDX_SIZE];
static uint8_t sp_lut_g[SP_IDX_SIZE];
static uint32_t sp_idx_mask = SP_IDX_SIZE - 1;   /* (1 << MIN(vbits, 11)) - 1, set by sp_build_lut */
static uint32_t sp_lut_key = 0;

/* YUV matrix of this camera's LV/BMP pipeline: ML core is built with CONFIG_REC709 on some
 * cameras (5D3) and BT.601 on others (EOS M). Modules can't see that #define, so ask the
 * core's rgb2yuv422() once: pure red gives Y=54 with BT.709 and Y=76 with BT.601. */
static int sp_rec709 = -1;
static int sp_is_rec709(void)
{
    if (sp_rec709 < 0)
    {
        uint32_t p = rgb2yuv422(255, 0, 0);
        sp_rec709 = (((p >> 8) & 0xFF) < 65) ? 1 : 0;
    }
    return sp_rec709;
}

/* r,g,b 0..255 -> packed UYVY (same Y for both pixels); coefficients as in imgconv.c */
static inline uint32_t sp_rgb2uyvy(int r, int g, int b, int rec709)
{
    int Y, U, V;
    if (rec709)
    {
        Y = ( 217 * r + 732 * g +  73 * b) >> 10;
        U = (-117 * r - 394 * g + 512 * b) >> 10;
        V = ( 512 * r - 465 * g -  46 * b) >> 10;
    }
    else
    {
        Y = ( 306 * r + 601 * g + 116 * b) >> 10;
        U = (-172 * r - 337 * g + 509 * b) >> 10;
        V = ( 509 * r - 427 * g -  82 * b) >> 10;
    }
    return (U & 0xFF) | (Y << 8) | ((V & 0xFF) << 16) | (Y << 24);
}
static int sp_rendered_ok = 0;   /* last mlv_play_render_frame() went through sp_render() completely */

#define SP_LUT_BUILTIN_COUNT 6
#define SP_LUT_MAX_USER      24
#define SP_LUT3D_SIZE        16
#define SP_LUT3D_ENTRIES     (SP_LUT3D_SIZE * SP_LUT3D_SIZE * SP_LUT3D_SIZE)
#define SP_CLU_MAGIC         "MLVCLU02"

typedef struct
{
    uint8_t y;
    int8_t  u;
    int8_t  v;
    uint8_t pad;
} sp_yuv_t;

typedef struct
{
    char     magic[8];
    uint32_t src_size;
    uint32_t hash;
    uint32_t reserved[4];
} sp_clu_hdr_t;

typedef struct
{
    char label[14];
    char path[MAX_PATH];
    uint32_t file_size;
} sp_user_lut_t;

static const char *sp_builtin_names[SP_LUT_BUILTIN_COUNT] = {
    "Orig", "Rec709", "Kodak", "Warm", "Teal", "B&W"
};

static struct semaphore *sp_lut_sem = NULL;
static sp_user_lut_t sp_user_luts[SP_LUT_MAX_USER];
static int sp_user_lut_count = 0;
static int sp_lut_scanned = 0;
static sp_yuv_t sp_lut3d[SP_LUT3D_ENTRIES]; /* 16 KB: Y (0..255), U/V (-128..127) */
static int sp_lut3d_loaded = -1;
static uint32_t sp_lut3d_hash = 0;

static void sp_lut_lock()   { if (sp_lut_sem) take_semaphore(sp_lut_sem, 0); }
static void sp_lut_unlock() { if (sp_lut_sem) give_semaphore(sp_lut_sem); }

static void prx_close(void);

static int sp_ends_with_ci(const char *s, const char *ext)
{
    size_t sl = strlen(s), el = strlen(ext);
    if (sl < el) return 0;
    s += sl - el;
    for (size_t i = 0; i < el; i++)
    {
        char a = s[i], b = ext[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return 1;
}

/* Scan ML/LUTS/ for user .cube files (called lazily under sp_lut_lock). */
static void sp_lut_scan_locked(void)
{
    if (sp_lut_scanned) return;
    sp_lut_scanned = 1;
    sp_user_lut_count = 0;

    struct fio_file file;
    struct fio_dirent *dirent = FIO_FindFirstEx("ML/LUTS/", &file);
    if (IS_ERROR(dirent))
    {
        return;
    }

    do
    {
        if (sp_user_lut_count >= SP_LUT_MAX_USER) break;
        if (file.mode & ATTR_DIRECTORY) continue;
        if (file.name[0] == '.' || file.name[0] == '_') continue;
        if (!sp_ends_with_ci(file.name, ".cube")) continue;
        if (file.size <= 4096) continue; /* skip macOS 4 KB AppleDouble ._* forks & tiny invalid files */

        sp_user_lut_t *e = &sp_user_luts[sp_user_lut_count++];
        snprintf(e->path, sizeof(e->path), "ML/LUTS/%s", file.name);
        e->file_size = file.size;

        /* display label: basename without .cube, max 10 chars for OSD */
        size_t len = strlen(file.name);
        size_t base = (len > 5) ? len - 5 : len;
        if (base > 10) base = 10;
        memcpy(e->label, file.name, base);
        e->label[base] = 0;
    }
    while (FIO_FindNextEx(dirent, &file) == 0);

    FIO_FindClose(dirent);

    /* simple alphabetical sort so play.lut config index stays stable */
    for (int i = 0; i < sp_user_lut_count - 1; i++)
    {
        for (int j = i + 1; j < sp_user_lut_count; j++)
        {
            if (strcmp(sp_user_luts[i].path, sp_user_luts[j].path) > 0)
            {
                sp_user_lut_t tmp = sp_user_luts[i];
                sp_user_luts[i] = sp_user_luts[j];
                sp_user_luts[j] = tmp;
            }
        }
    }
}

static int sp_lut_count(void)
{
    sp_lut_lock();
    sp_lut_scan_locked();
    int n = SP_LUT_BUILTIN_COUNT + sp_user_lut_count;
    sp_lut_unlock();
    return n;
}

static const char *sp_lut_name(int idx)
{
    int n = sp_lut_count();
    if (n <= 0) return "Orig";
    idx = MOD(idx, n);
    if (idx < SP_LUT_BUILTIN_COUNT)
    {
        return sp_builtin_names[idx];
    }
    return sp_user_luts[idx - SP_LUT_BUILTIN_COUNT].label;
}

static void mlv_play_lut_cycle(int show_toast)
{
    (void) show_toast;
    int n = sp_lut_count();
    mlv_play_lut = MOD(mlv_play_lut + 1, n);
    mlv_play_lut_dirty = 1;
    aud_free_buffers();
    prx_close();
}

static inline sp_yuv_t sp_rgb_to_yuv(int r, int g, int b)
{
    r = COERCE(r, 0, 255);
    g = COERCE(g, 0, 255);
    b = COERCE(b, 0, 255);
    int Y, U, V;
    if (sp_is_rec709())
    {
        Y = ( 217 * r + 732 * g +  73 * b) >> 10;
        U = (-117 * r - 394 * g + 512 * b) >> 10;
        V = ( 512 * r - 465 * g -  46 * b) >> 10;
    }
    else
    {
        Y = ( 306 * r + 601 * g + 116 * b) >> 10;
        U = (-172 * r - 337 * g + 509 * b) >> 10;
        V = ( 509 * r - 427 * g -  82 * b) >> 10;
    }
    sp_yuv_t out;
    out.y = (uint8_t)COERCE(Y, 0, 255);
    out.u = (int8_t)COERCE(U, -128, 127);
    out.v = (int8_t)COERCE(V, -128, 127);
    out.pad = 0;
    return out;
}

/* Tetrahedral interpolation in 16x16x16 YUV LUT; r,g,b in 0..255. */
static inline uint32_t sp_lut3d_eval(int r8, int g8, int b8)
{
    /* map 0..255 -> 0..3839 (Q8 in [0..14.996]) cleanly without divide */
    int r = r8 * 15; r += r >> 8;
    int g = g8 * 15; g += g >> 8;
    int b = b8 * 15; b += b >> 8;

    int r0 = r >> 8, dr = r & 0xFF;
    int g0 = g >> 8, dg = g & 0xFF;
    int b0 = b >> 8, db = b & 0xFF;

    const sp_yuv_t *c0 = &sp_lut3d[(r0 << 8) | (g0 << 4) | b0];
    const sp_yuv_t *c3 = c0 + 256 + 16 + 1;
    const sp_yuv_t *c1, *c2;
    int w1, w2, w3;

    if (dr >= dg)
    {
        if (dg >= db)      { c1 = c0 + 256; c2 = c1 + 16;  w1 = dr; w2 = dg; w3 = db; }
        else if (dr >= db) { c1 = c0 + 256; c2 = c1 + 1;   w1 = dr; w2 = db; w3 = dg; }
        else               { c1 = c0 + 1;   c2 = c1 + 256; w1 = db; w2 = dr; w3 = dg; }
    }
    else
    {
        if (db >= dg)      { c1 = c0 + 1;   c2 = c1 + 16;  w1 = db; w2 = dg; w3 = dr; }
        else if (db >= dr) { c1 = c0 + 16;  c2 = c1 + 1;   w1 = dg; w2 = db; w3 = dr; }
        else               { c1 = c0 + 16;  c2 = c1 + 256; w1 = dg; w2 = dr; w3 = db; }
    }

    int Y = c0->y + ((w1 * (c1->y - c0->y) + w2 * (c2->y - c1->y) + w3 * (c3->y - c2->y) + 128) >> 8);
    int U = c0->u + ((w1 * (c1->u - c0->u) + w2 * (c2->u - c1->u) + w3 * (c3->u - c2->u) + 128) >> 8);
    int V = c0->v + ((w1 * (c1->v - c0->v) + w2 * (c2->v - c1->v) + w3 * (c3->v - c2->v) + 128) >> 8);
    return (uint32_t)((U & 0xFF) | (Y << 8) | ((V & 0xFF) << 16) | (Y << 24));
}

/* Smoothstep S-curve in 0..255, blended by strength (0..256). Pure integer. */
static inline int sp_scurve(int x, int strength)
{
    x = COERCE(x, 0, 255);
    int s = (x * x * (765 - 2 * x)) / (255 * 255);
    return (x * (256 - strength) + s * strength) >> 8;
}

/* Gentle Canon 5D3 gamma-domain camera RGB -> Rec.709 3x3 matrix (row sums = 256). */
static inline void sp_cam_to_rec709(int r, int g, int b, int *ro, int *go, int *bo)
{
    int rr = ( 370 * r -  98 * g -  16 * b) >> 8;
    int gg = ( -42 * r + 332 * g -  34 * b) >> 8;
    int bb = (   6 * r -  84 * g + 334 * b) >> 8;
    *ro = COERCE(rr, 0, 255);
    *go = COERCE(gg, 0, 255);
    *bo = COERCE(bb, 0, 255);
}

static uint32_t sp_lut3d_compute_hash(void)
{
    uint32_t h = 2166136261u;
    const uint32_t *w = (const uint32_t *)sp_lut3d;
    for (int i = 0; i < SP_LUT3D_ENTRIES; i++)
    {
        h ^= w[i];
        h *= 16777619u;
    }
    return h ? h : 1u;
}

static void sp_build_lut3d_preset_locked(int id)
{
    for (int ri = 0; ri < SP_LUT3D_SIZE; ri++)
    {
        int rc = ri * 17;
        for (int gi = 0; gi < SP_LUT3D_SIZE; gi++)
        {
            int gc = gi * 17;
            for (int bi = 0; bi < SP_LUT3D_SIZE; bi++)
            {
                int bc = bi * 17;
                int r = rc, g = gc, b = bc;

                if (id == 0) /* Orig (unused when bypassed, identity fallback) */
                {
                }
                else if (id == 1) /* Rec709 */
                {
                    sp_cam_to_rec709(rc, gc, bc, &r, &g, &b);
                    r = sp_scurve(r, 96);
                    g = sp_scurve(g, 96);
                    b = sp_scurve(b, 96);
                }
                else if (id == 2) /* Kodak 2383 inspired */
                {
                    sp_cam_to_rec709(rc, gc, bc, &r, &g, &b);
                    r = sp_scurve(r, 175);
                    g = sp_scurve(g, 175);
                    b = sp_scurve(b, 175);
                    int luma = (77 * r + 150 * g + 29 * b) >> 8;
                    int lo = 255 - luma;
                    r = r + 5 + ((12 * luma - 9 * lo) >> 8);
                    g = g + 5 + (( 2 * luma + 2 * lo) >> 8);
                    b = b + 5 + ((-14 * luma + 9 * lo) >> 8);
                }
                else if (id == 3) /* Warm Film */
                {
                    sp_cam_to_rec709(rc, gc, bc, &r, &g, &b);
                    r = sp_scurve(r, 110);
                    g = sp_scurve(g, 110);
                    b = sp_scurve(b, 110);
                    int luma = (77 * r + 150 * g + 29 * b) >> 8;
                    int lift = (8 * (255 - luma)) >> 8;
                    r = r + lift + 11;
                    g = g + lift + 3;
                    b = b + lift - 13;
                }
                else if (id == 4) /* Teal & Orange */
                {
                    sp_cam_to_rec709(rc, gc, bc, &r, &g, &b);
                    r = sp_scurve(r, 155);
                    g = sp_scurve(g, 155);
                    b = sp_scurve(b, 155);
                    int luma = (77 * r + 150 * g + 29 * b) >> 8;
                    int lo = 255 - luma;
                    r = r + ((18 * luma - 18 * lo) >> 8);
                    g = g + (( 3 * luma +  5 * lo) >> 8);
                    b = b + ((-16 * luma + 16 * lo) >> 8);
                }
                else if (id == 5) /* B&W High Contrast */
                {
                    sp_cam_to_rec709(rc, gc, bc, &r, &g, &b);
                    int y = (92 * r + 138 * g + 26 * b) >> 8;
                    y = sp_scurve(y, 185);
                    r = g = b = y;
                }

                sp_lut3d[(ri << 8) | (gi << 4) | bi] = sp_rgb_to_yuv(r, g, b);
            }
        }
    }
    sp_lut3d_hash = sp_lut3d_compute_hash();
}

/* Parse ASCII decimal float into Q12 fixed-point (1.0 -> 4096). Zero FP ops. */
static const char *sp_parse_q12(const char *p, int *out)
{
    while (*p == ' ' || *p == '\t') p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') { p++; }
    if ((*p < '0' || *p > '9') && *p != '.') return NULL;

    int ipart = 0;
    while (*p >= '0' && *p <= '9')
    {
        if (ipart < 1000) ipart = ipart * 10 + (*p - '0');
        p++;
    }
    int fpart = 0, fdiv = 1;
    if (*p == '.')
    {
        p++;
        while (*p >= '0' && *p <= '9')
        {
            if (fdiv < 10000) { fpart = fpart * 10 + (*p - '0'); fdiv *= 10; }
            p++;
        }
    }
    int val = (ipart << 12) + (fpart * 4096 + (fdiv >> 1)) / fdiv;
    if (*p == 'e' || *p == 'E')
    {
        p++;
        int eneg = 0, eval = 0;
        if (*p == '-') { eneg = 1; p++; }
        else if (*p == '+') { p++; }
        while (*p >= '0' && *p <= '9') { eval = eval * 10 + (*p - '0'); p++; }
        if (eneg)
        {
            while (eval-- > 0 && val > 0) val /= 10;
        }
        else
        {
            while (eval-- > 0 && val < (1000 << 12)) val *= 10;
        }
    }
    *out = neg ? -val : val;
    return p;
}

static void sp_clu_path(const char *cube_path, char *out, size_t out_len)
{
    strncpy(out, cube_path, out_len - 1);
    out[out_len - 1] = 0;
    char *dot = strrchr(out, '.');
    if (dot && (size_t)(dot - out) + 5 <= out_len)
    {
        strcpy(dot, ".CLU");
    }
    else if (strlen(out) + 5 <= out_len)
    {
        strcat(out, ".CLU");
    }
}

/* Load user .cube file into sp_lut3d[4096] (using .CLU binary cache if valid). Returns 1 on success.
 * All FIO_ReadFile calls MUST use DMA-capable uncacheable memory (fio_malloc) so fio-ml.c never
 * triggers its cacheable-buffer fallback or ASSERT(count <= 8192). */
static int sp_load_cube_locked(const sp_user_lut_t *u)
{
    char clu_path[MAX_PATH];
    sp_clu_path(u->path, clu_path, sizeof(clu_path));

    uint8_t *rbuf = fio_malloc(8192);
    if (!rbuf) return 0;

    /* 1. Fast path: 16 KB binary .CLU cache (< 2 ms), read in 2x 8 KB DMA chunks */
    FILE *cf = FIO_OpenFile(clu_path, O_RDONLY | O_SYNC);
    if (cf)
    {
        sp_clu_hdr_t hdr;
        int ok = (FIO_ReadFile(cf, rbuf, sizeof(hdr)) == (int)sizeof(hdr));
        if (ok)
        {
            memcpy(&hdr, rbuf, sizeof(hdr));
            ok = !memcmp(hdr.magic, SP_CLU_MAGIC, 8) && (hdr.src_size == u->file_size);
        }
        if (ok)
        {
            uint8_t *dst = (uint8_t *)sp_lut3d;
            for (size_t off = 0; off < sizeof(sp_lut3d); off += 8192)
            {
                size_t chunk = MIN((size_t)8192, sizeof(sp_lut3d) - off);
                if (FIO_ReadFile(cf, rbuf, chunk) != (int)chunk)
                {
                    ok = 0;
                    break;
                }
                memcpy(dst + off, rbuf, chunk);
            }
        }
        FIO_CloseFile(cf);
        if (ok)
        {
            fio_free(rbuf);
            sp_lut3d_hash = hdr.hash ? hdr.hash : sp_lut3d_compute_hash();
            return 1;
        }
    }

    /* 2. Stream-parse .cube in pure integer math (handles 17^3, 32^3, 33^3, 65^3, etc.) */
    FILE *f = FIO_OpenFile(u->path, O_RDONLY | O_SYNC);
    if (!f)
    {
        fio_free(rbuf);
        return 0;
    }

    int N = 0;          /* declared LUT_3D_SIZE */
    int M = 0;          /* stored grid size (N if N<=33, else 17) */
    uint8_t *grid = NULL;
    int map_idx[17];    /* source indices kept when N > 33 */
    int dom_min[3] = { 0, 0, 0 };
    int dom_max[3] = { 4096, 4096, 4096 };
    int total_read = 0;
    int r_cur = 0, g_cur = 0, b_cur = 0;
    int mr = 0, mg = 0, mb = 0;

    char line[160];
    int llen = 0;
    int eof = 0;

    while (!eof)
    {
        int got = FIO_ReadFile(f, rbuf, 8192);
        if (got <= 0)
        {
            eof = 1;
            got = 0;
        }
        for (int pos = 0; pos <= got; pos++)
        {
            int is_end = (pos == got);
            char c = is_end ? '\n' : (char)rbuf[pos];
            if (is_end && !eof) break;

            if (c != '\n' && c != '\r')
            {
                if (llen < (int)sizeof(line) - 1) line[llen++] = c;
                continue;
            }
            if (llen == 0) continue;
            line[llen] = 0;
            llen = 0;

            const char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == 0 || *p == '#') continue;

            if (!strncmp(p, "LUT_3D_SIZE", 11) && (p[11] == ' ' || p[11] == '\t'))
            {
                int v = 0;
                p += 11;
                while (*p == ' ' || *p == '\t') p++;
                while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
                if (v >= 2 && v <= 128 && !grid)
                {
                    N = v;
                    M = (N <= 33) ? N : 17;
                    grid = malloc((size_t)M * M * M * 3);
                    if (!grid) { eof = 1; break; }
                    if (N > 33)
                    {
                        for (int k = 0; k < 17; k++)
                        {
                            map_idx[k] = (k * (N - 1) + 8) / 16;
                        }
                    }
                }
                continue;
            }
            if (!strncmp(p, "DOMAIN_MIN", 10) && (p[10] == ' ' || p[10] == '\t'))
            {
                const char *q = sp_parse_q12(p + 10, &dom_min[0]);
                if (q) q = sp_parse_q12(q, &dom_min[1]);
                if (q) sp_parse_q12(q, &dom_min[2]);
                continue;
            }
            if (!strncmp(p, "DOMAIN_MAX", 10) && (p[10] == ' ' || p[10] == '\t'))
            {
                const char *q = sp_parse_q12(p + 10, &dom_max[0]);
                if (q) q = sp_parse_q12(q, &dom_max[1]);
                if (q) sp_parse_q12(q, &dom_max[2]);
                continue;
            }
            if (!grid) continue;

            /* triplet: r g b */
            int vr, vg, vb;
            const char *q = sp_parse_q12(p, &vr);
            if (!q) continue;
            q = sp_parse_q12(q, &vg);
            if (!q) continue;
            q = sp_parse_q12(q, &vb);
            if (!q) continue;

            if (total_read < N * N * N)
            {
                int keep = 0;
                int dr = 0, dg = 0, db = 0;
                if (N <= 33)
                {
                    keep = 1;
                    dr = r_cur; dg = g_cur; db = b_cur;
                }
                else
                {
                    if (r_cur == map_idx[mr] && g_cur == map_idx[mg] && b_cur == map_idx[mb])
                    {
                        keep = 1;
                        dr = mr; dg = mg; db = mb;
                    }
                }
                if (keep)
                {
                    int out_r = COERCE((vr * 255 + 2048) >> 12, 0, 255);
                    int out_g = COERCE((vg * 255 + 2048) >> 12, 0, 255);
                    int out_b = COERCE((vb * 255 + 2048) >> 12, 0, 255);
                    size_t off = ((size_t)db * M * M + (size_t)dg * M + (size_t)dr) * 3;
                    grid[off + 0] = (uint8_t)out_r;
                    grid[off + 1] = (uint8_t)out_g;
                    grid[off + 2] = (uint8_t)out_b;
                }

                total_read++;
                r_cur++;
                if (N > 33 && mr < 16 && r_cur > map_idx[mr]) mr++;
                if (r_cur >= N)
                {
                    r_cur = 0; mr = 0;
                    g_cur++;
                    if (N > 33 && mg < 16 && g_cur > map_idx[mg]) mg++;
                    if (g_cur >= N)
                    {
                        g_cur = 0; mg = 0;
                        b_cur++;
                        if (N > 33 && mb < 16 && b_cur > map_idx[mb]) mb++;
                    }
                }
            }
        }
    }

    FIO_CloseFile(f);
    fio_free(rbuf);

    if (!grid || total_read < N * N * N)
    {
        if (grid) free(grid);
        return 0;
    }

    /* Bake camera->Rec709 + trilinear .cube lookup + Rec601 UYVY into sp_lut3d[4096] */
    int dspan[3];
    for (int c = 0; c < 3; c++)
    {
        dspan[c] = dom_max[c] - dom_min[c];
        if (dspan[c] <= 0) dspan[c] = 4096;
    }

    for (int ri = 0; ri < SP_LUT3D_SIZE; ri++)
    {
        int rc = ri * 17;
        for (int gi = 0; gi < SP_LUT3D_SIZE; gi++)
        {
            int gc = gi * 17;
            for (int bi = 0; bi < SP_LUT3D_SIZE; bi++)
            {
                int bc = bi * 17;
                int r7, g7, b7;
                sp_cam_to_rec709(rc, gc, bc, &r7, &g7, &b7);

                /* normalize to [0..4096] against DOMAIN_MIN/MAX, then scale to [0..(M-1)*256] */
                int in_q12[3] = {
                    (r7 * 4096 + 127) / 255,
                    (g7 * 4096 + 127) / 255,
                    (b7 * 4096 + 127) / 255
                };
                int pos[3], i0[3], i1[3], frac[3];
                for (int c = 0; c < 3; c++)
                {
                    int norm = ((in_q12[c] - dom_min[c]) * 4096) / dspan[c];
                    norm = COERCE(norm, 0, 4096);
                    pos[c] = (norm * (M - 1)) >> 4; /* Q8 in [0 .. (M-1)*256] */
                    i0[c] = pos[c] >> 8;
                    if (i0[c] >= M - 1) { i0[c] = M - 1; i1[c] = M - 1; frac[c] = 0; }
                    else { i1[c] = i0[c] + 1; frac[c] = pos[c] & 0xFF; }
                }

                const uint8_t *c000 = &grid[((i0[2] * M + i0[1]) * M + i0[0]) * 3];
                const uint8_t *c100 = &grid[((i0[2] * M + i0[1]) * M + i1[0]) * 3];
                const uint8_t *c010 = &grid[((i0[2] * M + i1[1]) * M + i0[0]) * 3];
                const uint8_t *c110 = &grid[((i0[2] * M + i1[1]) * M + i1[0]) * 3];
                const uint8_t *c001 = &grid[((i1[2] * M + i0[1]) * M + i0[0]) * 3];
                const uint8_t *c101 = &grid[((i1[2] * M + i0[1]) * M + i1[0]) * 3];
                const uint8_t *c011 = &grid[((i1[2] * M + i1[1]) * M + i0[0]) * 3];
                const uint8_t *c111 = &grid[((i1[2] * M + i1[1]) * M + i1[0]) * 3];

                int fr = frac[0], fg = frac[1], fb = frac[2];
                int out_rgb[3];
                for (int c = 0; c < 3; c++)
                {
                    int x00 = c000[c] * (256 - fr) + c100[c] * fr;
                    int x10 = c010[c] * (256 - fr) + c110[c] * fr;
                    int x01 = c001[c] * (256 - fr) + c101[c] * fr;
                    int x11 = c011[c] * (256 - fr) + c111[c] * fr;
                    int y0 = ((x00 >> 8) * (256 - fg) + (x10 >> 8) * fg) >> 8;
                    int y1 = ((x01 >> 8) * (256 - fg) + (x11 >> 8) * fg) >> 8;
                    out_rgb[c] = (y0 * (256 - fb) + y1 * fb + 128) >> 8;
                }

                sp_lut3d[(ri << 8) | (gi << 4) | bi] = sp_rgb_to_yuv(out_rgb[0], out_rgb[1], out_rgb[2]);
            }
        }
    }

    free(grid);
    sp_lut3d_hash = sp_lut3d_compute_hash();

    /* Best-effort write of 16 KB .CLU binary cache for instant future loads */
    FILE *wf = FIO_CreateFile(clu_path);
    if (wf)
    {
        sp_clu_hdr_t hdr;
        memset(&hdr, 0, sizeof(hdr));
        memcpy(hdr.magic, SP_CLU_MAGIC, 8);
        hdr.src_size = u->file_size;
        hdr.hash = sp_lut3d_hash;
        FIO_WriteFile(wf, &hdr, sizeof(hdr));
        FIO_WriteFile(wf, sp_lut3d, sizeof(sp_lut3d));
        FIO_CloseFile(wf);
    }

    return 1;
}

static uint32_t sp_lut_ensure(int idx)
{
    if (idx == 0)
    {
        return 0; /* Orig: 3D LUT bypassed completely */
    }
    sp_lut_lock();
    sp_lut_scan_locked();
    int total = SP_LUT_BUILTIN_COUNT + sp_user_lut_count;
    idx = (total > 0) ? MOD(idx, total) : 0;
    if (idx == 0)
    {
        sp_lut_unlock();
        return 0;
    }

    if (idx != sp_lut3d_loaded)
    {
        if (idx < SP_LUT_BUILTIN_COUNT)
        {
            sp_build_lut3d_preset_locked(idx);
            sp_lut3d_loaded = idx;
        }
        else if (sp_load_cube_locked(&sp_user_luts[idx - SP_LUT_BUILTIN_COUNT]))
        {
            sp_lut3d_loaded = idx;
        }
        else
        {
            /* fallback to Rec709 if a .cube file is invalid */
            sp_build_lut3d_preset_locked(1);
            sp_lut3d_loaded = idx;
        }
    }
    uint32_t h = sp_lut3d_hash;
    sp_lut_unlock();
    return h;
}

#define SP_MAX_COLS 1024
static uint32_t sp_col_r[SP_MAX_COLS];  /* (16-bit word index << 5) | right shift, R pixel of the cell */
static uint32_t sp_col_g[SP_MAX_COLS];  /* same, G pixel (x + 1) of the cell */

/* log2(x) in Q8 fixed point (linear mantissa, max error ~0.09 EV), x >= 1 */
static int sp_log2_q8(uint32_t x)
{
    int msb = 31;
    while (!(x & 0x80000000u))
    {
        x <<= 1;
        msb--;
    }
    return (msb << 8) | ((x >> 23) & 0xFF);
}

/* Digital-gain clips (mlv_lite 12..8-bit lossless) keep a 14-bit container but use only
 * black..white (e.g. 2047..2932 at 10-bit). Index the LUT by the bits the values really use,
 * not the container: 10-bit gets ~440 LUT steps instead of ~110 (no per-pixel cost). */
static int sp_eff_vbits(int vbits, int white)
{
    if (white <= 0) return vbits;
    uint32_t lim = (uint32_t)white + ((uint32_t)white >> 3);   /* small margin above white */
    int b = 0;
    while (b < 31 && (lim >> b)) b++;
    return (b >= 8 && b < vbits) ? b : vbits;
}

/* Real bit depth of a clip: container bpp minus the digital-gain steps (range vs full scale). */
static int sp_real_bits(int bpp, int black, int white)
{
    int full = (1 << bpp) - black;
    int range = white - black;
    if (bpp < 8 || bpp > 16 || range <= 0 || full <= 0) return bpp;
    int k = 0;
    while (k < 6 && bpp - k > 8 && ((range << k) * 3 < full * 2)) k++;
    return bpp - k;
}

static void sp_build_lut(int value_bits, int black, int white)
{
    int in_shift = MAX(value_bits, SP_IDX_BITS) - SP_IDX_BITS;
    int log_range = sp_log2_q8(MAX(white - black, 16));
    sp_idx_mask = (1u << MIN(value_bits, SP_IDX_BITS)) - 1;

    for (int i = 0; i < SP_IDX_SIZE; i++)
    {
        int v = (i << in_shift) + ((1 << in_shift) >> 1) - black;
        int ev = (v > 0) ? sp_log2_q8(v) - log_range : -100 * 256;   /* Q8, 0 = white */

        /* same curve as stock color preview: 10 EV shown, R/B +1 EV (WB 2,1,2), gamma 2 */
        int t_g  = COERCE(ev + 10 * 256, 0, 10 * 256);
        int t_rb = COERCE(ev + 11 * 256, 0, 10 * 256);
        int g8  = t_g  * 255 / (10 * 256);
        int rb8 = t_rb * 255 / (10 * 256);
        int g_out  = g8 * g8 / 255;
        int rb_out = rb8 * rb8 / 255;
        sp_lut_g[i] = (uint8_t)g_out;
        sp_lut_rb[i] = (uint8_t)rb_out;
    }
}

/* output rectangle: desqueezed image fitted into W x H (LV pixels ~square) */
static int sp_geometry(int xres, int yres, int ax, int ay, int W, int H, int *ow_, int *oh_, int *ox0_, int *oy0_)
{
    if (!ax || !ay)
    {
        ax = ay = 1;
    }
    int dw = xres * ax;
    int dh = yres * ay;
    if (dw <= 0 || dh <= 0)
    {
        return 0;
    }
    int ow = W;
    int oh = W * dh / dw;
    if (oh > H)
    {
        oh = H;
        ow = H * dw / dh;
    }
    ow &= ~1;
    oh &= ~3;
    if (ow < 2 || oh < 4)
    {
        return 0;
    }
    *ow_ = ow;
    *oh_ = oh;
    *ox0_ = ((W - ow) / 2) & ~1;
    *oy0_ = (H - oh) / 2;
    return 1;
}

/* lines computed per output frame: every 2nd (rows) or every 4th (rows4) line */
static int sp_vstep()
{
    return (mlv_play_read_mode == 2) ? 4 : 2;   /* full / rows / full-u: 2 */
}

/* source cell row for computed line j of n */
static inline uint32_t sp_src_row(int j, int n, int srows)
{
    return (uint32_t)((2 * j + 1) * srows) / (2 * n);
}

static inline uint32_t sp_col_entry(uint32_t bitpos, int cbits, int vbits)
{
    uint32_t w = bitpos >> 4;
    uint32_t off = bitpos & 15;
    /* top 11 bits of the value: value bits [32-off-cbits, 32-off-cbits+vbits) of the 32-bit window */
    uint32_t sh = 32 - off - cbits + MAX(vbits, SP_IDX_BITS) - SP_IDX_BITS;
    return (w << 5) | sh;
}

/* Auto-detect shifted Bayer phase (GRBG/GBRG vs RGGB) by comparing the two 2x2 diagonals
 * on an 8x8 sample grid. Green is ~1.5-2x brighter than (R+B)/2 on Canon sensors; if the
 * main diagonal (p00+p11) is clearly brighter than the anti-diagonal (p01+p10), Green sits
 * on (p00, p11) and we swap the cell channels to avoid magenta cast (e.g. EOS-M 3x1 crop). */
static int sp_detect_bayer_shift(const uint8_t *base, uint32_t rpitch, int srows, int np, int black, int vbits)
{
    int in_shift = MAX(vbits, SP_IDX_BITS) - SP_IDX_BITS;
    int black10 = black >> in_shift;
    uint32_t m = (1u << MIN(vbits, SP_IDX_BITS)) - 1;
    uint32_t diag = 0, anti = 0;

    for (int gy = 0; gy < 8; gy++)
    {
        uint32_t sy = (uint32_t)((gy + 2) * srows) / 11;
        if ((int)sy >= srows) sy = srows - 1;
        const uint16_t *A = (const uint16_t *)(base + 2 * sy * rpitch);
        const uint16_t *B = (const uint16_t *)((const uint8_t *)A + rpitch);

        for (int gx = 0; gx < 8; gx++)
        {
            int i = (gx + 2) * np / 11;
            if (i >= np) i = np - 1;
            uint32_t er = sp_col_r[i];
            uint32_t eg = sp_col_g[i];
            uint32_t wr = er >> 5, sr = er & 31;
            uint32_t wg = eg >> 5, sg = eg & 31;

            int p00 = (int)(((((uint32_t)A[wr] << 16) | A[wr + 1]) >> sr) & m) - black10;
            int p01 = (int)(((((uint32_t)A[wg] << 16) | A[wg + 1]) >> sg) & m) - black10;
            int p10 = (int)(((((uint32_t)B[wr] << 16) | B[wr + 1]) >> sr) & m) - black10;
            int p11 = (int)(((((uint32_t)B[wg] << 16) | B[wg + 1]) >> sg) & m) - black10;

            if (p00 > 0) diag += (uint32_t)p00;
            if (p11 > 0) diag += (uint32_t)p11;
            if (p01 > 0) anti += (uint32_t)p01;
            if (p10 > 0) anti += (uint32_t)p10;
        }
    }
    return (diag > anti + (anti >> 3)) ? 1 : 0;
}

static void sp_render(frame_buf_t *b)
{
    struct vram_info *vr = get_yuv422_vram();
    if (!vr || !vr->vram || !b->frameData)
    {
        yuv422_buffer_check();
        return;
    }

    int W = vr->width & ~1;
    int H = vr->height;
    int pitch = vr->pitch;
    int cbits = b->bitDepth;
    int vbits = sp_eff_vbits(b->valueBits, b->whiteLevel);
    int xres = b->xRes & ~1;
    int yres = b->yRes & ~1;
    uint32_t rpitch = b->xRes * cbits / 8;

    if (W <= 0 || H <= 0 || W / 2 > SP_MAX_COLS || cbits < 8 || cbits > 16 || xres < 2 || yres < 2 || (rpitch & 1))
    {
        return;
    }

    /* 1D & optional 3D LUT */
    uint32_t key = (vbits << 28) ^ (b->blackLevel << 14) ^ b->whiteLevel;
    if (key != sp_lut_key)
    {
        uint64_t t0 = get_us_clock();
        sp_build_lut(vbits, b->blackLevel, b->whiteLevel);
        sp_lut_key = key;
        perf_lut_us = MAX((uint32_t)(get_us_clock() - t0), 1);
    }
    int use_lut3d = (sp_lut_ensure(mlv_play_lut) != 0);
    const uint32_t m = sp_idx_mask;
    const int rec709 = sp_is_rec709();

    int ow, oh, ox0, oy0;
    if (!sp_geometry(xres, yres, b->xBinSkip, b->yBinSkip, W, H, &ow, &oh, &ox0, &oy0))
    {
        return;
    }

    /* column tables: one cell per UYVY pair, sampled at the cell centre */
    int np = ow / 2;
    int scols = xres / 2;
    for (int i = 0; i < np; i++)
    {
        uint32_t sx = (uint32_t)((2 * i + 1) * scols) / (2 * np);
        uint32_t pos = 2 * sx * cbits;
        sp_col_r[i] = sp_col_entry(pos, cbits, vbits);
        sp_col_g[i] = sp_col_entry(pos + cbits, cbits, vbits);
    }

    uint8_t *base = (uint8_t *) CACHEABLE(b->frameData);
    uint8_t *vram = vr->vram;
    int vstep = sp_vstep();
    int nlines = oh / vstep;
    int srows = yres / 2;
    int bayer_shift = sp_detect_bayer_shift(base, rpitch, srows, np, b->blackLevel, vbits);

    /* black bars above / below */
    if (oy0 > 0)
    {
        memset(vram, 0, oy0 * pitch);
    }
    if (oy0 + oh < H)
    {
        memset(vram + (oy0 + oh) * pitch, 0, (H - oy0 - oh) * pitch);
    }

    for (int j = 0; j < nlines; j++)
    {
        uint32_t sy = sp_src_row(j, nlines, srows);
        const uint16_t *A = (const uint16_t *)(base + 2 * sy * rpitch);
        const uint16_t *B = (const uint16_t *)((uint8_t *)A + rpitch);

        uint8_t *line = vram + (oy0 + vstep * j) * pitch;
        /* LV VRAM is uncached: store every pixel into all vstep lines here instead of
         * memcpy()ing the finished line, which would read it back from uncached memory */
        uint32_t *dst  = (uint32_t *)(line + ox0 * 2);
        uint32_t *dst2 = (uint32_t *)(line + pitch + ox0 * 2);
        uint32_t *dst3 = (uint32_t *)(line + 2 * pitch + ox0 * 2);
        uint32_t *dst4 = (uint32_t *)(line + 3 * pitch + ox0 * 2);
        const int v4 = (vstep == 4);

        for (int i = 0; i < np; i++)
        {
            uint32_t er = sp_col_r[i];
            uint32_t eg = sp_col_g[i];
            uint32_t wr = er >> 5, sr = er & 31;
            uint32_t wg = eg >> 5, sg = eg & 31;

            uint32_t a0 = ((uint32_t)A[wr] << 16) | A[wr + 1];
            uint32_t a1 = ((uint32_t)A[wg] << 16) | A[wg + 1];
            uint32_t b0 = ((uint32_t)B[wr] << 16) | B[wr + 1];
            uint32_t b1 = ((uint32_t)B[wg] << 16) | B[wg + 1];

            int r, g, bl;
            if (!bayer_shift)
            {
                r  = sp_lut_rb[(a0 >> sr) & m];
                g  = sp_lut_g[(((a1 >> sg) & m) + ((b0 >> sr) & m)) >> 1];
                bl = sp_lut_rb[(b1 >> sg) & m];
            }
            else
            {
                r  = sp_lut_rb[(a1 >> sg) & m];
                g  = sp_lut_g[(((a0 >> sr) & m) + ((b1 >> sg) & m)) >> 1];
                bl = sp_lut_rb[(b0 >> sr) & m];
            }

            /* Orig: direct matrix (BT.709 or BT.601, as the core); else 16x16x16 YUV 3D LUT */
            uint32_t v = use_lut3d ? sp_lut3d_eval(r, g, bl) : sp_rgb2uyvy(r, g, bl, rec709);
            dst[i] = v;
            dst2[i] = v;
            if (v4)
            {
                dst3[i] = v;
                dst4[i] = v;
            }
        }

        /* side bars on every line of the group (writes only) */
        if (ox0 > 0)
        {
            for (int k = 0; k < vstep; k++)
            {
                memset(line + k * pitch, 0, ox0 * 2);
                memset(line + k * pitch + (ox0 + ow) * 2, 0, (W - ox0 - ow) * 2);
            }
        }
    }
    sp_rendered_ok = 1;
}

/* Render a raw Bayer frame into an arbitrary W x H UYVY422 buffer (used for gallery .THM thumbnails).
 * Applies the same 2x2 superpixel LUT and RAWC binning desqueeze as sp_render(). */
static int sp_render_to_buf(const void *raw_data, int xres, int yres, int cbits, int vbits,
                            int black, int white, int ax, int ay,
                            uint8_t *dst_uyvy, int W, int H)
{
    vbits = sp_eff_vbits(vbits, white);
    xres &= ~1;
    yres &= ~1;
    W &= ~1;
    if (!raw_data || !dst_uyvy || W < 4 || H < 4 || W / 2 > SP_MAX_COLS ||
        cbits < 8 || cbits > 16 || xres < 2 || yres < 2)
    {
        return 0;
    }
    uint32_t rpitch = (uint32_t)xres * cbits / 8;
    if (rpitch & 1)
    {
        return 0;
    }

    uint32_t key = ((uint32_t)vbits << 28) ^ ((uint32_t)black << 14) ^ (uint32_t)white;
    if (key != sp_lut_key)
    {
        sp_build_lut(vbits, black, white);
        sp_lut_key = key;
    }

    int ow, oh, ox0, oy0;
    if (!sp_geometry(xres, yres, ax, ay, W, H, &ow, &oh, &ox0, &oy0))
    {
        return 0;
    }

    const uint32_t m = sp_idx_mask;
    const int rec709 = sp_is_rec709();
    int np = ow / 2;
    int scols = xres / 2;
    for (int i = 0; i < np; i++)
    {
        uint32_t sx = (uint32_t)((2 * i + 1) * scols) / (2 * np);
        uint32_t pos = 2 * sx * cbits;
        sp_col_r[i] = sp_col_entry(pos, cbits, vbits);
        sp_col_g[i] = sp_col_entry(pos + cbits, cbits, vbits);
    }

    const uint8_t *base = (const uint8_t *) CACHEABLE(raw_data);
    int pitch = W * 2;
    int srows = yres / 2;
    int bayer_shift = sp_detect_bayer_shift(base, rpitch, srows, np, black, vbits);

    /* fill background black (UYVY: U=0, Y=0, V=0, Y=0 -> 0x00) */
    memset(dst_uyvy, 0, (size_t)H * pitch);

    for (int j = 0; j < oh; j++)
    {
        uint32_t sy = sp_src_row(j, oh, srows);
        const uint16_t *A = (const uint16_t *)(base + 2 * sy * rpitch);
        const uint16_t *B = (const uint16_t *)((const uint8_t *)A + rpitch);
        uint32_t *dst = (uint32_t *)(dst_uyvy + (oy0 + j) * pitch + ox0 * 2);

        for (int i = 0; i < np; i++)
        {
            uint32_t er = sp_col_r[i];
            uint32_t eg = sp_col_g[i];
            uint32_t wr = er >> 5, sr = er & 31;
            uint32_t wg = eg >> 5, sg = eg & 31;

            uint32_t a0 = ((uint32_t)A[wr] << 16) | A[wr + 1];
            uint32_t a1 = ((uint32_t)A[wg] << 16) | A[wg + 1];
            uint32_t b0 = ((uint32_t)B[wr] << 16) | B[wr + 1];
            uint32_t b1 = ((uint32_t)B[wg] << 16) | B[wg + 1];

            int r, g, bl;
            if (!bayer_shift)
            {
                r  = sp_lut_rb[(a0 >> sr) & m];
                g  = sp_lut_g[(((a1 >> sg) & m) + ((b0 >> sr) & m)) >> 1];
                bl = sp_lut_rb[(b1 >> sg) & m];
            }
            else
            {
                r  = sp_lut_rb[(a1 >> sg) & m];
                g  = sp_lut_g[(((a0 >> sr) & m) + ((b1 >> sg) & m)) >> 1];
                bl = sp_lut_rb[(b0 >> sr) & m];
            }

            dst[i] = sp_rgb2uyvy(r, g, bl, rec709);
        }
    }
    return 1;
}

/* read only the raw rows sp_render will use (uncompressed footage).
 * Runs are aligned to 64 bytes in the (4 KB aligned) destination for DMA.
 * returns bytes read, or -1 if not applicable / failed */
static int32_t sp_read_rows(FILE *f, int64_t data_pos, uint8_t *dst, uint32_t size,
                            int xRes, int yRes, int bpp, int ax, int ay, uint32_t *calls)
{
    struct vram_info *vr = get_yuv422_vram();
    if (!vr || !vr->vram)
    {
        return -1;
    }
    uint32_t rpitch = xRes * bpp / 8;
    int ow, oh, ox0, oy0;
    if ((rpitch & 1) || !sp_geometry(xRes & ~1, yRes & ~1, ax, ay, vr->width & ~1, vr->height, &ow, &oh, &ox0, &oy0))
    {
        return -1;
    }

    int nlines = oh / sp_vstep();
    int srows = (yRes & ~1) / 2;
    uint32_t run_s = 0, run_e = 0;
    int have = 0;
    int32_t total = 0;

    for (int j = 0; j <= nlines; j++)
    {
        uint32_t s = 0, e = 0;
        if (j < nlines)
        {
            uint32_t sy = sp_src_row(j, nlines, srows);
            s = (2 * sy * rpitch) & ~63;
            e = MIN((2 * sy * rpitch + 2 * rpitch + 64 + 63) & ~63, size);   /* +64: read-ahead word of the last cell */
            if (s >= e)
            {
                continue;
            }
            if (have && s <= run_e)
            {
                run_e = MAX(run_e, e);
                continue;
            }
        }
        if (have)
        {
            FIO_SeekSkipFile(f, data_pos + run_s, SEEK_SET);
            if (FIO_ReadFile(f, dst + run_s, run_e - run_s) != (int32_t)(run_e - run_s))
            {
                return -1;
            }
            total += run_e - run_s;
            (*calls)++;
        }
        run_s = s;
        run_e = e;
        have = 1;
    }
    return total;
}

/* small/cached reads through a DMA-capable buffer: avoids fio-ml's
 * per-call fio_malloc + memcpy + "fixme" printf + ASSERT(count <= 8192) for cacheable destinations */
static void *mlv_play_hdr_dma = NULL;
static int32_t mlv_play_read_small(FILE *f, void *dst, uint32_t size)
{
    if (!mlv_play_hdr_dma)
    {
        mlv_play_hdr_dma = fio_malloc(512);
    }
    if (!mlv_play_hdr_dma)
    {
        return FIO_ReadFile(f, dst, size);
    }
    uint8_t *out = (uint8_t *)dst;
    uint32_t total = 0;
    while (total < size)
    {
        uint32_t chunk = MIN((uint32_t)512, size - total);
        int32_t r = FIO_ReadFile(f, mlv_play_hdr_dma, chunk);
        if (r <= 0)
        {
            return total > 0 ? (int32_t)total : r;
        }
        memcpy(out + total, mlv_play_hdr_dma, (uint32_t)r);
        total += (uint32_t)r;
        if ((uint32_t)r < chunk)
        {
            break;
        }
    }
    return (int32_t)total;
}

/* ---- proxy (.PRX next to the .MLV) ----
 * Built automatically during the first sequential 'super' playback: every rendered
 * frame's computed lines are copied from the LV buffer and appended to the PRX.
 * Later playbacks read the PRX frame (~0.3 MB instead of 2.5-3.5 MB raw) and blit it,
 * paced to the source fps. The MLV itself is never modified. A PRX is only used when
 * it is complete and matches the MLV (GUID, frame count, raw size) and the screen geometry. */
#define PRX_MAGIC   "MLVPRX01"

typedef struct
{
    char     magic[8];
    uint32_t version;
    uint32_t complete;
    uint64_t guid;
    uint32_t frame_count;
    uint32_t src_xres, src_yres, src_bpp;
    uint32_t W, H;
    uint32_t ow, oh, ox0, oy0, vstep;
    uint32_t line_bytes;
    uint32_t nlines;
    uint32_t frame_bytes;
    uint32_t frames_written;
    uint32_t lut_hash;
} prx_hdr_t;

#define PRX_OFF   0
#define PRX_BUILD 1
#define PRX_PLAY  2

static struct semaphore *prx_sem = NULL;
static FILE *prx_file = NULL;
static volatile uint32_t prx_mode = PRX_OFF;
static prx_hdr_t prx_hdr;
static uint32_t prx_next = 0;
static uint8_t *prx_wbuf = NULL;          /* one frame, DMA capable */
static uint8_t prx_hdr_buf[PRX_HDR_SIZE];
static char prx_path[MAX_PATH] = "";

static void prx_lock()   { if (prx_sem) take_semaphore(prx_sem, 0); }
static void prx_unlock() { if (prx_sem) give_semaphore(prx_sem); }

/* caller holds the lock */
static int prx_write_header_locked()
{
    uint8_t *hbuf = prx_wbuf ? prx_wbuf : prx_hdr_buf;
    memset(hbuf, 0, PRX_HDR_SIZE);
    memcpy(hbuf, &prx_hdr, sizeof(prx_hdr));
    FIO_SeekSkipFile(prx_file, PRX_HDR_OFFSET, SEEK_SET);
    return FIO_WriteFile(prx_file, hbuf, PRX_HDR_SIZE) == PRX_HDR_SIZE;
}

/* caller holds the lock */
static void prx_close_locked()
{
    if (prx_file)
    {
        /* save partial progress so next playback of the same clip+LUT resumes where it left off */
        if (prx_mode == PRX_BUILD && prx_next > prx_hdr.frames_written)
        {
            prx_hdr.frames_written = prx_next;
            prx_hdr.complete = (prx_next >= prx_hdr.frame_count) ? 1 : 0;
            prx_write_header_locked();
        }
        FIO_CloseFile(prx_file);
        prx_file = NULL;
    }
    prx_path[0] = 0;
    if (prx_wbuf)
    {
        fio_free(prx_wbuf);
        prx_wbuf = NULL;
    }
    prx_mode = PRX_OFF;
}

static void prx_close()
{
    prx_lock();
    prx_close_locked();
    prx_unlock();
}

/* decide at the first VIDF of a pass: play a complete PRX, resume a partial PRX, or build from 0 */
static void prx_setup(const char *mlv_name, uint64_t guid, uint32_t frame_count,
                      int xres, int yres, int bpp, int ax, int ay, int will_super)
{
    prx_close();
    snprintf(prx_status, sizeof(prx_status), "-");

    struct vram_info *vr = get_yuv422_vram();
    if (!will_super || !vr || !vr->vram || !frame_count || strlen(mlv_name) < 4 || strlen(mlv_name) >= MAX_PATH)
    {
        return;
    }

    prx_hdr_t want;
    memset(&want, 0, sizeof(want));
    memcpy(want.magic, PRX_MAGIC, 8);
    want.version = 3;
    want.guid = guid;
    want.frame_count = frame_count;
    want.src_xres = xres;
    want.src_yres = yres;
    want.src_bpp = bpp;
    want.W = vr->width & ~1;
    want.H = vr->height;
    int ow, oh, ox0, oy0;
    if (!sp_geometry(xres & ~1, yres & ~1, ax, ay, want.W, want.H, &ow, &oh, &ox0, &oy0))
    {
        return;
    }
    want.ow = ow; want.oh = oh; want.ox0 = ox0; want.oy0 = oy0;
    want.vstep = 2;
    want.line_bytes = ow * 2;
    want.nlines = oh / 2;
    want.frame_bytes = (want.line_bytes * want.nlines + 511) & ~511;
    want.lut_hash = sp_lut_ensure(mlv_play_lut);

    char name[MAX_PATH];
    strcpy(name, mlv_name);
    strcpy(&name[strlen(name) - 3], "PRX");

    /* check existing .PRX header for complete or partial match (same GUID, geometry, and LUT) */
    uint32_t fsize = 0;
    uint32_t resume_frames = 0;
    if (FIO_GetFileSize(name, &fsize) == 0 && fsize >= PRX_FRAMES_OFFSET)
    {
        FILE *f = FIO_OpenFile(name, O_RDONLY | O_SYNC);
        if (f)
        {
            prx_hdr_t have;
            FIO_SeekSkipFile(f, PRX_HDR_OFFSET, SEEK_SET);
            if (mlv_play_read_small(f, &have, sizeof(have)) == sizeof(have))
            {
                uint32_t frames_written = have.frames_written;
                uint32_t is_complete = have.complete;
                have.complete = 0;
                have.frames_written = 0;
                if (!memcmp(&have, &want, sizeof(want)))
                {
                    if (is_complete && frames_written == frame_count &&
                        fsize >= PRX_FRAMES_OFFSET + frame_count * want.frame_bytes)
                    {
                        prx_lock();
                        prx_hdr = want;
                        prx_hdr.complete = 1;
                        prx_hdr.frames_written = frame_count;
                        prx_next = frame_count;
                        prx_file = f;
                        prx_mode = PRX_PLAY;
                        snprintf(prx_status, sizeof(prx_status), "play");
                        prx_unlock();
                        return;
                    }
                    else if (frames_written > 0 && frames_written < frame_count &&
                             fsize >= PRX_FRAMES_OFFSET + frames_written * want.frame_bytes)
                    {
                        resume_frames = frames_written;
                    }
                }
            }
            FIO_CloseFile(f);
        }
    }

    /* build from 0 or resume from resume_frames in-place inside the unified .PRX (preserving THM & IDX slots) */
    uint8_t *wbuf = fio_malloc(want.frame_bytes);
    if (!wbuf)
    {
        return;
    }
    memset(wbuf, 0, want.frame_bytes);
    FILE *f = prx_open_or_create_slot(name, PRX_HDR_OFFSET);
    if (!f)
    {
        fio_free(wbuf);
        return;
    }
    prx_lock();
    prx_hdr = want;
    prx_hdr.frames_written = resume_frames;
    prx_file = f;
    prx_wbuf = wbuf;
    prx_next = resume_frames;
    strncpy(prx_path, name, sizeof(prx_path) - 1);
    prx_path[sizeof(prx_path) - 1] = 0;
    if (prx_write_header_locked())
    {
        FIO_SeekSkipFile(prx_file, (int64_t)PRX_FRAMES_OFFSET + (int64_t)resume_frames * want.frame_bytes, SEEK_SET);
        prx_mode = PRX_BUILD;
        snprintf(prx_status, sizeof(prx_status), "b%d/%d", resume_frames, frame_count);
    }
    else
    {
        prx_next = 0;
        prx_hdr.frames_written = 0;
        prx_close_locked();
    }
    prx_unlock();
}

/* render task, after sp_render(): append this frame to the PRX being built */
static void prx_after_render(frame_buf_t *b)
{
    if (prx_mode != PRX_BUILD)
    {
        return;
    }
    prx_lock();
    if (prx_mode == PRX_BUILD && prx_file && prx_wbuf)
    {
        struct vram_info *vr = get_yuv422_vram();
        if (b->frameIndex < prx_next)
        {
            /* already-built prefix frame or paused / redrawn frame */
        }
        else if (b->frameIndex != prx_next || !sp_rendered_ok || b->isProxy || mlv_play_zoom != 0 ||
                 !vr || !vr->vram ||
                 (uint32_t)(vr->width & ~1) != prx_hdr.W || (uint32_t)vr->height != prx_hdr.H ||
                 (uint32_t)sp_vstep() != prx_hdr.vstep)
        {
            /* frame order broken (skip / seek) or geometry changed: save progress so far and close */
            snprintf(prx_status, sizeof(prx_status), "abort@%d", prx_next);
            prx_close_locked();
        }
        else
        {
            for (uint32_t j = 0; j < prx_hdr.nlines; j++)
            {
                memcpy(prx_wbuf + j * prx_hdr.line_bytes,
                       vr->vram + (prx_hdr.oy0 + prx_hdr.vstep * j) * vr->pitch + prx_hdr.ox0 * 2,
                       prx_hdr.line_bytes);
            }
            FIO_SeekSkipFile(prx_file, (int64_t)PRX_FRAMES_OFFSET + (int64_t)prx_next * prx_hdr.frame_bytes, SEEK_SET);
            if (FIO_WriteFile(prx_file, prx_wbuf, prx_hdr.frame_bytes) != (int)prx_hdr.frame_bytes)
            {
                snprintf(prx_status, sizeof(prx_status), "wrerr@%d", prx_next);
                prx_close_locked();
            }
            else
            {
                prx_next++;
                snprintf(prx_status, sizeof(prx_status), "b%d/%d", prx_next, prx_hdr.frame_count);
                if (prx_next == prx_hdr.frame_count)
                {
                    prx_hdr.complete = 1;
                    prx_hdr.frames_written = prx_next;
                    snprintf(prx_status, sizeof(prx_status), prx_write_header_locked() ? "built" : "hdrerr");
                    prx_close_locked();
                }
            }
        }
    }
    prx_unlock();
}

/* reader task: load PRX frame 'index' into the buffer (works for both PRX_PLAY and the 0..prx_next-1 prefix of PRX_BUILD) */
static int prx_read_frame(frame_buf_t *b, uint32_t index)
{
    int ok = 0;
    prx_lock();
    if ((prx_mode == PRX_PLAY || (prx_mode == PRX_BUILD && index < prx_next)) &&
        prx_file && index < prx_hdr.frame_count &&
        b->frameBufferAligned && b->allocSize >= prx_hdr.frame_bytes + 0x1000)
    {
        FIO_SeekSkipFile(prx_file, (int64_t)PRX_FRAMES_OFFSET + (int64_t)index * prx_hdr.frame_bytes, SEEK_SET);
        ok = FIO_ReadFile(prx_file, b->frameBufferAligned, prx_hdr.frame_bytes) == (int)prx_hdr.frame_bytes;
    }
    prx_unlock();
    return ok;
}

/* render task: show a PRX frame */
static void prx_blit(frame_buf_t *b)
{
    struct vram_info *vr = get_yuv422_vram();
    if (!vr || !vr->vram || (uint32_t)(vr->width & ~1) != prx_hdr.W || (uint32_t)vr->height != prx_hdr.H)
    {
        return;
    }
    uint8_t *vram = vr->vram;
    int pitch = vr->pitch;
    int W = prx_hdr.W, H = prx_hdr.H;
    int ox0 = prx_hdr.ox0, oy0 = prx_hdr.oy0, ow = prx_hdr.ow, oh = prx_hdr.oh;
    const uint8_t *src = (const uint8_t *) CACHEABLE(b->frameBufferAligned);

    if (oy0 > 0)
    {
        memset(vram, 0, oy0 * pitch);
    }
    if (oy0 + oh < H)
    {
        memset(vram + (oy0 + oh) * pitch, 0, (H - oy0 - oh) * pitch);
    }
    for (uint32_t j = 0; j < prx_hdr.nlines; j++)
    {
        uint8_t *line = vram + (oy0 + prx_hdr.vstep * j) * pitch;
        const uint8_t *s = src + j * prx_hdr.line_bytes;
        /* copy from the cached source for every line of the group: the second copy hits the
         * D-cache, while copying line -> line would read back from uncached VRAM */
        for (uint32_t k = 0; k < prx_hdr.vstep; k++)
        {
            uint8_t *l = line + k * pitch;
            if (ox0 > 0)
            {
                memset(l, 0, ox0 * 2);
                memset(l + (ox0 + ow) * 2, 0, (W - ox0 - ow) * 2);
            }
            memcpy(l + ox0 * 2, s, prx_hdr.line_bytes);
        }
    }
}

/* proxy playback is always paced to the source fps; building needs every frame (no skipping) */
static int mlv_play_use_exact()
{
    if (prx_mode == PRX_PLAY)
    {
        return 1;
    }
    if (prx_mode == PRX_BUILD)
    {
        return 0;
    }
    return mlv_play_exact_fps;
}

/* ---- Direct AUDF streaming + ASIF hardware DMA audio (crop-mood) ----
 * Zero sidecar files, zero FAT32 writes, zero FIO in DMA ISR.
 * During .PRX realtime playback, streams AUDF PCM blocks directly from the open MLV
 * chunk handles through a 64 KB lock-free ring buffer + 2x 8 KB DMA ping-pong buffers
 * to Canon's ASIF hardware DAC. Completely inactive during raw/PRX_BUILD playback. */
#define AUD_RING_SIZE  65536   /* power of 2 (~340 ms at 48 kHz stereo 16-bit) */
#define AUD_RING_MASK  (AUD_RING_SIZE - 1)
#define AUD_DMA_CHUNK  8192    /* ~42.6 ms per DMA half-buffer */

static int aud_ready = 0;
static uint32_t aud_sample_rate = 0;
static uint16_t aud_channels = 0;
static uint16_t aud_bits = 0;
static uint32_t aud_bytes_per_sec = 0;
static uint32_t aud_block_align = 4;

/* Active MLV xref table & chunk handles (borrowed from mlv_play_mlv while open) */
static FILE **aud_chunks = NULL;
static uint32_t aud_chunk_count = 0;
static mlv_xref_t *aud_xrefs = NULL;
static uint32_t aud_xref_count = 0;
static uint32_t aud_xref_idx = 0;
static int64_t  aud_blk_pcm_pos = 0;
static uint32_t aud_blk_pcm_rem = 0;
static uint32_t aud_blk_file_num = 0;

/* Lock-free single-producer (reader task) / single-consumer (ASIF DMA CBR) ring */
static uint8_t *aud_ring = NULL;
static uint8_t *aud_dma_buf[2] = { NULL, NULL };
static volatile uint32_t aud_ring_wr = 0;
static volatile uint32_t aud_ring_rd = 0;
static volatile uint32_t aud_eof = 0;
static volatile uint32_t aud_playing = 0;
static volatile uint32_t aud_stop_req = 0;
static volatile int aud_dma_idx = 0;

static void aud_stopped_cbr(void *arg)
{
    (void) arg;
    aud_playing = 0;
}

static inline void aud_apply_pcm_boost(uint8_t *buf, uint32_t byte_len)
{
    int mode = mlv_play_audio;
    if (mode < 6 || !buf || byte_len < 2)
    {
        return;
    }
    int shift = (mode >= 7) ? 2 : 1; /* 6 => 2x (+6 dB), 7 => 4x (+12 dB) */
    int16_t *s = (int16_t *)buf;
    uint32_t count = byte_len >> 1;
    for (uint32_t i = 0; i < count; i++)
    {
        int32_t v = (int32_t)s[i] << shift;
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        s[i] = (int16_t)v;
    }
}

static inline int aud_hw_volume(int vol)
{
    /*
     * Canon 5D3 SetAudioVolumeOut (0xFF1370A4) accepts 0..10 (not 1..5!):
     *   vol 1..5 maps to -32.6 dB .. -13.1 dB, while vol 10 is +11.25 dB (full speaker output).
     * Map UI Vol:1..5 to hardware 2, 4, 6, 8, 10 (and Vol:6..7 keep hw=10 + software PCM boost).
     */
    if (vol <= 0) return 0;
    if (vol >= 5) return 10;
    return vol * 2;
}

static int aud_is_5d3 = -1;

static inline void aud_stop_from_cbr(void)
{
    if (aud_is_5d3 < 0)
    {
        aud_is_5d3 = is_camera("5D3", "*") ? 1 : 0;
    }
    if (aud_is_5d3)
    {
        StopASIFDMADAC(aud_stopped_cbr, 0);
    }
    else
    {
        /* On EOS-M (StateObject ASIF driver, 0xFF2BF930), aud_dma_cbr runs inside asifEventDispatch.
         * Calling StopASIFDMADAC from inside the callback posts EV_STOP_DMA_DAC to the same task
         * and waits on hSyncSem (self-deadlock -> assert at ./ASIF/ASIF.c:464).
         * Simply returning without calling SetNextASIFDACBuffer leaves next_buf == NULL (0xFF2BF968),
         * causing EOS-M's ROM state machine to stop DMA cleanly on its own! */
        aud_playing = 0;
    }
}

static void aud_dma_cbr(void *arg)
{
    (void) arg;
    if (aud_stop_req || mlv_play_paused || mlv_play_render_abort || mlv_play_stopfile || mlv_play_in_gallery)
    {
        aud_stop_from_cbr();
        return;
    }

    uint32_t rd = aud_ring_rd;
    uint32_t wr = aud_ring_wr;
    uint32_t avail = wr - rd;
    uint32_t align = aud_block_align ? aud_block_align : 4;
    uint8_t *dst = aud_dma_buf[aud_dma_idx];
    if (!dst || !aud_ring)
    {
        aud_stop_from_cbr();
        return;
    }

    if (avail == 0 && aud_eof)
    {
        aud_stop_from_cbr();
        return;
    }

    uint32_t take = MIN(avail, (uint32_t)AUD_DMA_CHUNK);
    take = (take / align) * align;

    if (take > 0)
    {
        uint32_t pos = rd & AUD_RING_MASK;
        uint32_t first = MIN(take, AUD_RING_SIZE - pos);
        memcpy(dst, aud_ring + pos, first);
        if (take > first)
        {
            memcpy(dst + first, aud_ring, take - first);
        }
        aud_ring_rd = rd + take;
        aud_apply_pcm_boost(dst, take);
        SetNextASIFDACBuffer(dst, (int)take);
    }
    else
    {
        /* brief card-busy underrun: feed ~21 ms of silence so DMA stays alive */
        memset(dst, 0, 4096);
        SetNextASIFDACBuffer(dst, 4096);
    }
    aud_dma_idx ^= 1;
}

static void aud_stop(void)
{
    if (!aud_playing)
    {
        return;
    }
    aud_stop_req = 1;
    for (int i = 0; i < 30 && aud_playing; i++)
    {
        msleep(5);
    }
    /* Always call StopASIFDMADAC from the reader thread once the callback has exited:
     * - On 5D3, ensures DMA is halted if the callback timed out.
     * - On EOS-M, transitions the ASIF StateObject from State 1 (DAC_RUNNING) back to State 0 (IDLE)
     *   so subsequent playbacks can start StartASIFDMADAC again (safe here because we are NOT inside aud_dma_cbr!). */
    StopASIFDMADAC(aud_stopped_cbr, 0);
    for (int i = 0; i < 20 && aud_playing; i++)
    {
        msleep(5);
    }
    aud_playing = 0;
    audio_configure(1);
}

static void aud_set_volume(int vol)
{
    if (aud_playing && vol > 0)
    {
        SetAudioVolumeOut(aud_hw_volume(vol));
    }
}

static void aud_free_buffers(void)
{
    aud_stop();
    if (aud_ring)       { fio_free(aud_ring);       aud_ring = NULL; }
    if (aud_dma_buf[0]) { fio_free(aud_dma_buf[0]); aud_dma_buf[0] = NULL; }
    if (aud_dma_buf[1]) { fio_free(aud_dma_buf[1]); aud_dma_buf[1] = NULL; }
}

static int aud_alloc_buffers(void)
{
    if (!aud_ring)       aud_ring       = fio_malloc(AUD_RING_SIZE);
    if (!aud_dma_buf[0]) aud_dma_buf[0] = fio_malloc(AUD_DMA_CHUNK);
    if (!aud_dma_buf[1]) aud_dma_buf[1] = fio_malloc(AUD_DMA_CHUNK);
    if (!aud_ring || !aud_dma_buf[0] || !aud_dma_buf[1])
    {
        aud_free_buffers();
        return 0;
    }
    return 1;
}

static void aud_close(void)
{
    aud_free_buffers();
    aud_ready = 0;
    aud_chunks = NULL;
    aud_chunk_count = 0;
    aud_xrefs = NULL;
    aud_xref_count = 0;
    aud_xref_idx = 0;
    aud_blk_pcm_rem = 0;
    aud_eof = 0;
}

/* Scan xrefs once before playback to find WAVI (regardless of timestamp sort order) and AUDF count. */
static void aud_init_clip(FILE **chunk_files, uint32_t chunk_count, mlv_xref_t *xrefs, uint32_t entry_count)
{
    aud_close();
    if (aud_is_5d3 < 0)
    {
        aud_is_5d3 = is_camera("5D3", "*") ? 1 : 0;
    }
    if (!chunk_files || !xrefs || !entry_count) return;

    int has_audf = 0;
    mlv_wavi_hdr_t wavi;
    memset(&wavi, 0, sizeof(wavi));

    for (uint32_t i = 0; i < entry_count; i++)
    {
        if (xrefs[i].frameType == MLV_FRAME_AUDF)
        {
            has_audf = 1;
        }
        else if (xrefs[i].frameType == MLV_FRAME_UNSPECIFIED && !wavi.samplingRate &&
                 xrefs[i].fileNumber < chunk_count)
        {
            FILE *f = chunk_files[xrefs[i].fileNumber];
            mlv_hdr_t h;
            FIO_SeekSkipFile(f, xrefs[i].frameOffset, SEEK_SET);
            if (mlv_play_read_small(f, &h, sizeof(h)) == sizeof(h) && !memcmp(h.blockType, "WAVI", 4))
            {
                FIO_SeekSkipFile(f, xrefs[i].frameOffset, SEEK_SET);
                if (mlv_play_read_small(f, &wavi, sizeof(wavi)) != sizeof(wavi))
                {
                    memset(&wavi, 0, sizeof(wavi));
                }
            }
        }
    }

    if (has_audf && wavi.samplingRate > 0 && wavi.bitsPerSample == 16 &&
        wavi.channels >= 1 && wavi.channels <= 2)
    {
        aud_chunks = chunk_files;
        aud_chunk_count = chunk_count;
        aud_xrefs = xrefs;
        aud_xref_count = entry_count;
        aud_sample_rate = wavi.samplingRate;
        aud_channels = wavi.channels;
        aud_bits = wavi.bitsPerSample;
        aud_block_align = (uint32_t)(wavi.channels * (wavi.bitsPerSample / 8));
        aud_bytes_per_sec = wavi.samplingRate * aud_block_align;
        aud_ready = 1;
    }
}

/* Advance to the next AUDF block in xrefs[]; returns 1 if a block with PCM data is ready. */
static int aud_next_block(void)
{
    while (aud_xref_idx < aud_xref_count)
    {
        uint32_t i = aud_xref_idx++;
        if (aud_xrefs[i].frameType != MLV_FRAME_AUDF) continue;
        if (aud_xrefs[i].fileNumber >= aud_chunk_count) continue;

        FILE *f = aud_chunks[aud_xrefs[i].fileNumber];
        mlv_audf_hdr_t ah;
        FIO_SeekSkipFile(f, aud_xrefs[i].frameOffset, SEEK_SET);
        if (mlv_play_read_small(f, &ah, sizeof(ah)) != sizeof(ah)) continue;

        uint32_t hdr_total = (uint32_t)sizeof(ah) + ah.frameSpace;
        if (ah.blockSize <= hdr_total) continue;

        aud_blk_file_num = aud_xrefs[i].fileNumber;
        aud_blk_pcm_pos = aud_xrefs[i].frameOffset + hdr_total;
        aud_blk_pcm_rem = ah.blockSize - hdr_total;
        return 1;
    }
    aud_eof = 1;
    return 0;
}

/* Reader task: top up ring buffer directly from MLV AUDF blocks (never called from DMA ISR) */
static void aud_pump(void)
{
    if (!aud_ready || !aud_ring || aud_eof)
    {
        return;
    }
    uint32_t wr = aud_ring_wr;
    uint32_t rd = aud_ring_rd;
    uint32_t free_sp = AUD_RING_SIZE - (wr - rd);
    if (free_sp < 16384 && aud_playing)
    {
        return;
    }

    uint32_t budget = MIN(free_sp, (uint32_t)24576);
    while (budget > 0 && !aud_eof)
    {
        if (aud_blk_pcm_rem == 0)
        {
            if (!aud_next_block()) break;
        }
        uint32_t to_read = MIN(budget, aud_blk_pcm_rem);
        uint32_t pos = wr & AUD_RING_MASK;
        uint32_t first = MIN(to_read, AUD_RING_SIZE - pos);
        FILE *f = aud_chunks[aud_blk_file_num];
        FIO_SeekSkipFile(f, aud_blk_pcm_pos, SEEK_SET);
        int32_t r = FIO_ReadFile(f, aud_ring + pos, first);
        if (r <= 0)
        {
            aud_blk_pcm_rem = 0;
            continue;
        }
        aud_blk_pcm_pos += r;
        aud_blk_pcm_rem -= (uint32_t)r;
        wr += (uint32_t)r;
        aud_ring_wr = wr;
        budget -= (uint32_t)r;
    }
}

/* Reader task: start ASIF DMA synced to video frameIndex */
static void aud_start_at_frame(uint32_t frame_idx, uint32_t fps_nom, uint32_t fps_den)
{
    if (!aud_ready || mlv_play_audio <= 0 || aud_playing)
    {
        return;
    }
    if (!aud_alloc_buffers())
    {
        return;
    }
    if (!fps_nom) { fps_nom = 24000; fps_den = 1001; }
    uint32_t align = aud_block_align ? aud_block_align : 4;
    uint64_t skip = ((uint64_t)frame_idx * fps_den * aud_bytes_per_sec) / fps_nom;
    skip = (skip / align) * align;

    aud_xref_idx = 0;
    aud_blk_pcm_rem = 0;
    aud_ring_rd = 0;
    aud_ring_wr = 0;
    aud_eof = 0;
    aud_stop_req = 0;

    /* Seek to byte offset across AUDF blocks */
    while (skip > 0 && aud_next_block())
    {
        if (skip < aud_blk_pcm_rem)
        {
            aud_blk_pcm_pos += (int64_t)skip;
            aud_blk_pcm_rem -= (uint32_t)skip;
            skip = 0;
        }
        else
        {
            skip -= aud_blk_pcm_rem;
            aud_blk_pcm_rem = 0;
        }
    }
    if (aud_eof) return;

    /* Pre-fill ring buffer before starting DMA */
    aud_pump();
    aud_pump();

    uint32_t avail = aud_ring_wr - aud_ring_rd;
    uint32_t n0 = MIN(avail, (uint32_t)AUD_DMA_CHUNK);
    n0 = (n0 / align) * align;
    if (n0 == 0) return;

    memcpy(aud_dma_buf[0], aud_ring, n0);
    aud_apply_pcm_boost(aud_dma_buf[0], n0);
    aud_ring_rd += n0;

    avail = aud_ring_wr - aud_ring_rd;
    uint32_t n1 = MIN(avail, (uint32_t)AUD_DMA_CHUNK);
    n1 = (n1 / align) * align;
    if (n1 > 0)
    {
        memcpy(aud_dma_buf[1], aud_ring + aud_ring_rd, n1);
        aud_apply_pcm_boost(aud_dma_buf[1], n1);
        aud_ring_rd += n1;
    }

    aud_dma_idx = 0;
    aud_playing = 1;
    SetSamplingRate((int)aud_sample_rate, 1);
    MEM(0xC0920210) = (aud_channels == 2 ? 2 : 0) | 4; /* 16-bit mono(4) or stereo(6) */
    PowerAudioOutput();
    audio_configure(1);
    SetAudioVolumeOut(aud_hw_volume(mlv_play_audio));
    StartASIFDMADAC(aud_dma_buf[0], (int)n0, n1 ? aud_dma_buf[1] : NULL, (int)n1, aud_dma_cbr, 0);
}

static int binning_skipping_y = 0;
static int binning_skipping_x = 0;

static void mlv_play_render_frame(frame_buf_t *buffer)
{
    sp_rendered_ok = 0;
    if (buffer->isProxy)
    {
        prx_blit(buffer);
        return;
    }

    /* superpixel renderer does not use raw_info / raw_set_geometry / raw_force_aspect_ratio */
    sp_auto = (mlv_play_quality != MLV_PLAY_QUALITY_SUPER) && (buffer->bitDepth != 14) && !raw_twk_available();
    if (mlv_play_quality == MLV_PLAY_QUALITY_SUPER || sp_auto)
    {
        sp_render(buffer);
        return;
    }

    raw_info.buffer = buffer->frameData;
    raw_info.bits_per_pixel = buffer->bitDepth;
    raw_info.black_level = buffer->blackLevel;
    raw_info.white_level = buffer->whiteLevel;
    raw_set_geometry(buffer->xRes, buffer->yRes, 0, 0, 0, 0);

    binning_skipping_y = buffer->yBinSkip;
    binning_skipping_x = buffer->xBinSkip;

    raw_force_aspect_ratio(binning_skipping_x, binning_skipping_y);

#if MLV_PLAY_PERF
    /* once per window: measure fixed overhead (LUT build + malloc) with an empty line range */
    if (!perf_lut_us && !raw_twk_available())
    {
        uint64_t t0 = get_us_clock();
        raw_preview_fast_ex((void*)-1, (void*)-1, 100, 100, mlv_play_quality);
        perf_lut_us = MAX((uint32_t)(get_us_clock() - t0), 1);
    }
#endif

    if(raw_twk_available())
    {
        raw_twk_render_ex(buffer->frameData, buffer->xRes, buffer->yRes, buffer->bitDepth, mlv_play_quality, buffer->blackLevel);
    }
    else
    {
        raw_preview_fast_ex((void*)-1,(void*)-1,-1,-1,mlv_play_quality);

        if (!mlv_play_paused)
        {
            check_dup_frame(buffer);
        }
    }
}

/* Info text over the video (BMP layer). Three of the four strings are constant for a clip and
 * the BMP layer keeps them between frames, so a full repaint (slow: idle-buffer copy + RBF text)
 * only happens when needed: INFO toggled / menu closed (mlv_play_info == 2), a constant string
 * changed (new clip), or every HUD_FULL_EVERY frames as a safety net. In between, only the
 * frame counter is repainted, every HUD_COUNTER_EVERY frames and only if it changed. */
#define HUD_FULL_EVERY    150
#define HUD_COUNTER_EVERY 5
static char hud_last[4][SCREEN_MSG_LEN];

static int hud_text_changed(const char *a, const char *b)
{
    return strcmp(a, b) != 0;
}

static void mlv_play_hud_text(frame_buf_t *buffer, uint32_t *loop)
{
    if (!mlv_play_info)
    {
        *loop = 0;
        return;
    }

    if (mlv_play_info == 2)
    {
        *loop = 0;
        mlv_play_info = 1;
    }

    int full = ((*loop) % HUD_FULL_EVERY) == 0
        || hud_text_changed(hud_last[0], buffer->messages.topLeft)
        || hud_text_changed(hud_last[1], buffer->messages.topRight)
        || hud_text_changed(hud_last[2], buffer->messages.botLeft);

    if (full)
    {
        BMP_LOCK
        (
            bmp_idle_copy(0, 1);
            bmp_draw_to_idle(1);
#if MLV_PLAY_PERF
            bmp_fill(COLOR_BG, 0, 0, 720, (5 + AB_PHASES) * font_med.height);
#else
            bmp_fill(COLOR_BG, 0, 0, 720, 2 * font_med.height);
#endif
            bmp_printf(FONT(FONT_MED, COLOR_WHITE, COLOR_BG), 0, 0, buffer->messages.topLeft);
            bmp_printf(FONT(FONT_MED, COLOR_WHITE, COLOR_BG) | FONT_ALIGN_RIGHT, os.x_max, 0, buffer->messages.topRight);
            bmp_printf(FONT(FONT_MED, COLOR_WHITE, COLOR_BG), 0, font_med.height, buffer->messages.botLeft);
            bmp_printf(FONT(FONT_MED, COLOR_WHITE, COLOR_BG) | FONT_ALIGN_RIGHT, os.x_max, font_med.height, buffer->messages.botRight);
#if MLV_PLAY_PERF
            bmp_printf(FONT(FONT_MED, COLOR_YELLOW, COLOR_BG), 0, 2 * font_med.height, "%s", perf_line);
            bmp_printf(FONT(FONT_MED, COLOR_YELLOW, COLOR_BG), 0, 3 * font_med.height, "%s", perf_line2);
            bmp_printf(FONT(FONT_MED, COLOR_CYAN, COLOR_BG), 0, 4 * font_med.height, "%s", ab_status);
            for (int i = 0; i < AB_PHASES; i++)
            {
                if (ab_result[i][0])
                {
                    bmp_printf(FONT(FONT_MED, COLOR_WHITE, COLOR_BG), 0, (5 + i) * font_med.height, "%s", ab_result[i]);
                }
            }
#endif
            bmp_draw_to_idle(0);
            bmp_idle_copy(1, 0);
        )
        snprintf(hud_last[0], SCREEN_MSG_LEN, "%s", buffer->messages.topLeft);
        snprintf(hud_last[1], SCREEN_MSG_LEN, "%s", buffer->messages.topRight);
        snprintf(hud_last[2], SCREEN_MSG_LEN, "%s", buffer->messages.botLeft);
        snprintf(hud_last[3], SCREEN_MSG_LEN, "%s", buffer->messages.botRight);
    }
    else if (((*loop) % HUD_COUNTER_EVERY) == 0)
    {
        if (hud_text_changed(hud_last[3], buffer->messages.botRight))
        {
            bmp_printf(FONT(FONT_MED, COLOR_WHITE, COLOR_BG) | FONT_ALIGN_RIGHT, os.x_max, font_med.height, "   %s", buffer->messages.botRight);
            snprintf(hud_last[3], SCREEN_MSG_LEN, "%s", buffer->messages.botRight);
        }
#if MLV_PLAY_PERF
        bmp_printf(FONT(FONT_MED, COLOR_YELLOW, COLOR_BG), 0, 2 * font_med.height, "%s   ", perf_line);
        bmp_printf(FONT(FONT_MED, COLOR_YELLOW, COLOR_BG), 0, 3 * font_med.height, "%s   ", perf_line2);
        bmp_printf(FONT(FONT_MED, COLOR_CYAN, COLOR_BG), 0, 4 * font_med.height, "%s   ", ab_status);
        for (int i = 0; i < AB_PHASES; i++)
        {
            if (ab_result[i][0])
            {
                bmp_printf(FONT(FONT_MED, COLOR_WHITE, COLOR_BG), 0, (5 + i) * font_med.height, "%s   ", ab_result[i]);
            }
        }
#endif
    }
    (*loop)++;
}

static void mlv_play_draw_hud(frame_buf_t *buffer, uint32_t *redraw_loop)
{
    mlv_play_hud_text(buffer, redraw_loop);

    if (buffer->totalFrames > 1 && mlv_play_osd_state != MLV_PLAY_MENU_SHOWN && mlv_play_osd_state != MLV_PLAY_MENU_FADEIN)
    {
        int bx = 24, by = 470, bw = 672, bh = 5;
        uint32_t idx = MIN(buffer->frameIndex + 1, buffer->totalFrames);
        int fill_w = (int)((uint64_t)bw * idx / buffer->totalFrames);
        bmp_fill(COLOR_GRAY(25), bx, by, bw, bh);
        if (fill_w > 0)
        {
            bmp_fill(mlv_play_paused ? COLOR_ORANGE : COLOR_CYAN, bx, by, fill_w, bh);
        }
    }
}

static void mlv_play_render_task(uint32_t priv)
{
    uint32_t redraw_loop = 0;
    
    frame_buf_t *buffer_paused = NULL;
    mlv_play_render_alive = 1;
    
    TASK_LOOP
    {
        frame_buf_t *buffer;
        
        /* signal to stop rendering */
        if(mlv_play_render_abort)
        {
            break;
        }
        
        /* user exited from playback */
        if(gui_state != GUISTATE_PLAYMENU)
        {
            break;
        }

        /* while gallery is active, release paused/queued frames and stay off VRAM/BMP */
        if (mlv_play_in_gallery)
        {
            if (buffer_paused)
            {
                msg_queue_post(mlv_play_queue_empty, (uint32_t) buffer_paused);
                buffer_paused = NULL;
            }
            uint32_t cnt = 0;
            msg_queue_count(mlv_play_queue_render, &cnt);
            while (cnt-- > 0)
            {
                frame_buf_t *stale = NULL;
                if (!msg_queue_receive(mlv_play_queue_render, &stale, 50) && stale)
                {
                    msg_queue_post(mlv_play_queue_empty, (uint32_t) stale);
                }
            }
            mlv_play_render_in_gallery_ack = 1;
            msleep(40);
            continue;
        }
        mlv_play_render_in_gallery_ack = 0;

        /* release paused buffer immediately when resuming playback or stopping current file */
        if ((!mlv_play_paused || mlv_play_should_stop()) && buffer_paused)
        {
            msg_queue_post(mlv_play_queue_empty, (uint32_t) buffer_paused);
            buffer_paused = NULL;
        }
        
        if(mlv_play_paused && !mlv_play_should_stop() && buffer_paused)
        {
            if (mlv_play_lut_dirty)
            {
                mlv_play_lut_dirty = 0;
                if (!buffer_paused->isProxy)
                {
                    mlv_play_render_frame(buffer_paused);
                }
            }
            msleep(50);
            continue;
        }

        if (mlv_play_stopfile || mlv_play_delete_requested)
        {
            uint32_t cnt = 0;
            msg_queue_count(mlv_play_queue_render, &cnt);
            while (cnt-- > 0)
            {
                frame_buf_t *stale = NULL;
                if (!msg_queue_receive(mlv_play_queue_render, &stale, 20) && stale)
                {
                    msg_queue_post(mlv_play_queue_empty, (uint32_t) stale);
                }
            }
            msleep(20);
            continue;
        }
        
        /* is there something to render? */
        if(msg_queue_receive(mlv_play_queue_render, &buffer, 50))
        {
            continue;
        }
        
        /* if in pause mode, render once for pause display and hold buffer */
        if(mlv_play_paused)
        {
            mlv_play_render_frame(buffer);
            prx_after_render(buffer);
            buffer_paused = buffer;
            continue;
        }

        if(!buffer->frameBuffer)
        {
            bmp_printf(FONT_MED, 30, 400, "buffer empty");
            beep();
            msleep(1000);
            msg_queue_post(mlv_play_queue_empty, (uint32_t) buffer);
            break;
        }

#if MLV_PLAY_PERF
        if (prx_mode != PRX_BUILD && strcmp(buffer->messages.botLeft, ab_file) != 0)
        {
            ab_start(buffer->messages.botLeft);
        }
#endif

        {
            uint64_t t0 = get_us_clock();
            if (mlv_play_bench != 1 && mlv_play_bench != 3)
            {
                mlv_play_render_frame(buffer);
                prx_after_render(buffer);
            }
            uint32_t dt = (uint32_t)(get_us_clock() - t0);
            perf_acc.render_us += dt;
            perf_acc.render_frames++;
            perf_render_max = MAX(perf_render_max, dt);
            uint64_t now_us = get_us_clock();
            if (perf_last_frame_us)
            {
                perf_gap_max = MAX(perf_gap_max, (uint32_t)(now_us - perf_last_frame_us));
            }
            perf_last_frame_us = now_us;
#if MLV_PLAY_PERF
            perf_window_tick();
#endif
        }
        
        /* info text over the video (repaints only what changed, see mlv_play_hud_text) */
        mlv_play_hud_text(buffer, &redraw_loop);

        /* slim bottom timeline progress bar (when OSD menu is hidden) */
        if (buffer->totalFrames > 1 && mlv_play_osd_state != MLV_PLAY_MENU_SHOWN && mlv_play_osd_state != MLV_PLAY_MENU_FADEIN)
        {
            int bx = 24, by = 470, bw = 672, bh = 5;
            uint32_t idx = MIN(buffer->frameIndex + 1, buffer->totalFrames);
            int fill_w = (int)((uint64_t)bw * idx / buffer->totalFrames);
            bmp_fill(COLOR_GRAY(25), bx, by, bw, bh);
            if (fill_w > 0)
            {
                bmp_fill(mlv_play_paused ? COLOR_ORANGE : COLOR_CYAN, bx, by, fill_w, bh);
            }
        }
        
        /* finished displaying, requeue frame buffer for refilling */
        msg_queue_post(mlv_play_queue_empty, (uint32_t) buffer);
    }
    
    if(buffer_paused)
    {
        msg_queue_post(mlv_play_queue_empty, (uint32_t) buffer_paused);
    }
    
    mlv_play_render_alive = 0;
    mlv_play_rendering = 0;
}

static uint32_t mlv_play_should_stop()
{
    /* powering off the camera is for sure a good reason to stop any playback */
    if(ml_shutdown_requested)
    {
        return 1;
    }
    
    /* not playing anymore because user left the current GUI state somehow */
    if(gui_state != GUISTATE_PLAYMENU)
    {
        return 1;
    }
    
    /* suddenly the render task exited, it makes no more sense to play */
    if(!mlv_play_rendering)
    {
        return 1;
    }
    
    /* the current file playback is requested to be stopped. reason might be a file change */
    if(mlv_play_stopfile)
    {
        return 1;
    }
    
    /* should the current file be deleted? stop playback first */
    if (mlv_play_delete_requested)
    {
        return 1;
    }
    
    return 0;
}

static void mlv_play_clear_screen()
{
    /* clear anything */
    vram_clear_lv();
    clrscr();
    
    /* update OSD */
    msg_queue_post(mlv_play_queue_osd, (uint32_t) 0);
    
    /* force redraw of info lines */
    mlv_play_info = mlv_play_info ? 2 : 0;
}

static void mlv_play_fps_tick(int expiry_value, void *priv)
{
    uint32_t offset = mlv_play_frame_dividers[mlv_play_frame_div_pos];
    mlv_play_frame_div_pos = (mlv_play_frame_div_pos + 1) % 3;

    if(mlv_play_timer_stop)
    {
        mlv_play_timer_stop = 0;
        return;
    }

    if (!mlv_play_paused)
    {
        msg_queue_post(mlv_play_queue_fps, 0);
    }

    SetHPTimerNextTick(expiry_value, offset, &mlv_play_fps_tick, &mlv_play_fps_tick, NULL);
}

static void mlv_play_stop_fps_timer()
{
    mlv_play_timer_stop = 1;
    for (int t = 0; t < 25 && mlv_play_timer_stop; t++)
    {
        msleep(20);
    }
    mlv_play_timer_stop = 0;
}

static void mlv_play_start_fps_timer(uint32_t fps_nom, uint32_t fps_denom)
{
    if (fps_nom == 0 || fps_nom < 2 * fps_denom)
    {
        /* fps too low or bad metadata? play at 24 fps */
        fps_denom = MAX(fps_denom, 1);
        fps_nom = 24 * fps_denom;
    }
    
    uint32_t three_frames = 3 * 1000000 * fps_denom / fps_nom;

    mlv_play_frame_dividers[0] = mlv_play_frame_dividers[1] = mlv_play_frame_dividers[2] = three_frames / 3;
    switch(three_frames % 3)
    {
        case 2:
            mlv_play_frame_dividers[1]++;
        case 1:
            mlv_play_frame_dividers[0]++;
        default:
            break;
    }

    /* ensure the queue is empty */
    mlv_play_flush_queue(mlv_play_queue_fps);
    
    /* reset counters */
    mlv_play_frame_div_pos = 0;
    mlv_play_timer_stop = 0;
    mlv_play_frames_skipped = 0;
    
    /* and finally start timer in 1 ms (safer than 1 us against wraparound) */
    SetHPTimerAfterNow(1000, &mlv_play_fps_tick, &mlv_play_fps_tick, NULL);
}


static void mlv_play_mlv(char *filename, FILE **chunk_files, uint32_t chunk_count)
{
    uint32_t fps_timer_started = 0;
    uint32_t frame_size = 0;
    uint32_t frame_count = 0;
    mlv_xref_hdr_t *block_xref = NULL;
    mlv_lens_hdr_t lens_block;
    mlv_rawi_hdr_t rawi_block;
    mlv_rawc_hdr_t rawc_block;
    mlv_wavi_hdr_t wavi_block;
    mlv_rtci_hdr_t rtci_block;
    mlv_file_hdr_t main_header;

    void *mlv_play_decomp_buf = NULL;
    uint32_t mlv_play_decomp_cap = 0;
    struct memSuite *mlv_play_decomp_suite = NULL;
    frame_buf_t *parked_bufs[3] = { NULL, NULL, NULL };
    int parked_count = 0;
    int alloc_ok_count = 0;

    /* make sure there is no crap in stack variables */
    memset(&lens_block, 0x00, sizeof(lens_block));
    memset(&rawi_block, 0x00, sizeof(rawi_block));
    memset(&rawc_block, 0x00, sizeof(rawc_block));
    memset(&wavi_block, 0x00, sizeof(wavi_block));
    memset(&rtci_block, 0x00, sizeof(rtci_block));
    memset(&main_header, 0x00, sizeof(main_header));
    
    /* read footer information and update global variables, will seek automatically */
    if(chunk_count < 1 || !mlv_play_is_mlv(chunk_files[0]))
    {
        bmp_printf(FONT_LARGE, 30, 100, "Invalid MLV:", filename);
        bmp_printf(FONT_MED, 40, 100 + font_large.height + 1, filename);
        return;
    }
    
    /* load or create index file */
    block_xref = mlv_play_get_index(filename, chunk_files, chunk_count);
    if (!block_xref)
    {
        bmp_printf(FONT_LARGE, 30, 100, "Index error:", filename);
        bmp_printf(FONT_MED, 40, 100 + font_large.height + 1, filename);
        return;
    }

    mlv_xref_t *xrefs = (mlv_xref_t *)&(((uint8_t*)block_xref)[sizeof(mlv_xref_hdr_t)]);
    
    /* index building would print on screen */
    mlv_play_clear_screen();
    
    uint32_t block_xref_pos = 0;
    int prx_decided = 0;
    aud_init_clip(chunk_files, chunk_count, xrefs, block_xref->entryCount);
    while(1)
    {
        /* after playback is finished, return to beginning and put into pause */
        if (block_xref_pos == block_xref->entryCount)
        {
            /* reset counter */
            block_xref_pos = 0;

            /* next pass: a PRX finished in this pass can be played, an unfinished one rebuilt */
            prx_decided = 0;
            aud_stop();

            /* miscellaneous cleanup */
            mlv_play_flush_queue(mlv_play_queue_fps);
            mlv_play_info = (mlv_play_info == 1 ? 2 : mlv_play_info);

            mlv_play_paused = 1;
        }

        /* no not pause reader anymore, the renderer might still need a frame */
        //while(mlv_play_paused && !mlv_play_should_stop())
        //{
        //    msleep(100);
        //}

        /* there are various reasons why this read/play task should get stopped */
        if(mlv_play_should_stop())
        {
            break;
        }

        /* AUDF blocks are streamed on-demand by aud_pump() during PRX_PLAY */
        if (xrefs[block_xref_pos].frameType == MLV_FRAME_AUDF)
        {
            block_xref_pos++;
            continue;
        }

        /* if in exact playback and this is a skippable VIDF frame */
        if(mlv_play_use_exact())
        {
            if (xrefs[block_xref_pos].frameType == MLV_FRAME_VIDF)
            {
                uint32_t frames_to_skip = 0;
                msg_queue_count(mlv_play_queue_fps, &frames_to_skip);

                /* skip this frame if we are behind */
                if(frames_to_skip > 0)
                {
                    uint32_t temp = 0;
                    msg_queue_receive(mlv_play_queue_fps, &temp, 50);

                    mlv_play_frames_skipped++;
                    block_xref_pos++;
                    continue;
                }
            }
        }
        else
        {
            /* if not, just keep the queue clean */
            mlv_play_flush_queue(mlv_play_queue_fps);
        }

        /* get the file and position of the next block */
        uint32_t in_file_num = xrefs[block_xref_pos].fileNumber;
        int64_t position = xrefs[block_xref_pos].frameOffset;
        
        /* select file and seek to the right position */
        FILE *in_file = chunk_files[in_file_num];
        
        /* use the common header structure to get file size */
        mlv_hdr_t buf;
        
        FIO_SeekSkipFile(in_file, position, SEEK_SET);
        if(mlv_play_read_small(in_file, &buf, sizeof(mlv_hdr_t)) != sizeof(mlv_hdr_t))
        {
            bmp_printf(FONT_MED, 30, 190, "File ends prematurely during block header");
            beep();
            msleep(1000);
            break;
        }
        FIO_SeekSkipFile(in_file, position, SEEK_SET);
        
        /* special case: if first block read, reset frame count as all MLVI blocks frame count will get accumulated */
        if(block_xref_pos == 0)
        {
            frame_count = 0;
        }
        
        /* file header */
        if(!memcmp(buf.blockType, "MLVI", 4))
        {
            mlv_file_hdr_t file_hdr;
            uint32_t hdr_size = MIN(sizeof(mlv_file_hdr_t), buf.blockSize);

            /* read the whole header block, but limit size to either our local type size or the written block size */
            if(FIO_ReadFile(in_file, &file_hdr, hdr_size) != (int32_t)hdr_size)
            {
                bmp_printf(FONT_MED, 30, 190, "File ends prematurely during MLVI");
                beep();
                msleep(1000);
                break;
            }

            frame_count += file_hdr.videoFrameCount;
            
            /* is this the first file? */
            if(file_hdr.fileNum == 0)
            {
                memcpy(&main_header, &file_hdr, sizeof(mlv_file_hdr_t));
            }
            else
            {
                /* no, its another chunk */
                if(main_header.fileGuid != file_hdr.fileGuid)
                {
                    bmp_printf(FONT_MED, 30, 190, "Error: GUID within the file chunks mismatch!");
                    beep();
                    msleep(1000);
                    break;
                }
            }
        }
        else if(!memcmp(buf.blockType, "LENS", 4))
        {
            if(FIO_ReadFile(in_file, &lens_block, sizeof(mlv_lens_hdr_t)) != sizeof(mlv_lens_hdr_t))
            {
                bmp_printf(FONT_MED, 30, 190, "File ends prematurely during LENS");
                beep();
                msleep(1000);
                break;
            }
        }
        else if(!memcmp(buf.blockType, "RTCI", 4))
        {
            if(FIO_ReadFile(in_file, &rtci_block, sizeof(mlv_rtci_hdr_t)) != sizeof(mlv_rtci_hdr_t))
            {
                bmp_printf(FONT_MED, 30, 190, "File ends prematurely during RTCI");
                beep();
                msleep(1000);
                break;
            }
        }
        else if(!memcmp(buf.blockType, "RAWI", 4))
        {
            if(FIO_ReadFile(in_file, &rawi_block, sizeof(mlv_rawi_hdr_t)) != sizeof(mlv_rawi_hdr_t))
            {
                bmp_printf(FONT_MED, 30, 190, "File ends prematurely during RAWI");
                beep();
                msleep(1000);
                break;
            }
            
            frame_size = rawi_block.xRes * rawi_block.yRes * rawi_block.raw_info.bits_per_pixel / 8;
            bits_per_pixel = rawi_block.raw_info.bits_per_pixel;
        }
        else if(!memcmp(buf.blockType, "RAWC", 4))
        {
            if(FIO_ReadFile(in_file, &rawc_block, sizeof(mlv_rawc_hdr_t)) != sizeof(mlv_rawc_hdr_t))
            {
                bmp_printf(FONT_MED, 30, 190, "File ends prematurely during RAWC");
                beep();
                msleep(1000);
                break;
            }
            binning_skipping_y = raw_capture_info.binning_y + raw_capture_info.skipping_y;
            binning_skipping_x = raw_capture_info.binning_x + raw_capture_info.skipping_x;
        }
        else if(!memcmp(buf.blockType, "WAVI", 4))
        {
            if(FIO_ReadFile(in_file, &wavi_block, sizeof(mlv_wavi_hdr_t)) != sizeof(mlv_wavi_hdr_t))
            {
                bmp_printf(FONT_MED, 30, 190, "File ends prematurely during WAVI");
                beep();
                msleep(1000);
                break;
            }
        }
        else if(!memcmp(buf.blockType, "AUDF", 4))
        {
            /* handled directly by aud_pump() during PRX_PLAY */
        }
        else if(!memcmp(buf.blockType, "VIDF", 4))
        {
            mlv_vidf_hdr_t vidf_block;
            
            if(mlv_play_read_small(in_file, &vidf_block, sizeof(mlv_vidf_hdr_t)) != sizeof(mlv_vidf_hdr_t))
            {
                bmp_printf(FONT_MED, 30, 190, "File ends prematurely during VIDF");
                beep();
                msleep(1000);
                break;
            }

            if (!prx_decided)
            {
                /* a PRX build from the previous pass may still have frames queued for rendering
                 * (end of clip pauses playback): let it finish or abort before deciding again.
                 * In inline render mode (EOS-M), all previous frames are already rendered, so
                 * any still-open build is incomplete and must be closed immediately. */
                if (!mlv_play_render_alive && prx_mode == PRX_BUILD)
                {
                    prx_close();
                }
                while (prx_mode == PRX_BUILD && !mlv_play_should_stop())
                {
                    msleep(20);
                }
                prx_decided = 1;
                int bpp_in = rawi_block.raw_info.bits_per_pixel;
                int will_super = (mlv_play_quality == MLV_PLAY_QUALITY_SUPER) || (bpp_in != 14 && !raw_twk_available());
#if MLV_PLAY_PROXY
                prx_setup(filename, main_header.fileGuid, frame_count,
                          rawi_block.xRes, rawi_block.yRes, bpp_in,
                          rawc_block.binning_x + rawc_block.skipping_x,
                          rawc_block.binning_y + rawc_block.skipping_y, will_super);
#else
                (void) bpp_in;
                (void) will_super;
#endif
                if (prx_mode == PRX_BUILD)
                {
                    /* every frame >= prx_next must be rendered by sp_render in order */
                    if (prx_next == 0)
                    {
                        aud_free_buffers();
                    }
                    mlv_play_read_mode = 0;
                    mlv_play_bench = 0;
                }
                else if (prx_mode == PRX_PLAY)
                {
                    /* proxy playback needs zero LJ92 decompression RAM: free 3-5 MB decomp suite if allocated */
                    if (mlv_play_decomp_suite)
                    {
                        DeleteMemorySuite(mlv_play_decomp_suite);
                        mlv_play_decomp_suite = NULL;
                    }
                    if (mlv_play_decomp_buf)
                    {
                        fio_free(mlv_play_decomp_buf);
                        mlv_play_decomp_buf = NULL;
                        mlv_play_decomp_cap = 0;
                    }
                }
            }
        
            frame_buf_t *buffer = NULL;
            
            /* now get a buffer from the queue (always poll inline OSD if osd_task is not running, e.g. on EOS-M) */
            while (msg_queue_receive(mlv_play_queue_empty, &buffer, 50) && !mlv_play_should_stop())
            {
                if (!mlv_play_osd_alive)
                {
                    mlv_play_inline_osd_poll(1);
                }
            }

            if (mlv_play_should_stop())
            {
                /* if we also got a buffer, play nicely and give it back */
                if(buffer)
                {
                    msg_queue_post(mlv_play_queue_empty, (uint32_t) buffer);
                }
                break;
            }
            
            /* check if the queued buffer has the correct size.
             * Only allocate 16-bit room when raw_twk will actually be used; superpixel uses 14-bit for LJ92. */
            int use_16bit = (main_header.videoClass & MLV_VIDEO_CLASS_FLAG_LJ92) &&
                            raw_twk_available() && (mlv_play_quality != MLV_PLAY_QUALITY_SUPER);
            uint32_t alloc_size;
            if (use_16bit)
                alloc_size = (uint32_t)rawi_block.xRes * rawi_block.yRes * 2 + 0x1100;
            else if (main_header.videoClass & MLV_VIDEO_CLASS_FLAG_LJ92)
                alloc_size = (uint32_t)rawi_block.xRes * rawi_block.yRes * 14 / 8 + 0x1100;
            else
                alloc_size = (uint32_t)frame_size + 0x1100; /* +0x1000 alignment, +0x100 read-ahead */

            if(buffer->frameSize != frame_size || buffer->allocSize != alloc_size)
            {
                /* the first few queued don't have anything allocated, so don't free */
                if(buffer->frameBuffer)
                {
                    fio_free(buffer->frameBuffer);
                    buffer->frameBuffer = NULL;
                }
                
                buffer->frameSize = frame_size;
                buffer->allocSize = alloc_size;
                buffer->filledSize = 0;
                buffer->frameBuffer = fio_malloc(alloc_size);
                if (buffer->frameBuffer)
                {
                    buffer->frameBufferAligned = (void *) (((uint32_t)buffer->frameBuffer + 0x1000) & ~0xFFF);
                    alloc_ok_count++;
                }
                else
                {
                    buffer->frameSize = 0;
                    buffer->allocSize = 0;
                    buffer->frameBufferAligned = NULL;
                }
            }

            if(!buffer->frameBuffer)
            {
                /* Low-RAM fallback (e.g. EOS-M 4K/5K LJ92): if we already have 1+ working buffer(s),
                 * park this extra shell and retry the same frame using an existing buffer. */
                if (alloc_ok_count > 0 && parked_count < 3)
                {
                    parked_bufs[parked_count++] = buffer;
                    continue;
                }
                msg_queue_post(mlv_play_queue_empty, (uint32_t) buffer);
                bmp_printf(FONT_MED, 30, 400, "Not enough RAM for %dx%d", rawi_block.xRes, rawi_block.yRes);
                beep();
                msleep(1500);
                break;
            }
            
            int32_t read_size = MIN(frame_size, vidf_block.blockSize - (sizeof(mlv_vidf_hdr_t) + vidf_block.frameSpace));
            void *read_buffer = buffer->frameBufferAligned;

            /* safety check to make sure the format matches, but allow the saved block to be larger (some dummy data at the end of frame is allowed) */
            if((uint32_t)read_size > buffer->frameSize)
            {
                bmp_printf(FONT_MED, 30, 400, "ERROR 0x%X 0x%X 0x%X 0x%X 0x%X", read_size, frame_size, buffer->frameSize, vidf_block.frameSpace, vidf_block.blockSize);
                beep();
                msleep(10000);
                break;
            } 
            
            /* skip frame space */
            FIO_SeekSkipFile(in_file, position + sizeof(mlv_vidf_hdr_t) + vidf_block.frameSpace, SEEK_SET);

            /* bench seq: compare back-to-back big reads with the per-frame pattern
             * (seek + 16 B block header + seek + 32 B VIDF header + seek + big read) */
            if (mlv_play_bench == 3 && seq_state == 0)
            {
                /* bench seq: back-to-back reads with different chunk sizes, ~16 MB each.
                 * MB/s rising with chunk size => fixed per-call overhead in Canon's driver */
                seq_state = 1;
                static const uint32_t sizes[] = { 128 << 10, 512 << 10, 1 << 20, 2 << 20, 4 << 20 };
                int64_t p = (position + sizeof(mlv_vidf_hdr_t) + vidf_block.frameSpace) & ~511LL;
                uint8_t *dst = buffer->frameBufferAligned;
                uint32_t room = buffer->allocSize > 0x1100 ? buffer->allocSize - 0x1100 : 0;
                char *out = seq_result;
                int left = SCREEN_MSG_LEN;
                int n0 = snprintf(out, left, "seq");
                out += n0; left -= n0;
                FIO_SeekSkipFile(in_file, p, SEEK_SET);
                for (int k = 0; k < (int)COUNT(sizes) && left > 12; k++)
                {
                    uint32_t len = MIN(sizes[k], room & ~511);
                    if (!len)
                    {
                        break;
                    }
                    uint32_t calls = (16 << 20) / len;
                    int ok = 1;
                    uint64_t t0 = get_us_clock();
                    for (uint32_t i = 0; ok && i < calls; i++)
                    {
                        ok = FIO_ReadFile(in_file, dst, len) == (int32_t)len;
                    }
                    uint32_t us = (uint32_t)(get_us_clock() - t0);
                    uint32_t mb10 = (ok && us) ? (uint32_t)((uint64_t)calls * len * 10 / us) : 0;
                    int w = snprintf(out, left, " %dK:%d.%d", len >> 10, mb10 / 10, mb10 % 10);
                    out += w; left -= w;
                    if (!ok)
                    {
                        break;
                    }
                }
                buffer->filledSize = 0;   /* contents are no longer this frame */
                seq_state = 2;
            }

            /* proxy frame instead of raw data? (also plays 0..prx_next-1 prefix when resuming PRX_BUILD) */
            int bench_skip = (mlv_play_bench == 2) && (buffer->filledSize == frame_size);
            int was_proxy = buffer->isProxy;
            int prx_hit = 0;
            if ((prx_mode == PRX_PLAY || prx_mode == PRX_BUILD) && !mlv_play_zoom)
            {
                if (bench_skip && was_proxy)
                {
                    prx_hit = 1;
                }
                else
                {
                    uint64_t t = get_us_clock();
                    prx_hit = prx_read_frame(buffer, vidf_block.frameNumber);
                    if (prx_hit)
                    {
                        perf_acc.read_us += (uint32_t)(get_us_clock() - t);
                        perf_acc.read_bytes += prx_hdr.frame_bytes;
                        perf_acc.read_calls++;
                        perf_acc.read_frames++;
                        buffer->filledSize = frame_size;
                    }
                }
            }
            buffer->isProxy = prx_hit;
            if (prx_hit)
            {
                buffer->frameData = buffer->frameBufferAligned;
            }
            else
            {
                /* transitioning from proxy prefix to raw/build (or playing raw): free audio RAM before decode */
                aud_free_buffers();
                if (main_header.videoClass & MLV_VIDEO_CLASS_FLAG_LJ92)
                {
                    uint32_t need_comp = (uint32_t)read_size;
                    if (!mlv_play_decomp_buf || need_comp > mlv_play_decomp_cap)
                    {
                        if (mlv_play_decomp_suite)
                        {
                            DeleteMemorySuite(mlv_play_decomp_suite);
                            mlv_play_decomp_suite = NULL;
                        }
                        if (mlv_play_decomp_buf)
                        {
                            fio_free(mlv_play_decomp_buf);
                            mlv_play_decomp_buf = NULL;
                            mlv_play_decomp_cap = 0;
                        }

                        uint32_t want = need_comp + (need_comp >> 2);
                        if (want < (512U << 10)) want = (512U << 10);
                        if (want > (uint32_t)frame_size) want = (uint32_t)frame_size;
                        uint32_t mem_size = (want + 0x1000) & ~0xFFF;

                        mlv_play_decomp_buf = fio_malloc(mem_size + 0x1000);
                        if (!mlv_play_decomp_buf && mem_size > ((need_comp + 0x1000) & ~0xFFF))
                        {
                            /* retry tight fit without headroom */
                            mem_size = (need_comp + 0x1000) & ~0xFFF;
                            mlv_play_decomp_buf = fio_malloc(mem_size + 0x1000);
                        }
                        if(!mlv_play_decomp_buf)
                        {
                            msg_queue_post(mlv_play_queue_empty, (uint32_t) buffer);
                            bmp_printf(FONT_MED, 20, 400, "Not enough RAM (LJ92)");
                            beep();
                            msleep(1500);
                            break;
                        }
                        
                        mlv_play_decomp_suite = CreateMemorySuite(mlv_play_decomp_buf, mem_size, 0);
                        if(!mlv_play_decomp_suite)
                        {
                            msg_queue_post(mlv_play_queue_empty, (uint32_t) buffer);
                            bmp_printf(FONT_MED, 20, 400, "Failed LJ92 memSuite");
                            beep();
                            msleep(1500);
                            break;
                        }
                        mlv_play_decomp_cap = mem_size;
                    }
                    read_buffer = mlv_play_decomp_buf;
                }
            }

            /* finally read the raw data */
            int is_lj92 = (main_header.videoClass & MLV_VIDEO_CLASS_FLAG_LJ92) ? 1 : 0;
            bench_skip = bench_skip && !was_proxy;
            int32_t perf_got = read_size;
            uint64_t perf_t0 = get_us_clock();
            if (prx_hit)
            {
                /* nothing to read from the MLV */
            }
            else if (bench_skip)
            {
                /* bench rn: keep the data already in this buffer */
            }
            else
            {
                int32_t got = -1;
                int bpp_in = rawi_block.raw_info.bits_per_pixel;
                int will_super = (mlv_play_quality == MLV_PLAY_QUALITY_SUPER) || (bpp_in != 14 && !raw_twk_available());
                if (!is_lj92 && will_super && (mlv_play_read_mode == 1 || mlv_play_read_mode == 2))
                {
                    got = sp_read_rows(in_file, position + sizeof(mlv_vidf_hdr_t) + vidf_block.frameSpace,
                                       read_buffer, read_size, rawi_block.xRes, rawi_block.yRes, bpp_in,
                                       rawc_block.binning_x + rawc_block.skipping_x,
                                       rawc_block.binning_y + rawc_block.skipping_y,
                                       &perf_acc.read_calls);
                }
                buffer->frameData = buffer->frameBufferAligned;
                if (got >= 0)
                {
                    perf_acc.read_bytes += got;
                }
                else
                {
                    /* frame data in MLV files starts mid-sector (e.g. 64 bytes after a 512 boundary);
                     * read from the sector boundary instead and point frameData past the extra bytes */
                    int64_t data_pos = position + sizeof(mlv_vidf_hdr_t) + vidf_block.frameSpace;
                    uint32_t delta = (uint32_t)(data_pos & 511);
                    uint32_t len = (delta + read_size + 511) & ~511;
                    if (!is_lj92 && mlv_play_read_mode != 3 && len + 0x1000 <= buffer->allocSize)
                    {
                        FIO_SeekSkipFile(in_file, data_pos - delta, SEEK_SET);
                        int32_t r = FIO_ReadFile(in_file, read_buffer, len);
                        perf_got = (r >= (int32_t)(delta + read_size)) ? read_size : r;
                        buffer->frameData = (uint8_t *)read_buffer + delta;
                        perf_acc.read_bytes += len;
                    }
                    else
                    {
                        FIO_SeekSkipFile(in_file, data_pos, SEEK_SET);
                        perf_got = FIO_ReadFile(in_file, read_buffer, read_size);
                        perf_acc.read_bytes += read_size;
                    }
                    perf_acc.read_calls++;
                }
                buffer->filledSize = frame_size;
            }
            if (!prx_hit)
            {
                perf_acc.read_us += (uint32_t)(get_us_clock() - perf_t0);
                perf_acc.read_frames++;
            }
            if(perf_got != (int32_t)read_size)
            {
                bmp_printf(FONT_MED, 30, 190, "File ends prematurely during VIDF raw data");
                beep();
                msleep(1000);
                break;
            }
        
            /* update dimensions */
            buffer->xRes = rawi_block.xRes;
            buffer->yRes = rawi_block.yRes;
            buffer->xBinSkip = rawc_block.binning_x + rawc_block.skipping_x;
            buffer->yBinSkip = rawc_block.binning_y + rawc_block.skipping_y;
            buffer->bitDepth = rawi_block.raw_info.bits_per_pixel;
            buffer->valueBits = rawi_block.raw_info.bits_per_pixel;
            buffer->frameIndex = vidf_block.frameNumber;
            buffer->totalFrames = frame_count;
            buffer->blackLevel = rawi_block.raw_info.black_level;
            buffer->whiteLevel = rawi_block.raw_info.white_level;

            if((main_header.videoClass & MLV_VIDEO_CLASS_FLAG_LJ92) && !prx_hit)
            {
                int output_bpp = (raw_twk_available() && mlv_play_quality != MLV_PLAY_QUALITY_SUPER) ? 16 : 14;
                int r = 0;
                if (!bench_skip)
                {
                    buffer->frameData = buffer->frameBufferAligned;
                    uint64_t perf_t1 = get_us_clock();
                    r = lossless_decompress_raw(
                        mlv_play_decomp_suite, buffer->frameBufferAligned,
                        rawi_block.xRes, rawi_block.yRes, 
                        output_bpp
                    );
                    perf_acc.dec_us += (uint32_t)(get_us_clock() - perf_t1);
                }

                if (r == -1)
                {
                    bmp_printf(FONT_MED, 20, 300, "(no decompression on this model)");
                }

                /* when we have lossless data, we decompressed it already to 16 bpp for raw_twk */
                if (output_bpp == 16)
                {
                    /* also raise black and white levels */
                    buffer->blackLevel = rawi_block.raw_info.black_level << (16 - rawi_block.raw_info.bits_per_pixel);
                    buffer->whiteLevel = rawi_block.raw_info.white_level << (16 - rawi_block.raw_info.bits_per_pixel);
                    buffer->bitDepth = 16;
                    buffer->valueBits = 16;
                }
                else
                {
                    /* decompressed into 14 bpp container, values keep their original scale */
                    buffer->bitDepth = 14;
                }
            }
            
            if((main_header.videoClass & 0x0F) != MLV_VIDEO_CLASS_RAW)
            {
                bmp_printf(FONT_MED, 20, 300, "(format not supported)");
            }
            else
            {
                /* fill strings to display */
                snprintf(buffer->messages.topLeft, SCREEN_MSG_LEN, "");
                snprintf(buffer->messages.topRight, SCREEN_MSG_LEN, "");

                if(lens_block.timestamp)
                {
                    char *focusMode;
                    
                    if((lens_block.autofocusMode & 0x0F) != AF_MODE_MANUAL_FOCUS)
                    {
                        focusMode = "AF";
                    }
                    else
                    {
                        focusMode = "MF";
                    }
                    
                    snprintf(buffer->messages.topRight, SCREEN_MSG_LEN, "%s, %dmm, %s, %s", lens_block.lensName, lens_block.focalLength, lens_block.stabilizerMode?"IS":"no IS", focusMode);
                }
                    
                if(rtci_block.timestamp)
                {
                    snprintf(buffer->messages.topLeft, SCREEN_MSG_LEN, "%02d.%02d.%04d %02d:%02d:%02d", rtci_block.tm_mday, rtci_block.tm_mon + 1, 1900 + rtci_block.tm_year, rtci_block.tm_hour, rtci_block.tm_min, rtci_block.tm_sec);
                }
                
                snprintf(buffer->messages.botLeft, SCREEN_MSG_LEN, "%s: %dx%d %dbpp%s", filename, rawi_block.xRes, rawi_block.yRes, sp_real_bits(rawi_block.raw_info.bits_per_pixel, rawi_block.raw_info.black_level, rawi_block.raw_info.white_level), (main_header.videoClass & MLV_VIDEO_CLASS_FLAG_LJ92)?" LJ92":"");
                snprintf(buffer->messages.botRight, SCREEN_MSG_LEN, "%s%d/%d",
                         prx_hit ? ((aud_ready && mlv_play_audio > 0) ? "PRX A " : "PRX ") :
                         ((prx_mode == PRX_BUILD) ? "PRX+ " : ""),
                         vidf_block.frameNumber + 1, frame_count);
                
                
                if (mlv_play_use_exact() || prx_hit)
                {
                    if (!fps_timer_started)
                    {
                        mlv_play_start_fps_timer(main_header.sourceFpsNom, main_header.sourceFpsDenom);
                        fps_timer_started = 1;
                    }

                    /* Sync hardware ASIF audio with realtime .PRX playback (including resumed prefix) */
                    if (prx_hit && aud_ready && mlv_play_audio > 0 && !mlv_play_paused)
                    {
                        if (!aud_playing)
                        {
                            aud_start_at_frame(vidf_block.frameNumber, main_header.sourceFpsNom, main_header.sourceFpsDenom);
                        }
                        aud_pump();
                    }
                    else if (aud_playing)
                    {
                        aud_stop();
                    }
                    
                    if (fps_timer_started && !mlv_play_paused)
                    {
                        /* wait till it is time to render (never block while paused or starve inline OSD) */
                        uint32_t temp = 0;
                        while(msg_queue_receive(mlv_play_queue_fps, &temp, 50))
                        {
                            if (aud_playing)
                            {
                                aud_pump();
                            }
                            if (!mlv_play_osd_alive)
                            {
                                mlv_play_inline_osd_poll(1);
                            }
                            if(mlv_play_should_stop() || mlv_play_paused)
                            {
                                if (aud_playing)
                                {
                                    aud_stop();
                                }
                                break;
                            }
                        }
                    }
                }
                else
                {
                    if (aud_playing)
                    {
                        aud_stop();
                    }
                    mlv_play_flush_queue(mlv_play_queue_fps);
                }
                
                /* if render task did not start (e.g. DryOS task pool full on EOS-M),
                 * render + draw HUD + poll OSD inline in mlv_play_task! */
                if (!mlv_play_render_alive)
                {
                    static uint32_t inline_hud_loop = 0;
                    if (vidf_block.frameNumber == 0) inline_hud_loop = 0;
                    mlv_play_render_frame(buffer);
                    prx_after_render(buffer);
                    mlv_play_draw_hud(buffer, &inline_hud_loop);
                    mlv_play_inline_osd_poll(1);
                    while (mlv_play_paused && !mlv_play_should_stop())
                    {
                        mlv_play_inline_osd_poll(30);
                        if (mlv_play_lut_dirty)
                        {
                            mlv_play_lut_dirty = 0;
                            if (!buffer->isProxy)
                            {
                                mlv_play_render_frame(buffer);
                                mlv_play_draw_hud(buffer, &inline_hud_loop);
                            }
                            if (mlv_play_osd_state == MLV_PLAY_MENU_SHOWN || mlv_play_osd_state == MLV_PLAY_MENU_FADEIN)
                            {
                                mlv_play_osd_force_redraw = 1;
                                mlv_play_osd_draw();
                            }
                        }
                        /* keep HUD visible if toggled via INFO while paused */
                        if (mlv_play_info == 2)
                        {
                            mlv_play_draw_hud(buffer, &inline_hud_loop);
                        }
                    }
                    msg_queue_post(mlv_play_queue_empty, (uint32_t) buffer);
                }
                else
                {
                    if (!mlv_play_osd_alive)
                    {
                        mlv_play_inline_osd_poll(1);
                    }
                    /* queue frame buffer for rendering, retry if queue is full (happens in pause or for slow rendering) */
                    while(1)
                    {
                        if(msg_queue_post(mlv_play_queue_render, (uint32_t) buffer))
                        {
                            /* in case we could not pass the buffer and stop is requested, give back buffer here also */
                            if(mlv_play_should_stop())
                            {
                                msg_queue_post(mlv_play_queue_empty, (uint32_t) buffer);
                                break;
                            }
                            else
                            {
                                if (!mlv_play_osd_alive)
                                {
                                    mlv_play_inline_osd_poll(10);
                                }
                                else
                                {
                                    msleep(10);
                                }
                            }
                        }
                        else
                        {
                            break;
                        }
                    }
                }
            }
        }
        block_xref_pos++;
    }
    
    if(fps_timer_started)
    {
        mlv_play_stop_fps_timer();
    }
    aud_close();
    prx_close();
    free(block_xref);
    
    /* return any parked low-RAM buffer shells to the pool */
    for (int p = 0; p < parked_count; p++)
    {
        if (parked_bufs[p])
        {
            msg_queue_post(mlv_play_queue_empty, (uint32_t) parked_bufs[p]);
        }
    }

    /* free decompression stuff if needed (suite first, then fio_free!) */
    if(mlv_play_decomp_suite)
    {
        DeleteMemorySuite(mlv_play_decomp_suite);
        mlv_play_decomp_suite = NULL;
    }

    if(mlv_play_decomp_buf)
    {
        fio_free(mlv_play_decomp_buf);
        mlv_play_decomp_buf = NULL;
    }
}

static void mlv_play_raw(char *filename, FILE **chunk_files, uint32_t chunk_count)
{
    uint32_t fps_timer_started = 0;
    uint32_t chunk_num = 0;
    
    /* read footer information and update global variables, will seek automatically */
    if(chunk_count < 1 || !mlv_play_is_raw(chunk_files[chunk_count-1]))
    {
        bmp_printf(FONT_LARGE, 30, 100, "Invalid RAW:", filename);
        bmp_printf(FONT_MED, 40, 100 + font_large.height + 1, filename);
        return;
    }
    mlv_play_read_footer(chunk_files[chunk_count-1]);
    
    /* update OSD */
    msg_queue_post(mlv_play_queue_osd, (uint32_t) 0);
    
    int i = 0;
    while(1)
    {
        /* after playback is finished, return to beginning and put into pause */
        if (i == frame_count - 1)
        {
            /* reset counters */
            i = 0;
            chunk_num = 0;

            /* reset file pointers */
            for(uint32_t j = 0; j < chunk_count; j++)
            {
                FIO_SeekSkipFile(chunk_files[j], 0, SEEK_SET);
            }

            /* miscellaneous cleanup */
            mlv_play_flush_queue(mlv_play_queue_fps);
            mlv_play_info = (mlv_play_info == 1 ? 2 : mlv_play_info);

            mlv_play_paused = 1;
        }

        /* check if we are too slow */
        if(mlv_play_exact_fps)
        {
            uint32_t frames_to_skip = 0;
            msg_queue_count(mlv_play_queue_fps, &frames_to_skip);

            /* skip frame if we should play at exact fps and we already should be one frame farther */
            if(frames_to_skip > 0)
            {
                uint32_t temp = 0;
                msg_queue_receive(mlv_play_queue_fps, &temp, 50);

                int64_t here = FIO_SeekSkipFile(chunk_files[chunk_num], 0, SEEK_CUR);
                int64_t end  = FIO_SeekSkipFile(chunk_files[chunk_num], 0, SEEK_END);
                int64_t next = here + frame_size;
                if (next >= end)
                {
                    chunk_num++;
                    if (chunk_num < chunk_count)
                    {
                        FIO_SeekSkipFile(chunk_files[chunk_num], next - end, SEEK_SET);
                    }
                    else
                    {
                        break;
                    }
                }
                else
                {
                    FIO_SeekSkipFile(chunk_files[chunk_num], next, SEEK_SET);
                }

                mlv_play_frames_skipped++;
                i++;
                continue;
            }
        }
        else
        {
            /* if not, just keep the queue clean */
            mlv_play_flush_queue(mlv_play_queue_fps);
        }

        /* there are various reasons why this read/play task should get stopped */
        if(mlv_play_should_stop())
        {
            break;
        }
        
        frame_buf_t *buffer = NULL;
        
        while(mlv_play_paused && !mlv_play_should_stop())
        {
            if (!mlv_play_osd_alive)
            {
                mlv_play_inline_osd_poll(30);
            }
            else
            {
                msleep(100);
            }
        }
        
        /* now get a buffer from the queue */
        while (msg_queue_receive(mlv_play_queue_empty, &buffer, 50) && !mlv_play_should_stop())
        {
            if (!mlv_play_osd_alive)
            {
                mlv_play_inline_osd_poll(1);
            }
        }
        
        if (mlv_play_should_stop())
        {
            break;
        }
        
        /* check if the queued buffer has the correct size */
        if((int32_t)buffer->frameSize != frame_size)
        {
            /* the first few queued dont have anything allocated, so dont free */
            if(buffer->frameBuffer)
            {
                free(buffer->frameBuffer);
            }
            
            buffer->frameSize = frame_size;
            buffer->frameBuffer = fio_malloc(buffer->frameSize);
        }

        if(!buffer->frameBuffer)
        {
            bmp_printf(FONT_MED, 30, 400, "allocation failed");
            beep();
            msleep(1000);
            break;
        }
        
        int32_t r = FIO_ReadFile(chunk_files[chunk_num], buffer->frameBuffer, frame_size);
        
        /* reading failed */
        if(r < 0)
        {
            bmp_printf(FONT_MED, 30, 400, "reading failed");
            beep();
            msleep(1000);
            break;
        }
        
        /* end of chunk reached */
        if (r != frame_size)
        {
            /* no more chunks to play */
            if((chunk_num + 1) >= chunk_count)
            {
                break;
            }
            
            /* read remaining block from next chunk */
            chunk_num++;
            int remain = frame_size - r;
            r = FIO_ReadFile(chunk_files[chunk_num], (void*)((uint32_t)buffer->frameBuffer + r), remain);
            
            /* it doesnt have enough data. thats enough errors... */
            if(r != remain)
            {
                bmp_printf(FONT_MED, 30, 400, "reading failed again");
                beep();
                msleep(1000);
                break;
            }
        }

        /* fill strings to display */
        snprintf(buffer->messages.topLeft, SCREEN_MSG_LEN, "");
        snprintf(buffer->messages.topRight, SCREEN_MSG_LEN, "");
            
        snprintf(buffer->messages.botLeft, SCREEN_MSG_LEN, "%s: %dx%d", filename, res_x, res_y);
        snprintf(buffer->messages.botRight, SCREEN_MSG_LEN, "%d/%d",  i+1, frame_count-1);
        
        
        /* update dimensions */
        buffer->xRes = res_x;
        buffer->yRes = res_y;
        buffer->bitDepth = 14;
        buffer->valueBits = 14;
        buffer->frameData = buffer->frameBufferAligned;
        buffer->isProxy = 0;
        buffer->blackLevel = raw_info.black_level;
        buffer->whiteLevel = raw_info.white_level;
        
        if (mlv_play_exact_fps)
        {
            if (!fps_timer_started)
            {
                mlv_play_start_fps_timer(fps1000, 1000);
                fps_timer_started = 1;
            }

            if (fps_timer_started)
            {
                /* wait till it is time to render */
                uint32_t temp = 0;
                while(msg_queue_receive(mlv_play_queue_fps, &temp, 50))
                {
                    if(mlv_play_should_stop())
                    {
                        break;
                    }
                }
            }
        }

        /* requeue frame buffer for rendering */
        msg_queue_post(mlv_play_queue_render, (uint32_t) buffer);

        i++;
    }

    if(fps_timer_started)
    {
        mlv_play_stop_fps_timer();
    }
}

static void mlv_play(char *filename, FILE **chunk_files, uint32_t chunk_count)
{
    mlv_play_stopfile = 0;
    
    if(mlv_play_is_mlv(chunk_files[0]))
    {
        mlv_play_mlv(filename, chunk_files, chunk_count);
    }
    else
    {
        mlv_play_raw(filename, chunk_files, chunk_count);
    }
}

static void mlv_playlist_build_path(char *directory)
{
    struct fio_file file;
    struct fio_dirent * dirent = NULL;
    
    dirent = FIO_FindFirstEx(directory, &file);
    if(IS_ERROR(dirent))
    {
        return;
    }
    
    char *full_path = malloc(MAX_PATH);

    do
    {
        if(file.name[0] == '.' || file.name[0] == '_' || !strncmp(file.name, "TRASHE", 6))
        {
            continue;
        }

        snprintf(full_path, MAX_PATH, "%s%s", directory, file.name);
        
        /* do not recurse here, but enqueue next directory */
        if(file.mode & ATTR_DIRECTORY)
        {
            strcat(full_path, "/");
            
            msg_queue_post(mlv_playlist_scan_queue, (uint32_t) strdup(full_path));
        }
        else if (file.size >= 65536)
        {
            char *suffix = &file.name[strlen(file.name) - 3];
            
            if(!strcmp("RAW", suffix) || !strcmp("MLV", suffix))
            {
                playlist_entry_t *entry = malloc(sizeof(playlist_entry_t));
                
                strncpy(entry->fullPath, full_path, sizeof(entry->fullPath));
                entry->fileSize = file.size;
                entry->timestamp = file.timestamp;
                
                /* update playlist */
                msg_queue_post(mlv_playlist_queue, (uint32_t) entry);
            }
        }
    }
    while(FIO_FindNextEx(dirent, &file) == 0);
    
    FIO_FindClose(dirent);
    free(full_path);
}

static void mlv_playlist_free()
{
    /* clear the playlist */
    mlv_playlist_entries = 0;
    if(mlv_playlist)
    {
        free(mlv_playlist);
    }
    mlv_playlist = NULL;
    
    /* clear items in queues */
    void *entry = NULL;
    while(!msg_queue_receive(mlv_playlist_queue, &entry, 50))
    {
        free(entry);
    }
    while(!msg_queue_receive(mlv_playlist_scan_queue, &entry, 50))
    {
        free(entry);
    }
}

static void mlv_playlist_build(uint32_t priv)
{
    playlist_entry_t *entry = NULL;
    
    /* clear the playlist */
    mlv_playlist_free();
    
    /* set up initial directories to scan. try to not recurse, but use scan and result queues */
    msg_queue_post(mlv_playlist_scan_queue, (uint32_t) strdup("A:/"));
    msg_queue_post(mlv_playlist_scan_queue, (uint32_t) strdup("B:/"));
    
    char *directory = NULL;
    while(!msg_queue_receive(mlv_playlist_scan_queue, &directory, 50))
    {
        mlv_playlist_build_path(directory);
        free(directory);
    }
    
    /* pre-allocate the number of enqueued playlist items */
    uint32_t msg_count = 0;
    msg_queue_count(mlv_playlist_queue, &msg_count);
    playlist_entry_t *playlist = malloc(msg_count * sizeof(playlist_entry_t));
    
    /* add all items */
    while(!msg_queue_receive(mlv_playlist_queue, &entry, 50))
    {
        playlist[mlv_playlist_entries++] = *entry;
        free(entry);
    }

    /* sort newest first (timestamp desc, then path desc) so latest clip is top-left in gallery */
    for (uint32_t i = 1; i < mlv_playlist_entries; i++)
    {
        playlist_entry_t key = playlist[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && (playlist[j].timestamp < key.timestamp ||
                          (playlist[j].timestamp == key.timestamp && strcmp(playlist[j].fullPath, key.fullPath) < 0)))
        {
            playlist[j + 1] = playlist[j];
            j--;
        }
        playlist[j + 1] = key;
    }
    
    mlv_playlist = playlist;
}

static int32_t mlv_playlist_find(playlist_entry_t current)
{
    for(uint32_t pos = 0; pos < mlv_playlist_entries; pos++)
    {
        if(!strcmp(current.fullPath, mlv_playlist[pos].fullPath))
        {
            return pos;
        }
    }
    
    return -1;
}

static void mlv_playlist_delete(playlist_entry_t current)
{
    int32_t pos = mlv_playlist_find(current);
    
    if(pos >= 0)
    {
        uint32_t remaining = mlv_playlist_entries - pos - 1;
        memcpy(&mlv_playlist[pos], &mlv_playlist[pos+1], remaining * sizeof(playlist_entry_t));
        mlv_playlist_entries--;
    }
}

static playlist_entry_t mlv_playlist_next(playlist_entry_t current)
{
    playlist_entry_t ret;
    
    strcpy(ret.fullPath, "");
    
    int32_t pos = mlv_playlist_find(current);
    
    if(pos >= 0 && (uint32_t)(pos + 1) < mlv_playlist_entries)
    {
        ret = mlv_playlist[pos + 1];
    }
    
    return ret;
}

static playlist_entry_t mlv_playlist_prev(playlist_entry_t current)
{
    playlist_entry_t ret;
    
    strcpy(ret.fullPath, "");
    
    int32_t pos = mlv_playlist_find(current);
    
    if(pos > 0)
    {
        ret = mlv_playlist[pos - 1];
    }
    
    return ret;
}

static void mlv_play_leave_playback()
{
    mlv_play_render_abort = 1;
    
    if (mlv_play_render_alive)
    {
        for (int w = 0; w < 15 && mlv_play_rendering; w++)
        {
            info_led_blink(1, 100, 100);
        }
    }
    mlv_play_rendering = 0;
    
    /* clean up buffers - free memories and all buffers */
    while(1)
    {
        frame_buf_t *buffer = NULL;
        
        if(msg_queue_receive(mlv_play_queue_render, &buffer, 50) && msg_queue_receive(mlv_play_queue_empty, &buffer, 50))
        {
            break;
        }
        
        /* free allocated buffers */
        if(buffer->frameBuffer)
        {
            fio_free(buffer->frameBuffer);
        }
        
        free(buffer);
    }
    
    vram_clear_lv();
    exit_play_qr_mode();
}

static void mlv_play_enter_playback()
{
    /* prepare display */
    NotifyBoxHide();
    enter_play_mode();
    
    /* render task is slave and controlled via these variables */
    mlv_play_render_abort = 0;
    mlv_play_render_alive = 0;
    mlv_play_osd_alive = 0;
    mlv_play_rendering = 1;
    task_create("mlv_play_render", 0x1d, 0x2000, mlv_play_render_task, NULL);
    task_create("mlv_play_osd_task", 0x15, 0x2000, mlv_play_osd_task, 0);
    
    mlv_play_zoom = 0;
    mlv_play_zoom_x_pct = 0;
    mlv_play_zoom_y_pct = 0;
    raw_twk_set_zoom(mlv_play_zoom, mlv_play_zoom_x_pct, mlv_play_zoom_y_pct);
    
    /* queue a few buffers that are not allocated yet */
    for(int num = 0; num < 3; num++)
    {
        frame_buf_t *buffer = malloc(sizeof(frame_buf_t));
        if (buffer)
        {
            memset(buffer, 0, sizeof(frame_buf_t));
            buffer->frameSize = 0;
            buffer->frameBuffer = NULL;
            
            msg_queue_post(mlv_play_queue_empty, (uint32_t) buffer);
        }
    }
    
    /* clear anything on screen */
    mlv_play_clear_screen();
}

/* ============================================================================
 * MLV Gallery (3x2 thumbnail grid stored in unified .PRX sidecar slot 0)
 * ============================================================================ */

typedef struct
{
    char     magic[4];       /* "MTHM" */
    uint32_t version;        /* 2 */
    uint32_t mlvSize;        /* file size validation */
    uint16_t xRes;
    uint16_t yRes;
    uint8_t  xBin;
    uint8_t  yBin;
    uint8_t  bpp;
    uint8_t  isLossless;
    uint8_t  isRaw;
    uint8_t  pad0;
    uint16_t fps100;         /* e.g. 2398 = 23.98 fps */
    uint32_t frameCount;
    uint16_t thumbW;
    uint16_t thumbH;
    uint8_t  reserved[480];  /* pad header to 512 bytes */
} mlv_thm_hdr_t;

static void gal_make_thm_path(const char *mlv_path, char *out, size_t out_len)
{
    strncpy(out, mlv_path, out_len - 1);
    out[out_len - 1] = '\0';
    size_t n = strlen(out);
    if (n >= 4)
    {
        strcpy(out + n - 3, "PRX");
    }
}

static const char *gal_basename(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* Compute universal desqueezed output resolution (dw x dh) from raw stored (xRes x yRes) and
 * sensor sampling factors (xBin, yBin) across all Canon cameras (5D3, EOS-M, 700D, 650D, 100D, 200D, etc.):
 * The denser axis (min(ax, ay)) preserves its true captured pixel count, while the sparser axis
 * is desqueezed by max(ax, ay) / min(ax, ay).
 *   - 1x1 (ax=1, ay=1) or 3x3 (ax=3, ay=3): dw = xRes, dh = yRes (1:1 square pixels)
 *   - 3x5 (ax=3, ay=5, e.g. EOS-M stock 1736x578): dw = 1736, dh = 578*5/3 = 963
 *   - 3x1 (ax=3, ay=1, e.g. 1x3 anamorphic 1920x1920): dw = 1920*3 = 5760, dh = 1920
 */
static void gal_desqueezed_wh(int xRes, int yRes, int xBin, int yBin, int *dw_out, int *dh_out)
{
    int ax = (xBin > 0) ? xBin : 1;
    int ay = (yBin > 0) ? yBin : 1;
    int dw = xRes;
    int dh = yRes;
    if (ax > ay)
    {
        dw = (xRes * ax + (ay >> 1)) / ay;
    }
    else if (ay > ax)
    {
        dh = (yRes * ay + (ax >> 1)) / ax;
    }
    *dw_out = dw;
    *dh_out = dh;
}

static void gal_format_mode_badge(const mlv_thm_hdr_t *h, char *out, size_t len)
{
    if (!h->xRes || !h->yRes)
    {
        snprintf(out, len, h->isRaw ? "RAW" : "MLV");
        return;
    }
    int dw = 0, dh = 0;
    gal_desqueezed_wh(h->xRes, h->yRes, h->xBin, h->yBin, &dw, &dh);

    if (dw >= 2000)
    {
        int k10 = dw / 100; /* e.g. 5760 -> 5.7K, 5208 -> 5.2K, 4512 -> 4.5K, 3408 -> 3.4K, 2520 -> 2.5K */
        snprintf(out, len, "%d.%dK", k10 / 10, k10 % 10);
    }
    else if (dw >= 1880 && dh >= 1200)
    {
        snprintf(out, len, "3:2");
    }
    else if (dw >= 1880 && dh >= 1000)
    {
        snprintf(out, len, "1080p");
    }
    else if (dw >= 1880 && dh >= 760 && dh <= 860)
    {
        snprintf(out, len, "2.35");
    }
    else if (dw >= 1240 && dw <= 1320 && dh >= 700 && dh <= 740)
    {
        snprintf(out, len, "720p");
    }
    else
    {
        snprintf(out, len, "%dx%d", (int)h->xRes, (int)h->yRes);
    }
}

static int gal_load_thm(const char *mlv_path, uint32_t expected_size, mlv_thm_hdr_t *hdr, uint8_t *uyvy_buf)
{
    char thm_path[MAX_PATH];
    gal_make_thm_path(mlv_path, thm_path, sizeof(thm_path));

    uint32_t fsize = 0;
    if (FIO_GetFileSize(thm_path, &fsize) || fsize < sizeof(mlv_thm_hdr_t) + GAL_THM_PIXELS)
    {
        return 0;
    }

    FILE *f = FIO_OpenFile(thm_path, O_RDONLY | O_SYNC);
    if (!f)
    {
        return 0;
    }

    int ok = 0;
    FIO_SeekSkipFile(f, PRX_THM_OFFSET, SEEK_SET);
    if (FIO_ReadFile(f, hdr, sizeof(*hdr)) == (int32_t)sizeof(*hdr) &&
        memcmp(hdr->magic, "MTHM", 4) == 0 &&
        hdr->version == 2 &&
        hdr->mlvSize == expected_size &&
        hdr->thumbW == GAL_TW && hdr->thumbH == GAL_TH)
    {
        if (FIO_ReadFile(f, uyvy_buf, GAL_THM_PIXELS) == GAL_THM_PIXELS)
        {
            ok = 1;
        }
    }
    FIO_CloseFile(f);
    return ok;
}

static void gal_save_thm(const char *mlv_path, const mlv_thm_hdr_t *hdr, const uint8_t *uyvy_buf)
{
    char thm_path[MAX_PATH];
    gal_make_thm_path(mlv_path, thm_path, sizeof(thm_path));
    FILE *f = prx_open_or_create_slot(thm_path, PRX_THM_OFFSET);
    if (!f)
    {
        return;
    }
    FIO_SeekSkipFile(f, PRX_THM_OFFSET, SEEK_SET);
    FIO_WriteFile(f, hdr, sizeof(*hdr));
    FIO_WriteFile(f, uyvy_buf, GAL_THM_PIXELS);
    FIO_CloseFile(f);
}

/* Read only headers + first VIDF from an MLV (no full scan / no .IDX creation) and render thumbnail. */
static int gal_build_thm_mlv(const char *mlv_path, uint32_t file_size, mlv_thm_hdr_t *hdr, uint8_t *uyvy_buf)
{
    FILE *f = FIO_OpenFile(mlv_path, O_RDONLY | O_SYNC);
    if (!f)
    {
        return 0;
    }

    memset(hdr, 0, sizeof(*hdr));
    memcpy(hdr->magic, "MTHM", 4);
    hdr->version = 2;
    hdr->mlvSize = file_size;
    hdr->thumbW = GAL_TW;
    hdr->thumbH = GAL_TH;

    mlv_file_hdr_t main_hdr;
    mlv_rawi_hdr_t rawi_hdr;
    mlv_rawc_hdr_t rawc_hdr;
    memset(&main_hdr, 0, sizeof(main_hdr));
    memset(&rawi_hdr, 0, sizeof(rawi_hdr));
    memset(&rawc_hdr, 0, sizeof(rawc_hdr));

    int have_mlvi = 0, have_rawi = 0, have_rawc = 0;
    int ok = 0;
    int64_t pos = 0;

    /* scan first blocks until we find the first VIDF (bounded to 64 blocks for safety) */
    for (int step = 0; step < 64; step++)
    {
        mlv_hdr_t blk;
        FIO_SeekSkipFile(f, pos, SEEK_SET);
        if (mlv_play_read_small(f, &blk, sizeof(blk)) != (int32_t)sizeof(blk) || blk.blockSize < sizeof(blk))
        {
            break;
        }
        FIO_SeekSkipFile(f, pos, SEEK_SET);

        if (!memcmp(blk.blockType, "MLVI", 4))
        {
            uint32_t sz = MIN(sizeof(main_hdr), blk.blockSize);
            if (FIO_ReadFile(f, &main_hdr, sz) != (int32_t)sz) break;
            have_mlvi = 1;
            hdr->frameCount = main_hdr.videoFrameCount;
            hdr->isLossless = (main_hdr.videoClass & MLV_VIDEO_CLASS_FLAG_LJ92) ? 1 : 0;
            if (main_hdr.sourceFpsDenom)
            {
                hdr->fps100 = (uint16_t)((uint64_t)main_hdr.sourceFpsNom * 100 / main_hdr.sourceFpsDenom);
            }
        }
        else if (!memcmp(blk.blockType, "RAWI", 4))
        {
            if (FIO_ReadFile(f, &rawi_hdr, sizeof(rawi_hdr)) != (int32_t)sizeof(rawi_hdr)) break;
            have_rawi = 1;
            hdr->xRes = rawi_hdr.xRes;
            hdr->yRes = rawi_hdr.yRes;
            hdr->bpp  = (uint8_t)sp_real_bits(rawi_hdr.raw_info.bits_per_pixel, rawi_hdr.raw_info.black_level, rawi_hdr.raw_info.white_level);
        }
        else if (!memcmp(blk.blockType, "RAWC", 4))
        {
            if (FIO_ReadFile(f, &rawc_hdr, sizeof(rawc_hdr)) != (int32_t)sizeof(rawc_hdr)) break;
            have_rawc = 1;
            hdr->xBin = rawc_hdr.binning_x + rawc_hdr.skipping_x;
            hdr->yBin = rawc_hdr.binning_y + rawc_hdr.skipping_y;
        }
        else if (!memcmp(blk.blockType, "VIDF", 4) && have_mlvi && have_rawi)
        {
            mlv_vidf_hdr_t vidf;
            if (mlv_play_read_small(f, &vidf, sizeof(vidf)) != (int32_t)sizeof(vidf)) break;

            int bpp = rawi_hdr.raw_info.bits_per_pixel;
            uint32_t fsize = (uint32_t)rawi_hdr.xRes * rawi_hdr.yRes * bpp / 8;
            if (fsize == 0 || blk.blockSize <= sizeof(vidf) + vidf.frameSpace) break;

            uint32_t read_size = MIN(fsize, blk.blockSize - (sizeof(vidf) + vidf.frameSpace));
            uint32_t raw_alloc = (uint32_t)rawi_hdr.xRes * rawi_hdr.yRes * 2 + 0x1100;
            void *raw_mem = fio_malloc(raw_alloc);
            if (!raw_mem) break;
            void *raw_aligned = (void *)(((uint32_t)raw_mem + 0x1000) & ~0xFFF);

            int64_t data_pos = pos + sizeof(vidf) + vidf.frameSpace;
            int is_lj92 = hdr->isLossless;
            int cbits = bpp;
            int vbits = bpp;
            void *render_src = raw_aligned;

            if (is_lj92)
            {
                int comp_sz = (int)((read_size + 0x1000) & ~0xFFF);
                void *comp_mem = fio_malloc(comp_sz + 0x1000);
                if (comp_mem)
                {
                    FIO_SeekSkipFile(f, data_pos, SEEK_SET);
                    if (FIO_ReadFile(f, comp_mem, read_size) == (int32_t)read_size)
                    {
                        struct memSuite *suite = CreateMemorySuite(comp_mem, comp_sz, 0);
                        if (suite)
                        {
                            int r = lossless_decompress_raw(suite, raw_aligned, rawi_hdr.xRes, rawi_hdr.yRes, 14);
                            DeleteMemorySuite(suite);
                            if (r >= 0)
                            {
                                cbits = 14;
                                ok = sp_render_to_buf(render_src, rawi_hdr.xRes, rawi_hdr.yRes,
                                                      cbits, vbits,
                                                      rawi_hdr.raw_info.black_level,
                                                      rawi_hdr.raw_info.white_level,
                                                      have_rawc ? hdr->xBin : 1,
                                                      have_rawc ? hdr->yBin : 1,
                                                      uyvy_buf, GAL_TW, GAL_TH);
                            }
                        }
                    }
                    fio_free(comp_mem);
                }
            }
            else
            {
                FIO_SeekSkipFile(f, data_pos, SEEK_SET);
                if (FIO_ReadFile(f, raw_aligned, read_size) == (int32_t)read_size)
                {
                    ok = sp_render_to_buf(render_src, rawi_hdr.xRes, rawi_hdr.yRes,
                                          cbits, vbits,
                                          rawi_hdr.raw_info.black_level,
                                          rawi_hdr.raw_info.white_level,
                                          have_rawc ? hdr->xBin : 1,
                                          have_rawc ? hdr->yBin : 1,
                                          uyvy_buf, GAL_TW, GAL_TH);
                }
            }

            fio_free(raw_mem);
            break;
        }

        pos += blk.blockSize;
    }

    FIO_CloseFile(f);
    if (ok)
    {
        gal_save_thm(mlv_path, hdr, uyvy_buf);
    }
    return ok;
}

static int gal_build_thm_raw(const char *raw_path, uint32_t file_size, mlv_thm_hdr_t *hdr, uint8_t *uyvy_buf)
{
    FILE *f = FIO_OpenFile(raw_path, O_RDONLY | O_SYNC);
    if (!f) return 0;

    memset(hdr, 0, sizeof(*hdr));
    memcpy(hdr->magic, "MTHM", 4);
    hdr->version = 2;
    hdr->mlvSize = file_size;
    hdr->isRaw = 1;
    hdr->thumbW = GAL_TW;
    hdr->thumbH = GAL_TH;

    lv_rec_file_footer_t footer;
    if (file_size <= sizeof(footer))
    {
        FIO_CloseFile(f);
        return 0;
    }
    FIO_SeekSkipFile(f, (int64_t)file_size - sizeof(footer), SEEK_SET);
    int ok = 0;
    if (FIO_ReadFile(f, &footer, sizeof(footer)) == (int32_t)sizeof(footer) &&
        memcmp(footer.magic, "RAWM", 4) == 0 && footer.frameSize > 0)
    {
        hdr->xRes = footer.xRes;
        hdr->yRes = footer.yRes;
        hdr->xBin = 1;
        hdr->yBin = 1;
        hdr->bpp = 14;
        hdr->fps100 = (uint16_t)(footer.sourceFpsx1000 / 10);
        hdr->frameCount = footer.frameCount;

        uint32_t alloc_sz = footer.frameSize + 0x1000;
        void *raw_mem = fio_malloc(alloc_sz);
        if (raw_mem)
        {
            FIO_SeekSkipFile(f, 0, SEEK_SET);
            if (FIO_ReadFile(f, raw_mem, footer.frameSize) == (int32_t)footer.frameSize)
            {
                ok = sp_render_to_buf(raw_mem, footer.xRes, footer.yRes, 14, 14,
                                      footer.raw_info.black_level, footer.raw_info.white_level,
                                      1, 1, uyvy_buf, GAL_TW, GAL_TH);
            }
            fio_free(raw_mem);
        }
    }
    FIO_CloseFile(f);
    if (ok)
    {
        gal_save_thm(raw_path, hdr, uyvy_buf);
    }
    return ok;
}

static void gal_blit_thumb(int vx, int vy, const uint8_t *uyvy_buf)
{
    struct vram_info *vr = get_yuv422_vram();
    if (!vr || !vr->vram)
    {
        yuv422_buffer_check();
        return;
    }
    if (vx < 0 || vy < 0 || vx + GAL_TW > vr->width || vy + GAL_TH > vr->height)
    {
        return;
    }
    uint8_t *vram = vr->vram;
    int pitch = vr->pitch;
    int row_bytes = GAL_TW * 2;
    for (int y = 0; y < GAL_TH; y++)
    {
        memcpy(vram + (vy + y) * pitch + vx * 2, uyvy_buf + y * row_bytes, row_bytes);
    }
}

static void gal_clear_thumb(int vx, int vy)
{
    struct vram_info *vr = get_yuv422_vram();
    if (!vr || !vr->vram) return;
    if (vx < 0 || vy < 0 || vx + GAL_TW > vr->width || vy + GAL_TH > vr->height) return;
    uint8_t *vram = vr->vram;
    int pitch = vr->pitch;
    int row_bytes = GAL_TW * 2;
    for (int y = 0; y < GAL_TH; y++)
    {
        memset(vram + (vy + y) * pitch + vx * 2, 0, row_bytes);
    }
}

static void gal_draw_header(int sel, int total, int page, int pages, const char *path)
{
    bmp_fill(COLOR_ALMOST_BLACK, 0, 0, 720, 38);
    bmp_fill(COLOR_GRAY(35), 0, 38, 720, 1);
    ui_icon_film(16, 12, COLOR_ORANGE);
    bmp_printf(FONT(FONT_MED, COLOR_WHITE, COLOR_ALMOST_BLACK), 42, 10, "MLV GALLERY");

    char right[64];
    const char *drv = (path && path[0] == 'B') ? "SD" : "CF";
    snprintf(right, sizeof(right), "%d/%d  Page %d/%d  [%s]",
             sel + 1, total, page + 1, pages, drv);
    bmp_printf(FONT(FONT_MED, COLOR_GRAY(80), COLOR_ALMOST_BLACK) | FONT_ALIGN_RIGHT, 704, 10, "%s", right);
}

static void gal_draw_cell_frame(int slot, int selected, const char *path, const mlv_thm_hdr_t *hdr, int has_meta)
{
    int col = slot % GAL_COLS;
    int row = slot / GAL_COLS;
    int x = GAL_MARGIN_X + col * (GAL_CELL_W + GAL_GAP_X);
    int y = GAL_TOP_Y + row * (GAL_CELL_H + GAL_GAP_Y);

    /* clear outer halo & draw frame */
    bmp_fill(COLOR_EMPTY, x + 4, y + 4, GAL_TW, GAL_TH);
    if (selected)
    {
        ui_draw_rect(COLOR_ORANGE, x, y, GAL_CELL_W, GAL_CELL_H, 3);
        ui_draw_rect(COLOR_WHITE, x + 3, y + 3, GAL_CELL_W - 6, GAL_CELL_H - 6, 1);
    }
    else
    {
        ui_draw_rect(COLOR_ALMOST_BLACK, x, y, GAL_CELL_W, GAL_CELL_H, 3);
        ui_draw_rect(COLOR_GRAY(35), x + 2, y + 2, GAL_CELL_W - 4, GAL_CELL_H - 4, 2);
    }

    /* bottom label bar inside cell */
    int ly = y + 4 + GAL_TH;
    int lbg = selected ? COLOR_ORANGE : COLOR_ALMOST_BLACK;
    int lfg = selected ? COLOR_BLACK : COLOR_WHITE;
    bmp_fill(lbg, x + 4, ly, GAL_TW, 20);

    /* clip basename without extension */
    char shortname[16];
    const char *bn = gal_basename(path);
    strncpy(shortname, bn, sizeof(shortname) - 1);
    shortname[sizeof(shortname) - 1] = '\0';
    char *dot = strrchr(shortname, '.');
    if (dot) *dot = '\0';
    bmp_printf(FONT(FONT_MED, lfg, lbg), x + 8, ly + 1, "%s", shortname);

    if (has_meta)
    {
        char badge[16];
        gal_format_mode_badge(hdr, badge, sizeof(badge));
        int bfg = selected ? COLOR_BLACK : COLOR_CYAN;
        bmp_printf(FONT(FONT_MED, bfg, lbg) | FONT_ALIGN_RIGHT, x + GAL_CELL_W - 8, ly + 1, "%s", badge);
    }

    /* play badge in top-right of selected thumbnail */
    if (selected)
    {
        bmp_fill(COLOR_BG, x + GAL_CELL_W - 28, y + 8, 20, 20);
        ui_icon_play(x + GAL_CELL_W - 23, y + 12, COLOR_ORANGE);
    }
}

static void gal_draw_footer(const playlist_entry_t *e, const mlv_thm_hdr_t *hdr, int has_meta, int del_confirm, int del_sel)
{
    bmp_fill(COLOR_ALMOST_BLACK, 0, 396, 720, 84);
    bmp_fill(COLOR_GRAY(35), 0, 396, 720, 1);

    if (del_confirm)
    {
        bmp_fill(COLOR_GRAY(20), 16, 402, 688, 34);
        ui_draw_rect(COLOR_RED, 16, 402, 688, 34, 2);
        ui_icon_trash(26, 412, COLOR_RED);
        bmp_printf(FONT(FONT_MED, COLOR_WHITE, COLOR_GRAY(20)), 46, 410,
                   "Delete %.16s?", gal_basename(e->fullPath));

        /* [ Cancel ] button (del_sel == 0, default) */
        int c_bg = (del_sel == 0) ? COLOR_ORANGE : COLOR_GRAY(35);
        int c_fg = (del_sel == 0) ? COLOR_BLACK  : COLOR_GRAY(75);
        bmp_fill(c_bg, 456, 406, 112, 26);
        if (del_sel == 0) ui_draw_rect(COLOR_WHITE, 456, 406, 112, 26, 1);
        bmp_printf(FONT(FONT_MED, c_fg, c_bg) | FONT_ALIGN_CENTER, 456 + 56, 410, "Cancel");

        /* [ Delete ] button (del_sel == 1) */
        int d_bg = (del_sel == 1) ? COLOR_RED   : COLOR_GRAY(35);
        int d_fg = (del_sel == 1) ? COLOR_WHITE : COLOR_GRAY(75);
        bmp_fill(d_bg, 580, 406, 112, 26);
        if (del_sel == 1) ui_draw_rect(COLOR_WHITE, 580, 406, 112, 26, 1);
        bmp_printf(FONT(FONT_MED, d_fg, d_bg) | FONT_ALIGN_CENTER, 580 + 56, 410, "Delete");

        bmp_printf(FONT(FONT_MED, COLOR_GRAY(75), COLOR_ALMOST_BLACK) | FONT_ALIGN_CENTER, 360, 446,
                   "Left/Right: Select    SET: Confirm    Other: Cancel");
        return;
    }
    else
    {
        char info[96];
        uint32_t mb = e->fileSize / (1024 * 1024);
        if (has_meta && hdr->xRes && hdr->yRes)
        {
            char mode[16];
            gal_format_mode_badge(hdr, mode, sizeof(mode));
            uint32_t secs = (hdr->fps100 > 0) ? (hdr->frameCount * 100 / hdr->fps100) : 0;
            uint32_t fps_i = (hdr->fps100 + 50) / 100;
            snprintf(info, sizeof(info), "%s  %s %dx%d  %db%s  %dp  %02d:%02d  %dM",
                     gal_basename(e->fullPath), mode, hdr->xRes, hdr->yRes,
                     hdr->bpp, hdr->isLossless ? " LJ92" : "",
                     fps_i, secs / 60, secs % 60, mb);
        }
        else
        {
            snprintf(info, sizeof(info), "%s   %d MB", gal_basename(e->fullPath), mb);
        }
        bmp_printf(FONT(FONT_MED, COLOR_WHITE, COLOR_ALMOST_BLACK), 16, 406, "%s", info);
    }

    /* bottom key legend with pixel icons (4 clear keys, evenly spaced inside 720px) */
    int ky = 446;
    ui_icon_play(24, ky + 3, COLOR_ORANGE);
    bmp_printf(FONT(FONT_MED, COLOR_GRAY(75), COLOR_ALMOST_BLACK), 40, ky, "SET Play");

    ui_icon_info(188, ky + 3, COLOR_YELLOW);
    bmp_printf(FONT(FONT_MED, COLOR_GRAY(75), COLOR_ALMOST_BLACK), 206, ky, "INFO Detail");

    ui_icon_trash(382, ky + 3, COLOR_RED);
    bmp_printf(FONT(FONT_MED, COLOR_GRAY(75), COLOR_ALMOST_BLACK), 400, ky, "TRASH Del");

    ui_icon_exit(550, ky + 3, COLOR_WHITE);
    bmp_printf(FONT(FONT_MED, COLOR_GRAY(75), COLOR_ALMOST_BLACK), 566, ky, "MENU Exit");
}

/* Read extended metadata blocks (MLVI/RAWI/RAWC/RTCI/EXPO/LENS/WBAL/WAVI) without scanning video frames */
typedef struct
{
    int      have_mlvi, have_rawi, have_rawc, have_rtci, have_expo, have_lens, have_wbal, have_wavi;
    uint16_t xRes, yRes, bpp, blackLevel, whiteLevel;
    uint8_t  xBin, yBin, isLossless, isRaw;
    uint32_t fps100, frameCount, fileSize;
    uint16_t yr, mon, day, hr, min, sec;
    uint32_t iso, shutter_us, kelvin;
    uint16_t focal_mm, aperture100;
    char     lensName[36];
    uint16_t aud_ch;
    uint32_t aud_rate;
} gal_detail_t;

static void gal_read_detail(const playlist_entry_t *e, const mlv_thm_hdr_t *thm, int has_thm, gal_detail_t *d)
{
    memset(d, 0, sizeof(*d));
    d->fileSize = e->fileSize;
    if (has_thm)
    {
        d->xRes = thm->xRes;
        d->yRes = thm->yRes;
        d->xBin = thm->xBin;
        d->yBin = thm->yBin;
        d->bpp  = thm->bpp;
        d->isLossless = thm->isLossless;
        d->isRaw = thm->isRaw;
        d->fps100 = thm->fps100;
        d->frameCount = thm->frameCount;
    }

    const char *ext = e->fullPath + strlen(e->fullPath) - 3;
    if (!streq(ext, "MLV") && !streq(ext, "mlv"))
    {
        d->isRaw = 1;
        return;
    }

    FILE *f = FIO_OpenFile(e->fullPath, O_RDONLY | O_SYNC);
    if (!f) return;

    int64_t pos = 0;
    for (int step = 0; step < 32; step++)
    {
        mlv_hdr_t blk;
        FIO_SeekSkipFile(f, pos, SEEK_SET);
        if (mlv_play_read_small(f, &blk, sizeof(blk)) != (int32_t)sizeof(blk) || blk.blockSize < sizeof(blk))
            break;
        FIO_SeekSkipFile(f, pos, SEEK_SET);

        if (!memcmp(blk.blockType, "MLVI", 4))
        {
            mlv_file_hdr_t h;
            uint32_t sz = MIN(sizeof(h), blk.blockSize);
            memset(&h, 0, sizeof(h));
            if (FIO_ReadFile(f, &h, sz) == (int32_t)sz)
            {
                d->have_mlvi = 1;
                d->frameCount = h.videoFrameCount;
                d->isLossless = (h.videoClass & MLV_VIDEO_CLASS_FLAG_LJ92) ? 1 : 0;
                if (h.sourceFpsDenom)
                    d->fps100 = (uint32_t)((uint64_t)h.sourceFpsNom * 100 / h.sourceFpsDenom);
            }
        }
        else if (!memcmp(blk.blockType, "RAWI", 4))
        {
            mlv_rawi_hdr_t h;
            if (FIO_ReadFile(f, &h, sizeof(h)) == (int32_t)sizeof(h))
            {
                d->have_rawi = 1;
                d->xRes = h.xRes;
                d->yRes = h.yRes;
                d->bpp  = h.raw_info.bits_per_pixel;
                d->blackLevel = h.raw_info.black_level;
                d->whiteLevel = h.raw_info.white_level;
            }
        }
        else if (!memcmp(blk.blockType, "RAWC", 4))
        {
            mlv_rawc_hdr_t h;
            if (FIO_ReadFile(f, &h, sizeof(h)) == (int32_t)sizeof(h))
            {
                d->have_rawc = 1;
                d->xBin = h.binning_x + h.skipping_x;
                d->yBin = h.binning_y + h.skipping_y;
            }
        }
        else if (!memcmp(blk.blockType, "RTCI", 4))
        {
            mlv_rtci_hdr_t h;
            if (FIO_ReadFile(f, &h, sizeof(h)) == (int32_t)sizeof(h))
            {
                d->have_rtci = 1;
                d->yr  = 1900 + h.tm_year;
                d->mon = h.tm_mon + 1;
                d->day = h.tm_mday;
                d->hr  = h.tm_hour;
                d->min = h.tm_min;
                d->sec = h.tm_sec;
            }
        }
        else if (!memcmp(blk.blockType, "EXPO", 4))
        {
            mlv_expo_hdr_t h;
            if (FIO_ReadFile(f, &h, sizeof(h)) == (int32_t)sizeof(h))
            {
                d->have_expo = 1;
                d->iso = h.isoValue;
                d->shutter_us = (uint32_t)h.shutterValue;
            }
        }
        else if (!memcmp(blk.blockType, "LENS", 4))
        {
            mlv_lens_hdr_t h;
            if (FIO_ReadFile(f, &h, sizeof(h)) == (int32_t)sizeof(h))
            {
                d->have_lens = 1;
                d->focal_mm = h.focalLength;
                d->aperture100 = h.aperture;
                memcpy(d->lensName, h.lensName, 32);
                d->lensName[32] = '\0';
            }
        }
        else if (!memcmp(blk.blockType, "WBAL", 4))
        {
            mlv_wbal_hdr_t h;
            if (FIO_ReadFile(f, &h, sizeof(h)) == (int32_t)sizeof(h))
            {
                d->have_wbal = 1;
                d->kelvin = h.kelvin;
            }
        }
        else if (!memcmp(blk.blockType, "WAVI", 4))
        {
            mlv_wavi_hdr_t h;
            if (FIO_ReadFile(f, &h, sizeof(h)) == (int32_t)sizeof(h))
            {
                d->have_wavi = 1;
                d->aud_ch = h.channels;
                d->aud_rate = h.samplingRate;
            }
        }
        else if (!memcmp(blk.blockType, "VIDF", 4))
        {
            break; /* metadata blocks precede first VIDF */
        }

        pos += blk.blockSize;
    }

    FIO_CloseFile(f);
}

static void gal_draw_detail_popup(const playlist_entry_t *e, const gal_detail_t *d)
{
    int px = 52, py = 54, pw = 616, ph = 330;
    bmp_fill(COLOR_ALMOST_BLACK, px, py, pw, ph);
    ui_draw_rect(COLOR_ORANGE, px, py, pw, ph, 3);
    ui_draw_rect(COLOR_WHITE, px + 3, py + 3, pw - 6, ph - 6, 1);

    /* title bar */
    bmp_fill(COLOR_ORANGE, px + 4, py + 4, pw - 8, 30);
    ui_icon_info(px + 14, py + 13, COLOR_BLACK);
    const char *drv = (e->fullPath[0] == 'B') ? "SD" : "CF";
    bmp_printf(FONT(FONT_MED, COLOR_BLACK, COLOR_ORANGE), px + 34, py + 10,
               "CLIP DETAILS - %s", gal_basename(e->fullPath));
    bmp_printf(FONT(FONT_MED, COLOR_BLACK, COLOR_ORANGE) | FONT_ALIGN_RIGHT, px + pw - 14, py + 10,
               "[%s]", drv);

    int lx = px + 20;
    int vx = px + 168;
    int y = py + 48;
    int dy = 34;

    /* 1. Date & Size */
    bmp_printf(FONT(FONT_MED, COLOR_GRAY(70), COLOR_ALMOST_BLACK), lx, y, "File / Date");
    if (d->have_rtci)
    {
        bmp_printf(FONT(FONT_MED, COLOR_WHITE, COLOR_ALMOST_BLACK), vx, y,
                   "%d MB   %02d.%02d.%04d %02d:%02d:%02d",
                   d->fileSize / (1024 * 1024), d->day, d->mon, d->yr, d->hr, d->min, d->sec);
    }
    else
    {
        bmp_printf(FONT(FONT_MED, COLOR_WHITE, COLOR_ALMOST_BLACK), vx, y,
                   "%d MB", d->fileSize / (1024 * 1024));
    }
    y += dy;

    /* 2. Resolution & Desqueeze */
    int ax = d->xBin ? d->xBin : 1;
    int ay = d->yBin ? d->yBin : 1;
    int dw = 0, dh = 0;
    gal_desqueezed_wh(d->xRes, d->yRes, ax, ay, &dw, &dh);
    bmp_printf(FONT(FONT_MED, COLOR_GRAY(70), COLOR_ALMOST_BLACK), lx, y, "Resolution");
    if (ax != ay)
    {
        bmp_printf(FONT(FONT_MED, COLOR_CYAN, COLOR_ALMOST_BLACK), vx, y,
                   "%dx%d -> %dx%d (%dx%d)",
                   d->xRes, d->yRes, dw, dh, ax, ay);
    }
    else if (ax > 1)
    {
        bmp_printf(FONT(FONT_MED, COLOR_CYAN, COLOR_ALMOST_BLACK), vx, y,
                   "%dx%d (%dx%d bin)", d->xRes, d->yRes, ax, ay);
    }
    else
    {
        bmp_printf(FONT(FONT_MED, COLOR_CYAN, COLOR_ALMOST_BLACK), vx, y,
                   "%dx%d (1:1)", d->xRes, d->yRes);
    }
    y += dy;

    /* 3. Format */
    bmp_printf(FONT(FONT_MED, COLOR_GRAY(70), COLOR_ALMOST_BLACK), lx, y, "Format");
    bmp_printf(FONT(FONT_MED, COLOR_YELLOW, COLOR_ALMOST_BLACK), vx, y,
               "%d-bit %s   OB:%d",
               d->bpp ? sp_real_bits(d->bpp, d->blackLevel, d->whiteLevel) : 14,
               d->isRaw ? "RAW" : (d->isLossless ? "LJ92 Lossless" : "Uncompressed"),
               d->blackLevel);
    y += dy;

    /* 4. Timing & Bitrate */
    uint32_t secs = (d->fps100 > 0) ? (d->frameCount * 100 / d->fps100) : 0;
    uint32_t mbps10 = (secs > 0) ? (uint32_t)((uint64_t)d->fileSize * 10 / (secs * 1024ULL * 1024ULL)) : 0;
    bmp_printf(FONT(FONT_MED, COLOR_GRAY(70), COLOR_ALMOST_BLACK), lx, y, "Timing");
    bmp_printf(FONT(FONT_MED, COLOR_WHITE, COLOR_ALMOST_BLACK), vx, y,
               "%d.%02d fps   %df (%02d:%02d)   %d.%d MB/s",
               d->fps100 / 100, d->fps100 % 100, d->frameCount,
               secs / 60, secs % 60, mbps10 / 10, mbps10 % 10);
    y += dy;

    /* 5. Exposure */
    bmp_printf(FONT(FONT_MED, COLOR_GRAY(70), COLOR_ALMOST_BLACK), lx, y, "Exposure");
    if (d->have_expo)
    {
        char sh_str[20];
        if (d->shutter_us > 0 && d->shutter_us < 500000)
            snprintf(sh_str, sizeof(sh_str), "1/%ds", (int)((1000000U + d->shutter_us / 2) / d->shutter_us));
        else if (d->shutter_us >= 500000)
            snprintf(sh_str, sizeof(sh_str), "%d.%ds", d->shutter_us / 1000000, (d->shutter_us / 100000) % 10);
        else
            snprintf(sh_str, sizeof(sh_str), "-");

        if (d->have_wbal && d->kelvin)
            bmp_printf(FONT(FONT_MED, COLOR_WHITE, COLOR_ALMOST_BLACK), vx, y,
                       "ISO %d   %s   %dK", d->iso, sh_str, d->kelvin);
        else
            bmp_printf(FONT(FONT_MED, COLOR_WHITE, COLOR_ALMOST_BLACK), vx, y,
                       "ISO %d   %s", d->iso, sh_str);
    }
    else
    {
        bmp_printf(FONT(FONT_MED, COLOR_GRAY(60), COLOR_ALMOST_BLACK), vx, y, "N/A");
    }
    y += dy;

    /* 6. Lens */
    bmp_printf(FONT(FONT_MED, COLOR_GRAY(70), COLOR_ALMOST_BLACK), lx, y, "Lens");
    if (d->have_lens && (d->lensName[0] || d->focal_mm))
    {
        if (d->aperture100)
            bmp_printf(FONT(FONT_MED, COLOR_WHITE, COLOR_ALMOST_BLACK), vx, y,
                       "%.22s  %dmm f/%d.%d",
                       d->lensName[0] ? d->lensName : "Lens",
                       d->focal_mm, d->aperture100 / 100, (d->aperture100 / 10) % 10);
        else
            bmp_printf(FONT(FONT_MED, COLOR_WHITE, COLOR_ALMOST_BLACK), vx, y,
                       "%.26s  %dmm",
                       d->lensName[0] ? d->lensName : "Lens", d->focal_mm);
    }
    else
    {
        bmp_printf(FONT(FONT_MED, COLOR_GRAY(60), COLOR_ALMOST_BLACK), vx, y, "Manual / No Lens");
    }
    y += dy;

    /* 7. Audio */
    bmp_printf(FONT(FONT_MED, COLOR_GRAY(70), COLOR_ALMOST_BLACK), lx, y, "Audio");
    if (d->have_wavi && d->aud_rate)
    {
        bmp_printf(FONT(FONT_MED, COLOR_WHITE, COLOR_ALMOST_BLACK), vx, y,
                   "%d Hz  %s", d->aud_rate, d->aud_ch == 2 ? "Stereo" : "Mono");
    }
    else
    {
        bmp_printf(FONT(FONT_MED, COLOR_GRAY(60), COLOR_ALMOST_BLACK), vx, y, "No Audio");
    }

    /* bottom hint */
    bmp_fill(COLOR_GRAY(30), px + 16, py + ph - 36, pw - 32, 1);
    bmp_printf(FONT(FONT_MED, COLOR_ORANGE, COLOR_ALMOST_BLACK) | FONT_ALIGN_CENTER, px + pw / 2, py + ph - 28,
               "[INFO/MENU] Close     [SET] Delete Clip");
}

/* Interactive 3x2 MLV Gallery. Returns selected playlist index (>= 0) to play, or -1 to exit. */
static int mlv_gallery_run(int initial_sel)
{
    if (mlv_playlist_entries == 0)
    {
        return -1;
    }

    uint8_t *thm_buf = fio_malloc(GAL_THM_PIXELS);
    if (!thm_buf)
    {
        return initial_sel >= 0 ? initial_sel : 0;
    }

    /* pause both render_task and osd_task and wait for handshake before touching VRAM/BMP */
    mlv_play_osd_state = MLV_PLAY_MENU_HIDDEN;
    mlv_play_zoom = 0;
    raw_twk_set_zoom(0, 0, 0);
    mlv_play_in_gallery = 1;
    for (int w = 0; w < 60 && ((mlv_play_render_alive && !mlv_play_render_in_gallery_ack) ||
                               (mlv_play_osd_alive && !mlv_play_osd_in_gallery_ack)); w++)
    {
        msleep(10);
    }
    mlv_play_flush_queue(mlv_play_queue_osd);
    clrscr();
    vram_clear_lv();

    int sel = COERCE(initial_sel, 0, (int)mlv_playlist_entries - 1);
    int cur_page = -1;
    int del_confirm = 0;
    int del_sel = 0;
    int show_detail = 0;

    static mlv_thm_hdr_t page_hdr[GAL_PER_PAGE];
    int page_loaded[GAL_PER_PAGE];
    memset(page_hdr, 0, sizeof(page_hdr));
    memset(page_loaded, 0, sizeof(page_loaded));

    int result = -1;

    while (!mlv_play_render_abort && gui_state == GUISTATE_PLAYMENU)
    {
        if (mlv_playlist_entries == 0)
        {
            result = -1;
            break;
        }
        if (sel >= (int)mlv_playlist_entries)
        {
            sel = (int)mlv_playlist_entries - 1;
        }

        int page = sel / GAL_PER_PAGE;
        int pages = ((int)mlv_playlist_entries + GAL_PER_PAGE - 1) / GAL_PER_PAGE;

        if (page != cur_page)
        {
            if (cur_page >= 0)
            {
                del_confirm = 0;
                del_sel = 0;
            }
            cur_page = page;
            memset(page_hdr, 0, sizeof(page_hdr));
            memset(page_loaded, 0, sizeof(page_loaded));

            /* clear full screen and paint dark backdrop + header + cell shells */
            vram_clear_lv();
            bmp_fill(COLOR_ALMOST_BLACK, 0, 0, 720, 480);
            gal_draw_header(sel, (int)mlv_playlist_entries, page, pages, mlv_playlist[sel].fullPath);

            int base_idx = page * GAL_PER_PAGE;
            for (int s = 0; s < GAL_PER_PAGE; s++)
            {
                int idx = base_idx + s;
                int col = s % GAL_COLS;
                int row = s / GAL_COLS;
                int vx = GAL_MARGIN_X + col * (GAL_CELL_W + GAL_GAP_X) + 4;
                int vy = GAL_TOP_Y + row * (GAL_CELL_H + GAL_GAP_Y) + 4;
                gal_clear_thumb(vx, vy);
                if (idx < (int)mlv_playlist_entries)
                {
                    gal_draw_cell_frame(s, idx == sel, mlv_playlist[idx].fullPath, &page_hdr[s], 0);
                }
            }
            gal_draw_footer(&mlv_playlist[sel], &page_hdr[sel - base_idx], 0, del_confirm, del_sel);

            /* progressively load or build thumbnails for this page; yield immediately on keypress */
            for (int s = 0; s < GAL_PER_PAGE; s++)
            {
                int idx = base_idx + s;
                if (idx >= (int)mlv_playlist_entries) break;

                uint32_t pending = 0;
                msg_queue_count(mlv_play_queue_osd, &pending);
                if (pending > 0 || mlv_play_render_abort) break;

                int col = s % GAL_COLS;
                int row = s / GAL_COLS;
                int vx = GAL_MARGIN_X + col * (GAL_CELL_W + GAL_GAP_X) + 4;
                int vy = GAL_TOP_Y + row * (GAL_CELL_H + GAL_GAP_Y) + 4;

                const char *path = mlv_playlist[idx].fullPath;
                uint32_t fsz = mlv_playlist[idx].fileSize;
                int ok = gal_load_thm(path, fsz, &page_hdr[s], thm_buf);
                if (!ok)
                {
                    const char *ext = path + strlen(path) - 3;
                    if (streq(ext, "MLV") || streq(ext, "mlv"))
                    {
                        ok = gal_build_thm_mlv(path, fsz, &page_hdr[s], thm_buf);
                    }
                    else
                    {
                        ok = gal_build_thm_raw(path, fsz, &page_hdr[s], thm_buf);
                    }
                }
                if (ok)
                {
                    page_loaded[s] = 1;
                    gal_blit_thumb(vx, vy, thm_buf);
                    gal_draw_cell_frame(s, idx == sel, path, &page_hdr[s], 1);
                    if (idx == sel)
                    {
                        gal_draw_footer(&mlv_playlist[sel], &page_hdr[s], 1, del_confirm, del_sel);
                    }
                }
            }
        }

        /* if any slot on current page wasn't loaded yet (interrupted by earlier keypress), load one while idle */
        uint32_t key = 0;
        if (msg_queue_receive(mlv_play_queue_osd, &key, 60))
        {
            if (show_detail) continue;
            int base_idx = cur_page * GAL_PER_PAGE;
            for (int s = 0; s < GAL_PER_PAGE; s++)
            {
                int idx = base_idx + s;
                if (idx < (int)mlv_playlist_entries && !page_loaded[s])
                {
                    int col = s % GAL_COLS;
                    int row = s / GAL_COLS;
                    int vx = GAL_MARGIN_X + col * (GAL_CELL_W + GAL_GAP_X) + 4;
                    int vy = GAL_TOP_Y + row * (GAL_CELL_H + GAL_GAP_Y) + 4;
                    const char *path = mlv_playlist[idx].fullPath;
                    uint32_t fsz = mlv_playlist[idx].fileSize;
                    int ok = gal_load_thm(path, fsz, &page_hdr[s], thm_buf);
                    if (!ok)
                    {
                        const char *ext = path + strlen(path) - 3;
                        if (streq(ext, "MLV") || streq(ext, "mlv"))
                            ok = gal_build_thm_mlv(path, fsz, &page_hdr[s], thm_buf);
                        else
                            ok = gal_build_thm_raw(path, fsz, &page_hdr[s], thm_buf);
                    }
                    page_loaded[s] = ok ? 1 : -1; /* -1: tried and failed, don't spin */
                    if (ok)
                    {
                        gal_blit_thumb(vx, vy, thm_buf);
                        gal_draw_cell_frame(s, idx == sel, path, &page_hdr[s], 1);
                        if (idx == sel)
                        {
                            gal_draw_footer(&mlv_playlist[sel], &page_hdr[s], 1, del_confirm, del_sel);
                        }
                    }
                    break;
                }
            }
            continue;
        }

        int old_sel = sel;

        /* detail popup handling: SET/TRASH opens safe delete confirm (default Cancel); any other key closes */
        if (show_detail)
        {
            show_detail = 0;
            if (key == MODULE_KEY_PRESS_SET || key == MODULE_KEY_JOY_CENTER || key == MODULE_KEY_TRASH)
            {
                del_confirm = 1;
                del_sel = 0; /* always default to Cancel */
            }
            cur_page = -1;
            continue;
        }

        /* safe delete confirmation: always starts on [Cancel] (del_sel=0);
         * user must move to [Delete] (del_sel=1) and press SET to actually delete. */
        if (del_confirm)
        {
            int s = sel - cur_page * GAL_PER_PAGE;
            if (key == MODULE_KEY_PRESS_LEFT || key == MODULE_KEY_PRESS_RIGHT ||
                key == MODULE_KEY_WHEEL_LEFT || key == MODULE_KEY_WHEEL_RIGHT ||
                key == MODULE_KEY_WHEEL_UP   || key == MODULE_KEY_WHEEL_DOWN)
            {
                del_sel = !del_sel;
                gal_draw_footer(&mlv_playlist[sel], &page_hdr[s], page_loaded[s] == 1, 1, del_sel);
                continue;
            }
            else if ((key == MODULE_KEY_PRESS_SET || key == MODULE_KEY_JOY_CENTER) && del_sel == 1)
            {
                playlist_entry_t victim = mlv_playlist[sel];
                mlv_playlist_delete(victim);
                mlv_play_delete_work(victim.fullPath);
                del_confirm = 0;
                del_sel = 0;
                cur_page = -1; /* force full page redraw */
                continue;
            }
            else
            {
                del_confirm = 0;
                del_sel = 0;
                gal_draw_footer(&mlv_playlist[sel], &page_hdr[s], page_loaded[s] == 1, 0, 0);
                continue;
            }
        }

        switch (key)
        {
            case MODULE_KEY_PRESS_SET:
            case MODULE_KEY_JOY_CENTER:
                result = sel;
                goto done;

            case MODULE_KEY_PLAY:
            case MODULE_KEY_MENU:
                result = -1;
                mlv_play_render_abort = 1;
                goto done;

            case MODULE_KEY_INFO:
                {
                    static gal_detail_t detail;
                    int s = sel - cur_page * GAL_PER_PAGE;
                    gal_read_detail(&mlv_playlist[sel], &page_hdr[s], page_loaded[s] == 1, &detail);
                    gal_draw_detail_popup(&mlv_playlist[sel], &detail);
                    show_detail = 1;
                }
                break;

            case MODULE_KEY_TRASH:
                del_confirm = 1;
                del_sel = 0; /* always default to Cancel */
                {
                    int s = sel - cur_page * GAL_PER_PAGE;
                    gal_draw_footer(&mlv_playlist[sel], &page_hdr[s], page_loaded[s] == 1, 1, 0);
                }
                break;

            case MODULE_KEY_PRESS_RIGHT:
            case MODULE_KEY_WHEEL_RIGHT:
            case MODULE_KEY_WHEEL_DOWN:
                if (sel + 1 < (int)mlv_playlist_entries) sel++;
                break;

            case MODULE_KEY_PRESS_LEFT:
            case MODULE_KEY_WHEEL_LEFT:
            case MODULE_KEY_WHEEL_UP:
                if (sel > 0) sel--;
                break;

            case MODULE_KEY_PRESS_DOWN:
                if (sel + GAL_COLS < (int)mlv_playlist_entries) sel += GAL_COLS;
                break;

            case MODULE_KEY_PRESS_UP:
                if (sel >= GAL_COLS) sel -= GAL_COLS;
                break;
        }

        if (sel != old_sel && (sel / GAL_PER_PAGE) == cur_page)
        {
            int base_idx = cur_page * GAL_PER_PAGE;
            int os_slot = old_sel - base_idx;
            int ns_slot = sel - base_idx;
            gal_draw_cell_frame(os_slot, 0, mlv_playlist[old_sel].fullPath, &page_hdr[os_slot], page_loaded[os_slot] == 1);
            gal_draw_cell_frame(ns_slot, 1, mlv_playlist[sel].fullPath,     &page_hdr[ns_slot], page_loaded[ns_slot] == 1);
            gal_draw_header(sel, (int)mlv_playlist_entries, cur_page, pages, mlv_playlist[sel].fullPath);
            gal_draw_footer(&mlv_playlist[sel], &page_hdr[ns_slot], page_loaded[ns_slot] == 1, 0, 0);
        }
    }

done:
    fio_free(thm_buf);
    mlv_play_in_gallery = 0;
    mlv_play_flush_queue(mlv_play_queue_osd);
    mlv_play_clear_screen();
    return result;
}

static struct semaphore * mlv_play_sem = 0;

static void mlv_play_task(void *priv)
{
    /* we will later restore that value */
    int old_black_level = raw_info.black_level;
    int old_white_level = raw_info.white_level;
    int old_bits_per_pixel = raw_info.bits_per_pixel;
    
    if (take_semaphore(mlv_play_sem, 100))
    {
        NotifyBox(2000, "mlv_play already running");
        beep();
        return;
    }
    
    FILE **chunk_files = NULL;
    uint32_t chunk_count = 0;
    char *filename = (char *)priv;
    int start_in_gallery = (filename == NULL);

    /* get into Canon's PLAY mode */
    mlv_play_enter_playback();

    /* create playlist (sorted newest first) */
    mlv_play_show_dlg(0, "Scanning clips...");
    mlv_playlist_build(0);
    mlv_play_show_dlg(0, 0);

    if (mlv_playlist_entries <= 0 && !filename)
    {
        mlv_play_show_dlg(2000, "No videos found");
        goto cleanup;
    }

    int gal_sel = 0;
    if (filename)
    {
        uint32_t size = 0;
        if (FIO_GetFileSize(filename, &size))
        {
            char msg[50];
            snprintf(msg, sizeof(msg), "%s\nnot found", strrchr(filename, '/') + 1);
            mlv_play_show_dlg(4000, msg);
            start_in_gallery = 1;
            filename = NULL;
        }
        else
        {
            strncpy(mlv_play_current_filename, filename, sizeof(mlv_play_current_filename) - 1);
            playlist_entry_t tmp;
            strncpy(tmp.fullPath, filename, sizeof(tmp.fullPath) - 1);
            int32_t pos = mlv_playlist_find(tmp);
            if (pos >= 0) gal_sel = pos;
        }
    }

    mlv_play_return_to_gallery = start_in_gallery ? 1 : 0;

    do
    {
        if (mlv_play_return_to_gallery)
        {
            mlv_play_return_to_gallery = 0;
            mlv_play_stopfile = 0;
            mlv_play_paused = 0;
            int chosen = mlv_gallery_run(gal_sel);
            if (chosen < 0 || chosen >= (int)mlv_playlist_entries || mlv_play_render_abort)
            {
                goto cleanup;
            }
            gal_sel = chosen;
            strncpy(mlv_play_current_filename, mlv_playlist[chosen].fullPath, sizeof(mlv_play_current_filename) - 1);
        }

        if (!strlen(mlv_play_current_filename))
        {
            goto cleanup;
        }
        
        strcpy(mlv_play_next_filename, "");
        
        /* open all chunks of that movie file */
        chunk_files = mlv_play_load_chunks(mlv_play_current_filename, &chunk_count);

        if(!chunk_files || !chunk_count)
        {
            bmp_printf(FONT_MED, 30, 190, "failed to load chunks");
            beep();
            msleep(1000);
            mlv_play_return_to_gallery = 1;
            continue;
        }
        
        /* clear anything on screen */
        mlv_play_clear_screen();
        mlv_play_paused = 0;
        mlv_play_osd_item = 2; /* pause/play */
        mlv_play_osd_state = MLV_PLAY_MENU_HIDDEN;
        
        /* ok now start real playback routines */
        mlv_play(mlv_play_current_filename, chunk_files, chunk_count);
        mlv_play_close_chunks(chunk_files, chunk_count);
        
        /* all files closed, anything to delete? */
        mlv_play_delete_if_requested();

        /* if user requested gallery (MENU or Grid button), loop straight back to gallery */
        if (mlv_play_return_to_gallery)
        {
            playlist_entry_t cur;
            strncpy(cur.fullPath, mlv_play_current_filename, sizeof(cur.fullPath) - 1);
            int32_t pos = mlv_playlist_find(cur);
            if (pos >= 0) gal_sel = pos;
            mlv_play_stopfile = 0;
            continue;
        }
        
        /* playback finished. wait until next/prev/gallery/exit */
        while(!strlen(mlv_play_next_filename) && !mlv_play_return_to_gallery && !mlv_play_should_stop())
        {
            if (!mlv_play_osd_alive)
            {
                mlv_play_inline_osd_poll(50);
            }
            else
            {
                msleep(50);
            }
        }

        /* anything more to delete? */
        mlv_play_delete_if_requested();

        if (mlv_play_return_to_gallery)
        {
            mlv_play_stopfile = 0;
            continue;
        }

        strncpy(mlv_play_current_filename, mlv_play_next_filename, sizeof(mlv_play_next_filename));

        /* track current index for when user later returns to gallery */
        if (strlen(mlv_play_current_filename))
        {
            playlist_entry_t cur;
            strncpy(cur.fullPath, mlv_play_current_filename, sizeof(cur.fullPath) - 1);
            int32_t pos = mlv_playlist_find(cur);
            if (pos >= 0) gal_sel = pos;
        }

        /* stop file request was handled, if there was any */
        mlv_play_stopfile = 0;

    } while(!mlv_play_should_stop());
    
cleanup:
    mlv_play_in_gallery = 0;
    mlv_play_return_to_gallery = 0;
    mlv_playlist_free();
    mlv_play_leave_playback();
    mlv_play_delete_requested = 0;
    mlv_play_osd_delete_selected = 0;
    give_semaphore(mlv_play_sem);
    
    /* undo black and white level change */
    raw_info.black_level = old_black_level;
    raw_info.white_level = old_white_level;
    raw_info.bits_per_pixel = old_bits_per_pixel;
}


void mlv_play_file(char *filename)
{
    gui_stop_menu();
    
    task_create("mlv_play_task", 0x1e, 0x4000, mlv_play_task, (void*)filename);
}

FILETYPE_HANDLER(mlv_play_filehandler)
{
    /* there is no header and clean interface yet */
    switch(cmd)
    {
        case FILEMAN_CMD_INFO:
        {
            FILE* f = FIO_OpenFile( filename, O_RDONLY | O_SYNC );
            if (!f)
            {
                return 0;
            }
            
            if(mlv_play_is_mlv(f))
            {
                strcpy(data, "A MLV v2.0 Video");
            }
            else if(mlv_play_is_raw(f))
            {
                strcpy(data, "A RAW Video");
            }
            else
            {
                strcpy(data, "Invalid RAW video format");
            }
            return 1;
        }
        
        case FILEMAN_CMD_VIEW_OUTSIDE_MENU:
        {
            mlv_play_file(filename);
            return 1;
        }
    }
    
    return 0; /* command not handled */
}

static unsigned int mlv_play_keypress_cbr(unsigned int key)
{
    if (mlv_play_rendering)
    {
        switch(key)
        {
            case MODULE_KEY_UNPRESS_SET:
            case MODULE_KEY_TOUCH_1_FINGER:
            case MODULE_KEY_UNTOUCH_1_FINGER:
            case MODULE_KEY_TOUCH_2_FINGER:
            case MODULE_KEY_UNTOUCH_2_FINGER:
            {
                return 0;
            }

            case MODULE_KEY_PRESS_SET:
            case MODULE_KEY_WHEEL_UP:
            case MODULE_KEY_WHEEL_DOWN:
            case MODULE_KEY_WHEEL_LEFT:
            case MODULE_KEY_WHEEL_RIGHT:
            case MODULE_KEY_JOY_CENTER:
            case MODULE_KEY_PRESS_UP:
            case MODULE_KEY_PRESS_UP_RIGHT:
            case MODULE_KEY_PRESS_UP_LEFT:
            case MODULE_KEY_PRESS_RIGHT:
            case MODULE_KEY_PRESS_LEFT:
            case MODULE_KEY_PRESS_DOWN_RIGHT:
            case MODULE_KEY_PRESS_DOWN_LEFT:
            case MODULE_KEY_PRESS_DOWN:
            case MODULE_KEY_UNPRESS_UDLR:
            case MODULE_KEY_INFO:
            case MODULE_KEY_Q:
            case MODULE_KEY_PICSTYLE:
            case MODULE_KEY_PRESS_DP:
            case MODULE_KEY_UNPRESS_DP:
            case MODULE_KEY_RATE:
            case MODULE_KEY_TRASH:
            case MODULE_KEY_PLAY:
            case MODULE_KEY_MENU:
            case MODULE_KEY_PRESS_ZOOMIN:
            {
                msg_queue_post(mlv_play_queue_osd, (uint32_t) key);
                return 0;
            }

            /* ignore zero keycodes. pass through or not? */
            case 0:
            {
                return 0;
            }
            
            /* any other key aborts playback */
            default:
            {
                /*
                int loops = 0;
                while(loops < 50)
                {
                    bmp_printf(FONT_MED, 30, 400, "key 0x%02X not handled, exiting", key);
                    loops++;
                }
                */
                mlv_play_render_abort = 1;
                return 0;
            }
        }
    }
    else if(raw_video_enabled || mlv_video_enabled)
    {
        if (!is_movie_mode())
            return 1;

        if (!liveview_display_idle())
            return 1;
        
        if (RECORDING)
            return 1;
        
        switch(key)
        {
            case MODULE_KEY_PLAY:
            {
                task_create("mlv_play_task", 0x1e, 0x4000, mlv_play_task, NULL);
                return 0;
            }
        }
        
    }
    
    return 1;
}

static unsigned int mlv_play_init()
{
    /* setup log file */
    mlv_play_trace_ctx = trace_start("mlv_play", "mlv_play.log");
    trace_set_flushrate(mlv_play_trace_ctx, 500);
    trace_format(mlv_play_trace_ctx, TRACE_FMT_TIME_REL | TRACE_FMT_COMMENT, ' ');
    
    /* setup queues for frame buffers */
    mlv_play_queue_empty = (struct msg_queue *) msg_queue_create("mlv_play_queue_empty", 10);
    mlv_play_queue_render = (struct msg_queue *) msg_queue_create("mlv_play_queue_render", 10);
    mlv_play_queue_osd = (struct msg_queue *) msg_queue_create("mlv_play_queue_osd", 10);
    mlv_play_queue_fps = (struct msg_queue *) msg_queue_create("mlv_play_queue_fps", 100);
    
    mlv_playlist_queue = (struct msg_queue *) msg_queue_create("mlv_playlist_queue", 500);
    mlv_playlist_scan_queue = (struct msg_queue *) msg_queue_create("mlv_playlist_scan_queue", 500);
    
    fileman_register_type("RAW", "RAW Video", mlv_play_filehandler);
    fileman_register_type("MLV", "MLV Video", mlv_play_filehandler);
    
    mlv_play_sem = create_named_semaphore("mlv_play_running", 1);
    prx_sem = create_named_semaphore("mlv_play_prx", 1);
    sp_lut_sem = create_named_semaphore("mlv_play_lut", 1);

    return 0;
}

static unsigned int mlv_play_deinit()
{
    return 0;
}

MODULE_INFO_START()
    MODULE_INIT(mlv_play_init)
    MODULE_DEINIT(mlv_play_deinit)
MODULE_INFO_END()

MODULE_CBRS_START()
    MODULE_CBR(CBR_KEYPRESS, mlv_play_keypress_cbr, 0)
MODULE_CBRS_END()

MODULE_PROPHANDLERS_START()
MODULE_PROPHANDLERS_END()

MODULE_CONFIGS_START()
    MODULE_CONFIG(mlv_play_quality)
    MODULE_CONFIG(mlv_play_exact_fps)
    MODULE_CONFIG(mlv_play_lut)
    MODULE_CONFIG(mlv_play_audio)
MODULE_CONFIGS_END()
