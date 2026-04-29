#define CRT_SECURE_NO_WARNING
#define SDL_MAIN_HANDLED
#include <iostream>
extern"C"
{
#include <SDL.h>
#include <SDL_audio.h>
#include <libavutil/avutil.h>
#include <libavutil/fifo.h>
#include <libavutil/time.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>

}
int main()
{
	av_log_set_level(AV_LOG_DEBUG);

	//const AVCodec* codec = nullptr;
	//codec = avcodec_find_decoder_by_name("h264_cuvid");

	//if (codec)
	//{
	//	std::cout << "OK!" << std::endl;
	//}


 //   std::cout << "Decoder name: " << codec->name << std::endl;
 //   std::cout << "Long name: " << (codec->long_name ? codec->long_name : "N/A") << std::endl;
 //   std::cout << "ID: " << codec->id << std::endl;

 //   std::cout << "Supported pixel formats: " << std::endl;
 //   const enum AVPixelFormat* pix_fmt = codec->pix_fmts;
 //   if (pix_fmt) {
 //       while (*pix_fmt != AV_PIX_FMT_NONE) {
 //           const char* fmt_name = av_get_pix_fmt_name(*pix_fmt);
 //           std::cout << "  - " << (fmt_name ? fmt_name : "Unknown") << std::endl;
 //           ++pix_fmt;
 //       }
 //   }
 //   else {
 //       std::cout << "  None listed." << std::endl;
 //   }

 //   std::cout << "OK!" << std::endl;

	//AVFifo* fifo = av_fifo_alloc2(1024, 1, 0);
	//uint8_t data[512];
	//uint8_t buf[512];

	//// 写一次
	//int ret = av_fifo_write(fifo, data, 512);
	//printf("can_write after write: %d\n", av_fifo_can_write(fifo)); // 应该是 512

	//// 读一次
	//ret = av_fifo_read(fifo, buf, 512);
	//printf("can_write after read: %d\n", av_fifo_can_write(fifo)); // 应该回到 1024

	//printf("%d %d INT_MAX:%d,INT_MAX+1:%d\n", 2147483647, 2147483647 + 1, INT_MAX, INT_MAX + 1);

	//这里用来测试windows平台或者其他平台是否支持单调时钟（这个时钟时以从程序开始的某一个点为基准来自动运行的时间，不受系统时间影响）
	//使用“系统时间”有什么问题呢？，一旦你要根据 2个系统时间来计算一段时间时，你就要考虑这个时间段内，知道你不需要为止，系统时间会不会因为离线状态
	//或者其他原因而 发生变化，这就很容易导致后序的所有相对时间不准确，
	//相反，如果采用“单调时钟”，那么在程序的生命周期内，这个起始时间（基准）绝对是不会受影响的，嗯嗯，用它，
	//如果用不了，那就只能是 av_gettime() 了，你可以自己加逻辑，每使用一次就去校准一次系统时间，麻烦而且还有可能校准失败，或者精度不够小
	//if (av_gettime_relative_is_monotonic()) 
	//{
	//	printf("Using monotonic clock for timing ✅\n");
	//}
	//else 
	//{
	//	printf("Warning: Not using monotonic clock ⚠️\n");
	//}

	//std::cout << std::endl;

	//while (1)
	//{
	//	std::cout << av_gettime_relative() << std::endl;
	//}

	const AVCodec* codec = nullptr;

	codec = avcodec_find_encoder_by_name("h264_qsv"); // Inter
	if (!codec)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to find encoder -- h264_qsv!\n");
		return -1;
	}

	return 0;
}