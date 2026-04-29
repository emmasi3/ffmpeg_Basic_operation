#define _CRT_SECURE_NO_WARNINGS
#include <d3d11.h>
#include <dxgi1_2.h>
#include <iostream>
#include <string>
#include <stdio.h>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

int main()
{
	AVFrame* frame;

	int i = 1;
	int fps = 25;
	AVCodecContext* ctx;
	ctx->time_base = { 1, 1000000 };

	frame->pts = av_rescale_q(i, AVRational{ 1, fps }, ctx->time_base);
	// 1、时间戳：在这里，av_rescale_q() 方法将 i 从本身的时间基转换为 ctx->time_base; 时间基下的数值
	//	如：fps = 25; time_base = {1, 1000000}; 且 i = 1，计算得到 ctx->time_base 下的时间戳为 4万，
	//	最终反向计算 frame->pts 显示时间戳的实际显示时间为 0.04 s，也就是 40ms时，没问题，就这样算
	// 2、当然，这里的 i 的时间基为什么是 AVRational{1, fps}; 因为这里的 i 是for循环中记录的帧索引数值，也就是 i = 1 时代表实际时间是 1.0 / 25s;
	//	i = 2 时代表实际时间是 2.0 / 25s; 
	//	时间戳 * 时间基 = 实际时间(s)，具体最终的单位是 ms 还是 s，额，一般是 s，注意点就行
	// 3、当录屏时，只需要设置 frame->pts 就行了，pkt->pts 不用管，编码器会解决的，
	//	如：fsp = 25；time_base = {1, 1000000}; 按照帧索引，pkt->pts 按照 40000 依次往上积累，嗯嗯
	//	（1）当我不按照 frame->pts = 帧索引 * (时间基分母 / 帧率分子); 这个公式严格计算，仅仅用 frame->pts = i; 那么最终收到的 pkt->pts 
	//		不是按照 40000 网上累计的，但是 .h264 视频播放的速率没有出问题
	//	（2）pkt->time_base 是默认的 {0, 1}; 不会编码器设置，这是我实验过的
	//

	return 0;
}