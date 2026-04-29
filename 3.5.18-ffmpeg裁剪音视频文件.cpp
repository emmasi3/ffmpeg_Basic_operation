#include <iostream>
#include <string>
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

	if (argc < 5)
	{
		av_log(NULL, AV_LOG_ERROR, "the arguments must be more than 5(five)! but now the num_args is %d\n", argc);
		return -1;
	}

	src = argv[1];
	dst = argv[2];

	double START_TIME = std::stod(argv[3]);
	double END_TIME = std::stod(argv[4]);

	//1.处理一些参数
	char err_buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
	AVFormatContext* pFmtCtx = nullptr;
	AVFormatContext* oFmtCtx = nullptr;
	const AVOutputFormat* outFmt = nullptr;
	AVPacket pkt;

	int* stream_map = nullptr;
	int stream_idx = 0;

	int64_t* dts_start_time = nullptr;
	int64_t* pts_start_time = nullptr;

	int64_t pkt_dts_in_us = 0;
	int64_t pkt_pts_in_us = 0;
	int64_t end_time_in_us = END_TIME * AV_TIME_BASE;
	// 精确计算结束时间（纳秒级处理）
	//const int64_t end_time_in_ns = llround(END_TIME * 1e9); // 转换为纳秒


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

		//5.给目的文件创建（绑定）许多个新的流（需要的流）
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

	// seek
	// 寻找目的帧 ，指定时间戳“START_TIME”最近或者对应的 i 帧
	// 这里的第三个参数 timestamp，时间戳，要求是以时间基为单位，所以 * AV_TIME_BASE ，这是一个宏常量，一百万，秒转换为微秒，* 一百万就好
	ret = av_seek_frame(pFmtCtx, -1, START_TIME * AV_TIME_BASE, AVSEEK_FLAG_BACKWARD);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(oFmtCtx, AV_LOG_ERROR, "%s\n", err_buf);
		goto _ERROR;
	}
	
	// 初始化相对时间戳数组
	dts_start_time = (int64_t*)av_calloc(pFmtCtx->nb_streams, sizeof(int64_t));
	pts_start_time = (int64_t*)av_calloc(pFmtCtx->nb_streams, sizeof(int64_t));
	std::memset(dts_start_time, -1, sizeof(dts_start_time) * pFmtCtx->nb_streams);
	std::memset(pts_start_time, -1, sizeof(pts_start_time) * pFmtCtx->nb_streams);


	//8.读取原数据（视频、音频、字幕）到目的文件
	while (av_read_frame(pFmtCtx, &pkt) >= 0)
	{
		AVStream* inStream = pFmtCtx->streams[pkt.stream_index];
		if (stream_map[pkt.stream_index] < 0) // 检查是否为 -1 标记
		{
			av_packet_unref(&pkt);
			continue;
		}

		// （1）记录每一个pkt包的时间戳（演示、解码）
		if (dts_start_time[pkt.stream_index] == -1 && pkt.dts > 0)
		{
			dts_start_time[pkt.stream_index] = pkt.dts;
		}
		if (pts_start_time[pkt.stream_index] == -1 && pkt.pts > 0)
		{
			pts_start_time[pkt.stream_index] = pkt.pts;
		}

		//（4）判断结束时间，退出循环
		//if (av_q2d(inStream->time_base) * pkt.pts >= END_TIME) // 这里计算显示时间戳是否超时，也就是判断结束时间
		//{
		//	av_log(NULL, AV_LOG_INFO, "success!\n");
		//	av_packet_unref(&pkt);
		//	break;
		//}

		pkt_dts_in_us = av_rescale_q(pkt.dts, inStream->time_base, AV_TIME_BASE_Q);
		pkt_pts_in_us = av_rescale_q(pkt.pts, inStream->time_base, AV_TIME_BASE_Q);
		if (pkt_dts_in_us >= end_time_in_us || pkt_pts_in_us >= end_time_in_us)
		{
			av_log(NULL, AV_LOG_INFO, "success!\n");
			av_packet_unref(&pkt);
			break;
		}

		//// 转换时间戳到纳秒（避免浮点误差）
		//AVRational a = { 1, 1e9 };
		//const int64_t pkt_pts_ns = av_rescale_q(pkt.pts, inStream->time_base, a);
		//// 终止条件判断
		//if (pkt_pts_ns >= end_time_in_ns)
		//{
		//	av_log(NULL, AV_LOG_INFO, "终止于 %.3f 秒\n", pkt_pts_ns / 1e9);
		//	av_packet_unref(&pkt);
		//	break;
		//}

		// （2）计算相对时间戳？
		pkt.dts = pkt.dts - dts_start_time[pkt.stream_index];
		pkt.pts = pkt.pts - pts_start_time[pkt.stream_index];

		// 处理视频流中的 dts和pts需要满足的大小关系，也就是“解码时间必须在显示时间之前”，这有点脑子都能想来，但是计算机算出来的，有时候会 dts>pts
		// （3）dts <= pts ,出现异常情况，只能是遇见 B 帧
		if (pkt.dts > pkt.pts)
		{
			pkt.dts = pkt.pts;
		}

		// 不是-1的话，就更改需要传递的pkt的stream_index
		pkt.stream_index = stream_map[pkt.stream_index];
		AVStream* outStream = oFmtCtx->streams[pkt.stream_index];

		av_packet_rescale_ts(&pkt, inStream->time_base, outStream->time_base); // 这里是计算正确的时间信息，但是由于是裁剪，从裁剪开始的时间戳应该是 0，而不是原时间戳
	
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
	if (dts_start_time)
	{
		av_free(dts_start_time);
		dts_start_time = nullptr;
	}
	if (pts_start_time)
	{
		av_free(pts_start_time);
		pts_start_time = nullptr;
	}

	return 0;
}
/*
* （1）知识点补充：每一个pkt的实际戳，记录的是开始时间戳，也就是pkt包的第一帧的时间戳
* 
* （2）在裁剪过后，要重新计算pkt包的相对时间戳，相对于什么呢？当然是用户输入的 START_TIME 裁剪片段的开始时间戳，也就是av_seek_frame函数定位到的时间戳，
*  、也就是第一次进入 while 循环内 av_read_frame 读取到的 pkt 包的时间戳，由于不止一路流，所以需要有一个数组来记录 START_TIME 对应的各路流的开始时间戳
*  、作为“相对时间戳”
* 
* （3）dts <= pts ，这是必须要满足的，但是 B 帧除外，有些视频文件中，B帧的 dts > pts，这主要是因为 B 帧是双向预测帧，这没有错，但是FFmpeg在检查时，可能会
*  、采用严格的检查模式，所以，强制性要求，无论是 B 帧还是其他特殊帧，都必须满足 dts <= pts，所以，需要我们自己去更改
* 
* （4）inStream->time_base 这是一个结构体，不是一个数字，里面有2个成员变量，num\den，就是分子分母，那么time_base本质就是一个分数，上面需要一个具体数字
*  、所以，FFmpeg就提供了相应的 API 来计算具体的 double 值，很显然，这样的不断计算，在处理高码率、超清甚至4k视频时，计算量非常大，所以性能损耗大
*  、另一个方法是 将 END_TIME 从秒->微秒，也就是 END_TIME * AV_TIME_BASE，直接转换为微秒进行比较，没有计算的性能损耗，速度快，但是准备工作也多，不想上面
*  、那个，直接用
* 
* （5）最2B的问题，裁剪出来的视频哪哪都对，就是会多出来10秒左右，为什么呢？一看原来是比较的两个量中的 pkt.pts 应该是原视频的，结果特么前面计算相对时间戳
*  、修改了原值，放到前面就解决了，嗯嗯！！！
* 
* （6）（4）代码事例中的判断方式，有三种，都可以，结果大差不差
*/