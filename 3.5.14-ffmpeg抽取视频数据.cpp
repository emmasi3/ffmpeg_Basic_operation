#include <iostream>
extern "C"
{
#include <libavformat/avformat.h>
#include<libavutil/log.h>
#include <libavutil/avutil.h>
}

int main(int argc,char* argv[])
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
	AVFormatContext* oFmtCtx = nullptr;
	const AVOutputFormat* outFmt = nullptr;
	AVStream* inStream = nullptr;
	AVStream* outStream = nullptr;
	AVPacket pkt;

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

	//4.打开目的文件上下文
	oFmtCtx = avformat_alloc_context();
	if (!oFmtCtx)
	{
		av_log(oFmtCtx, AV_LOG_ERROR, "no memory!\n");
		return -1;
	}
	outFmt = av_guess_format(NULL, dst, NULL);
	oFmtCtx->oformat = outFmt;

	//5.给目的文件创建（绑定）一个新的视频流
	outStream = avformat_new_stream(oFmtCtx, NULL);
	//绑定 --> 便于写入数据给目的文件
	ret = avio_open2(&oFmtCtx->pb, dst, AVIO_FLAG_WRITE, NULL, NULL);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(oFmtCtx, AV_LOG_ERROR, "%s\n", err_buf);
		goto _ERROR;
	}

	//6.设置输出视频参数
	inStream = pFmtCtx->streams[idx];
	avcodec_parameters_copy(outStream->codecpar, inStream->codecpar);
	outStream->codecpar->codec_tag = 0;

	//7.写多媒体文件头
	ret = avformat_write_header(oFmtCtx, NULL);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(oFmtCtx, AV_LOG_ERROR, "%s\n", err_buf);
		goto _ERROR;
	}

	//8.读取原数据到目的文件
	while (av_read_frame(pFmtCtx, &pkt) >= 0 )
	{
		if (pkt.stream_index == idx)
		{
			pkt.pts = av_rescale_q_rnd(pkt.pts, inStream->time_base, outStream->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
			pkt.dts = av_rescale_q_rnd(pkt.dts, inStream->time_base, outStream->time_base, (AVRounding)(AV_ROUND_PASS_MINMAX | AV_ROUND_NEAR_INF));
			pkt.duration = av_rescale_q(pkt.duration, inStream->time_base, outStream->time_base);
			pkt.stream_index = 0;
			pkt.pos = -1;
			av_interleaved_write_frame(oFmtCtx, &pkt);
			av_packet_unref(&pkt);
		}
	}

	//9.写多媒体文件尾
	ret = av_write_trailer(oFmtCtx);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(oFmtCtx, AV_LOG_ERROR, "%s\n", err_buf);
		goto _ERROR;
	}

	av_log(NULL, AV_LOG_INFO, "success!!!\n");

	//10.释放资源
_ERROR:
	if (pFmtCtx)
	{
		avformat_close_input(&pFmtCtx);
		pFmtCtx = nullptr;
	}
	if (oFmtCtx->pb)
	{
		avio_close(oFmtCtx->pb);
	}
	if (oFmtCtx)
	{
		avformat_close_input(&pFmtCtx);
		pFmtCtx = nullptr;
	}

	return 0;
}
/*
* 1.对于抽取音频数据来说，pts显示时间戳 == dts编码时间戳
*  .但是对于视频数据来说，pts == dts 很多时候不成立
* 
*/