#define _CRT_SECURE_NO_WARNINGS
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <iostream>
#include <string>
#include <stdio.h>
#include <memory>
#include <wrl/client.h>
#include <vector>
#include <map>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
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
	int fps = 120;
	// 每帧间隔 ms
	int64_t MS = 1000 / fps;
	// Frequency 计时器的频率
	int64_t Frequency_s;
	// 两帧实际间隔 ms
	int64_t delta_time = 0;
	// D3D设备
	ComPtr<ID3D11Device> g_device;
	// D3D设备上下文
	ComPtr<ID3D11DeviceContext> g_devCtx;
	// 桌面复制
	ComPtr<IDXGIOutputDuplication> g_duplication;
	// 缓存 GPU 纹理，这个 ComPtr 是专用的智能指针(模板)，可以查看微软文档
	// ComPtr<ID3D11Texture2D> g_cachedTex;
	// 帧索引
	int64_t frame_index = 0;
	// AVFrame* 池大小
	static const int TEXTURE_BUFFER_SIZE = 4;
	// 存放 AVFrame* 的数组，内部元素均由 av_hwframe_get_buffer() 提供
	std::vector<AVFrame*> hwFramePool;
	// hwFramepool 当前索引
	uint64_t curentIndex = 0;

	//ComPtr<ID3D11Texture2D> g_texturePool[TEXTURE_BUFFER_SIZE];
	//int g_currentWriteIndex = 0; // 当前写入的缓冲区索引
	//int g_currentReadIndex = 0;  // 当前读取的缓冲区索引, 保证读取到的是最新的一帧索引
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
	AVPacket* pkt, AVFormatContext* oFmtCtx)
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
		int ret = encode(ctx, frame, pkt, oFmtCtx);
		if (ret < 0)
		{
			exit(1); // 强制退出程序，因为编码器已经报错了，再次编码没意义了
		}
		return true;
	}

	return false;
}

/**
* @brief 判断此时是否该使用最新的 DXGI 帧
* @return true 时间到了，可以放
* @return false 时间没到，再等会
*/
static bool isPass(int i, bool timeout = false)
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
			// 不是在超时逻辑中调用该函数，就更新时间帧
			if (!timeout)
			{
				sylar::PreviousFramePts = now.QuadPart;
			}
			return true;
		}
		// 否则，不送入，继续循环
		return false;
	}
	else if (i == 0)
	{
		sylar::PreviousFramePts = now.QuadPart;
		return true;
	}
}

static int encode(AVCodecContext* ctx, AVFrame* frame,
	AVPacket* pkt, AVFormatContext* oFmtCtx);

/**
* @brief 初始化 DXGI 组件，包括：
* 			g_cachedTex;
*			g_device;
*			g_devCtx;
*			g_duplication;
* @return true 初始化成功
* @return false 失败
*/
static bool initDXGI()
{
	int screenWidth = GetSystemMetrics(SM_CXSCREEN);
	int screenHeight = GetSystemMetrics(SM_CYSCREEN);

	// 优先复用 FFmpeg 的 D3D11 设备，避免跨设备资源导致 E_INVALIDARG
	if (!sylar::g_device || !sylar::g_devCtx)
	{
		// 兜底：若外部没提供，才自行创建
		D3D_DRIVER_TYPE DriverTypes[] = { D3D_DRIVER_TYPE_HARDWARE,
			D3D_DRIVER_TYPE_WARP, D3D_DRIVER_TYPE_REFERENCE, };
		UINT NumDriverTypes = ARRAYSIZE(DriverTypes);

		D3D_FEATURE_LEVEL FeatureLevels[] = {
			D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
			D3D_FEATURE_LEVEL_9_1 };
		UINT NumFeatureLevels = ARRAYSIZE(FeatureLevels);

		D3D_FEATURE_LEVEL FeatureLevel;
		HRESULT hr = E_FAIL;
		for (UINT index = 0; index < NumDriverTypes; index++) {
			hr = D3D11CreateDevice(
				nullptr, DriverTypes[index], nullptr, 0,
				FeatureLevels, NumFeatureLevels, D3D11_SDK_VERSION,
				&sylar::g_device, &FeatureLevel, &sylar::g_devCtx);
			if (SUCCEEDED(hr)) break;
		}
		if (FAILED(hr)) return false;
	}

	HRESULT hr = S_OK;

	// 2、获取DXGITest设备
	IDXGIDevice* _pDXGITestDev = nullptr;
	hr = sylar::g_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&_pDXGITestDev));
	if (FAILED(hr))
	{
		return false;
	}

	// 3、获取DXGITest适配器
	IDXGIAdapter* _pDXGITestAdapter = nullptr;
	hr = _pDXGITestDev->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&_pDXGITestAdapter));
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
	hr = _pDXGIOutput1->DuplicateOutput(sylar::g_device.Get(), &sylar::g_duplication);
	if (FAILED(hr))
	{
		return false;
	}

	// init 初始化完成
	// 创建 GPU 缓存纹理描述
	D3D11_TEXTURE2D_DESC texDesc = {};
	texDesc.Width = screenWidth;
	texDesc.Height = screenHeight;
	texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	texDesc.ArraySize = 1;
	texDesc.MipLevels = 1;
	texDesc.SampleDesc.Count = 1;
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags =
		D3D11_BIND_SHADER_RESOURCE |
		D3D11_BIND_RENDER_TARGET;

	/*
	* 7、创建多个纹理缓冲区（用来缓存最新帧）
	* 在 encode_DXGI_CPU.cpp 中，并没有采用 pool 的方式缓存多帧，这里为什么要这么做？
	* 首先来看看 DXGI 的GPU路径的原理：
	* （1）桌面复制接口通过 AcquireNextFrame拿到一帧
	* （2）CopyResource() 通过这个方法GPU拷贝到 pool 中
	* （3）outTexture 接收指针
	* （4）送入编码器消费
	* 关键是这个消费者是谁？h264_nvenc，这消费者直接是显卡，假设刚送入一帧frame
	* 显卡就立即编码它，没问题，就不需要这个 pool了，但是显卡编码是 “异步” 的
	* 但是送入 frame 之后，我们还要不断地去 CopyResource() 覆盖掉 frame 中的数据，
	* 如果仅仅只有一个 frame 的话？有没有可能 -- 那边显卡刚要通过指针去拿到 GPU 显存中的
	* 一帧数据，结构这一帧数据直接被我们使用 CopyResource() 覆盖掉了？完了，对吧
	* 为了规避这种情况，英伟达实例中采用 pool 缓存多帧数据，让原本应该立即覆盖的frame
	* 放到后面被覆盖，就是这个时间差，显卡已经编码好了这一帧，问题解决了，这是显卡本身做出了一个承诺
	* 没问题，但是更为深层次、准确的原因，我并没有去看，你以后有时间去研究一下吧
	* for (int i = 0; i < sylar::TEXTURE_BUFFER_SIZE; ++i)
	* {
	* 	 hr = sylar::g_device->CreateTexture2D(&texDesc, nullptr, &sylar::g_texturePool[i]);
	*	 if (FAILED(hr))
	*	 {
	*		 av_log(NULL, AV_LOG_ERROR, " CreateTexture2D failed\n");
	*		 return false;
	*	 }
	* }
	*/

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
		else if (ret < 0)
		{
			return -1;
		}

		// 设置pkt包参数
		pkt->stream_index = oFmtCtx->streams[0]->index;
		// 转化pkt的时间戳 -- 符合mp4格式的时间基(某些格式有自己的标准)
		av_packet_rescale_ts(pkt, ctx->time_base,
			oFmtCtx->streams[pkt->stream_index]->time_base);
		// 送入文件
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
		// av_interleaved_write_frame() 调用的缓冲区，但是缓冲区还是存在的，没有被释放
		av_packet_unref(pkt);
	}

	return 0;
}

void PrintTextureInfo(ID3D11Texture2D* tex, const char* name)
{
	if (!tex) {
		std::cout << name << " is null" << std::endl;
		return;
	}

	D3D11_TEXTURE2D_DESC desc;
	tex->GetDesc(&desc);

	std::cout << "=== Texture Info: " << name << " ===" << std::endl;
	std::cout << "Width: " << desc.Width << std::endl;
	std::cout << "Height: " << desc.Height << std::endl;
	std::cout << "Format: " << desc.Format << std::endl;
	std::cout << "MipLevels: " << desc.MipLevels << std::endl;
	std::cout << "ArraySize: " << desc.ArraySize << std::endl;
	std::cout << "BindFlags: " << desc.BindFlags << std::endl;
	std::cout << "CPUAccessFlags: " << desc.CPUAccessFlags << std::endl;
	std::cout << "Usage: " << desc.Usage << std::endl;
	std::cout << "SampleDesc.Count: " << desc.SampleDesc.Count
		<< ", Quality: " << desc.SampleDesc.Quality << std::endl;
	std::cout << "======================================" << std::endl;
}

void CopyTemporaryNV12ToHWFrame(
	ID3D11Texture2D* tmpNV12,    // 临时 NV12（VideoProcessor 输出）
	ID3D11Texture2D* hwFrameNV12 // hwFramePool 中对应 NV12
)
{
	if (!sylar::g_devCtx || !tmpNV12 || !hwFrameNV12) {
		std::cerr << "CopyTemporaryNV12ToHWFrame: null texture!" << std::endl;
		return;
	}

	// 获取源和目标描述
	D3D11_TEXTURE2D_DESC srcDesc, dstDesc;
	tmpNV12->GetDesc(&srcDesc);
	hwFrameNV12->GetDesc(&dstDesc);

	// 只拷贝 ArraySlice = 0
	for (UINT plane = 0; plane < 2; ++plane) { // NV12 两个平面：Y=0, UV=1
		D3D11_BOX srcBox = {};
		srcBox.left = 0;
		srcBox.top = 0;
		srcBox.front = 0;
		srcBox.right = srcDesc.Width;
		srcBox.bottom = srcDesc.Height;
		srcBox.back = 1;

		// dstSubresource = miplevel + ArraySlice * planeCount
		UINT dstSubresource = D3D11CalcSubresource(
			0,       // MipLevel
			0 + plane, // ArraySlice = 0, plane offset
			1         // MipLevels
		);

		sylar::g_devCtx->CopySubresourceRegion(
			hwFrameNV12,
			dstSubresource,
			0, 0, 0,
			tmpNV12,
			plane,
			&srcBox
		);
	}
}

static bool GetD3D11TextureFromAVFrame(AVFrame* frame,
	ID3D11Texture2D** outTex,
	UINT* outSubresource)
{
	if (!frame || !outTex || !outSubresource) return false;

	// AV_PIX_FMT_D3D11 下，data[0]通常是 ID3D11Texture2D*
	// data[1]通常存放子资源索引（通过 intptr_t 传递）
	auto* tex = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
	if (!tex) return false;

	UINT sub = static_cast<UINT>(reinterpret_cast<uintptr_t>(frame->data[1]));

	*outTex = tex;
	(*outTex)->AddRef(); // 调用者释放
	*outSubresource = sub;
	return true;
}

// -----------------------------
// 2) 创建(一次性) VideoProcessor 资源
// -----------------------------
struct BgraToNv12VP
{
	ComPtr<ID3D11VideoDevice>           videoDev;
	ComPtr<ID3D11VideoContext>          videoCtx;
	ComPtr<ID3D11VideoProcessorEnumerator> vpEnum;
	ComPtr<ID3D11VideoProcessor>        vp;
	UINT                                width = 0;
	UINT                                height = 0;
};

static bool InitVP(BgraToNv12VP& s, UINT width, UINT height)
{
	if (!sylar::g_device || !sylar::g_devCtx) return false;

	if (s.vp && s.width == width && s.height == height) {
		return true; // 已初始化且尺寸匹配
	}

	s = {}; // reset

	HRESULT hr = sylar::g_device->QueryInterface(__uuidof(ID3D11VideoDevice),
		reinterpret_cast<void**>(s.videoDev.GetAddressOf()));
	if (FAILED(hr) || !s.videoDev) {
		std::cerr << "QueryInterface(ID3D11VideoDevice) failed\n";
		return false;
	}

	hr = sylar::g_devCtx->QueryInterface(__uuidof(ID3D11VideoContext),
		reinterpret_cast<void**>(s.videoCtx.GetAddressOf()));
	if (FAILED(hr) || !s.videoCtx) {
		std::cerr << "QueryInterface(ID3D11VideoContext) failed\n";
		return false;
	}

	D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
	contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	contentDesc.InputFrameRate.Numerator = 60;
	contentDesc.InputFrameRate.Denominator = 1;
	contentDesc.OutputFrameRate.Numerator = 60;
	contentDesc.OutputFrameRate.Denominator = 1;
	contentDesc.InputWidth = width;
	contentDesc.InputHeight = height;
	contentDesc.OutputWidth = width;
	contentDesc.OutputHeight = height;
	contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

	hr = s.videoDev->CreateVideoProcessorEnumerator(&contentDesc, &s.vpEnum);
	if (FAILED(hr)) {
		std::cerr << "CreateVideoProcessorEnumerator failed\n";
		return false;
	}

	hr = s.videoDev->CreateVideoProcessor(s.vpEnum.Get(), 0, &s.vp);
	if (FAILED(hr)) {
		std::cerr << "CreateVideoProcessor failed\n";
		return false;
	}

	s.width = width;
	s.height = height;
	return true;
}

// -----------------------------
// 3) BGRA 纹理转换到 AVFrame 对应 NV12 纹理
// -----------------------------
bool ConvertDesktopBGRA_To_AVFrameNV12(
	ID3D11Texture2D* srcBGRA,   // 来自 AcquireNextFrame 的桌面纹理
	AVFrame* hwFrame            // av_hwframe_get_buffer 分配的 D3D11 frame
)
{
	if (!srcBGRA || !hwFrame) {
		std::cerr << "ConvertDesktopBGRA_To_AVFrameNV12: null input\n";
		return false;
	}

	// 源描述
	D3D11_TEXTURE2D_DESC srcDesc = {};
	srcBGRA->GetDesc(&srcDesc);

	// 目标纹理 + 目标子资源索引（来自 AVFrame->data[1]）
	ComPtr<ID3D11Texture2D> dstTex;
	UINT dstSubresource = 0;
	if (!GetD3D11TextureFromAVFrame(hwFrame, &dstTex, &dstSubresource)) {
		std::cerr << "GetD3D11TextureFromAVFrame failed\n";
		return false;
	}

	D3D11_TEXTURE2D_DESC dstDesc = {};
	dstTex->GetDesc(&dstDesc);

	// 基本校验
	if (dstDesc.Format != DXGI_FORMAT_NV12) {
		std::cerr << "Destination frame texture is not NV12\n";
		return false;
	}
	if (dstDesc.Width != srcDesc.Width || dstDesc.Height != srcDesc.Height) {
		std::cerr << "Size mismatch src(" << srcDesc.Width << "x" << srcDesc.Height
			<< ") dst(" << dstDesc.Width << "x" << dstDesc.Height << ")\n";
		return false;
	}
	if (srcDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
		srcDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
		std::cerr << "Unexpected source format: " << srcDesc.Format << "\n";
		return false;
	}

	static BgraToNv12VP s_vp;
	if (!InitVP(s_vp, srcDesc.Width, srcDesc.Height)) {
		std::cerr << "InitVP failed\n";
		return false;
	}

	HRESULT hr = S_OK;

	// 输入视图（桌面 BGRA）
	D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inViewDesc = {};
	inViewDesc.FourCC = 0;
	inViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
	inViewDesc.Texture2D.MipSlice = 0;
	inViewDesc.Texture2D.ArraySlice = 0; // 桌面复制通常是单 slice

	ComPtr<ID3D11VideoProcessorInputView> inView;
	hr = s_vp.videoDev->CreateVideoProcessorInputView(
		srcBGRA, s_vp.vpEnum.Get(), &inViewDesc, &inView);
	if (FAILED(hr)) {
		std::cerr << "CreateVideoProcessorInputView failed, hr=0x" << std::hex << hr << std::dec << "\n";
		return false;
	}

	// 输出视图（关键：使用 AVFrame 指定的 subresource/slice）
	D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outViewDesc = {};

	// 当前工程里 MipLevels 基本为 1，所以 subresource 可以直接视作 arraySlice
	UINT dstArraySlice = dstSubresource;
	if (dstArraySlice >= dstDesc.ArraySize) {
		std::cerr << "Invalid dstSubresource/slice: " << dstSubresource
			<< ", ArraySize=" << dstDesc.ArraySize << "\n";
		return false;
	}

	if (dstDesc.ArraySize > 1) {
		outViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
		outViewDesc.Texture2DArray.MipSlice = 0;
		outViewDesc.Texture2DArray.FirstArraySlice = dstArraySlice;
		outViewDesc.Texture2DArray.ArraySize = 1;
	}
	else {
		outViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
		outViewDesc.Texture2D.MipSlice = 0;
	}

	ComPtr<ID3D11VideoProcessorOutputView> outView;
	hr = s_vp.videoDev->CreateVideoProcessorOutputView(
		dstTex.Get(), s_vp.vpEnum.Get(), &outViewDesc, &outView);
	if (FAILED(hr)) {
		std::cerr << "CreateVideoProcessorOutputView failed, hr=0x" << std::hex << hr << std::dec << "\n";
		return false;
	}

	RECT srcRect = { 0, 0, (LONG)srcDesc.Width, (LONG)srcDesc.Height };
	RECT dstRect = { 0, 0, (LONG)dstDesc.Width, (LONG)dstDesc.Height };
	s_vp.videoCtx->VideoProcessorSetStreamSourceRect(s_vp.vp.Get(), 0, TRUE, &srcRect);
	s_vp.videoCtx->VideoProcessorSetStreamDestRect(s_vp.vp.Get(), 0, TRUE, &dstRect);

	// 颜色空间（可按需调整）
	D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCS = {};
	inCS.Usage = 0;
	inCS.RGB_Range = 1; // full RGB
	inCS.YCbCr_Matrix = 1; // BT.709
	inCS.YCbCr_xvYCC = 0;
	inCS.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;

	D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCS = {};
	outCS.Usage = 0;
	outCS.RGB_Range = 0;
	outCS.YCbCr_Matrix = 1; // BT.709
	outCS.YCbCr_xvYCC = 0;
	outCS.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;

	s_vp.videoCtx->VideoProcessorSetStreamColorSpace(s_vp.vp.Get(), 0, &inCS);
	s_vp.videoCtx->VideoProcessorSetOutputColorSpace(s_vp.vp.Get(), &outCS);

	D3D11_VIDEO_PROCESSOR_STREAM stream = {};
	stream.Enable = TRUE;
	stream.OutputIndex = 0;
	stream.InputFrameOrField = 0;
	stream.PastFrames = 0;
	stream.FutureFrames = 0;
	stream.pInputSurface = inView.Get();

	hr = s_vp.videoCtx->VideoProcessorBlt(
		s_vp.vp.Get(),
		outView.Get(),
		0,
		1,
		&stream);

	if (FAILED(hr)) {
		std::cerr << "VideoProcessorBlt failed, hr=0x" << std::hex << hr << std::dec << "\n";
		return false;
	}

	return true;
}

/**
* @brief 获取最新帧，并给到 outFrame，并附带超时处理
* @param ctx 编码器山下文
* @param outFrame 未初始化(为分配空间)的AVFrame**指针，接收最新的桌面帧
*
* @return 1 表示成功
* @return 0 超时并且没有拿到帧(没到帧间隔时间)
* @return -1 表示出现错误，终止程序
*/
static int CaptureFrame(AVCodecContext* ctx, AVFrame** outFrame, int i)
{
	HRESULT hr;
	DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
	ComPtr<IDXGIResource> desktopResource;
	hr = sylar::g_duplication->AcquireNextFrame(0,
		&frameInfo,
		&desktopResource);

	//static uint64_t timeout_counts = 0;

	// 超时
	if (hr == DXGI_ERROR_WAIT_TIMEOUT)
	{
		//std::cout << "超时: " << timeout_counts++ << std::endl;
		if (desktopResource)
		{
			desktopResource->Release();
			desktopResource = nullptr;
		}

		// 但是达到帧间隔
		if (isPass(i, true))
		{
			// 计算 sylar::currentIndex 的上一个索引
			size_t prevIndex = (sylar::curentIndex + sylar::hwFramePool.size() - 1) % sylar::hwFramePool.size();
			// 将上一帧给出
			*outFrame = sylar::hwFramePool[prevIndex];
			return 1;
		}

		return 0;
	}
	// 错误，返回-1
	if (FAILED(hr))
	{
		return -1;
	}
	// 成功拿到新帧
	ComPtr<ID3D11Texture2D> dxgiTex;
	desktopResource.As(&dxgiTex);

	// 4、拷贝 DXGI 桌面帧到FFmpeg的 D3D11 Texture
	// ID3D11Texture2D* nv12Tex = (ID3D11Texture2D*)sylar::hwFramePool[sylar::curentIndex]->data[0];
	// sylar::g_devCtx->CopyResource(nv12Tex, dxgiTex.Get()); 
	// 直接用GPU拷贝进入对应 hwframe->data[0] 硬件帧缓冲区中，
	// 会因为编码器支持的硬件帧格式而发生错误，导致整个视频全都是绿色···，CPU版本正常是因为：显卡内部自己转换了格式，ffmpeg -h encoder=h264_nvenc
	// 中明确输出：软件帧格式支持很多，包括 BGRA，所以可以，但是硬件帧那一行仅仅显示
	// Supported hardware devices: cuda cuda d3d11va d3d11va，没有BGRA，编码器接收的是 NV12 像素格式

	if (!ConvertDesktopBGRA_To_AVFrameNV12(dxgiTex.Get(), sylar::hwFramePool[sylar::curentIndex]))
	{
		av_log(NULL, AV_LOG_ERROR, "BGRA->NV12 convert failed\n");
		sylar::g_duplication->ReleaseFrame();
		return -1;
	}

	// 5、返回 outFrame;
	*outFrame = sylar::hwFramePool[sylar::curentIndex];
	// 6、更新索引（环形）
	sylar::curentIndex = (sylar::curentIndex + 1) % sylar::hwFramePool.size();
	// 调用完 AcquireNextFrame 必须调用 ReleaseFrame()
	sylar::g_duplication->ReleaseFrame();

	return 1;
}

/**
* @brief 主循环处理桌面纹理，并处理文件尾信息
*/
static int sendTextoFrametoEncode(AVCodecContext* ctx, AVPacket* pkt, AVFormatContext* oFmtCtx)
{
	int ret = 0;

	AVFrame* frame = nullptr;
	for (int i = 0; i < 1000; ++i)
	{
		ret = CaptureFrame(ctx, &frame, i);
		// 获取成功
		if (ret == 1)
		{
			// 检查是否到了送入编码器的时间
			if (isPass(i))
			{
				// frmae 时间戳
				frame->pts = av_rescale_q(sylar::frame_index,
					AVRational{ 1, sylar::fps }, ctx->time_base);

				// 编码
				ret = encode(ctx, frame, pkt, oFmtCtx);
				if (ret < 0)
				{
					return -1;
				}

				std::cout << "成功: " << i << std::endl;
				// 更新帧索引
				++sylar::frame_index;
			}
			else // 时间未到
			{
				--i;
				continue;
			}

		}
		else if (ret == 0) // 获取失败（一般是超时了，并且没有达到帧间隔）
		{
			--i;
			continue;
		}
		else // 返回错误，终止程序
		{
			av_log(NULL, AV_LOG_ERROR, "CaptureFrame() ERROR\n");
			return -1;
		}
	}
	// 刷新缓冲区
	encode(ctx, nullptr, pkt, oFmtCtx);

	// 写文件尾
	ret = av_write_trailer(oFmtCtx);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "av_write_trailer failed!\n");
		return -1;
	}

	return 0;
}

/**
* @brief 创建 D3D11 硬件设备
* @return 数据缓冲区的引用
*/
static AVBufferRef* InitHWDevice_D3D11()
{
	AVBufferRef* hw_device_ctx = nullptr;
	int ret = av_hwdevice_ctx_create(
		&hw_device_ctx,
		AV_HWDEVICE_TYPE_D3D11VA,
		NULL,
		NULL,
		0);
	if (ret < 0)
	{
		return nullptr;
	}

	// 关键：拿到 FFmpeg 内部的 D3D11 设备，后续 DXGI/VP 必须复用它
	AVHWDeviceContext* hwctx = reinterpret_cast<AVHWDeviceContext*>(hw_device_ctx->data);
	AVD3D11VADeviceContext* d3d11hw =
		reinterpret_cast<AVD3D11VADeviceContext*>(hwctx->hwctx);

	if (!d3d11hw || !d3d11hw->device)
	{
		av_buffer_unref(&hw_device_ctx);
		return nullptr;
	}

	sylar::g_device = d3d11hw->device; // ComPtr 会 AddRef

	if (d3d11hw->device_context)
	{
		sylar::g_devCtx = d3d11hw->device_context; // ComPtr 会 AddRef
	}
	else
	{
		ID3D11DeviceContext* ctx = nullptr;
		sylar::g_device->GetImmediateContext(&ctx);
		sylar::g_devCtx = ctx;
		if (ctx) ctx->Release();
	}

	return hw_device_ctx;
}

static int InitHWFrames_D3D11(
	AVCodecContext* ctx,
	AVBufferRef* hw_device_ctx,
	int width,
	int height)
{
	int ret = 0;
	char err_buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };

	AVBufferRef* hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx);
	AVHWFramesContext* frames_ctx =
		(AVHWFramesContext*)hw_frames_ref->data;

	frames_ctx->format = AV_PIX_FMT_D3D11; // GPU Texture
	frames_ctx->sw_format = AV_PIX_FMT_NV12;  // NVENC 支持
	frames_ctx->width = width;
	frames_ctx->height = height;
	frames_ctx->initial_pool_size = sylar::TEXTURE_BUFFER_SIZE;

	// 3. 设置 D3D11 私有字段 BindFlags
	AVD3D11VAFramesContext* d3d11_ctx =
		(AVD3D11VAFramesContext*)frames_ctx->hwctx;

	d3d11_ctx->BindFlags =
		D3D11_BIND_RENDER_TARGET |   // VideoProcessor 输出必须
		D3D11_BIND_SHADER_RESOURCE;  // 编码链路可保留

	// 该方法会调用相应的 硬件Texture2D API，填充 frames_ctx 对应的帧池，在送入之前，一定要设置合理的
	// hw_frames_ref 参数，例如：D3D11的话，参数要符合 CreateTexture2D() 的参数要求
	ret = av_hwframe_ctx_init(hw_frames_ref);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(NULL, AV_LOG_ERROR, "av_hwframe_ctx_init() is failed：%s\n", err_buf);
		av_buffer_unref(&hw_frames_ref);
		return -1;
	}

	// ctx->hw_device_ctx, 这个字段的解释中提到，如果用户自己设置输入帧，那么就需要设置 hw_frames_ctx，而不是hw_device_ctx字段
	ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
	av_buffer_unref(&hw_frames_ref);

	return 0;
}

static AVCodecContext* InitNVENCEncoder(
	AVBufferRef* hw_device_ctx,
	int width,
	int height,
	int fps)
{
	const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
	if (!codec)
	{
		av_log(NULL, AV_LOG_ERROR, "h264_nvenc not Found!\n");
		return nullptr;
	}

	AVCodecContext* ctx = avcodec_alloc_context3(codec);
	if (!ctx)
	{
		av_log(NULL, AV_LOG_ERROR, "avcodec_alloc_context3(codec)\n");
		return nullptr;
	}

	// 设置编码器参数（可以从其他地方来，这里选择自己写）
	ctx->width = width;
	ctx->height = height;
	ctx->bit_rate = 15 * 1000 * 1000;

	ctx->time_base = { 1, sylar::fps };
	ctx->framerate = { sylar::fps, 1 };

	ctx->gop_size = 10;
	ctx->max_b_frames = 1; // 特么的，直接设置为0，没什么影响！
	ctx->pix_fmt = AV_PIX_FMT_D3D11; // 硬件像素格式 
	ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx); // 看官方注释

	if (codec->id == AV_CODEC_ID_H264) // 私有设置，可以没有
	{
		av_opt_set(ctx->priv_data, "preset", "slow", 0);
	}

	// 创建 D3D11 硬件帧池
	int ret = InitHWFrames_D3D11(ctx, hw_device_ctx, width, height);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "InitHWFrames_D3D11 failed!\n");
		return nullptr;
	}

	ret = avcodec_open2(ctx, codec, NULL);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "avcodec_open2(ctx, codec, NULL)\n");
		return nullptr;
	}

	return ctx;
}

static AVFormatContext* openDstFile(AVCodecContext* codec_ctx)
{
	// 1、创建目标文件上下文，文件路径
	AVFormatContext* oFmtCtx = nullptr;
	AVStream* outStream = nullptr;
	const char* dst = "./new/DXGI.mp4";
	oFmtCtx = avformat_alloc_context();
	if (!oFmtCtx)
	{
		av_log(oFmtCtx, AV_LOG_ERROR, "oFmtCtx = avformat_alloc_context()! Error\n");
		return nullptr;
	}
	// 2、猜测输出格式 (根据dst, mp4、flv、mov、···)
	oFmtCtx->oformat = av_guess_format(NULL, dst, NULL);
	if (!oFmtCtx->oformat)
	{
		av_log(NULL, AV_LOG_ERROR, "outFmt = av_guess_format! Error\n");
		return nullptr;
	}
	// 3、创建输出流（视频流）流ID
	outStream = avformat_new_stream(oFmtCtx, NULL);
	if (!outStream)
	{
		av_log(NULL, AV_LOG_ERROR, "outStream = avformat_new_stream(oFmtCtx, NULL) Error\n");
		return nullptr;
	}
	//  、填充视频流编解码器参数（从codec_ctx中获取）
	int ret = avcodec_parameters_from_context(outStream->codecpar, codec_ctx);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "avcodec_parameters_from_context(outStream->codecpar, codec_ctx) Error\n");
		return nullptr;
	}
	outStream->codecpar->codec_tag = 0;
	// 4、打开文件IO
	ret = avio_open2(&oFmtCtx->pb, dst, AVIO_FLAG_WRITE, NULL, NULL);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "avio_open2 Error\n");
		return nullptr;
	}

	// 文件头加选项
	AVDictionary* mux_opts = nullptr;
	av_dict_set(&mux_opts, "movflags", "+negative_cts_offsets", 0);

	// 5、写多媒体文件头
	ret = avformat_write_header(oFmtCtx, &mux_opts);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "avformat_write_header(oFmtCtx, NULL) Error\n");
		return nullptr;
	}
	// 释放选项
	av_dict_free(&mux_opts);

	return oFmtCtx;
}

static int init_hwFramePool(AVCodecContext* ctx)
{
	int ret = 0;
	// 设置容量
	sylar::hwFramePool.reserve(sylar::TEXTURE_BUFFER_SIZE);

	for (int i = 0; i < sylar::TEXTURE_BUFFER_SIZE; ++i)
	{
		AVFrame* hwframe = av_frame_alloc();
		ret = av_hwframe_get_buffer(ctx->hw_frames_ctx, hwframe, 0);
		if (ret < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "av_hwframe_get_buffer() failed!\n");
			return -1;
		}
		// 放入数组
		sylar::hwFramePool.push_back(hwframe);
	}

	return 0;
}


int main(int argc, char* argv[])
{
	// 初始化计时器频率
	getFrequencys();

	av_log_set_level(AV_LOG_INFO);
	char err_buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
	AVPacket* pkt = nullptr;
	AVCodecContext* codec_ctx = nullptr;
	FILE* f = nullptr;
	AVFrame* frame = nullptr;
	// 目标文件上下文
	AVFormatContext* oFmtCtx = nullptr;
	int ret = 0;

	// 1、初始化并获取设备
	AVBufferRef* hw_device_ctx = InitHWDevice_D3D11();

	//// 1、初始化 DXGI -- 这一步放在 初始化 g_device && g_devCtx 之后就行，不用放在最末尾
	//ret = initDXGI();
	//if (!ret)
	//{
	//	av_log(NULL, AV_LOG_ERROR, "The initDXGI() return false!\n");
	//	goto _ERROR;
	//}

	// 2、初始化编码器(其中包含创建hwdevice)
	codec_ctx = InitNVENCEncoder(hw_device_ctx, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), sylar::fps);
	if (!codec_ctx)
	{
		av_log(NULL, AV_LOG_ERROR, "InitNVENCEncoder() failed\n");
		goto _ERROR;
	}

	// 初始化 frame，并指定各项参数
	//do
	//{
	//	// 创建AVFrame（保存原始视频数据）
	//	frame = av_frame_alloc();
	//	if (!frame)
	//	{
	//		av_log(NULL, AV_LOG_ERROR, "no memory\n");
	//		goto _ERROR;
	//	}
	//	// 设置计算frame的data应该分配空间大小的必要参数
	//	frame->width = codec_ctx->width;
	//	frame->height = codec_ctx->height;
	//	frame->format = codec_ctx->pix_fmt;
	//	// 分配具体空间（CPU / 软件帧），如果是硬件帧，就不用这个 API 了，sendTextoFrametoEncode() 中会处理
	//	if (frame->format != AV_PIX_FMT_D3D11)
	//	{
	//		ret = av_frame_get_buffer(frame, 0);
	//		if (ret < 0)
	//		{
	//			av_log(NULL, AV_LOG_ERROR, "Could not allocate the video frame!\n");
	//			goto _ERROR;
	//		}
	//	}
	//} while (0);

	// 3、初始化frame数组
	ret = init_hwFramePool(codec_ctx);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "main: init_hwFramePool() is failed!\n");
		goto _ERROR;
	}

	// 使用AVPACKET来接收编码后的一帧数据
	pkt = av_packet_alloc();
	if (!pkt)
	{
		av_log(NULL, AV_LOG_ERROR, "no memory!\n");
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


	// 1、初始化 DXGI
	ret = initDXGI();
	if (!ret)
	{
		av_log(NULL, AV_LOG_ERROR, "The initDXGI() return false!\n");
		goto _ERROR;
	}

	// 主循环，并送去编码，写入文件，写文件尾部信息
	ret = sendTextoFrametoEncode(codec_ctx, pkt, oFmtCtx);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "The sendTextoFrametoEncode() return %d\n", ret);
		goto _ERROR;
	}


_ERROR:
	if (hw_device_ctx)
	{
		av_buffer_unref(&hw_device_ctx);
	}

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

	if (oFmtCtx)
	{
		if (oFmtCtx->pb)
		{
			avio_close(oFmtCtx->pb);
		}
		avformat_free_context(oFmtCtx);
	}

	if (f)
	{
		fclose(f);
	}

	return 0;
}
