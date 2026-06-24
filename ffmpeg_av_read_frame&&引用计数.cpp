#include <iostream>
#include <string>
extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

/*
* @brief 此测试文件目的：看 av_read_frame 对于传入的 pkt 有什么要求？并且对于 pkt 的处理如何？比如引用计数！
* 
* 1、av_read_frame 是否接受一个通过 av_packet_alloc() 分配内存空间的 AVPacket 包？
*   能，这也是标准流程；
* 
* 2、是否能够接受 nullptr，不能，虽然文档中没有明确表明，但即使是从正常的思维中也可以进行排排除，如：他既然是要往 pkt 中写入数据，那么
*   最起码要有一个容器吧？也就是由空间要能够写入吧？传入 nullptr 什么意思？往空里面写？不合理；
* 
* 3、av_packet_alloc() 分配的 AVPacket，buf 也就是 AVBufferRef，是空的，也就是他只分配了 AVPacket 结构体本身的内存空间，字段都是 NULL；
* 
* 4、av_read_frame 官方注释中：内部会初始化AVPacket包，这里的 “未初始化” 的包，指的就是 AVPacket 结构体变量 or 通过 av_packet_alloc() 分配的；
* 
* 5、官方注释中：该数据包不能有需要释放的数据，意思也就是它在检查用户给的 AVPacket* 时，不会负责释放它底层的音视频数据(堆内存)，要用户自己管理
*   当然，这样的一种情况：（1）项目 QT 播放器中，使用 pkt 包接收了一次音视频数据后，送入生产者队列，但是并没有调用 av_packet_unref 减引用计数，
*   也就是该 pkt 在下次给到 av_read_frame 时，内部是有 “需要 delete 释放的数据的”，这时候，错了吗？
*   解释：不会有任何问题，内部会将 AVBufferRef : buf，pkt->buf 直接替换为新获取到的 音视频数据，不在托管上一份数据，这时候，如果队列那边没有接管
*       上一份 音视频数据的包，就会发生内存泄漏，也就是用户已经丢失了该堆内存的所有权，你找不到了，释放不了；
*       所以最好是，谁申请，谁释放，最好在生产者端就做好；
*       但是错了吗？没有！
*       只需要保证消费端取出包之后，即使释放就行，没有一点问题！
*/



/*
* @brief 分析数据包
*/
static int basic_read_example(const char* filename) 
{
    AVFormatContext* fmt_ctx = NULL;
    AVPacket* pkt = NULL;
    int ret = 0;

    // 1. 打开输入文件
    if ((ret = avformat_open_input(&fmt_ctx, filename, NULL, NULL)) < 0) {
        fprintf(stderr, "无法打开文件\n");
        return ret;
    }

    // 2. 获取流信息
    if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
        fprintf(stderr, "无法获取流信息\n");
        avformat_close_input(&fmt_ctx);
        return ret;
    }

    // 3. 打印格式信息
    av_dump_format(fmt_ctx, 0, filename, 0);

    // 4. 创建AVPacket
    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "无法分配packet\n");
        avformat_close_input(&fmt_ctx);
        return AVERROR(ENOMEM);
    }

    //（1）查看刚分配内存，pkt的引用计数
    if (pkt->buf)
    {
        printf("count(pkt) = %d\n",
            av_buffer_get_ref_count(pkt->buf));
    }

    // 5. 读取数据包循环
    int video_packets = 0;
    int audio_packets = 0;
    int other_packets = 0;

    //printf("开始读取数据包...\n");
    while (1) {
        ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                printf("文件读取完成\n");
            }
            else {
                fprintf(stderr, "读取错误\n");
            }
            break;
        }

        // （2）读取到包之后，再次查看
        if (pkt->buf)
        {
            printf("count(pkt) = %d\n",
                av_buffer_get_ref_count(pkt->buf));
        }

        // 6. 处理不同流类型
        AVStream* stream = fmt_ctx->streams[pkt->stream_index];
        AVMediaType type = stream->codecpar->codec_type;

        switch (type) {
        case AVMEDIA_TYPE_VIDEO:
            video_packets++;
            break;
        case AVMEDIA_TYPE_AUDIO:
            audio_packets++;
            break;
        default:
            other_packets++;
            break;
        }

        // 7. 显示packet信息
        AVRational time_base = stream->time_base;
        double pts_seconds = pkt->pts * av_q2d(time_base);
        double dts_seconds = pkt->dts * av_q2d(time_base);

        //printf("流[%d]: 类型=%s, 大小=%6d, PTS=%.3fs, DTS=%.3fs, 时长=%.3fs, 关键帧=%s\n",
        //    pkt->stream_index,
        //    av_get_media_type_string(type),
        //    pkt->size,
        //    pts_seconds,
        //    dts_seconds,
        //    pkt->duration * av_q2d(time_base),
        //    (pkt->flags & AV_PKT_FLAG_KEY) ? "是" : "否");

        // 8. 释放当前packet
        av_packet_unref(pkt);

        // （3）看减引用计数后，计数是否还有效
        if (pkt->buf)
        {
            printf("count(pkt) = %d\n",
                av_buffer_get_ref_count(pkt->buf));
        }
    }

    // 9. 统计信息
    printf("\n统计信息:\n");
    printf("  视频包: %d\n", video_packets);
    printf("  音频包: %d\n", audio_packets);
    printf("  其他包: %d\n", other_packets);
    printf("  总包数: %d\n", video_packets + audio_packets + other_packets);

    // 10. 清理资源
    av_packet_free(&pkt);
    avformat_close_input(&fmt_ctx);

    return 0;
}

int main()
{
    const std::string filename = "./new/y.mp4";
    basic_read_example(filename.c_str());

    return 0;
}