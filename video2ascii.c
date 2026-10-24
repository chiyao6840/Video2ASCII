/*
 * video2ascii.c
 * ---------------------------------------------------------------------------
 * 在 Linux 上把视频实时转换成"彩色字符串"画面播放的小工具。
 *
 * 功能：
 *   1. 循环提示用户输入视频文件路径；若不是可解码的视频文件，则回到上一步重输。
 *   2. 用 FFmpeg 解码，先把像素缩放到 768x432。
 *   3. 按 30 FPS 定时输出，行内用 ANSI 24 位真彩色字符串渲染。
 *   4. 可选择"视频 + 音频"(SDL2) 或"仅视频"。
 *   5. 左上角实时显示画面帧率。
 *
 * 依赖：FFmpeg (libavformat/libavcodec/libavutil/libswscale/libswresample) >= 5.0、SDL2。
 *
 * 关于 "输出画面 768x432"：
 *   视频像素先缩放到 768x432。终端里每个字符比像素高，所以把画面切成
 *   8x16 像素的字符单元 -> 96 列 x 27 行的字符网格，正好铺满 768x432。
 *   想得到更密的字符画面，改下面的 CELL_W / CELL_H 即可（见 README）。
 * ---------------------------------------------------------------------------
 */

#define _POSIX_C_SOURCE 200809L
#define SDL_MAIN_HANDLED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
#include <libavutil/rational.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#include <SDL2/SDL.h>

/* ------------------------------------------------------------------ */
/* 可调参数                                                            */
/* ------------------------------------------------------------------ */

#define OUT_W        768          /* 缩放后的像素宽 */
#define OUT_H        432          /* 缩放后的像素高 */
#define CELL_W       8            /* 每个字符覆盖的像素宽 */
#define CELL_H       16           /* 每个字符覆盖的像素高 */
#define GRID_W       (OUT_W / CELL_W)   /* 字符网格列数：96 */
#define GRID_H       (OUT_H / CELL_H)   /* 字符网格行数：27 */
#define TARGET_FPS   30           /* 输出帧率 */

/* 亮度 -> 字符 的映射表（从暗到亮） */
static const char RAMP[] = " .:-=+*#%@";
#define RAMP_LEN ((int)(sizeof(RAMP) - 1))

/* ------------------------------------------------------------------ */
/* 全局状态                                                            */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_quit = 0;   /* 收到 Ctrl+C 或按键 q 后置 1 */
static int g_sdl_ok = 0;                    /* SDL 音频子系统是否可用 */

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

/* 单调时钟，单位秒 */
static double now_sec(void)
{
    return av_gettime_relative() / 1000000.0;
}

/* 去掉行尾的 \r\n 及首尾空白 */
static void trim(char *s)
{
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' '  || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i > 0) memmove(s, s + i, strlen(s + i) + 1);
}

/* ------------------------------------------------------------------ */
/* 终端处理                                                            */
/* ------------------------------------------------------------------ */

static struct termios g_saved_tio;
static int g_raw_on = 0;

static void term_raw_on(void)
{
    if (!isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &g_saved_tio) != 0) return;
    struct termios t = g_saved_tio;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0)
        g_raw_on = 1;
}

static void term_raw_off(void)
{
    if (g_raw_on) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_tio);
        g_raw_on = 0;
    }
}

/* 非阻塞检测是否按下了 q / Q / ESC */
static int quit_key_pressed(void)
{
    if (!isatty(STDIN_FILENO)) return 0;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = { 0, 0 };
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
        char c;
        if (read(STDIN_FILENO, &c, 1) == 1) {
            if (c == 'q' || c == 'Q' || c == 27) return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 播放器                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    AVFormatContext *fmt;
    int              vindex;
    int              aindex;
    AVStream        *vstream;
    AVStream        *astream;

    /* 视频 */
    AVCodecContext  *vctx;
    struct SwsContext *sws;
    AVFrame         *cur;         /* 当前正在显示的帧 */
    AVFrame         *next;        /* 预读的下一帧 */
    AVFrame         *rgb;         /* 缩放后的 RGB24 帧 */
    AVPacket        *pkt;
    int              have_cur;
    int              have_next;
    double           cur_pts;
    double           next_pts;
    double           last_pts;
    double           vstart;
    double           frame_dur;
    int              decoders_flushed;

    /* 音频 */
    int                 audio_on;
    AVCodecContext     *actx;
    AVFrame            *aframe;
    struct SwrContext  *swr;
    uint8_t            *abuf;
    int                 abuf_cap;
    int                 a_channels;
    SDL_AudioDeviceID   adev;

    /* 渲染缓冲 */
    char  *out;
    size_t out_cap;

    /* 帧率统计 */
    double      shown_fps;
    double      fps_win_start;
    long long   fps_frames;
} Player;

/* 预读一帧到 p->next，成功返回 0 并设置 have_next/next_pts；
 * 结束返回 AVERROR_EOF；出错返回负值。
 * 顺带把途中遇到的音频包解码并塞进 SDL 队列。 */
static int pull_video_frame(Player *p);

static void drain_audio(Player *p)
{
    if (!p->audio_on) return;

    while (avcodec_receive_frame(p->actx, p->aframe) == 0) {
        int out_samples = swr_get_out_samples(p->swr, p->aframe->nb_samples);
        if (out_samples < 0) {
            av_frame_unref(p->aframe);
            continue;
        }
        int need = out_samples * p->a_channels * (int)sizeof(int16_t);
        if (need > p->abuf_cap) {
            uint8_t *nb = (uint8_t *)realloc(p->abuf, (size_t)need);
            if (!nb) {
                av_frame_unref(p->aframe);
                return;
            }
            p->abuf = nb;
            p->abuf_cap = need;
        }

        uint8_t *outp = p->abuf;
        int got = swr_convert(p->swr,
                              &outp, out_samples,
                              (const uint8_t * const *)p->aframe->extended_data,
                              p->aframe->nb_samples);
        if (got > 0) {
            SDL_QueueAudio(p->adev, p->abuf,
                           (Uint32)(got * p->a_channels * (int)sizeof(int16_t)));
        }
        av_frame_unref(p->aframe);
    }
}

static int pull_video_frame(Player *p)
{
    for (;;) {
        int ret = avcodec_receive_frame(p->vctx, p->next);
        if (ret == 0) {
            int64_t ts = p->next->best_effort_timestamp;
            double pts;
            if (ts == AV_NOPTS_VALUE)
                pts = p->last_pts + p->frame_dur;
            else
                pts = ts * av_q2d(p->vstream->time_base) - p->vstart;
            p->last_pts = pts;
            p->next_pts = pts;
            p->have_next = 1;
            return 0;
        }

        if (ret == AVERROR(EAGAIN)) {
            ret = av_read_frame(p->fmt, p->pkt);
            if (ret < 0) {
                /* 文件读完：冲刷解码器，取剩余的帧 */
                if (!p->decoders_flushed) {
                    avcodec_send_packet(p->vctx, NULL);
                    if (p->actx) {
                        avcodec_send_packet(p->actx, NULL);
                        drain_audio(p);
                    }
                    p->decoders_flushed = 1;
                    continue;
                }
                return AVERROR_EOF;
            }

            if (p->pkt->stream_index == p->vindex) {
                avcodec_send_packet(p->vctx, p->pkt);
            } else if (p->audio_on && p->actx &&
                       p->pkt->stream_index == p->aindex) {
                avcodec_send_packet(p->actx, p->pkt);
                drain_audio(p);
            }
            av_packet_unref(p->pkt);
        } else {
            return ret;
        }
    }
}

/* 把预读帧变成当前帧（交换缓冲，避免拷贝） */
static void advance_frame(Player *p)
{
    AVFrame *t = p->cur;
    p->cur  = p->next;
    p->next = t;
    p->cur_pts  = p->next_pts;
    p->have_cur = 1;
    p->have_next = 0;
}

/* 把 p->cur 缩放成 RGB24 并渲染成彩色字符串输出 */
static void render_frame(Player *p)
{
    uint8_t *dst[4]  = { p->rgb->data[0], NULL, NULL, NULL };
    int      dstl[4] = { p->rgb->linesize[0], 0, 0, 0 };

    sws_scale(p->sws,
              (const uint8_t * const *)p->cur->data, p->cur->linesize,
              0, p->vctx->height,
              dst, dstl);

    const unsigned char *base   = p->rgb->data[0];
    const int            stride = p->rgb->linesize[0];

    char fpsbuf[32];
    snprintf(fpsbuf, sizeof(fpsbuf), "FPS %4.1f", p->shown_fps);
    int flen = (int)strlen(fpsbuf);

    size_t off = 0;
    off += (size_t)sprintf(p->out + off, "\033[1;1H");

    for (int y = 0; y < GRID_H; y++) {
        for (int x = 0; x < GRID_W; x++) {
            unsigned r = 0, g = 0, b = 0;

            for (int yy = 0; yy < CELL_H; yy++) {
                const unsigned char *row =
                    base + (size_t)(y * CELL_H + yy) * stride +
                    (size_t)x * CELL_W * 3;
                for (int xx = 0; xx < CELL_W; xx++) {
                    r += row[xx * 3 + 0];
                    g += row[xx * 3 + 1];
                    b += row[xx * 3 + 2];
                }
            }
            const int cnt = CELL_W * CELL_H;
            r /= cnt; g /= cnt; b /= cnt;

            unsigned cr = r, cg = g, cb = b;
            char ch;

            if (y == 0 && x < flen) {
                /* 左上角帧率 */
                ch = fpsbuf[x];
                cr = 255; cg = 255; cb = 0;
            } else {
                int lum = (int)((r * 299 + g * 587 + b * 114) / 1000);
                if (lum < 0)   lum = 0;
                if (lum > 255) lum = 255;
                ch = RAMP[lum * (RAMP_LEN - 1) / 255];
            }

            off += (size_t)sprintf(p->out + off,
                                   "\033[38;2;%u;%u;%um%c", cr, cg, cb, ch);
        }
        if (y < GRID_H - 1)
            p->out[off++] = '\n';    /* 最后一行不换行，避免滚屏 */
    }
    off += (size_t)sprintf(p->out + off, "\033[0m");

    fwrite(p->out, 1, off, stdout);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* 音频初始化（SDL2 + swresample）                                     */
/* ------------------------------------------------------------------ */

static void setup_audio(Player *p, int want_audio)
{
    p->audio_on = 0;
    if (!want_audio || p->aindex < 0 || !p->astream) return;
    if (!g_sdl_ok) return;

    const AVCodec *codec = avcodec_find_decoder(p->astream->codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "[提示] 找不到音频解码器，仅输出视频。\n");
        return;
    }

    p->actx = avcodec_alloc_context3(codec);
    if (!p->actx) return;
    avcodec_parameters_to_context(p->actx, p->astream->codecpar);
    p->actx->pkt_timebase = p->astream->time_base;
    if (avcodec_open2(p->actx, codec, NULL) < 0) {
        fprintf(stderr, "[提示] 音频解码器打开失败，仅输出视频。\n");
        avcodec_free_context(&p->actx);
        return;
    }

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq     = (p->actx->sample_rate > 0) ? p->actx->sample_rate : 48000;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    want.samples  = 1024;
    want.callback = NULL;                 /* 使用 SDL_QueueAudio */

    p->adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (p->adev == 0) {
        fprintf(stderr, "[提示] 打开音频设备失败：%s，仅输出视频。\n",
                SDL_GetError());
        avcodec_free_context(&p->actx);
        return;
    }
    p->a_channels = have.channels;

    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout in_layout;
    memset(&in_layout, 0, sizeof(in_layout));
    if (p->actx->ch_layout.nb_channels > 0)
        av_channel_layout_copy(&in_layout, &p->actx->ch_layout);
    else
        av_channel_layout_default(&in_layout, 2);

    int ret = swr_alloc_set_opts2(&p->swr,
                                  &out_layout, AV_SAMPLE_FMT_S16, have.freq,
                                  &in_layout,  p->actx->sample_fmt,
                                  p->actx->sample_rate, 0, NULL);
    av_channel_layout_uninit(&in_layout);
    if (ret < 0 || !p->swr || swr_init(p->swr) < 0) {
        fprintf(stderr, "[提示] 音频重采样初始化失败，仅输出视频。\n");
        if (p->swr) swr_free(&p->swr);
        SDL_CloseAudioDevice(p->adev);
        p->adev = 0;
        avcodec_free_context(&p->actx);
        return;
    }

    p->aframe = av_frame_alloc();
    p->abuf_cap = 0;
    p->abuf = NULL;
    p->audio_on = 1;
}

static void teardown_audio(Player *p)
{
    if (p->adev) {
        SDL_CloseAudioDevice(p->adev);
        p->adev = 0;
    }
    if (p->swr) swr_free(&p->swr);
    if (p->aframe) av_frame_free(&p->aframe);
    if (p->actx) avcodec_free_context(&p->actx);
    free(p->abuf);
    p->abuf = NULL;
}

/* ------------------------------------------------------------------ */
/* 主播放流程                                                          */
/* ------------------------------------------------------------------ */

static int play_video(AVFormatContext *fmt, int vindex, int aindex, int want_audio)
{
    Player p;
    memset(&p, 0, sizeof(p));
    int eof = 0;                     /* 是否已读到文件结尾 */
    int last_shown = 0;              /* 结尾帧是否已经显示过 */
    p.fmt    = fmt;
    p.vindex = vindex;
    p.aindex = aindex;
    p.vstream = fmt->streams[vindex];
    p.astream = (aindex >= 0) ? fmt->streams[aindex] : NULL;

    /* ---------------- 视频解码器 ---------------- */
    const AVCodec *vcodec = avcodec_find_decoder(p.vstream->codecpar->codec_id);
    if (!vcodec) {
        fprintf(stderr, "[错误] 找不到视频解码器。\n");
        return -1;
    }
    p.vctx = avcodec_alloc_context3(vcodec);
    if (!p.vctx) return -1;
    avcodec_parameters_to_context(p.vctx, p.vstream->codecpar);
    p.vctx->pkt_timebase = p.vstream->time_base;
    p.vctx->thread_count = 0;                 /* 自动多线程 */

    if (avcodec_open2(p.vctx, vcodec, NULL) < 0) {
        fprintf(stderr, "[错误] 视频解码器打开失败。\n");
        avcodec_free_context(&p.vctx);
        return -1;
    }

    /* 帧间隔（用于时间戳缺失时的回退） */
    AVRational fr = p.vstream->avg_frame_rate;
    if (fr.num <= 0 || fr.den <= 0) fr = p.vstream->r_frame_rate;
    if (fr.num > 0 && fr.den > 0 && av_q2d(fr) > 0.0)
        p.frame_dur = 1.0 / av_q2d(fr);
    else
        p.frame_dur = 1.0 / TARGET_FPS;

    p.vstart = 0.0;
    if (p.vstream->start_time != AV_NOPTS_VALUE)
        p.vstart = p.vstream->start_time * av_q2d(p.vstream->time_base);

    /* 缩放上下文：解码像素 -> 768x432 RGB24 */
    p.sws = sws_getContext(p.vctx->width, p.vctx->height, p.vctx->pix_fmt,
                           OUT_W, OUT_H, AV_PIX_FMT_RGB24,
                           SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!p.sws) {
        fprintf(stderr, "[错误] 无法创建缩放上下文。\n");
        avcodec_free_context(&p.vctx);
        return -1;
    }

    /* ---------------- 缓冲 ---------------- */
    p.cur  = av_frame_alloc();
    p.next = av_frame_alloc();
    p.rgb  = av_frame_alloc();
    p.pkt  = av_packet_alloc();
    if (!p.cur || !p.next || !p.rgb || !p.pkt) {
        fprintf(stderr, "[错误] 内存分配失败。\n");
        goto cleanup;
    }
    p.rgb->format = AV_PIX_FMT_RGB24;
    p.rgb->width  = OUT_W;
    p.rgb->height = OUT_H;
    if (av_frame_get_buffer(p.rgb, 32) < 0) {
        fprintf(stderr, "[错误] RGB 帧缓冲分配失败。\n");
        goto cleanup;
    }
    p.last_pts = -p.frame_dur;   /* 时间戳缺失时从 0 开始推算 */

    p.out_cap = (size_t)GRID_W * GRID_H * 24 + (size_t)GRID_H * 8 + 64;
    p.out = (char *)malloc(p.out_cap);
    if (!p.out) {
        fprintf(stderr, "[错误] 输出缓冲分配失败。\n");
        goto cleanup;
    }

    /* ---------------- 音频 ---------------- */
    setup_audio(&p, want_audio);

    /* ---------------- 进入全屏字符模式 ---------------- */
    printf("\033[2J\033[?25l");     /* 清屏 + 隐藏光标 */
    fflush(stdout);
    term_raw_on();

    /* 预读前两帧 */
    if (pull_video_frame(&p) < 0) {
        fprintf(stderr, "[错误] 无法解码视频帧。\n");
        goto restore;
    }
    advance_frame(&p);               /* cur = 第 1 帧 */
    if (pull_video_frame(&p) < 0) {  /* 只有一帧 */
        p.have_next = 0;
        eof = 1;
    }

    /* ---------------- 播放循环 ---------------- */
    {
        double play_start = now_sec();
        long long n = 0;

        p.fps_win_start = play_start;
        p.fps_frames    = 0;
        p.shown_fps     = 0.0;

        if (p.audio_on)
            SDL_PauseAudioDevice(p.adev, 0);   /* 开始放音 */

        for (;;) {
            if (g_quit || quit_key_pressed()) break;

            /* 目标时间：锁定 30 FPS；若渲染落后则跳过，追上墙钟 */
            double wt = now_sec() - play_start;
            double t  = (double)n / TARGET_FPS;
            if (t < wt) {
                n = (long long)(wt * TARGET_FPS);
                t = (double)n / TARGET_FPS;
            }

            /* 选出在时刻 t 应该显示的帧 */
            while (p.have_next && p.next_pts <= t) {
                advance_frame(&p);
                if (pull_video_frame(&p) < 0) {
                    p.have_next = 0;
                    eof = 1;
                    break;
                }
            }

            if (!p.have_cur) break;
            if (eof && last_shown) break;    /* 结尾帧已显示过，收工 */

            double target = play_start + t;
            double nd = now_sec();
            if (nd < target)
                av_usleep((int64_t)((target - nd) * 1000000.0));

            render_frame(&p);
            if (eof) last_shown = 1;         /* 结尾帧保证显示一次 */

            /* 统计实际帧率 */
            p.fps_frames++;
            double nf = now_sec();
            if (nf - p.fps_win_start >= 0.5) {
                p.shown_fps = p.fps_frames / (nf - p.fps_win_start);
                p.fps_frames = 0;
                p.fps_win_start = nf;
            }

            n++;
        }
    }

restore:
    term_raw_off();
    printf("\033[0m\033[?25h");                 /* 恢复颜色 + 显示光标 */
    printf("\033[%d;1H\n", GRID_H + 1);         /* 光标移到画面下方 */
    fflush(stdout);

cleanup:
    teardown_audio(&p);
    free(p.out);
    if (p.rgb) av_frame_free(&p.rgb);
    if (p.cur) av_frame_free(&p.cur);
    if (p.next) av_frame_free(&p.next);
    if (p.pkt) av_packet_free(&p.pkt);
    if (p.sws) sws_freeContext(p.sws);
    if (p.vctx) avcodec_free_context(&p.vctx);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    av_log_set_level(AV_LOG_ERROR);
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    printf("============================================================\n");
    printf("  视频 -> 彩色字符串画面  (FFmpeg + SDL2)\n");
    printf("  输出: %dx%d 像素 -> %dx%d 字符网格 @ %d FPS\n",
           OUT_W, OUT_H, GRID_W, GRID_H, TARGET_FPS);
    printf("============================================================\n");

    SDL_SetMainReady();
    g_sdl_ok = (SDL_Init(SDL_INIT_AUDIO) == 0);
    if (!g_sdl_ok)
        fprintf(stderr, "[提示] SDL2 初始化失败：%s（将无法输出音频）\n",
                SDL_GetError());

    char path[4096];

    while (!g_quit) {
        printf("\n请输入视频文件路径（直接回车重新输入，输入 q 退出）：\n> ");
        fflush(stdout);

        if (!fgets(path, sizeof(path), stdin)) break;
        trim(path);
        if (path[0] == '\0') continue;
        if (!strcmp(path, "q") || !strcmp(path, "Q") ||
            !strcmp(path, "quit") || !strcmp(path, "exit"))
            break;

        /* ---- 检测是否为可播放的视频文件，否则回到上一步 ---- */
        AVFormatContext *fmt = NULL;
        if (avformat_open_input(&fmt, path, NULL, NULL) < 0) {
            printf("[错误] 无法打开文件：%s\n", path);
            continue;
        }
        if (avformat_find_stream_info(fmt, NULL) < 0) {
            printf("[错误] 读取流信息失败，可能不是有效的媒体文件。\n");
            avformat_close_input(&fmt);
            continue;
        }

        int vindex = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        if (vindex < 0) {
            printf("[错误] 该文件不包含视频流，不是视频文件，请重新输入。\n");
            avformat_close_input(&fmt);
            continue;
        }
        if (!avcodec_find_decoder(fmt->streams[vindex]->codecpar->codec_id)) {
            printf("[错误] 找不到该视频编码的解码器，无法播放。\n");
            avformat_close_input(&fmt);
            continue;
        }

        int aindex = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
        AVStream *vs = fmt->streams[vindex];
        double dur = (fmt->duration > 0) ? fmt->duration / (double)AV_TIME_BASE : 0.0;
        printf("[信息] 视频 %dx%d  时长 %.1fs  音频: %s\n",
               vs->codecpar->width, vs->codecpar->height, dur,
               (aindex >= 0) ? "有" : "无");

        /* ---- 选择是否输出音频 ---- */
        int want_audio = 0;
        if (aindex >= 0) {
            if (!g_sdl_ok) {
                printf("[提示] SDL2 不可用，仅输出视频。\n");
            } else {
                printf("是否同时输出音频？（y/N）：");
                fflush(stdout);
                char ans[32];
                if (fgets(ans, sizeof(ans), stdin))
                    want_audio = (ans[0] == 'y' || ans[0] == 'Y');
            }
        } else {
            printf("[信息] 未检测到音频流，仅输出视频。\n");
        }

        printf("开始播放……（按 q 或 Ctrl+C 退出）\n");
        fflush(stdout);

        g_quit = 0;
        play_video(fmt, vindex, aindex, want_audio);
        avformat_close_input(&fmt);

        if (g_quit) break;
        printf("播放结束。\n");
    }

    if (g_sdl_ok) SDL_Quit();
    printf("再见！\n");
    return 0;
}
