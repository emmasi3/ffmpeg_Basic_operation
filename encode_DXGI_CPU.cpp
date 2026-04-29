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

/*
* [该文件的一些想法和问题]
* 
* 1、设置的编码器上下文 codec_ctx->pix_fmt = BGRA; 使用 h264_nvenc 编码器
*	并且该编码器支持 BGRA 像素格式，那么最终输出的 DXGI.h264 视频的像素格式是什么？
*	测试之后，格式为 yuv420p，为什么？
*	Reason: （1）并没有使用 ffmepg提供的像素格式转换方法，进行任何的格式转换
*		也就是说，frame->data[0] 中的数据就是 BGRA 格式的
*			（2）FFmpeg不会隐式地进行像素格式转换
*			（3）但是输出的 .h264视频格式明显不是 BGRA，所以只能是 编码器本身的问题
*			（4）但是编码器很显然不会去做格式转换的活，所以只能是
*			frame->data 数据送到编码器之前像素格式就已经转换为了 yuv420p
*			（5）只能是 显卡本身，也就是 N卡，这个hardware 硬件本身，进行了格式转换
*			嗯嗯，frame->data 通过 avcodec_send_frame 给到N卡本身，再由N卡给到它内部的
*			编码器，这期间进行了转换
* 
* 2、关于没有设置好帧率情况下，h264视频明显 “慢” 的现象解释：
*	（1）编码器上下文中，我们手动指定了 fps = 1 / 25； 也就是 40ms显示一帧
*	这是固定的
*	（2）AcquireNextFrame(20,,) 我们设定的20ms，是超时时间，并且一旦因为超过20ms而失败，那么就会打印日志
*	很显然，没有打印，那么可以确定，DXGI 采集屏幕的速度 >> 20ms；并且
*	我们将每一次采集的帧都写入了 frame，并给到编码器
*	（3）假设，10ms “采集” 一次桌面，所以视频本应该 10ms 显示一帧，但是设定的是 40ms 显示一帧
*	很显然，真实世界发生的事件，假设需要 500ms完成，那么在编码器强制指定 25fps 情况下
*	需要 500 * 2.5 ms 的时间完成，很明显，变慢了
* 
*/

/**
* @brief 全局变量
*/
namespace sylar
{
	// AcquireNextFrame 获取到的上一帧时间戳
	int64_t PreviousFramePts = 0;
	// 编码器设定的帧率(分母)
	int fps = 60;
	// 每帧间隔 ms
	int64_t MS = 1000 / fps;
	// Frequency 计时器的频率
	int64_t Frequency_s;
	// 两帧实际间隔 ms
	int64_t delta_time = 0;
	// 帧索引
	int64_t frame_index = 0;
}

static void getFrequencys()
{
	// 初始化 QPC 计时器频率 -- 用来计算最终的时间
	LARGE_INTEGER Frequency;
	QueryPerformanceFrequency(&Frequency);
	sylar::Frequency_s = Frequency.QuadPart;
	// 初始化时间戳（初始）
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	sylar::PreviousFramePts = now.QuadPart;
}

static int encode(AVCodecContext* ctx, AVFrame* frame,
	AVPacket* pkt, AVFormatContext* oFmtCtx);

/**
* @brief 判断 AcquireNextFrame 返回 TimeOut 时是否该使用最新的 DXGI 帧来填充，
*		因为 frame 中还保留着上一次送入编码器的帧数据，并且AcquireNextFrame
*		返回超时时，是因为桌面没有变化，所以在帧间隔期间将上一帧数据送入编码器是合理的
*		因为桌面在这期间没有变化
* @return true 时间到了，可以放
* @return false 时间没到，再等会
*/
static bool AcquireTimeOut_isPass(AVCodecContext* ctx, AVFrame* frame,
	AVPacket* pkt, AVFormatContext* oFmtCtx, int i)
{
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	// 计算实际时间戳变化量
	int64_t delta_ticks = now.QuadPart - sylar::PreviousFramePts;
	// 计算实际两帧间隔 ms
	sylar::delta_time = (delta_ticks * 1000) / sylar::Frequency_s;
	// 间隔 >= MS 时，可以送入，直接在这里编码
	// std::cout << sylar::delta_time << std::endl;
	if (sylar::delta_time >= sylar::MS)
	{
		std::cout << "delta_time: " << sylar::delta_time << std::endl;
		// 更新时间戳
		frame->pts = av_rescale_q(sylar::frame_index, AVRational{ 1, sylar::fps }, ctx->time_base);
		int ret = encode(ctx, frame, pkt, oFmtCtx);
		if (ret < 0)
		{
			exit(1); // 强制退出程序，因为编码器已经报错了，再次编码没意义了
		}
		// 更新时间戳，同步 isPass() 中的逻辑
		sylar::PreviousFramePts = now.QuadPart;
		// 更新帧索引
		++sylar::frame_index;
		return true;
	}

	return false;
}

/**
* @brief 判断此时是否该使用最新的 DXGI 帧
* @return true 时间到了，可以放
* @return false 时间没到，再等会
*/
static bool isPass(DXGI_OUTDUPL_FRAME_INFO& frameInfo, int i)
{
	// 使用 QPC 高性能时间戳
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	// 计算实际时间戳变化量
	int64_t delta_ticks = now.QuadPart - sylar::PreviousFramePts;
	// 计算实际两帧间隔 ms
	sylar::delta_time = (delta_ticks * 1000) / sylar::Frequency_s;
	// 判断是否是 第 1 帧，因为第一帧需要无脑送进去，并且因为主循环中，i != 0 的情况居多，
	// 为了防止分支预测错误的损耗叠加，将最可能的情况放到前面
	if (i != 0)
	{
		// 间隔 >= MS 时，可以送入
		// std::cout << sylar::delta_time << std::endl;
		if (sylar::delta_time >= sylar::MS)
		{
			std::cout << "delta_time: " << sylar::delta_time << std::endl;
			sylar::PreviousFramePts = now.QuadPart;
			return true;
		}
		// 否则，不送入，继续循环
		return false;
	}
	else if(i == 0)
	{
		sylar::PreviousFramePts = now.QuadPart;
		return true;
	}
}

/**
* @brief 判断时间戳是否和帧率匹配，通过 “上一帧时间”
* 弃用：Reason：
* 在 main 函数下
*/
static bool isPass_version1(DXGI_OUTDUPL_FRAME_INFO& frameInfo, int i);

static int encode(AVCodecContext* ctx, AVFrame* frame,
	AVPacket* pkt, AVFormatContext* oFmtCtx);

static bool acquireRGBAtoFrame(AVCodecContext* ctx,
	AVFrame* frame, AVPacket* pkt, AVFormatContext* oFmtCtx)
{
	int screenWidth = GetSystemMetrics(SM_CXSCREEN);
	int screenHeight = GetSystemMetrics(SM_CYSCREEN);

	// 支持的驱动程序类型：
	D3D_DRIVER_TYPE DriverTypes[] = { D3D_DRIVER_TYPE_HARDWARE,
		D3D_DRIVER_TYPE_WARP, D3D_DRIVER_TYPE_REFERENCE, };
	UINT NumDriverTypes = ARRAYSIZE(DriverTypes);
	// 支持的功能级别
	D3D_FEATURE_LEVEL FeatureLevels[] = {
		D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
		D3D_FEATURE_LEVEL_9_1 };
	UINT NumFeatureLevels = ARRAYSIZE(FeatureLevels);

	D3D_FEATURE_LEVEL FeatureLevel;
	// 1、创建D3D设备
	ID3D11Device* _pDX11Dev = nullptr;
	ID3D11DeviceContext* _pDX11DevCtx = nullptr;
	HRESULT hr = 0;
	for (UINT index = 0; index < NumDriverTypes; index++) {
		hr = D3D11CreateDevice(nullptr, DriverTypes[index], nullptr, 0, FeatureLevels, NumFeatureLevels, D3D11_SDK_VERSION, &_pDX11Dev, &FeatureLevel, &_pDX11DevCtx);
		if (SUCCEEDED(hr))
			break;
	}

	// 2、获取DXGITest设备
	IDXGIDevice* _pDXGITestDev = nullptr;
	hr = _pDX11Dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&_pDXGITestDev));
	if (FAILED(hr))
	{
		return false;
	}

	// 3、获取DXGITest适配器
	IDXGIAdapter* _pDXGITestAdapter = nullptr;
	hr = _pDXGITestDev->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void **>(&_pDXGITestAdapter));
	if (FAILED(hr))
	{
		return false;
	}

	// 4、获取输出
	UINT i = 0;
	IDXGIOutput* _pDXGIOutput = nullptr;
	hr = _pDXGITestAdapter->EnumOutputs(i, &_pDXGIOutput);
	if (FAILED(hr))
	{
		return false;
	}

	// 获取输出描述结构
	DXGI_OUTPUT_DESC DesktopDesc;
	_pDXGIOutput->GetDesc(&DesktopDesc);

	// 5、请求接口给Output1
	IDXGIOutput1* _pDXGIOutput1 = nullptr;
	hr = _pDXGIOutput->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&_pDXGIOutput1));
	if (FAILED(hr))
	{
		return false;
	}

	// 6、创建桌面副本
	IDXGIOutputDuplication* _pDXGIOutputDup = nullptr;
	hr = _pDXGIOutput1->DuplicateOutput(_pDX11Dev, &_pDXGIOutputDup);
	if (FAILED(hr))
	{
		return false;
	}

	// init 初始化完成

	// 7、循环调用 AcquireNextFrame 获取下一帧图像数据，以及 frameinfo 帧信息
	for (int i = 0; i < 1000; ++i)
	{
		IDXGIResource* desktopResource = nullptr;
		DXGI_OUTDUPL_FRAME_INFO frameInfo;
		hr = _pDXGIOutputDup->AcquireNextFrame(0, &frameInfo, &desktopResource);

		// 失败处理逻辑
		if (FAILED(hr))
		{
			if (hr == DXGI_ERROR_WAIT_TIMEOUT)
			{
				if (desktopResource)
				{
					desktopResource->Release();
					desktopResource = nullptr;
				}
				//std::cout << " AcquireNextFrame_time_out" << std::endl;
				_pDXGIOutputDup->ReleaseFrame();

				// 如果超时，判断是否超出了每帧间隔，如果超出了，立即放入最新一帧
				if (AcquireTimeOut_isPass(ctx, frame, pkt, oFmtCtx, i))
				{
					continue;
				}

				--i;
				continue;
			}
			else
			{
				std::cout << " AcquireNextFrame错误: " << hr << std::endl;
				return false;
			}
		}

		if (desktopResource == nullptr)
		{
			std::cout << " AcquireNextFrame超时" << std::endl;
			--i;
			continue;
		}

		// 查询下一帧暂存缓冲区
		ID3D11Texture2D* _pDX11Texture = nullptr;
		hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&_pDX11Texture));
		desktopResource->Release();
		desktopResource = nullptr;
		if (FAILED(hr))
		{
			return false;
		}

		// 复制旧描述
		ID3D11Texture2D* _pCopyBuffer = nullptr;
		D3D11_TEXTURE2D_DESC desc;
		if (_pDX11Texture) {
			_pDX11Texture->GetDesc(&desc);
		}
		else if (_pCopyBuffer) {
			_pCopyBuffer->GetDesc(&desc);
		}
		else {
			std::cout << " GetDesc错误" << std::endl;
			return false;
		}

		// 为填充帧图像创建一个新的暂存缓冲区
		if (_pCopyBuffer == nullptr) {
			D3D11_TEXTURE2D_DESC CopyBufferDesc;
			CopyBufferDesc.Width = desc.Width;
			CopyBufferDesc.Height = desc.Height;
			CopyBufferDesc.MipLevels = 1;
			CopyBufferDesc.ArraySize = 1;
			CopyBufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			CopyBufferDesc.SampleDesc.Count = 1;
			CopyBufferDesc.SampleDesc.Quality = 0;
			CopyBufferDesc.Usage = D3D11_USAGE_STAGING;
			CopyBufferDesc.BindFlags = 0;
			CopyBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			CopyBufferDesc.MiscFlags = 0;

			hr = _pDX11Dev->CreateTexture2D(&CopyBufferDesc, nullptr, &_pCopyBuffer);
			if (FAILED(hr))
			{
				std::cout << " CreateTexture2D错误: " << hr << std::endl;
				return false;
			}
		}

		if (_pDX11Texture) {
			// 将下一个暂存缓冲区复制到新的暂存缓冲区，为什么要复制，这俩不是一样吗？不一样，_pDX11Texture CPU不可读，没有权限，那是GPU用的
			_pDX11DevCtx->CopyResource(_pCopyBuffer, _pDX11Texture);
		}

		// 为映射位创建暂存缓冲区
		IDXGISurface* CopySurface = nullptr;
		hr = _pCopyBuffer->QueryInterface(__uuidof(IDXGISurface), (void**)&CopySurface);
		if (FAILED(hr)) {
			std::cout << " QueryInterface错误: " << hr << std::endl;
			return false;
		}

		// 将位复制到用户空间
		DXGI_MAPPED_RECT MappedSurface;
		hr = CopySurface->Map(&MappedSurface, DXGI_MAP_READ);

		//保存为bmp格式
		// 文件名
		//char picNameB[128] = { 0 };
		//snprintf(picNameB, sizeof(picNameB), "D:\\3\\ffmpeg操作详解\\ffmpeg操作详解\\DXGI--output\\Screen%d.bmp", i);
		//saveAsBmp(picNameB, MappedSurface.pBits, screenWidth, screenHeight, 32, true);

		// 将 MappedSurface.pBits 原始帧数据写入 frame;

		uint8_t* src = MappedSurface.pBits;	// 图像数据起始位置
		int srcPitch = MappedSurface.Pitch; // 图面宽度(字节为单位) -- 不一定 == width * perpix_size;

		uint8_t* dst = frame->data[0];	   // 缓冲区起始位置
		int dstLinesize = frame->linesize[0]; // FFmpeg期望帧宽度（包含对齐 padding,so != frame->width）

		if (av_frame_make_writable(frame) < 0)
		{
			av_log(NULL, AV_LOG_WARNING, "av_frame_make_writable(frame) < 0\n");
		}

		for (int y = 0; y < frame->height; ++y)
		{
			memcpy(
				dst + y * dstLinesize, 
				src + y * srcPitch, 
				frame->width * 4		// 这里用真实应该写入的字节数约束，一般 width * perpix_size < linesize[0]，所以肯定是能够容纳下的
			);
		}

		// 判断时间戳是否和帧率匹配，通过 “上一帧时间”
		// !isPass(frameInfo, i)
		if (!isPass(frameInfo, i))
		{
			//std::cout << " isPass() return false " << std::endl;
			if (_pDXGIOutputDup) {
				hr = _pDXGIOutputDup->ReleaseFrame();
			}

			CopySurface->Unmap();
			hr = CopySurface->Release();
			CopySurface = nullptr;

			if (_pDXGIOutputDup) {
				hr = _pDXGIOutputDup->ReleaseFrame();
			}

			if (_pDX11Texture)
			{
				_pDX11Texture->Release();
				_pDX11Texture = nullptr;
			}

			if (_pCopyBuffer)
			{
				_pCopyBuffer->Release();
				_pCopyBuffer = nullptr;
			}

			--i;
			continue;
		}

		// 设定显示时间戳，之前的用的frame->pts = i; 是因为 ctx->time_base = AVRational{1, sylar::fps}; 刚好一帧是一个帧间隔
		// 但是一般 time_base 不会是这样的  pts = 帧索引 * (时间基分母 / 帧率分子)，单位为 s 秒，其中 ()  内为每帧间隔(ms)
		frame->pts = av_rescale_q(sylar::frame_index, AVRational{ 1, sylar::fps }, ctx->time_base);
		// 送去编码
		int ret = encode(ctx, frame, pkt, oFmtCtx);
		if (ret < 0)
		{
			return false;
		}
		// 更新帧索引
		++sylar::frame_index;

		CopySurface->Unmap();
		hr = CopySurface->Release();
		CopySurface = nullptr;

		if (_pDXGIOutputDup) {
			hr = _pDXGIOutputDup->ReleaseFrame();
		}

		if (_pDX11Texture)
		{
			_pDX11Texture->Release();
			_pDX11Texture = nullptr;
		}

		if (_pCopyBuffer)
		{
			_pCopyBuffer->Release();
			_pCopyBuffer = nullptr;
		}

	}
	// 处理剩余数据
	encode(ctx, NULL, pkt, oFmtCtx);

	//9.写多媒体文件尾
	int ret = av_write_trailer(oFmtCtx);
	if (ret < 0)
	{
		av_log(oFmtCtx, AV_LOG_ERROR, "%s\n", "av_write_trailer failed!");
		return false;
	}

	// 释放所有资源
	if (_pDXGIOutputDup) _pDXGIOutputDup->Release();
	if (_pDXGIOutput1) _pDXGIOutput1->Release();
	if (_pDXGIOutput) _pDXGIOutput->Release();
	if (_pDXGITestAdapter) _pDXGITestAdapter->Release();
	if (_pDXGITestDev) _pDXGITestDev->Release();
	if (_pDX11DevCtx) _pDX11DevCtx->Release();
	if (_pDX11Dev) _pDX11Dev->Release();

	return true;
}


/*
* @brief 编码一帧frame数据，并写入文件
*/
static int encode(AVCodecContext* ctx, AVFrame* frame,
	AVPacket* pkt, AVFormatContext* oFmtCtx)
{
	int ret = 0;

	ret = avcodec_send_frame(ctx, frame);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to Send the frame to encoder\n");
		return -1;
	}

	while (true)
	{
		ret = avcodec_receive_packet(ctx, pkt);
		if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
		{
			return 0;
		}
		else if(ret < 0)
		{
			return -1;
		}

		//std::cout << "pkt->pts: " << pkt->pts << " pkt->dts: " << pkt->dts << std::endl;
		// fwrite(pkt->data, 1, pkt->size, f);
		// 设置 pkt->stream_index; 这个编码器不会管，你要自己设置，其余的时间戳都不用管
		pkt->stream_index = 0;
		av_packet_rescale_ts(pkt, ctx->time_base, oFmtCtx->streams[0]->time_base);
		// 写入文件(交叉)
		ret = av_interleaved_write_frame(oFmtCtx, pkt);
		if (ret < 0 && ret != AVERROR(EAGAIN))
		{
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errbuf, sizeof(errbuf));

			av_log(oFmtCtx, AV_LOG_ERROR,
				"av_interleaved_write_frame failed: %s\n",
				errbuf);
			return -1;
		}
		// 清空pkt对于缓冲区的引用，并将pkt其余字段重置，对应的缓冲区已经给到了
		// av_interleaved_write_frame 调用的缓冲区，但是缓冲区还是存在的，没有被释放
		av_packet_unref(pkt);
	}

	return 0;
}

/**
* @brief 设置输出文件上下文的一些参数，尤其是 outStream 文件中的输出流的 codecpar
*/
static AVFormatContext* openDstFile(AVCodecContext* codec_ctx)
{
	// 1、创建目标文件上下文
	AVFormatContext* oFmtCtx = nullptr;
	AVStream* outStream = nullptr;
	oFmtCtx = avformat_alloc_context();
	const char* dst = "./new/DXGI.mp4";
	if (!oFmtCtx)
	{
		av_log(oFmtCtx, AV_LOG_ERROR, "oFmtCtx = avformat_alloc_context()! Error\n");
		return nullptr;
	}
	// 2、猜测输出格式
	oFmtCtx->oformat = av_guess_format(NULL, dst, NULL);
	if (!oFmtCtx->oformat)
	{
		av_log(NULL, AV_LOG_ERROR, "outFmt = av_guess_format! Error\n");
		return nullptr;
	}
	// 3、创建输出流
	outStream = avformat_new_stream(oFmtCtx, NULL);
	if (!outStream)
	{
		av_log(NULL, AV_LOG_ERROR, "outStream = avformat_new_stream(oFmtCtx, NULL) Error\n");
		return nullptr;
	}
	// 4、打开文件
	int ret = avio_open2(&oFmtCtx->pb, dst, AVIO_FLAG_WRITE, NULL, NULL);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "avio_open2 Error\n");
		return nullptr;
	}
	// 设置视频流编码器参数
	ret = avcodec_parameters_from_context(outStream->codecpar, codec_ctx);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "avcodec_parameters_from_context(outStream->codecpar, codec_ctx) Error\n");
		return nullptr;
	}
	outStream->codecpar->codec_tag = 0;
	// outStream->time_base = codec_ctx->time_base;
	// 5、写多媒体文件头
	ret = avformat_write_header(oFmtCtx, NULL);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "avformat_write_header(oFmtCtx, NULL) Error\n");
		return nullptr;
	}
	
	return oFmtCtx;
}

int main(int argc, char* argv[])
{
	// 初始化计时器频率
	getFrequencys();

	av_log_set_level(AV_LOG_INFO);
	char err_buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
	AVPacket* pkt = nullptr;
	AVCodecContext* codec_ctx = nullptr;
	AVFormatContext* oFmtCtx = nullptr;
	FILE* f = nullptr;
	AVFrame* frame = nullptr;
	int ret = 0;

	// 找编码器
	const AVCodec* codec = nullptr;
	std::string codec_name = "h264_nvenc";
	codec = avcodec_find_encoder_by_name(codec_name.c_str());
	if (!codec)
	{
		av_log(NULL, AV_LOG_ERROR, "don't find the codec：%s\n", codec_name);
		goto _ERROR;
	}


	// 创建编码器上下文
	codec_ctx = nullptr;
	codec_ctx = avcodec_alloc_context3(codec);
	if (!codec_ctx)
	{
		av_log(NULL, AV_LOG_ERROR, "no memory!\n");
		goto _ERROR;
	}

	// 设置编码器参数（可以从其他地方来，这里选择自己写）
	codec_ctx->width = GetSystemMetrics(SM_CXSCREEN);
	codec_ctx->height = GetSystemMetrics(SM_CYSCREEN);
	codec_ctx->bit_rate = 15 * 1000 * 1000;

	codec_ctx->time_base = { 1, 1000000 };
	codec_ctx->framerate = { sylar::fps, 1 };

	codec_ctx->gop_size = 10;
	codec_ctx->max_b_frames = 0; // 在直播或者实时录屏中，B 帧一般设置为 0，一个是因为延迟
	codec_ctx->pix_fmt = AV_PIX_FMT_BGRA; // 像素格式（原视频数据，因为DXGI桌面API返回的就是 BGRA格式 微软声明）

	if (codec->id == AV_CODEC_ID_H264) // 这是一些私有设置，就像是 TCP 协议的各种设置一样
	{
		av_opt_set(codec_ctx->priv_data, "preset", "slow", 0);
	}

	// 绑定上下文和编码
	ret = avcodec_open2(codec_ctx, codec, NULL);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(codec_ctx, AV_LOG_ERROR, "Can't open the Codec：%s\n", err_buf);
		goto _ERROR;
	}

	//// 创建输出文件，暂时设为 .h264 后缀
	//f = fopen("./new/DXGI.h264", "wb");
	//if (!f)
	//{
	//	av_log(NULL, AV_LOG_ERROR, "Can't open the file：\n", "");
	//	goto _ERROR;
	//}
	// 1、创建输出文件上下文，输出文件为 dst = ./new/DXGI.mp4; ,多媒体文件头已经写入
	oFmtCtx = openDstFile(codec_ctx);
	if (!oFmtCtx)
	{
		av_log(NULL, AV_LOG_ERROR, "oFmtCtx = openDstFile(codec_ctx) is failed!\n");
		goto _ERROR;
	}

	// 创建AVFrame（保存原始视频数据）
	frame = av_frame_alloc();
	if (!frame)
	{
		av_log(NULL, AV_LOG_ERROR, "no memory\n");
		goto _ERROR;
	}

	// 设置计算frame的data应该分配空间大小的必要参数
	frame->width = codec_ctx->width;
	frame->height = codec_ctx->height;
	frame->format = codec_ctx->pix_fmt;

	ret = av_frame_get_buffer(frame, 0);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Could not allocate the video frame!\n");
		goto _ERROR;
	}

	// 使用AVPACKET来接收编码后的一帧数据
	pkt = av_packet_alloc();
	if (!pkt)
	{
		av_log(NULL, AV_LOG_ERROR, "no memory!\n");
		goto _ERROR;
	}

	// 主循环，读取帧数据，并给到 encode
	ret = acquireRGBAtoFrame(codec_ctx, frame, pkt, oFmtCtx);
	if (!ret)
	{
		av_log(NULL, AV_LOG_ERROR, "The acquireRGBAtoFrame() return false!\n");
		goto _ERROR;
	}

_ERROR:
	if (codec_ctx)
	{
		avcodec_free_context(&codec_ctx);
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

/**
* @brief 判断时间戳是否和帧率匹配，通过 “上一帧时间”
* 弃用：Reason：
* （1）在这个函数处理下，是可以基本达到帧数正常的情况，也就是最终输出的 DXGI.h264 视频
*	事件发生的快慢与现实基本一致
* （2）但是由于 DXGI 的特性，在上次 AcquireNextFrame 调用成功之后，如果这次返回 “超时”
*	那么对应的 DXGI_OUTDUPL_FRAME_INFO 也就是传进去的 frameInfo 的 LastPresentTime 被置为
*	0，也就是无法提供我们想要的时间戳了，哎~
* （3）并且，一个成熟的录屏 -- 编码过程，应该是 达到特定时间，ffmpeg 直接从一个 DXGI 的缓冲
*	中，拉取 lastest_frame，最新的一帧数据
*	在这样的设计下，编码线程与采集线程，是可以分开的，互不干扰，嗯嗯，这是一个理想的模式
*	即使在不开多线程的模式下，这个设计思路的可维护、可拓展性没有变化
*	但是反观我自个儿按照最简单粗暴的思路，直接根据 DXGI 提供的时间戳来判断是否该编码这一帧
*	很显然，这样的思路和理想的模式，根本就是两回事，编码和采集两个工作的联系太深了，不利于
*	后期的维护，嗯嗯，而且也很难解决 AcquireNextFrame 失败之后，时间戳 == 0的情况，嗯嗯，
*	所以打算重新写这个函数
*/
static bool isPass_version1(DXGI_OUTDUPL_FRAME_INFO& frameInfo, int i)
{
	// 值为 0 时(语义去查微软文档)，如何处理
	if (frameInfo.LastPresentTime.QuadPart == 0)
	{
		/*
		* 不用管，一般视频帧率不超过 60fps，录屏更是无所谓，也就是 16.6ms 一帧，当这个值为 0 时
		* 能走到这里，DXGI中的视频帧只不过没有更新而已，不是没有，而且，没有等待太长时间，所以不需要太多的处理，直接返回即可
		* 哦对了，如果 i == 0; 也就是第一帧，那更不用管了，直接返回即可
		*/
		return false;
	}

	if (sylar::PreviousFramePts == 0)
	{
		sylar::PreviousFramePts = frameInfo.LastPresentTime.QuadPart;
	}

	// 计算变化量计时器时间戳，单位 个
	uint64_t delta_ticks = frameInfo.LastPresentTime.QuadPart - sylar::PreviousFramePts;
	// 将变化量转换为 ms（之所以 * 1000000，微软文档表示：避免误差，嗯嗯，自己看）
	sylar::delta_time = (delta_ticks * 1000) / sylar::Frequency_s;
	// 与 MS 的差距
	int diff_time = sylar::delta_time - sylar::MS;
	// 处理思路，如果 < 0，表明当前获取的帧的时间戳(假设为-10)，也就是这一帧数据距离我们期望的 MS 早了 10 ms，再去获取下一帧，尽量靠近 MS
	// 也就是 diff_time --> 0（期望的）
	// 当然，如果 diff_time > 0，必须立马将这一帧写入，嗯嗯
	if (diff_time < 0)
	{
		// 误差在 5ms 以内，直接写入，否则放弃此帧(return false)
		if (diff_time >= -1)
		{
			//std::cout << "diff_time : " << diff_time << std::endl;
			// 更新时间戳（存放这一帧的 ticks）
			sylar::PreviousFramePts = frameInfo.LastPresentTime.QuadPart;
			return true;
		}
		else
		{
			return false;
		}
	}
	// >= 0，立即写入
	// 更新时间戳（存放这一帧的 ticks）
	sylar::PreviousFramePts = frameInfo.LastPresentTime.QuadPart;

	return true;
}