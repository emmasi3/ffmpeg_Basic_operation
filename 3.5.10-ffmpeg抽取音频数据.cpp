#include <iostream>
extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavutil/avutil.h>

}

int main(int argc, char* argv[]) // 命令行传入参数个数，参数集
{
	std::cout << "程序开始了！" << std::endl;
	int ret = 0;
	// src 中音频流ID
	int idx = 0;

	// 1. 处理一些参数
	char* src = nullptr;
	char* dst = nullptr;
	AVFormatContext* pFmtCtx = nullptr;
	AVFormatContext* oFmtCtx = nullptr;

	const AVOutputFormat* outFmt = nullptr;
	AVStream* inStream = nullptr;
	AVStream* outStream = nullptr;
	AVPacket pkt;

	char err_buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };

	if (argc < 3)
	{
		av_log(NULL, AV_LOG_INFO, "Arguments must be more than 3,but now num_args = %d\n",argc);
		
		goto _ERROR;
	}

	src = argv[1];
	dst = argv[2];
	av_log(NULL, AV_LOG_INFO, "hello\n");

	// 2. 打开多媒体文件
	ret = avformat_open_input(&pFmtCtx, src, NULL, NULL);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(NULL, AV_LOG_ERROR, "%s\n", err_buf);
		memset(err_buf, 0, sizeof(err_buf));

		goto _ERROR;
	}

	// 3. 从 src 文件中找到音频流 Id
	idx = av_find_best_stream(pFmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (idx < 0)
	{
		av_strerror(idx, err_buf, sizeof(err_buf));
		av_log(pFmtCtx, AV_LOG_ERROR, "%s\n", err_buf);
		goto _ERROR;
	}

	// 4. 打开目的文件的上下文
	oFmtCtx = avformat_alloc_context();
	if (!oFmtCtx)
	{
		av_log(oFmtCtx, AV_LOG_ERROR, "no memory!\n");
		goto _ERROR;
	}
	//设置输出格式
	outFmt = av_guess_format(NULL, dst, NULL);
	oFmtCtx->oformat = outFmt;

	// 5. 为目的文件，创建一个新的音频流
	outStream = avformat_new_stream(oFmtCtx, NULL);

	//绑定
	ret = avio_open2(&oFmtCtx->pb, dst, AVIO_FLAG_WRITE, NULL, NULL);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(oFmtCtx, AV_LOG_ERROR, "%s\n", err_buf);
		memset(err_buf, 0, sizeof(err_buf));

		goto _ERROR;
	}

	// 6. 设置输出音频参数
	inStream = pFmtCtx->streams[idx];
	avcodec_parameters_copy(outStream->codecpar, inStream->codecpar); // 中间的是--》参数 ————意思
	outStream->codecpar->codec_tag = 0; // 设置为 0 ，自动识别多媒体文件的格式，确定封装器的类型，如果足够熟悉，可以设置为目标值

	// 7. 写多媒体文件头到目的文件
	ret = avformat_write_header(oFmtCtx, NULL);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(oFmtCtx, AV_LOG_ERROR, "%s\n", err_buf);
		memset(err_buf, 0, sizeof(err_buf));

		goto _ERROR;
	}

	// 8. 从源多媒体文件读取音频数据到目的文件中
	while (av_read_frame(pFmtCtx, &pkt) >= 0)
	{
		if (pkt.stream_index == idx) // 判断读取的是不是音频流数据，有的时候不是，因为修改（传递）音视频文件的时候，格式转化在所难免，所以音频一般和视频混合传递，用的包有的时候是同一个，多线程
		{
			// 转换pkt的各种时间戳相关字段
			av_packet_rescale_ts(&pkt, inStream->time_base, outStream->time_base);
			// 读取到的数据包是一个裸流，FFmpeg并不知道他是 video、audio 哪一个数据流，这个要自行设置
			pkt.stream_index = outStream->index;
			// 写入文件 IO
			ret = av_interleaved_write_frame(oFmtCtx, &pkt); // 这一步是核心写入操作，通过oFmtCtx作为媒介中间体，将数据写入 dst 指向的文件
			if (ret < 0)
			{
				av_packet_unref(&pkt);
				break;
			}
		}

		// 释放旧资源
		av_packet_unref(&pkt);
	}

	// 9. 写多媒体文件尾到文件中
	av_write_trailer(oFmtCtx);

	// 10. 将申请的资源释放
_ERROR:
	if (pFmtCtx)
	{
		avformat_close_input(&pFmtCtx);
		pFmtCtx = nullptr;
	}

	if (oFmtCtx)
	{
		if (oFmtCtx->pb)
		{
			avio_close(oFmtCtx->pb);
		}
		avformat_free_context(oFmtCtx);
	}

	return 0;
}

/*
1.packet buffer -- oFmtCtx->pb 的缩写

2.(AVRounding)(AV_ROUND_PASS_MINMAX | AV_ROUND_UP) 这里本来不应该用强制类型转换的，在那个函数中，应该会自动处理，但是由于不同的编译器，处理力度，
也就有了这样的结果，强制转换就算不在这儿做，函数内部也会做，只不过，做的更加优雅一点

3.pkt.dts = pkt.pts; // 该数据包应该被“解码”的相对时间点（时间戳是一个“时机”）
	这里的 pts 是演示时间戳，也就是该数据包应该被呈现的时机（时间点）

4.时间基，是标准，timebase = {1,1000},就是将1s 当做1000份 分开，嗯嗯
*/