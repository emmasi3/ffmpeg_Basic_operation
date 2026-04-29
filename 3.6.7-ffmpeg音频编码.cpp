#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h> // 用于通道布局的定义

}

static int check_sample_fmt(const AVCodec* codec, enum AVSampleFormat sample_fmt);

static int encode(AVCodecContext* ctx, AVFrame* frame, AVPacket* pkt, FILE* out);

static int select_best_sample_rate(const AVCodec* codec);

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
	AVChannelLayout stereo_layout;
	uint16_t* samples = nullptr;

	float tincr = 0;
	float t = 0;

	//1.输入参数
	if (argc < 2)
	{
		av_log(NULL, AV_LOG_ERROR, "the arguments must be more than 2!\n");
		goto _ERROR;
	}

	dst = argv[1];
	/*avcodec_name = argv[2];*/

	//2.查找编码器（根据输入）
	//codec = avcodec_find_encoder_by_name(avcodec_name);
	codec = avcodec_find_encoder_by_name("libfdk_aac"); //这个API可以使用第三方的aac编码器
	//codec = avcodec_find_encoder(AV_CODEC_ID_AAC); //这个API只能使用FFmpeg内部的编码器
	if (!codec)
	{
		std::cout << codec << std::endl;
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
	ctx->bit_rate = 64000; // b/s
	ctx->sample_fmt = AV_SAMPLE_FMT_S16; // （1）16位深，也就是2字节--采样大小，对这个不清楚的可以看八小时码字员笔记（AAC最后）
	if (check_sample_fmt(codec, ctx->sample_fmt) == -1)
	{
		av_log(NULL, AV_LOG_ERROR, "Encoder does not support sample format!\n");
		goto _ERROR;
	}

	// 设定采样率
	ctx->sample_rate = select_best_sample_rate(codec);

	// 设定声道
	stereo_layout = AV_CHANNEL_LAYOUT_STEREO;
	av_channel_layout_copy(&ctx->ch_layout, &stereo_layout);

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

	//7.创建AVFrame（保存原始音频数据）
	frame = av_frame_alloc(); // 这个函数只是分配一个外壳
	if (!frame)
	{
		av_log(NULL, AV_LOG_ERROR, "no memory\n");
		goto _ERROR;
	}

	// 设置计算frame的data应该分配空间大小的必要参数
	frame->nb_samples = ctx->frame_size; // 采样个数
	frame->format = ctx->sample_fmt;
	frame->sample_rate = ctx->sample_rate;
	//frame->ch_layout = ctx->ch_layout;
	ret = av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout);

	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(NULL, AV_LOG_ERROR, "%s\n", err_buf);
		goto _ERROR;
	}

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

	//9.生成音频内容 
	t = 0;
	tincr = 2 * M_PI * 440 / ctx->sample_rate;

	for (int i = 0; i < 200; i++)
	{
		ret = av_frame_make_writable(frame);
		if (ret < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "Could not allocate space!\n");
			goto _ERROR;
		}

		samples = (uint16_t*)frame->data[0];

		for (int i = 0; i < frame->ch_layout.nb_channels; i++) {  // 遍历每个通道

			for (int j = 0; j < ctx->frame_size; j++) {  // 遍历每个采样点（每个声道的采样点个数）	
				samples[i + j * frame->ch_layout.nb_channels] = (int)(sin(t) * 10000);// （3）正确的索引方式,参考采样点在内存中的分布
				t += tincr;
			}
		}

		//10.编码 
		ret = encode(ctx, frame, pkt, f);
		if (ret == -1) goto _ERROR;
	}

	//10.处理编码器残留数据
	encode(ctx, NULL, pkt, f);

	av_log(NULL, AV_LOG_INFO, "success!\n");

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

static int encode(AVCodecContext* ctx, AVFrame* frame, AVPacket* pkt, FILE* out)
{
	int ret = 0;
	char err_buf[AV_ERROR_MAX_STRING_SIZE];

	ret = avcodec_send_frame(ctx, frame); // 送去编码
	if (ret < 0) {
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(NULL, AV_LOG_ERROR, "Failed to send frame to encoder: %s\n", err_buf);
		return -1;
	}

	while (1)
	{
		ret = avcodec_receive_packet(ctx, pkt); // 接受编码好的数据
		if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
		{
			av_strerror(ret, err_buf, sizeof(err_buf));
			av_log(NULL, AV_LOG_INFO, "%s\n", err_buf);
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

static int check_sample_fmt(const AVCodec* codec, enum AVSampleFormat sample_fmt)
{
	const enum AVSampleFormat* p = codec->sample_fmts;

	while (*p != AV_SAMPLE_FMT_NONE) // 这是末尾
	{
		if (*p == sample_fmt)
		{
			return 0;
		}
		++p;
	}

	return -1;
}

static int select_best_sample_rate(const AVCodec* codec)
{
	int best_samplerate = 0;

	if (!codec->supported_samplerates) // 如果不存在
	{
		return 44100; // （设置）最常用的采样率
	}

	const int* p = codec->supported_samplerates;

	// 寻找的标准：越接近44100最好，abs是绝对值函数
	for (; *p; p++)
	{
		if (!best_samplerate || abs(44100 - *p) < abs(44100 - best_samplerate))
		{
			best_samplerate = *p;
		}
	}


	return best_samplerate;

}
/*
* （1）采样大小和采样率的解释和认知
* 
* （2）av_frame_make_writable() 使 AVFrame 的数据变为可写。
*  、它解决了编码器锁定数据的问题，防止在编码器处理数据时对数据进行修改。
*  、如果数据已经是可写的，函数会直接返回成功。如果数据被锁定，它会分配新的内存并拷贝数据，
*  、确保后续的修改不会影响编码器正在处理的数据。
* 
* （3）正确的索引方式,参考采样点在内存中的分布
* 
* （4）这里的填充采样数据的方式有点不符合 “音频交错存储”的规范，也就是 L0 R0 L1 R1 ···这样的内存分布方式，只不过修改起来挺简单的
*/