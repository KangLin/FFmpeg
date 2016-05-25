/*
* Copyright (c) 2016 KangLin<kl222@126.com>
*
* This file is part of FFmpeg.
*
* FFmpeg is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* FFmpeg is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with FFmpeg; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

/**
 * @file
 * Snapshot video filter, it can save a snapshot picture.
 * Author: KangLin<kl222@126.com>
 */

#include <float.h>
#include <stdint.h>

#include "libavutil/attributes.h"
#include "libavutil/avstring.h"
#include "libavutil/avassert.h"
#include "libavutil/opt.h"
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/timestamp.h"
#include "libavformat/avformat.h"
#include "libavformat/os_support.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

typedef struct SnapshotContext {
    /* common A/V fields */
    const AVClass *class;

    char *dir;
    char *filename;

    int bEnable;
    AVFormatContext *ofmt_ctx;
    AVCodecContext *codec_ctx;
    AVCodec *codec;
    AVPacket *out_packet;

}SnapshotContext;

#define OFFSET(x) offsetof(SnapshotContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM

static const AVOption snapshot_options[] = {
    { "directory", NULL, OFFSET(dir), AV_OPT_TYPE_STRING, { .str = "snapshot" }, .flags = FLAGS },
    { "filename", NULL, OFFSET(filename), AV_OPT_TYPE_STRING, { .str = "snapshot.png" }, .flags = FLAGS },
    { NULL },
};

static int snapshot_close(AVFilterContext *ctx) {
    int ret = 0;
    SnapshotContext *s = ctx->priv;

    avcodec_free_context(&s->codec_ctx);
    s->codec_ctx = NULL;

    if (s->ofmt_ctx) {
        av_packet_free(&s->out_packet);
        avformat_free_context(s->ofmt_ctx);
        s->ofmt_ctx = NULL;
    }
    return ret;
}

static int snapshot_open(AVFilterContext *ctx, AVFrame* frame){
    int ret = 0;
    SnapshotContext *s = ctx->priv;
    AVStream *out_stream = NULL;
    char f[1024];
    f[0] = 0;
    if (s->ofmt_ctx)
        return 0;

    if (s->dir){
        av_strlcpy(f, s->dir, 1024);
        if (mkdir(s->dir, 0777) == -1 && errno != EEXIST) {
            av_log(ctx, AV_LOG_ERROR, "Could not create directory %s with use_localtime_mkdir\n", s->dir);
            return AVERROR(errno);
        }
        av_strlcat(f, "/", 1024);
    }
    if (s->filename)
        av_strlcat(f, s->filename, 1024);
    else{
        av_log(ctx, AV_LOG_ERROR, "please set filename.\n");
        return AVERROR(EPERM);
    }

    ret = avformat_alloc_output_context2(&s->ofmt_ctx, NULL, NULL, f);
    if (ret < 0){
        av_log(ctx, AV_LOG_ERROR, "open file is fail:%d;filename:%s\n", ret, f);
        return ret;
    }
    if (!s->ofmt_ctx) {
        av_log(ctx, AV_LOG_ERROR, "open file is fail:%d\n", ret);
        return ret;
    }

    s->out_packet = av_packet_alloc();

    do{
        s->codec = avcodec_find_encoder(s->ofmt_ctx->oformat->video_codec);
        if (!s->codec){
            av_log(ctx, AV_LOG_ERROR, "encodec isn't found.codec id:%d\n",
                   s->ofmt_ctx->oformat->video_codec);
            break;
        }

        out_stream = avformat_new_stream(s->ofmt_ctx, s->codec);
        if (!out_stream) {
            av_log(ctx, AV_LOG_ERROR, "Failed allocating output stream\n");
            ret = AVERROR_UNKNOWN;
            break;
        }

        //Write file header
        ret = avformat_write_header(s->ofmt_ctx, NULL);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "avformat_write_header is fail:%d\n", ret);
            break;
        }

        s->codec_ctx = avcodec_alloc_context3(s->codec);
        if (!s->codec_ctx)
            break;
        s->codec_ctx->width = frame->width;
        s->codec_ctx->height = frame->height;
        s->codec_ctx->pix_fmt = s->codec->pix_fmts[0];
        s->codec_ctx->time_base = (AVRational){ 1, 1 };

        if ((ret = avcodec_open2(s->codec_ctx, s->codec, NULL)) < 0) {
            av_log(ctx, AV_LOG_ERROR, "Cannot open video encoder:%d\n", ret);
            break;
        }

        return ret;
    }while (0);

    snapshot_close(ctx);

    return ret;
}

static int snapshot_save(AVFilterContext *ctx, AVFrame* frame){
    int ret = 0;
    SnapshotContext *s = ctx->priv;
    int get_out_frame = 0;

    ret = snapshot_open(ctx, frame);
    if (ret < 0)
        return ret;

    ret = avcodec_encode_video2(s->codec_ctx, s->out_packet, frame, &get_out_frame);
    if (ret < 0){
        av_log(ctx, AV_LOG_ERROR, "Cannot open video encoder:%d\n", ret);
        snapshot_close(ctx);
        return ret;
    }

    if (get_out_frame)
    {
        ret = av_interleaved_write_frame(s->ofmt_ctx, s->out_packet);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Error muxing packet\n");
        }
        //Write file trailer
        av_write_trailer(s->ofmt_ctx);
        s->bEnable = 0;
        snapshot_close(ctx);
    }

    return ret;
}

static int snapshot_filter_frame(AVFilterLink *inlink, AVFrame *frame){
    int ret = 0;
    AVFilterContext *ctx = inlink->dst;
    SnapshotContext *s = ctx->priv;
    if (s->bEnable) {
        ret = snapshot_save(ctx, frame);
        if (ret) {
            av_log(ctx, AV_LOG_ERROR, "snapshot save fail:%d\n", ret);
            s->bEnable = 0;
        }
    }
    ret = ff_filter_frame(inlink->dst->outputs[0], frame);
    return ret;
}

static av_cold void snapshot_uninit(AVFilterContext *ctx){
    snapshot_close(ctx);
}

static av_cold int snapshot_init(AVFilterContext *ctx){
    SnapshotContext *snapshot = ctx->priv;
    snapshot->bEnable = 0;
    return 0;
}

static int snapshot_process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                                    char *res, int res_len, int flags)
{
    int ret = 0;
    SnapshotContext *snapshot = ctx->priv;

    if (!strcmp(cmd, "filename")){
        ret = av_opt_set(snapshot, "filename", args, 0);
        snapshot->bEnable = 1;
    }
    return ret;
}

static const AVFilterPad avfilter_snapshot_inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .filter_frame = snapshot_filter_frame,
    },
    { NULL }
};

static const AVFilterPad avfilter_snapshot_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFILTER_DEFINE_CLASS(snapshot);
AVFilter ff_vf_snapshot = {
    .name = "snapshot",
    .description = NULL_IF_CONFIG_SMALL("Snapshot filter, it can save a snapshot picture. Supports .png, .jpg, .bmp formats"),
    .priv_size = sizeof(SnapshotContext),
    .priv_class = &snapshot_class,
    .init = snapshot_init,
    .uninit = snapshot_uninit,
    .process_command = snapshot_process_command,
    .inputs = avfilter_snapshot_inputs,
    .outputs = avfilter_snapshot_outputs,
};
