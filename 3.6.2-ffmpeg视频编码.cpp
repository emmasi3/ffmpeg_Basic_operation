#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>

}

static int encode(AVCodecContext* ctx, AVFrame* frame,AVPacket* pkt,FILE* out)
{
	int ret = 0;

	ret = avcodec_send_frame(ctx, frame); // 送去编码
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to Send the frame to encoder\n");
		return -1;
	}

	while (1)
	{
		ret = avcodec_receive_packet(ctx, pkt); // 接受编码好的数据
		if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
		{
			return 0;
		}
		else if (ret < 0)
		{
			return -1;
		}

		fwrite(pkt->data, 1, pkt->size, out);
		av_packet_unref(pkt);
	}

	return 0;
}

int main(int argc, char* argv[])
{
	av_log_set_level(AV_LOG_DEBUG);

	int ret = 0;
	char err_buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
	FILE* f = nullptr;

	char* dst = nullptr;
	char* avcodec_name = nullptr;
	const AVCodec* codec = nullptr;
	AVCodecContext* ctx = nullptr;
	AVFrame* frame = nullptr;
	AVPacket* pkt = nullptr;

	//1.输入参数
	if (argc < 3)
	{
		av_log(NULL, AV_LOG_ERROR, "the arguments must be more than 3!\n");
		goto _ERROR;
	}

	dst = argv[1];
	avcodec_name = argv[2];

	//2.查找编码器（根据输入）
	codec = avcodec_find_encoder_by_name(avcodec_name);
	if (!codec)
	{
		av_log(NULL, AV_LOG_ERROR, "don't find the codec：%s\n", avcodec_name);
		goto _ERROR;
	}

	//3.创建编码器上下文（这里只是分配内存，没有涉及到绑定）
	ctx = avcodec_alloc_context3(codec);
	if (!ctx)
	{
		av_log(NULL, AV_LOG_ERROR, "no memory!\n");
		goto _ERROR;
	}

	//4.设置编码器参数
	ctx->width = 640;
	ctx->height = 480;
	ctx->bit_rate = 500000;

	ctx->time_base = { 1,25 };
	ctx->framerate = { 25,1 };

	ctx->gop_size = 10;
	ctx->max_b_frames = 1; // 一般不超过3，保证视频质量过关
	ctx->pix_fmt = AV_PIX_FMT_YUV420P; // 像素格式（原视频数据）

	if (codec->id == AV_CODEC_ID_H264) // 这是一些私有设置，就像是 TCP 协议的各种设置一样
	{
		av_opt_set(ctx->priv_data, "preset", "slow", 0);
	}

	//5.将编码器绑定到上下文
	ret = avcodec_open2(ctx, codec, NULL);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(ctx, AV_LOG_ERROR, "Can't open the Codec：%s\n", err_buf);
		goto _ERROR;
	}

	//6.创建输出文件
	f = fopen(dst, "wb");
	if (!f)
	{
		av_log(NULL, AV_LOG_ERROR, "Can't open the file：\n", dst);
		goto _ERROR;
	}

	//7.创建AVFrame（保存原始视频数据）
	frame = av_frame_alloc(); // 这个函数只是分配一个外壳
	if (!frame)
	{
		av_log(NULL, AV_LOG_ERROR, "no memory\n");
		goto _ERROR;
	}

	// 设置计算frame的data应该分配空间大小的必要参数
	frame->width = ctx->width;
	frame->height = ctx->height;
	frame->format = ctx->pix_fmt; // 像素格式

	ret = av_frame_get_buffer(frame, 0); // 这里才会给frame的data域分配内存
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Could not allocate the video frame!\n");
		goto _ERROR;
	}

	//8.创建AVPacket（保存编码后的视频）
	pkt = av_packet_alloc();
	if (!pkt)
	{
		av_log(NULL, AV_LOG_ERROR, "no memory!\n");
		goto _ERROR;
	}

	//9.生成视频内容
	for (int i = 0; i < 25; i++) // 这个 i 表示视频帧个数
	{
		ret = av_frame_make_writable(frame); // 就是检查frmae的data区域的缓冲区可写，如果不可写，就会让他变得可写
		if (ret < 0)
		{
			break;
		}

		//Y分量
		for (int y = 0; y < ctx->height; y++)
		{
			for (int x = 0; x < ctx->width; x++)
			{
				frame->data[0][y * frame->linesize[0] + x] = x + y + i * 3; //（1）这里递增的时候为什么 + x？而不是x * frame->linesize[0]？
			}
		}

		//UV分量
		for (int y = 0; y < ctx->height / 2; y++)
		{
			for (int x = 0; x < ctx->width / 2; x++)
			{
				frame->data[1][y * frame->linesize[1] + x] = 128 + y + i * 2;
				frame->data[2][y * frame->linesize[2] + x] = 64 + y + i * 5;
			}
		}

		frame->pts = i;

		//10.编码
		ret = encode(ctx, frame, pkt, f);
		if (ret == -1) 
			goto _ERROR;
	}

	//10.处理编码器残留数据
	encode(ctx, NULL, pkt, f); // 关键是第二个参数AVFrame,在函数中的具体体现为，send``函数在获得这个NULL参数后，会直接将

_ERROR:
	if (ctx)
	{
		avcodec_free_context(&ctx);
	}

	if (frame)
	{
		av_frame_free(&frame);
	}

	if (pkt)

	{
		av_packet_free(&pkt);
	}

	if (f)
	{
		fclose(f);
	}

	return 0;
}

/*
* （1）在本目录下，有YUV矩阵在内存中的分布图，必须参考着看
*  、这里的 frame->linesize[0]表示的是一行的字节数（一个Y分量，占据一个字节（8bit），存储的不是int类型，而是 unsigned char(1字节)类型），
*  、所以在一行Y分量中，可以通过 + x来遍历所有Y分量
*  、想要遍历“下一行”就要加上前一行的个数就行
*  、这里也就看出，存放 Y分量的是矩阵吗？不是，那只是人为的想象有一个矩阵而已，实际就是一个（长条）连续的数组储存着所有的 Y分量
* 
* （2）总结下来，就这几步：
* 1、创建编码器及其上下文
* 2、设置编码器的具体参数（通过上下文）
* 3、绑定编码器和上下文
* 4、创建输出文件（这里用FILE* ，最简单直接的方式，没有用FFmpeg提供的API）
* 5、创建接收原始视频数据的AVFrame* frame，并设置具体的像素参数（为了计算机能够正常计算分配空间）
* 6、创建AVPacket* pkt用来保存编码后的数据
* 
* 7、在while循环中不断获取原始视频帧数据，每获得一帧数据就通过encode函数来实现编码并写入目的文件
* 8、再次调用 encode函数用来获取 coder 中残留的frame的数据
* 、（通过avcodec_send_frame(ctx, frame) 这个函数第二个参数传递 NULL 来触发ctx的刷新数据包的操作）
*/