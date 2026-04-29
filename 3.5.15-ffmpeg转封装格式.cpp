#include <iostream>
extern "C"
{
#include <libavformat/avformat.h>
#include<libavutil/log.h>
#include <libavutil/avutil.h>
}

int main(int argc, char* argv[])
{
	av_log_set_level(AV_LOG_DEBUG);

	int ret = 0;

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
	AVPacket pkt;

	int* stream_map = nullptr;
	int stream_idx = 0;


	//2.打开多媒体文件（将上下文结构体与源文件关联起来）
	ret = avformat_open_input(&pFmtCtx, src, NULL, NULL);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(pFmtCtx, AV_LOG_ERROR, "%s\n", err_buf);
		goto _ERROR;
	}

	//4.打开目的文件上下文，这个函数直接代替了原来的两步
	avformat_alloc_output_context2(&oFmtCtx, NULL, NULL, dst);
	if (!oFmtCtx)
	{
		av_log(NULL, AV_LOG_ERROR, "no memory!\n");
		goto _ERROR;
	}

	stream_map = (int*)av_calloc(pFmtCtx->nb_streams, sizeof(int));
	if (!stream_map)
	{
		av_log(NULL, AV_LOG_ERROR, "mo memory to stream_map!\n");
		goto _ERROR;
	}

	// 遍历每一个流，只要{视频、音频、字幕流}，其余都不要，也就是过滤
	for (int i = 0; i < pFmtCtx->nb_streams; i++)
	{
		AVStream* inStream = pFmtCtx->streams[i];	
		AVCodecParameters* inCodecPar = inStream->codecpar; // 编解码器信息（参数）
		if ((inCodecPar->codec_type != AVMEDIA_TYPE_AUDIO) &&
			(inCodecPar->codec_type != AVMEDIA_TYPE_VIDEO) &&
			(inCodecPar->codec_type != AVMEDIA_TYPE_SUBTITLE)
			)
		{//如果不是这三路流，那么就要标记出来，目的：换封装的时候，不需要的就不要了
			stream_map[i] = -1; // 这是不需要的
			continue;
		}
		//如果是需要的
		stream_map[i] = stream_idx++;

		//5.给目的文件创建（绑定）一个新的流（需要的流）
		AVStream* outStream = avformat_new_stream(oFmtCtx, NULL);
		if (!outStream)
		{
			av_log(oFmtCtx, AV_LOG_ERROR, "no memory to outStream!\n");
			goto _ERROR;
		}
		//6.设置输出目标流参数，这里直接是copy
		avcodec_parameters_copy(outStream->codecpar, inStream->codecpar);
		outStream->codecpar->codec_tag = 0; // 根据输出的后缀来自动确定封装器
		
	}

	//绑定 --> 便于写入数据给目的文件
	ret = avio_open2(&oFmtCtx->pb, dst, AVIO_FLAG_WRITE, NULL, NULL);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(oFmtCtx, AV_LOG_ERROR, "%s\n", err_buf);
		goto _ERROR;
	}

	//7.写多媒体文件头
	ret = avformat_write_header(oFmtCtx, NULL);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(oFmtCtx, AV_LOG_ERROR, "%s\n", err_buf);
		goto _ERROR;
	}

	//8.读取原数据（视频、音频、字幕）到目的文件
	while (av_read_frame(pFmtCtx, &pkt) >= 0)
	{
		if (stream_map[pkt.stream_index] < 0) // 检查是否为 -1 标记
		{
			av_packet_unref(&pkt);
			continue;
		}
		AVStream* inStream = pFmtCtx->streams[pkt.stream_index];
		
		pkt.stream_index = stream_map[pkt.stream_index]; // 不是-1的话，就更改需要传递的pkt的stream_index
		
		AVStream* outStream = oFmtCtx->streams[pkt.stream_index];
		
		av_packet_rescale_ts(&pkt, inStream->time_base, outStream->time_base);
		
		pkt.pos = -1; // 11、有了这个设置，是不是就不用设置上面的 stream_index 了？错
		av_interleaved_write_frame(oFmtCtx, &pkt);
		av_packet_unref(&pkt);
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
	if (stream_map)
	{
		av_free(stream_map);
		stream_map = nullptr;
	}

	return 0;
}
/*
* 11、这里的pkt.pos = -1，只是为了让ffmpeg自动处理包与包在文件中的位置关系，和pkt这个包中负载的数据流的 stream_index 没有一点关系，stream_index是辨识
*   、这一路数据流到底是 视频、音频还是字幕···的
* 
*/