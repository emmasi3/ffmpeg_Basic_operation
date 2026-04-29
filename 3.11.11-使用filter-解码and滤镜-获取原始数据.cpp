#include <iostream>

extern"C"
{
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include<libavutil/opt.h>
}

static AVFormatContext* fmt_ctx = nullptr;
AVCodecContext* codec_ctx = nullptr;

static int idx_v = -1;

//滤镜处理
static int filter_video(AVFrame *frame)
{
    return 0;
}

//解码 + 滤镜处理
static int decode_frame_and_filter(AVFrame* frame)
{
    int ret = -1;

    ret = avcodec_receive_frame(codec_ctx, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
    {
        goto _EXIT;
    }
    else if (ret < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Error while receiving a frame from decoder!\n");
        goto _EXIT;
    }

    filter_video(frame);

    return 0;
_EXIT:
    return ret;
}

static int open_input_file(const char* filename)
{
    int ret = -1;
    const AVCodec* codec_v = nullptr;

    ret = avformat_open_input(&fmt_ctx, filename, NULL, NULL);
    if (ret < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Failed to open the file:%s\n", filename);
        goto _ERROR;
    }

    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Failed to find stream to fmt_ctx\n");
        goto _ERROR;
    }

    ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec_v, NULL);
    if (ret < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "not find best stream\n");
        goto _ERROR;
    }
    //解码器上下文（从源文件的流信息中读取）
    codec_ctx = avcodec_alloc_context3(codec_v);
    if (!codec_ctx)
    {
        av_log(NULL, AV_LOG_ERROR, "no memory!\n");
        goto _ERROR;
    }

    ret = avcodec_parameters_to_context(codec_ctx, fmt_ctx->streams[idx_v]->codecpar);
    if (ret < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "could not copy codecpar to codec_ctx\n");
        goto _ERROR;
    }

    //打开解码器
    ret = avcodec_open2(codec_ctx, codec_v, NULL);
    if (ret < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "not to open the codec_v!\n");
        goto _ERROR;
    }

    return 0;
_ERROR:
    return ret;
}

static int init_filters(const char* filter_desc,
    AVFormatContext* fmt_ctx,
    AVCodecContext* dec_ctx,
    int v_stream_index,
    AVFilterGraph** graph,
    AVFilterContext** buf_ctx,
    AVFilterContext** bufsink_ctx) {

    int ret = -1;

    char args[512] = {};
    AVRational time_base = fmt_ctx->streams[v_stream_index]->time_base;

    AVFilterInOut* inputs = avfilter_inout_alloc();
    AVFilterInOut* outputs = avfilter_inout_alloc();

    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_GRAY8, AV_PIX_FMT_NONE };

    if (!inputs || !outputs) {
        av_log(NULL, AV_LOG_ERROR, "No Memory when alloc inputs or outputs!\n");
        return AVERROR(ENOMEM);
    }

    *graph = avfilter_graph_alloc();
    if (!(*graph)) {
        av_log(NULL, AV_LOG_ERROR, "No Memory when create graph!\n");
        return AVERROR(ENOMEM);
    }
    //原始数据存放点
    const AVFilter* bufsrc = avfilter_get_by_name("buffer");
    if (!bufsrc) {
        av_log(NULL, AV_LOG_ERROR, "Failed to get buffer filter!\n");
        return -1;
    }
    //处理后的数据存放点
    const AVFilter* bufsink = avfilter_get_by_name("buffersink");
    if (!bufsink) {
        av_log(NULL, AV_LOG_ERROR, "Failed to get buffersink filter!\n");
        return -1;
    }

    //输入 buffer filter
    //"[in]drawbox=xxxx[out]"
    snprintf(args, 512,
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
        dec_ctx->width, dec_ctx->height,
        dec_ctx->pix_fmt,
        time_base.num, time_base.den,
        dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den);

    if ((ret = avfilter_graph_create_filter(buf_ctx, bufsrc, "in", args, NULL, *graph)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to create buffer filter context!\n");
        goto __ERROR;
    }

    //输出 buffer sink filter
    if ((ret = avfilter_graph_create_filter(bufsink_ctx, bufsink, "out", NULL, NULL, *graph)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to create buffer sink filter context!\n");
        goto __ERROR;
    }
    av_opt_set_int_list(*bufsink_ctx, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);

    //create in/out
    inputs->name = av_strdup("out");
    inputs->filter_ctx = *bufsink_ctx; //1、这里你可能有点疑惑，为什么inputs输入端要连接最终的 图的输出端“bufsink_ctx”而不是buf_ctx
    inputs->pad_idx = 0;
    inputs->next = NULL;

    outputs->name = av_strdup("in");
    outputs->filter_ctx = *buf_ctx;
    outputs->pad_idx = 0;
    outputs->next = NULL;

    //create filter and add graph for filter desciption
    if ((ret = avfilter_graph_parse_ptr(*graph, filter_desc, &inputs, &outputs, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to parse filter description!\n");
        goto __ERROR;
    }

    if ((ret = avfilter_graph_config(*graph, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to config graph!\n");
    }

__ERROR:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret;
}

int main(int argc, char* argv[])
{
    int ret = -1;

    AVPacket pkt_v;
    AVFrame* frame = nullptr;
    const char* filter_desc = "drawbox=30:10:64:664:red";
    const char* filename = nullptr;
    av_log_set_level(AV_LOG_DEBUG);

    if (argc < 2)
    {
        av_log(NULL, AV_LOG_ERROR, "the arguments must be more than 2!\n");
        goto _ERROR;
    }

    frame = av_frame_alloc();
    if (!frame)
    {
        av_log(NULL, AV_LOG_ERROR, "no memory to alloc frame\n");
        goto _ERROR;
    }

    filename = argv[1];
    if (open_input_file(filename) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "the open_input_file(filename) is failed!\n");
        goto _ERROR;
    }
    //else
    //{
    //    if (init_filters(filter_desc) < 0)
    //    {
    //        av_log(NULL, AV_LOG_ERROR, "the initialize filter is failed!\n");
    //        goto _ERROR;
    //    }
    //}

    while (true)
    {
        if ((ret = av_read_frame(fmt_ctx, &pkt_v)) < 0)
        {
            break;
        }

        if (pkt_v.stream_index == idx_v)
        {
            ret = avcodec_send_packet(codec_ctx, &pkt_v);
            if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "Failed to sned packet to decoder!\n");
                break;
            }
            //解码，并且对解码后的数据进行滤镜处理
            if (decode_frame_and_filter(frame) < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "Failed to decode or filter!\n");
                break;
            }
        }

        
    }


    return 0;
_ERROR:
    if (fmt_ctx)
        avformat_free_context(fmt_ctx);
    if (codec_ctx)
        avcodec_free_context(&codec_ctx);
    if (frame)
        av_frame_free(&frame);
}