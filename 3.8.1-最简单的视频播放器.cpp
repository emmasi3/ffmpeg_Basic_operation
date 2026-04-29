#define CRT_SECURE_NO_WARNING
#include <iostream>
extern"C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include<libavutil/log.h>
#include <SDL.h>

}

class _VideoState
{
public:
	_VideoState() {};
	_VideoState(AVCodecContext* avctx, AVPacket* pkt,
		AVFrame* frame, SDL_Texture* texture) :
		avctx(avctx), pkt(pkt), frame(frame), texture(texture)
	{

	}

	AVCodecContext* avctx;
	AVPacket* pkt;
	AVFrame* frame;
	SDL_Texture* texture;

	~_VideoState()
	{
		if (avctx) avcodec_free_context(&avctx);
		if (pkt) av_packet_free(&pkt);
		if (frame) av_frame_free(&frame);
		if (texture) SDL_DestroyTexture(texture);
	}
}decodepar;

static SDL_Window* win = nullptr;
static SDL_Renderer* renderer = nullptr;
static int w_width = 640;
static int w_height = 480;

static void render(_VideoState& par)
{
	SDL_UpdateYUVTexture(par.texture, NULL,
		par.frame->data[0], par.frame->linesize[0],
		par.frame->data[1], par.frame->linesize[1],
		par.frame->data[2], par.frame->linesize[2]
	);
	//用“绘图颜色”清理当前渲染目标，没设置默认为黑窗口
	SDL_RenderClear(renderer);
	//送到GPU（计算）
	SDL_RenderCopy(renderer, par.texture, NULL, NULL);
	//刷新窗口 ---- 显示一帧图像
	SDL_RenderPresent(renderer);
}

static int decode(_VideoState& par)
{
	int ret = -1;
	char buf[1024];

	//送去解码
	ret = avcodec_send_packet(par.avctx, par.pkt);
	if (ret < 0)
	{
		av_log(par.avctx, AV_LOG_ERROR, "Failed to send frame to decoder!\n");
		return -1;
	}

	while (ret >= 0)
	{
		ret = avcodec_receive_frame(par.avctx, par.frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
		{
			return 0;
		}
		else if (ret < 0)
		{
			return -1; // 退出
		}
		
		//渲染
		render(par);
	}

}

int main(int argc,char* argv[])
{
	av_log_set_level(AV_LOG_DEBUG);
	char err_buf[AV_ERROR_MAX_STRING_SIZE];

	int ret = -1;
	int idx = 1;
	char* src = nullptr;

	AVFormatContext* fmtCtx = nullptr;
	AVStream* inStream = nullptr;
	const AVCodec* codec_d = nullptr;
	AVCodecContext* ctx_d = nullptr;
	AVPacket* pkt = nullptr;
	AVFrame* frame = nullptr;

	SDL_Texture* texture = nullptr;
	SDL_Event event;

	//1.判断输入参数
	if (argc < 2)
	{
		av_log(NULL,AV_LOG_ERROR ,"the arguments is must more than 1\n");
		return ret;
	}

	src = argv[1];

	//2.初始化SDL，并创建窗口和Render
	ret = SDL_Init(SDL_INIT_VIDEO);
	if (ret != 0)
	{
		SDL_Log("Failed to initialize SDL--video!\n");
		return -1;
	}

	win = SDL_CreateWindow("Sample Player",
		SDL_WINDOWPOS_UNDEFINED,
		SDL_WINDOWPOS_UNDEFINED,
		w_width,w_height,
		SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
		);
	if (!win)
	{
		SDL_Log("Failed to CreateWindow!\n");
		goto _EXIT;
	}

	renderer = SDL_CreateRenderer(win, -1, 0);
	if (!renderer)
	{
		SDL_Log("Failed to Create renderer!\n");
		goto _EXIT;
	}

	//3.打开多媒体文件，获取流信息
	ret = avformat_open_input(&fmtCtx, src, NULL, NULL);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(NULL, AV_LOG_ERROR, "%s\n", err_buf);
		goto _EXIT;
	}

	ret = avformat_find_stream_info(fmtCtx, NULL); // 这个函数能够获取流信息，储存在 fmtCtx->streams 中
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(NULL, AV_LOG_ERROR, "%s\n", err_buf);
		goto _EXIT;
	}

	//4.查找最好的（目标）视频流
	idx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (idx < 0)
	{
		av_strerror(idx, err_buf, sizeof(err_buf));
		av_log(fmtCtx, AV_LOG_ERROR, "%s\n", err_buf);
		goto _EXIT;
	}

	//5.根据codec_id，获得解码器
	inStream = fmtCtx->streams[idx];
	codec_d = avcodec_find_decoder(inStream->codecpar->codec_id);
	if (!codec_d)
	{
		av_log(NULL, AV_LOG_ERROR, "%s\n", "Could not find Codec!");
		goto _EXIT;
	}

	//6.创建解码器上下文
	ctx_d = avcodec_alloc_context3(codec_d);
	if (!ctx_d)
	{
		av_log(NULL, AV_LOG_ERROR, "%s\n", "Failed to alloc ctx_d,no memory!");
		goto _EXIT;
	}

	//7.从视频流中拷贝解码器参数到解码器上下文中
	ret = avcodec_parameters_to_context(ctx_d, inStream->codecpar);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(ctx_d, AV_LOG_ERROR, "%s\n", err_buf);
		goto _EXIT;
	}

	//8.将解码器和其上下文绑定
	ret = avcodec_open2(ctx_d, codec_d, NULL);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(ctx_d, AV_LOG_ERROR, "%s\n", err_buf);
		goto _EXIT;
	}
	
	//9.根据“视频”的宽/高创建纹理
	texture = SDL_CreateTexture(renderer,
		SDL_PIXELFORMAT_IYUV, // 像素格式
		SDL_TEXTUREACCESS_STREAMING,// 纹理访问模式--pattern
		ctx_d->width,
		ctx_d->height
		);

	//10.从多媒体文件中读取数据，进行解码
	pkt = av_packet_alloc();
	frame = av_frame_alloc();

	decodepar.frame = frame;
	decodepar.texture = texture;
	decodepar.avctx = ctx_d;

	while (av_read_frame(fmtCtx, pkt)>= 0)
	{
		if (pkt->stream_index == idx)
		{
			//11.对解码后的视频帧进行渲染
			decodepar.pkt = pkt;
			decode(decodepar);
		}
		//12.处理SDL事件
		SDL_PollEvent(&event);
		switch (event.type)
		{
		case SDL_QUIT:
			goto _QUIT; // 正常退出
			break;
		default:
			break;
		}
		
		av_packet_unref(pkt); // 重置引用计数
	}
	//显示最后一帧数据
	decodepar.pkt = NULL;
	decode(decodepar);

	//13.释放相应资源
_QUIT:
	ret = 0;

_EXIT:

	if (win)
	{
		SDL_DestroyWindow(win);
		win = nullptr;
	}

	if (renderer)
	{
		SDL_DestroyRenderer(renderer);
		renderer = nullptr;
	}

	if (texture)
	{
		SDL_DestroyTexture(texture);
	}

	if (frame)
	{
		av_frame_free(&frame);
	}

	if (pkt)
	{
		av_packet_free(&pkt);
	}

	if (ctx_d)
	{
		avcodec_free_context(&ctx_d);
	}

	if (fmtCtx)
	{
		avformat_close_input(&fmtCtx);
	}

	SDL_Quit();

	return 0;
}