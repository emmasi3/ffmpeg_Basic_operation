#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
extern"C"
{
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavutil/opt.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
}


/**
 * @brief open media file
 * @param filename xxx
 * @param [out] fmt_ctx xxx
 * @param [out] dec_ctx xxx
 * @return 0: success, <0: failure
 */
static int open_input_file(const char* filename,
    AVFormatContext** fmt_ctx,
    AVCodecContext** dec_ctx,
    int* v_stream_index) {

    int ret = -1;
    const AVCodec* dec = NULL;

    if ((ret = avformat_open_input(fmt_ctx, filename, NULL, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to open file %s\n", filename);
        return ret;
    }

    if ((ret = avformat_find_stream_info((*fmt_ctx), NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to find stream information!\n");
        return ret;
    }

    if ((ret = av_find_best_stream(*fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Can't find video stream!\n");
        return ret;
    }

    *v_stream_index = ret;

    *dec_ctx = avcodec_alloc_context3(dec);
    if (!(*dec_ctx)) {
        return AVERROR(ENOMEM);
    }

    avcodec_parameters_to_context(*dec_ctx, (*fmt_ctx)->streams[*v_stream_index]->codecpar);

    if ((ret = avcodec_open2(*dec_ctx, dec, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to open decoder!\n");
        return ret;
    }

    return 0;
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

    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };

    if (!inputs || !outputs) {
        av_log(NULL, AV_LOG_ERROR, "No Memory when alloc inputs or outputs!\n");
        return AVERROR(ENOMEM);
    }

    *graph = avfilter_graph_alloc();
    if (!(*graph)) {
        av_log(NULL, AV_LOG_ERROR, "No Memory when create graph!\n");
        return AVERROR(ENOMEM);
    }

    const AVFilter* bufsrc = avfilter_get_by_name("buffer");
    if (!bufsrc) {
        av_log(NULL, AV_LOG_ERROR, "Failed to get buffer filter!\n");
        return -1;
    }

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

    //create in/out，这里理解一下！！！
    inputs->name = av_strdup("out");
    inputs->filter_ctx = *bufsink_ctx;
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

static int do_frame_write(AVFrame* filt_frame, FILE* out) {

    fwrite(filt_frame->data[0], 1, filt_frame->width * filt_frame->height, out);
    fwrite(filt_frame->data[1], 1, filt_frame->width * filt_frame->height * 1 / 4, out);
    fwrite(filt_frame->data[2], 1, filt_frame->width * filt_frame->height * 1 / 4, out);

    return 0;
}

//do filter
static int filter_video(AVFrame* frame,
    AVFrame* filt_frame,
    AVFilterContext* buf_ctx,
    AVFilterContext* bufsink_ctx,
    FILE* out) {

    int ret;

    if ((ret = av_buffersrc_add_frame(buf_ctx, frame)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to feed to filter graph!\n");
        return ret;
    }

    while (1) {
        ret = av_buffersink_get_frame(bufsink_ctx, filt_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }

        if (ret < 0) {
            return ret;
        }

        //这里的do_frame函数，仅仅是将yuv黑白数据写进了.yuv文件中，其实可以在此函数中用SDL进行渲染操作，也就是直接播放的操作
        do_frame_write(filt_frame, out);
        av_frame_unref(filt_frame);
    }

    av_frame_unref(frame);

    return ret;
}

//解码视频帧并对视频帧进行滤镜处理
static int decode_frame_and_filter(AVFrame* frame,
    AVFrame* filt_frame,
    AVCodecContext* dec_ctx,
    AVFilterContext* buf_ctx,
    AVFilterContext* bufsink_ctx,
    FILE* out) {

    int ret = avcodec_receive_frame(dec_ctx, frame);
    if (ret < 0) {
        if (ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
            av_log(NULL, AV_LOG_ERROR, "Error while receiving a frame from decoder!\n");
        }

        return ret;
    }

    return filter_video(frame,
        filt_frame,
        buf_ctx,
        bufsink_ctx,
        out);
}


int main(int argc, const char* argv[]) {

    int ret;
    FILE* out = NULL;

    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* dec_ctx = nullptr;

    AVFilterGraph* graph = NULL;
    AVFilterContext* buf_ctx = NULL;
    AVFilterContext* bufsink_ctx = NULL;

    int v_stream_index = -1;

    AVPacket packet;
    AVFrame* frame = NULL;
    AVFrame* filt_frame = NULL;

    const char* filter_desc = "drawbox=30:10:64:64:red";
    const char* filename = "./new/y.mp4";
    const char* outfile = "./new/filter.yuv";

    av_log_set_level(AV_LOG_DEBUG);

    frame = av_frame_alloc();
    filt_frame = av_frame_alloc();
    if (!frame || !filt_frame) {
        av_log(NULL, AV_LOG_ERROR, "No Memory to alloc frame\n");
        exit(-1);
    }

    out = fopen(outfile, "wb");
    if (!out) {
        av_log(NULL, AV_LOG_ERROR, "Failed to open yuv file!\n");
        exit(-1);
    }

    if ((ret = open_input_file(filename,
        &fmt_ctx,
        &dec_ctx,
        &v_stream_index)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to open media file\n");
        goto __ERROR;
    }
    else {
        if ((ret = init_filters(filter_desc,
            fmt_ctx,
            dec_ctx,
            v_stream_index,
            &graph,
            &buf_ctx,
            &bufsink_ctx)) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Failed to initialize filter!\n");
            goto __ERROR;
        }
    }

    //read avpacket from media file
    while (1) {

        if ((ret = av_read_frame(fmt_ctx, &packet)) < 0) {
            goto __ERROR;
        }

        if (packet.stream_index == v_stream_index) {
            ret = avcodec_send_packet(dec_ctx, &packet);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Failed to send avpakcet to decoder!\n");
                goto __ERROR;
            }

            if ((ret = decode_frame_and_filter(frame,
                filt_frame,
                dec_ctx,
                buf_ctx,
                bufsink_ctx,
                out)) < 0) {

                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    continue;
                }

                av_log(NULL, AV_LOG_ERROR, "Failed to decode or filter!\n");
                goto __ERROR;
            }
        }
    }

__ERROR:
    if (graph) {
        avfilter_graph_free(&graph);
    }

    if (dec_ctx) {
        avcodec_free_context(&dec_ctx);
    }

    if (fmt_ctx) {
        avformat_close_input(&fmt_ctx);
    }

    if (frame) {
        av_frame_free(&frame);
    }

    if (filt_frame) {
        av_frame_free(&filt_frame);
    }

    return ret;
}
/*
* 1、传递给avformat_open_input()函数的 AVFormatContext 可以是 nullptr或者NULL,但是决不能是 “野指针”否则直接报错，
* 当然，这个API内部会自动给这个 上下文指针，用alloc（）相关函数分配内存空间，不用操心
* 
* 2、其实在整个过程中，最需要关注的就是 初始化filter的部分，也就是完整构建“过滤图”的部分，其余部分都和播放器那块一样，基本没什么差异
* 
* 3、ret == AVERROR(EAGAIN)的时候，frame里面的数据已经给到了 buffersrc 这个缓冲区，或者是解码缓冲区，这时候需要更多的数据给到 解码缓冲区或者buffersrc，
* 所以调用 av_frame_unref()重置字段是可行的
* 
* 4、这里的流程图
* [buffer] → [drawbox] → [buffersink]
*  ^             ^           ^
*  |             |           |
* outputs      解析后       inputs
* 这里的inputs和outputs指的并不是 输入端和输出端，而是“开放的输(入)出端点”，你可以简单理解为方便传递给下一个滤镜处理（buffersink之后，这种情况很少）
* 
* 5、为什么要用 inputs和outputs作为中转站来指向 输出端和输入端？为什么不直接用 buf_ctx和bufsink_ctx？
* 原因：filter_desc = "[in0] scale=640:360 [mid]; [mid] drawtext=text='Hello' [out0]";
* 在这样的一个复杂的过滤链中，需要 [in0]、[mid]···标签，标记出每一个单独的滤镜处理后的“输出”，方便传递给下一个，如果直接用 buf_ctx和bufsink_ctx，
* 复杂的过滤链实现“难度较大”
* 
* 10、ffplay -i filter.yuv -pixel_format gray8 -video_size 3840x2160 ，这可以播放加了 drawbox 滤镜的yuv原始视频，-pix_fmt 和 -s 在这个ffmpeg版本中不支持
* 但是这个 gray8 可以写为 GRAY8，没什么影响
* 这个 3840x2160 ，这个写成“x”还是“*”，好像指定的大小出来的结果都一样，内部可能优化过
* 
*/