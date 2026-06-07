#include <iostream>
extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include<libavutil/log.h>
#include <libavutil/avutil.h>
}

// 静态函数：为输入流中需要的 H.264 流创建并初始化 bitstream filter（h264_mp4toannexb）
// 返回一个长度为 pFmtCtx->nb_streams 的 AVBSFContext* 数组（失败时返回 nullptr）。
// 数组中对应流没有过滤器时为 nullptr。调用者负责通过 av_bsf_free 释放每个上下文并 av_free 数组本身。
static AVBSFContext** create_bsf_contexts(AVFormatContext* pFmtCtx)
{
	if (!pFmtCtx)
		return nullptr;

	int nb_streams = pFmtCtx->nb_streams;
	AVBSFContext** bsf_ctxs = (AVBSFContext**)av_calloc(nb_streams, sizeof(AVBSFContext*));

	if (!bsf_ctxs) 
		return nullptr;

	for (int i = 0; i < nb_streams; ++i)
	{
		AVStream* inStream = pFmtCtx->streams[i];
		AVCodecParameters* par = inStream->codecpar;
		// 仅对视频 H.264 流创建过滤器（MP4 的 avcC -> AnnexB）
		if (par && par->codec_type == AVMEDIA_TYPE_VIDEO && par->codec_id == AV_CODEC_ID_H264)
		{
			const AVBitStreamFilter* filter = av_bsf_get_by_name("h264_mp4toannexb");
			if (!filter)
			{
				av_log(NULL, AV_LOG_WARNING, "h264_mp4toannexb filter not found\n");
				continue; // 不阻止其它流
			}
			AVBSFContext* bsf = nullptr;
			if (av_bsf_alloc(filter, &bsf) < 0 || !bsf)
			{
				av_log(NULL, AV_LOG_ERROR, "av_bsf_alloc failed for stream %d\n", i);
				continue;
			}
			// 复制输入流的 codecpar 到 bsf 的 par_in
			if (avcodec_parameters_copy(bsf->par_in, par) < 0)
			{
				av_bsf_free(&bsf);
				av_log(NULL, AV_LOG_ERROR, "avcodec_parameters_copy failed for bsf par_in\n");
				continue;
			}
			bsf->time_base_in = inStream->time_base; // 保持 time_base
			if (av_bsf_init(bsf) < 0)
			{
				av_bsf_free(&bsf);
				av_log(NULL, AV_LOG_ERROR, "av_bsf_init failed for stream %d\n", i);
				continue;
			}
			bsf_ctxs[i] = bsf;
		}
		else
		{
			bsf_ctxs[i] = nullptr;
		}
	}
	return bsf_ctxs;
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

	AVBSFContext** bsf_ctxs = nullptr; // 按输入流索引存放对应的 bitstream filter（如 h264_mp4toannexb）

	int* stream_map = nullptr;
	int stream_idx = 0;


	//2.打开多媒体文件（将上下文结构体与源文件关联起来）
	ret = avformat_open_input(&pFmtCtx, src, NULL, NULL);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(NULL, AV_LOG_ERROR, "%s\n", err_buf);
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

	// 为需要的输入流创建 bitstream filter（只做 H.264 的 mp4->annexb 转换）
	//bsf_ctxs = create_bsf_contexts(pFmtCtx);

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
		int in_index = pkt.stream_index;
		if (stream_map[in_index] < 0) // 检查是否为 -1 标记
		{
			av_packet_unref(&pkt);
			continue;
		}

		// 如果该输入流有 bsf，那么把包送入 bsf，取出过滤后的包并写出
		if (bsf_ctxs && bsf_ctxs[in_index])
		{
			AVBSFContext* bsf = bsf_ctxs[in_index];
			// 发送包到 bsf
			ret = av_bsf_send_packet(bsf, &pkt);
			if (ret < 0)
			{
				av_log(NULL, AV_LOG_ERROR, "av_bsf_send_packet failed: %d\n", ret);
				av_packet_unref(&pkt);
				continue;
			}
			// 从 bsf 中取出可能产生的多个包
			AVPacket filt_pkt;
			//av_init_packet(&filt_pkt);
			while (av_bsf_receive_packet(bsf, &filt_pkt) == 0)
			{
				// map 到输出流索引
				AVStream* inStream = pFmtCtx->streams[in_index];
				int out_index = stream_map[in_index];
				AVStream* outStream = oFmtCtx->streams[out_index];
				// 时间基转换
				av_packet_rescale_ts(&filt_pkt, inStream->time_base, outStream->time_base);
				filt_pkt.stream_index = out_index;
				filt_pkt.pos = -1;
				av_interleaved_write_frame(oFmtCtx, &filt_pkt);
				av_packet_unref(&filt_pkt);
				//av_init_packet(&filt_pkt);
			}
			// 原始 pkt 可以释放
			av_packet_unref(&pkt);
		}
		else // 没有 bsf，直接复制并写出
		{
			AVStream* inStream = pFmtCtx->streams[in_index];
			int out_index = stream_map[in_index];
			AVStream* outStream = oFmtCtx->streams[out_index];

			pkt.stream_index = out_index; // 不是-1的话，就更改需要传递的pkt的stream_index
			av_packet_rescale_ts(&pkt, inStream->time_base, outStream->time_base);
			pkt.pos = -1; // 保持-1，让 muxer 处理文件位置
			av_interleaved_write_frame(oFmtCtx, &pkt);
			av_packet_unref(&pkt);
		}
	}

	// 在读完所有包后，可能需要对所有 bsf 做 flush，以取出残留包
	if (bsf_ctxs)
	{
		for (int i = 0; i < pFmtCtx->nb_streams; ++i)
		{
			AVBSFContext* bsf = bsf_ctxs[i];
			if (!bsf) continue;
			// 发送 NULL 刷新
			av_bsf_send_packet(bsf, NULL);
			AVPacket filt_pkt;
			//av_init_packet(&filt_pkt);
			while (av_bsf_receive_packet(bsf, &filt_pkt) == 0)
			{
				int out_index = stream_map[i];
				if (out_index < 0)
				{
					av_packet_unref(&filt_pkt);
					continue;
				}
				AVStream* inStream = pFmtCtx->streams[i];
				AVStream* outStream = oFmtCtx->streams[out_index];
				av_packet_rescale_ts(&filt_pkt, inStream->time_base, outStream->time_base);
				filt_pkt.stream_index = out_index;
				filt_pkt.pos = -1;
				av_interleaved_write_frame(oFmtCtx, &filt_pkt);
				av_packet_unref(&filt_pkt);
				//av_init_packet(&filt_pkt);
			}
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
	// 释放 bsf 上下文
	if (bsf_ctxs)
	{
		for (int i = 0; i < (pFmtCtx ? pFmtCtx->nb_streams : 0); ++i)
		{
			if (bsf_ctxs[i])
			{
				av_bsf_free(&bsf_ctxs[i]);
			}
		}
		av_free(bsf_ctxs);
		bsf_ctxs = nullptr;
	}

	// 关闭并释放输出上下文
	if (oFmtCtx)
	{
		if (oFmtCtx->pb)
		{
			avio_closep(&oFmtCtx->pb);
		}
		avformat_free_context(oFmtCtx);
		oFmtCtx = nullptr;
	}

	// 关闭并释放输入上下文
	if (pFmtCtx)
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