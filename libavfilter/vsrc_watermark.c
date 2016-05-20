/*
 * @file Watermark video source, It is enable to change watermark file.
 * Author:KangLin<kl222@126.com>
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
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

typedef struct WatermarkContext {
	/* common A/V fields */
	const AVClass *class;

	char * pFileName;
	AVFormatContext *format_ctx;
	AVCodecContext *codec_ctx;
	AVCodec *dec;
	int video_stream_index;
	AVPacket packet;
	AVFrame* frame;
	int64_t pts;
	AVRational time_base, frame_rate;

}WatermarkContext;

#define OFFSET(x) offsetof(WatermarkContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM

static const AVOption watermark_options[] = {
	{ "filename", NULL, OFFSET(pFileName), AV_OPT_TYPE_STRING, .flags = FLAGS },
	{ "rate", "set video rate", OFFSET(frame_rate), AV_OPT_TYPE_VIDEO_RATE, { .str = "25" }, 0, 0, FLAGS }, 
	{ "r", "set video rate", OFFSET(frame_rate), AV_OPT_TYPE_VIDEO_RATE, { .str = "25" }, 0, 0, FLAGS }, 
	{ NULL },
};

static int watermark_read_one_frame(AVFilterContext *ctx){
	WatermarkContext *watermark = ctx->priv;
	int ret = 0;
	int got_frame = 0;

	do{
		ret = av_read_frame(watermark->format_ctx, &watermark->packet);
		if (ret < 0)
		{
			av_log(ctx, AV_LOG_ERROR, "Cannot read frame\n");
			return ret;
		}
		if (watermark->packet.stream_index != watermark->video_stream_index)
			continue;

		if (!watermark->frame)
			watermark->frame = av_frame_alloc();
		
		ret = avcodec_decode_video2(watermark->codec_ctx, watermark->frame,
			&got_frame, &watermark->packet);
		if (ret < 0) {
			av_log(ctx, AV_LOG_WARNING, "Error decoding video\n");
			continue;
		}

		if (!got_frame)
			continue;

		break;

	} while (1);

	return ret;
}

static int watermark_open(AVFilterContext *ctx)
{
	int ret = 0;
	WatermarkContext *watermark = ctx->priv;

	if (!watermark->pFileName){
		av_log(ctx, AV_LOG_ERROR, "No filename provided!\n");
		return AVERROR(EINVAL);
	}

	if ((ret = avformat_open_input(&watermark->format_ctx, watermark->pFileName,
		NULL, NULL)) < 0) {
		av_log(ctx, AV_LOG_ERROR,
			"Failed to avformat_open_input '%s'\n", watermark->pFileName);
		return ret;
	}
	if ((ret = avformat_find_stream_info(watermark->format_ctx, NULL)) < 0)
		av_log(ctx, AV_LOG_WARNING, "Failed to find stream info\n");


	/* select the video stream */
	ret = av_find_best_stream(watermark->format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1,
		&watermark->dec, 0);
	if (ret < 0) {
		av_log(ctx, AV_LOG_ERROR, "Cannot find a video stream in the input file\n");
		return ret;
	}
	if (!watermark->dec)
	{
		av_log(ctx, AV_LOG_WARNING, "Don't find codec context.\n");
	}

	watermark->video_stream_index = ret;
	watermark->codec_ctx = watermark->format_ctx->streams[watermark->video_stream_index]->codec;

	/* init the video decoder */
	if ((ret = avcodec_open2(watermark->codec_ctx, watermark->dec, NULL)) < 0) {
		av_log(ctx, AV_LOG_ERROR, "Cannot open video decoder\n");
		return ret;
	}

	ret = watermark_read_one_frame(ctx);
	return ret;
}

static int watermark_close(AVFilterContext *ctx){
	WatermarkContext *watermark = ctx->priv;
	if (watermark->frame){
		av_frame_free(&watermark->frame);
		watermark->frame = 0;
		av_free_packet(&watermark->packet);
	}
	if (watermark->codec_ctx){
		avcodec_close(watermark->codec_ctx);
		watermark->codec_ctx = 0;
	}
	if (watermark->format_ctx){
		avformat_close_input(&watermark->format_ctx);
		watermark->format_ctx = 0;
	}
	return 0;
}

static av_cold int watermark_init(AVFilterContext *ctx){
	int ret = 0;
	WatermarkContext *watermark = ctx->priv;

	watermark->time_base = av_inv_q(watermark->frame_rate);
	watermark->pts = 0;

	if (!watermark->pFileName){
		av_log(ctx, AV_LOG_ERROR, "No filename provided!\n");
		return AVERROR(EINVAL);
	}
	ret = watermark_open(ctx);
	return ret;
}

static av_cold void watermark_uninit(AVFilterContext *ctx){
	watermark_close(ctx);
}

static int watermark_query_formats(AVFilterContext *ctx){
	WatermarkContext *watermark = ctx->priv;
	int list[] = { 0, -1 };
	list[0] = watermark->format_ctx->streams[watermark->video_stream_index]->codecpar->format;
	return ff_set_common_formats(ctx, ff_make_format_list(list));
}

static int watermark_config_props(AVFilterLink *outlink) {
	WatermarkContext *watermark = outlink->src->priv;
	
	if (!watermark->frame)
	{
		av_log(outlink->src, AV_LOG_ERROR, "frame is invalid\n");
		return AVERROR(EFAULT);
	}
	outlink->w = watermark->frame->width;
	outlink->h = watermark->frame->height;
	outlink->sample_aspect_ratio = watermark->frame->sample_aspect_ratio;
	
	outlink->frame_rate = watermark->frame_rate;
	outlink->time_base = watermark->time_base;

	return 0;
}

static int watermark_request_frame(AVFilterLink *outlink){
	WatermarkContext *watermark = outlink->src->priv;
	int ret = 0;
	
	if (watermark->frame)
	{
		AVFrame *frame = av_frame_clone(watermark->frame);
		frame->pts = watermark->pts++;
		ret = ff_filter_frame(outlink, frame);
	}
	
	return ret;
}

static int watermark_process_command(AVFilterContext *ctx, const char *cmd, const char *args,
	char *res, int res_len, int flags)
{
	WatermarkContext *watermark = ctx->priv;
	int ret = 0;
	if (!strcmp(cmd, "filename")){
		ret = av_opt_set(watermark, "filename", args, 0);
		if (!ret){
			watermark_close(ctx);
			watermark_open(ctx);
		}
	}
	return 0;
}

static const AVFilterPad avfilter_vsrc_watermark_outputs[] = {
	{
		.name = "default",
		.type = AVMEDIA_TYPE_VIDEO,
		.request_frame = watermark_request_frame,
		.config_props = watermark_config_props,
	},
	{ NULL }
};

AVFILTER_DEFINE_CLASS(watermark);
AVFilter ff_vsrc_watermark = {
	.name = "watermark",
	.description = NULL_IF_CONFIG_SMALL("Watermark."),
	.priv_size = sizeof(WatermarkContext),
	.priv_class = &watermark_class,
	.init = watermark_init,
	.uninit = watermark_uninit,
	.query_formats = watermark_query_formats,
	.process_command = watermark_process_command,
	.inputs = NULL,
	.outputs = avfilter_vsrc_watermark_outputs,
};
