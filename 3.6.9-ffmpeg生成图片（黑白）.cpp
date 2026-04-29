#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
extern "C"
{
#include <libavformat/avformat.h>
#include<libavutil/log.h>
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>

}

static int decode(AVCodecContext* ctx, AVFrame* frame, AVPacket* pkt, const char* filename);

static void savePic(unsigned char* buf, int linesize, int width, int height, char* name);

int main(int argc, char* argv[])
{
	av_log_set_level(AV_LOG_DEBUG);

	int ret = 0;
	int idx = 0;

	char* src = nullptr; // 输入文件 path
	char* dst = nullptr; // 输出文件

	if (argc < 3)
	{
		av_log(NULL, AV_LOG_ERROR, "the arguments must be more than 3! but now the num_args is %d\n", argc);
		return -1;
	}

	src = argv[1];
	dst = argv[2];

	char err_buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };

	//1.处理一些参数
	AVFormatContext* pFmtCtx = nullptr;
	AVCodecContext* ctx = nullptr;
	AVStream* inStream = nullptr;
	AVStream* outStream = nullptr;
	AVPacket* pkt = nullptr;
	const AVCodec* codec = nullptr;

	AVFrame* frame = nullptr;

	//2.打开多媒体文件（将上下文结构体与源文件关联起来）
	ret = avformat_open_input(&pFmtCtx, src, NULL, NULL);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(pFmtCtx, AV_LOG_ERROR, "%s\n", err_buf);
		goto _ERROR;
	}

	//3.从多媒体文件中获取视频流ID
	idx = av_find_best_stream(pFmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (idx < 0)
	{
		av_strerror(idx, err_buf, sizeof(err_buf));
		av_log(pFmtCtx, AV_LOG_ERROR, "%s\n", err_buf);
		goto _ERROR;
	}

	//4.查找解码器
	inStream = pFmtCtx->streams[idx];
	codec = avcodec_find_decoder(inStream->codecpar->codec_id);
	if (!codec)
	{
		av_log(NULL, AV_LOG_ERROR, "don't find the codec：%s\n","libx264");
		goto _ERROR;
	}

	//5.创建解码器上下文（这里只是分配内存，没有涉及到绑定）
	ctx = avcodec_alloc_context3(codec);
	if (!ctx)
	{
		av_log(NULL, AV_LOG_ERROR, "no memory!\n");
		goto _ERROR;
	}

	avcodec_parameters_to_context(ctx, inStream->codecpar);

	//5.将解码器绑定到上下文
	ret = avcodec_open2(ctx, codec, NULL);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(ctx, AV_LOG_ERROR, "Can't open the Codec：%s\n", err_buf);
		goto _ERROR;
	}

	//6.创建AVPacket（保存原始视频帧数据）
	pkt = av_packet_alloc();
	if (!pkt)
	{
		av_log(NULL, AV_LOG_ERROR, "no memory!\n");
		goto _ERROR;
	}

	//7.创建AVFrame（保存解码后的数据）
	frame = av_frame_alloc();
	if (!frame)
	{
		av_log(NULL, AV_LOG_ERROR, "no memory\n");
		goto _ERROR;
	}

	//8.读取原数据到pkt中（送去解码）
	while (av_read_frame(pFmtCtx, pkt) >= 0)
	{
		if (pkt->stream_index == idx)
		{
			decode(ctx, frame, pkt, dst);
		}
	}

	decode(ctx, frame, NULL, dst); // 清理解码器

	av_log(NULL, AV_LOG_INFO, "success!!!\n");

	//5.释放资源
_ERROR:
	if (pFmtCtx)
	{
		avformat_close_input(&pFmtCtx);
		pFmtCtx = nullptr;
	}
	if (ctx)
	{
		avcodec_free_context(&ctx);
		ctx = nullptr;
	}
	if (frame)
	{
		av_frame_free(&frame);
		frame = nullptr;
	}
	if (pkt)
	{
		av_packet_free(&pkt);
		pkt = nullptr;
	}

	return 0;
}

static int decode(AVCodecContext* ctx, AVFrame* frame, AVPacket* pkt, const char * filename)
{
	int ret = 0;
	char err_buf[AV_ERROR_MAX_STRING_SIZE];
	char buf[1024];

	ret = avcodec_send_packet(ctx, pkt); // 送去解码
	if (ret < 0) {
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(NULL, AV_LOG_ERROR, "Failed to send frame to encoder: %s\n", err_buf);
		return -1;
	}

	while (1)
	{
		ret = avcodec_receive_frame(ctx, frame); // 接受解码好的数据
		if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
		{
			return 0;
		}
		else if (ret < 0)
		{
			return -1;
		}

		snprintf(buf, sizeof(buf), "%s-%d", filename, ctx->frame_num);
		savePic(frame->data[0], frame->linesize[0], frame->width, frame->height, buf);
		if (pkt)
		{
			av_packet_unref(pkt);
		}
	}

	return 0;
}

static void savePic(unsigned char* buf, int linesize, int width, int height, char* name)
{
	FILE* f = nullptr;
	f = fopen(name, "wb");

	fprintf(f, "P5\n%d %d\n%d\n", width, height, 255);
	for (int i = 0; i < height; i++)
	{
		fwrite(buf + i * linesize, 1, width, f);
	}

	fclose(f);
}
/*
* （1）为什么编码视频或者音频的时候，需要手动显示的设置frame的width、height、format？而此时解码的时候，却不需要了？
* avcodec_receive_frame(ctx, frame);// ?? 自动填充frame->width/height/format（可能动态变化）
* 也就是说，一旦需要储存“原始数据”，就需要手动指定它的 width、height、format，，这是一定的，但是这个函数特殊就特殊在，
* 会自动设置frame的相关信息，不用手动设置
* 
*/