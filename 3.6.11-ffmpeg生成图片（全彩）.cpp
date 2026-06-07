#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

}

#define WORD uint16_t
#define DWORD uint32_t
#define LONG int32_t

/*
* 1、关键修正1：强制结构体紧凑对齐，这两个结构体的大小必须为 14 -- 40 字节
*	这是 BMP 文件格式协议强制要求的，但是编译器可能会因为 windows 平台的 4字节对齐要求从而给这两个结构体加上
*	补位字节，导致 14 -- 16 字节，这多出来的 2 字节写到 BMP 文件头部，解析的时候，会直接出错，所以必须要强制规定 对齐方式
* 2、为什么是 4字节 对齐？
*	windows 32位系统，在解析数据时，4字节解析速度最快，嗯嗯！
* 3、但其实，这俩结构体在 <wingdi.h> 中有定义，顺序一模一样
*/
#pragma pack(push, 1) 
// 这个结构体是受协议规定的，bmp文件头
typedef struct {
	uint16_t  bfType;
	uint32_t  bfSize;
	uint16_t  bfReserved1;
	uint16_t  bfReserved2;
	uint32_t  bfOffBits;
} BITMAPFILEHEADER;

// 这个结构体是受协议规定的，bmp图像信息头
typedef struct {
	uint32_t  biSize;
	int32_t   biWidth;
	int32_t   biHeight;
	uint16_t  biPlanes;
	uint16_t  biBitCount;
	uint32_t  biCompression;
	uint32_t  biSizeImage;
	int32_t   biXPelsPerMeter;
	int32_t   biYPelsPerMeter;
	uint32_t  biClrUsed;
	uint32_t  biClrImportant;
} BITMAPINFOHEADER;
#pragma pack(pop)

static int decode(AVCodecContext* ctx,SwsContext* swsCtx, AVFrame* frame, AVPacket* pkt, const char* filename);

static void savePic(unsigned char* buf, int linesize, int width, int height, char* name);

static void saveBMP(SwsContext* swsCtx, AVFrame* frame, int w, int h, const char* name);

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

	SwsContext* swsCtx = nullptr;

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
		av_log(NULL, AV_LOG_ERROR, "don't find the codec：%s\n", "libx264");
		goto _ERROR;
	}

	// 创建解码器上下文（这里只是分配内存，没有涉及到绑定） 这里分配内存有点误解！
	// 分配并初始化 AVCodecContext，根据传入的 codec 来初始化一些参数
	// 真正初始化解码器在 avcodec_open2()
	ctx = avcodec_alloc_context3(codec);
	if (!ctx)
	{
		av_log(NULL, AV_LOG_ERROR, "no memory!\n");
		goto _ERROR;
	}

	// 具体从文件中读取到的 流参数（真实），上面要使用 avcodec_alloc_context3(codec); 是设计要求，另一个是防止漏掉参数 
	avcodec_parameters_to_context(ctx, inStream->codecpar);

	//5.将解码器绑定到上下文
	ret = avcodec_open2(ctx, codec, NULL);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(ctx, AV_LOG_ERROR, "Can't open the Codec：%s\n", err_buf);
		goto _ERROR;
	}

	// sws_getCachedContext
	//5.1、获得Sws上下文
	swsCtx = sws_getContext
						(ctx->width, ctx->height, AV_PIX_FMT_YUV420P, 
						3840, 2160, AV_PIX_FMT_BGR24,
						SWS_BICUBIC, NULL, NULL, NULL);

	if (!swsCtx)
	{
		av_log(NULL, AV_LOG_ERROR, "%s\n", "swsCtx is failure!");
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
			if (pkt->size <= 0) {
				av_log(NULL, AV_LOG_ERROR, "Packet size is invalid\n");
				return -1;
			}
			decode(ctx, swsCtx, frame, pkt, dst);
		}
	}

	decode(ctx, swsCtx, frame, NULL, dst); // 清理解码器

	av_log(NULL, AV_LOG_INFO, "success!!!\n");

	//9.释放资源
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

static int decode(AVCodecContext* ctx,SwsContext* swsCtx, AVFrame* frame, AVPacket* pkt, const char* filename)
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

		snprintf(buf, sizeof(buf), "%s-%d.bmp", filename, ctx->frame_num);
		//savePic(frame->data[0], frame->linesize[0], frame->width, frame->height, buf);
		//if (pkt)
		//{
		//	av_packet_unref(pkt);
		//}

		saveBMP(swsCtx, frame, 3840, 2160, buf);
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

//static void saveBMP(SwsContext* swsCtx, AVFrame* frame, int w, int h, const char* name) {
//	// 关键修正2：正确计算数据大小和逐行写入 
//	const int dataSize = w * h * 3;
//
//		//2.构造 BITMAPINFOHEADER
//	BITMAPINFOHEADER infoHeader;
//	infoHeader.biSize = sizeof(BITMAPINFOHEADER);
//	infoHeader.biWidth = w;
//	infoHeader.biHeight = -h;
//	infoHeader.biBitCount = 24;
//	infoHeader.biCompression = 0;
//	infoHeader.biSizeImage = dataSize;
//	infoHeader.biClrImportant = 0;
//	infoHeader.biClrUsed = 0;
//	infoHeader.biXPelsPerMeter = 0;
//	infoHeader.biYPelsPerMeter = 0;
//	infoHeader.biPlanes = 1;
//
//	//3.构建 VITMAPFILEHEADER
//	BITMAPFILEHEADER fileHeader;
//	fileHeader.bfType = 0x4d42; // 'BM'用于标志
//	fileHeader.bfSize =sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + dataSize;
//	fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
//	fileHeader.bfReserved1 = 0;
//	fileHeader.bfReserved2 = 0;
//
//	AVFrame* frameBGR = av_frame_alloc();
//	frameBGR->format = AV_PIX_FMT_BGR24;
//	frameBGR->width = w;
//	frameBGR->height = h;
//	av_frame_get_buffer(frameBGR, 0);
//
//	sws_scale(swsCtx, frame->data, frame->linesize, 0,
//		frame->height, frameBGR->data, frameBGR->linesize);
//
//	if (FILE* f = fopen(name, "wb")) {
//		//fwrite(&bfh, 1, sizeof(bfh), f);
//		//fwrite(&bih, 1, sizeof(bih), f); 没错
//
//		fwrite(&fileHeader, sizeof(fileHeader), 1, f);
//		fwrite(&infoHeader, sizeof(infoHeader), 1, f);
//		//for (int y = 0; y < h; ++y) { 没错
//		//	fwrite(frameBGR->data[0] + y * frameBGR->linesize[0], 1, w * 3, f);
//		//}
//		fwrite(frameBGR->data[0], 1, dataSize, f);
//		fclose(f);
//	}
//	//av_frame_free(&frameBGR); 没错
//	av_freep(&frameBGR->data[0]);
//	av_free(frameBGR);
//}

static void saveBMP(SwsContext* swsCtx, AVFrame* frame, int w, int h, const char* name)
{
	// 每行字节数 -- windows 规定的，必须采用 4字节对齐，也就是bmp图像总字节数应该为4的倍数，所以只需要让每行的字节数为 4 的倍数即可
	// 1、这里你可能很疑惑，既然是 4 字节对齐，为什么不能写 rowSize = (w * 3 + 3) / 4 * 4; ？
	//	在这里，像素格式为 BGR24，一个像素 -- 3字节，但是有时候，一个像素是 1.5 字节，懂了吧？字节还可再分为小数，但是二进制位运算
	//	是不可以再分的，所以该 “向上取整” 公式，采用 4 bytes == 32 bits 的方式
	int rowSize = (w * 24 + 31) / 32 * 4;
	int dataSize = rowSize * h; 
	//1.先进行转换，将 YUV Frame转换成 BRG24 Frame
	AVFrame* frameBGR = av_frame_alloc();
	frameBGR->width = w;
	frameBGR->height = h;
	frameBGR->format = AV_PIX_FMT_BGR24;

	av_frame_get_buffer(frameBGR, 0);

	sws_scale(swsCtx, frame->data,
		frame->linesize,
		0, frame->height,
		frameBGR->data, frameBGR->linesize);

	//2.构造 BITMAPINFOHEADER
	BITMAPINFOHEADER infoHeader;
	infoHeader.biSize = sizeof(BITMAPINFOHEADER);
	infoHeader.biWidth = w;
	infoHeader.biHeight = -h; // 负值表示，解析时采用 “从下而上” 的读取方式，正值时，采用 “从下而上” 的读取方式，这里用 -h 防止图像倒置
	infoHeader.biBitCount = 24;
	infoHeader.biCompression = 0;
	infoHeader.biSizeImage = dataSize;
	infoHeader.biClrImportant = 0;
	infoHeader.biClrUsed = 0;
	infoHeader.biXPelsPerMeter = 0;
	infoHeader.biYPelsPerMeter = 0;
	infoHeader.biPlanes = 1;

	//3.构建 VITMAPFILEHEADER
	BITMAPFILEHEADER fileHeader;
	fileHeader.bfType = 0x4d42; // 'BM'用于标志
	fileHeader.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + dataSize; // 文件总大小
	fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER); // 需要偏移 offset 多少字节才能到数据部分！FILE + INFO 之后
	fileHeader.bfReserved1 = 0;
	fileHeader.bfReserved2 = 0;

	//4.将数据写入文件
	FILE* f = nullptr;
	f = fopen(name, "wb");
	if (!f)
	{
		av_log(NULL, AV_LOG_ERROR, "%s\n", "the file could not open!");
	}

	av_log(NULL, AV_LOG_DEBUG, "Writing BMP file: %s\n", name);
	av_log(NULL, AV_LOG_DEBUG, "Image size: %d bytes\n", dataSize);
	av_log(NULL, AV_LOG_DEBUG, "FileHeader size: %zu\n", sizeof(BITMAPFILEHEADER));
	av_log(NULL, AV_LOG_DEBUG, "InfoHeader size: %zu\n", sizeof(BITMAPINFOHEADER));


	if (f)
	{
		fwrite(&fileHeader, sizeof(BITMAPFILEHEADER), 1, f);
		fwrite(&infoHeader, sizeof(BITMAPINFOHEADER), 1, f);
		fwrite(frameBGR->data[0], 1, dataSize, f);
	}

	//5.释放资源
	if (f)
	{
		fclose(f);
	}
	av_frame_free(&frameBGR);

}

/*
* 总结：
*（1）
* // 默认编译环境下（如MSVC x64）
* struct BITMAPFILEHEADER {
*    uint16_t bfType;      // 2字节 
*    uint32_t bfSize;      // 4字节（起始偏移应为2，但编译器可能插入2字节填充）
*    // ...其他字段 
* };
* // 实际内存布局：2(bfType) + 2(填充) + 4(bfSize) = 8字节偏移 
* // 但BMP规范要求bfSize必须从第2字节开始（无填充）！
* 也就是G++编译器会在编译结构体时，采用4字节对齐的方式，提高cpu的访问效率，这就非常有可能填充0导致长度发生变化，进而导致BMP或者一些对头信息严格要求的
* 检查机制不能够识别，这种信息不配位的情况，我遇到的问题就是这种，所以以后遇到但凡涉及到 协议的头信息，格式的规范等等，硬性的格式信息的填充时，务必考虑
* 编译器或者当前操作系统是否会对原本正确的格式信息“填充”或者“转换”导致信息的偏差，进而不符合规范而失败
* 
*/