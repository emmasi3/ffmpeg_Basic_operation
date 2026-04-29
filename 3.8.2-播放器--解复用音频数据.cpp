#define CRT_SECURE_NO_WARNING
#include <iostream>
#include <windows.h>
extern"C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include<libavutil/log.h>
#include <libavutil/fifo.h>
#include <libswresample/swresample.h>
#include <SDL.h>

}

#define AUDIO_BUFFER_SIZE 1024

static SDL_Window* win = nullptr;
static SDL_Renderer* renderer = nullptr;
static int w_width = 640;
static int w_height = 480;

class PacketQueue;
//（解码）参数集合
class _VideoState
{
public:
	_VideoState() {};
	_VideoState(AVCodecContext* avctx, AVPacket* pkt,
		AVFrame* frame, SDL_Texture* texture,AVCodecContext* ctx_a,
		AVPacket* pkt_a,AVFrame* frame_a) :
		ctx_v(avctx), pkt_v(pkt), frame_v(frame), texture(texture),
		ctx_a(ctx_a),pkt_a(pkt_a),frame_a(frame_a), swr_ctx(nullptr),
		audio_buf(nullptr),audio_buf_size(0),audio_buf_index(0),
		audio_queue(nullptr)
	{

	}

	AVCodecContext* ctx_v;
	AVPacket* pkt_v;
	AVFrame* frame_v;

	struct SwrContext* swr_ctx;

	uint8_t* audio_buf; // 暂时存放音频数据
	unsigned int audio_buf_size;// 存放的大小
	int audio_buf_index;// 被音频设备读取到的位置（索引）

	SDL_Texture* texture;

	AVCodecContext* ctx_a;
	AVPacket* pkt_a;
	AVFrame* frame_a;

	PacketQueue* audio_queue; // 不能直接将这个类作为另一个类的成员变量，只能是指针，因为在检测到PacketQueue时，需要找到他的构造函数（定义）但是现在并没有找到，但是它的内存空间是可以分配的，所以传输指针没问题

}decodepar;
//队列类定义（PacketQueue）
class PacketQueue
{
public:
	PacketQueue() :pkts(nullptr), nb_packets(0), duration(0),
		size(0), mutex(nullptr), cond(nullptr) {
	}

	AVFifo* pkts;
	int nb_packets;
	int64_t duration;
	int size;

	SDL_mutex* mutex; // 互斥锁
	SDL_cond* cond; // 条件变量，有唤醒一个
}p_queue;
//中间商（不赚差价、代步工具）
typedef struct MyPacketEle
{
	AVPacket* pkt;
};

static void render(_VideoState& par)
{
	SDL_UpdateYUVTexture(par.texture, NULL,
		par.frame_v->data[0], par.frame_v->linesize[0],
		par.frame_v->data[1], par.frame_v->linesize[1],
		par.frame_v->data[2], par.frame_v->linesize[2]
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
	ret = avcodec_send_packet(par.ctx_v, par.pkt_v);
	if (ret < 0)
	{
		av_log(par.ctx_v, AV_LOG_ERROR, "Failed to send frame to decoder!\n");
		return -1;
	}

	while (ret >= 0)
	{
		ret = avcodec_receive_frame(par.ctx_v, par.frame_v);
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
//队列初始化
static int packet_queue_init(PacketQueue* q)
{
	q->pkts = av_fifo_alloc2(1, sizeof(MyPacketEle), AV_FIFO_FLAG_AUTO_GROW);
	if (!q->pkts)
	{
		return AVERROR(ENOMEM);
	}

	q->mutex = SDL_CreateMutex();
	if (!q->mutex)
	{
		return AVERROR(ENOMEM);
	}

	q->cond = SDL_CreateCond();
	if (!q->cond)
	{
		return AVERROR(ENOMEM);
	}

	return 0;
}
//放入（具体操作）
static int packet_queue_put_priv(PacketQueue* q, AVPacket* pkt)
{
	MyPacketEle mypkt;
	int ret;

	mypkt.pkt = pkt;

	ret = av_fifo_write(q->pkts, &mypkt.pkt, 1); // 这里存储的mypkt的地址，和mypkt的第一个成员的地址一致
	if (ret < 0)
	{
		return ret;
	}
	q->nb_packets++;
	q->size += mypkt.pkt->size + sizeof(mypkt); // 这里为什么要有 两部分？“指针+真实数据”一个都不能少！！！
	q->duration = mypkt.pkt->duration;

	SDL_CondSignal(q->cond);

	return 0;
}
//放入（队列）
static int packet_queue_put(PacketQueue* q, AVPacket* pkt)
{
	AVPacket* pkt1 = nullptr;
	int ret;

	pkt1 = av_packet_alloc();
	if (!pkt1)
	{
		av_packet_unref(pkt);
		return -1;
	}

	av_packet_move_ref(pkt1, pkt);

	SDL_LockMutex(q->mutex);
	//**
	ret = packet_queue_put_priv(q, pkt1);
	//**
	SDL_UnlockMutex(q->mutex);

	if (ret < 0)
	{
		av_packet_free(&pkt1);
	}

	return ret;
}
//获取数据
static int packet_queue_get(PacketQueue* q,AVPacket* pkt,int block)
{
	MyPacketEle mypkt;
	int ret;

	SDL_LockMutex(q->mutex);
	while (1)
	{
		if (av_fifo_read(q->pkts, &mypkt, 1) >= 0)
		{
			q->nb_packets--;
			q->size -= mypkt.pkt->size + sizeof(mypkt);
			q->duration -= mypkt.pkt->duration;
			av_packet_move_ref(pkt, mypkt.pkt); // 这里就是移动语义，高效，窃取指针
			av_packet_free(&mypkt.pkt); // 这个程序最多取一个数据包，所以直接释放整个包
			ret = 1;
			break;
		}
		else if (!block)
		{
			ret = 0;
			break;
		}
		else
		{
			SDL_CondWait(q->cond, q->mutex);
		}
	}
	SDL_UnlockMutex(q->mutex);

	return ret;
}
//清空队列
static void packet_queue_clear(PacketQueue* q)
{
	MyPacketEle mypkt;

	SDL_LockMutex(q->mutex);
	//这样一个一个取出来再销毁的操作浪费时间，除非你用智能指针，否则也只有这样了，当然也可以选择线程，但是线程有更重要的事情去做，性价比不高
	while (av_fifo_read(q->pkts, &mypkt, 1) > 0)
	{
		av_packet_free(&mypkt.pkt);
	}

	q->nb_packets = 0;
	q->size = 0;
	q->duration = 0;

	SDL_UnlockMutex(q->mutex);
}
//销毁队列
static void packet_queue_destroy(PacketQueue* q)
{
	packet_queue_clear(q);
	av_fifo_freep2(&q->pkts);
	SDL_DestroyMutex(q->mutex);
	SDL_DestroyCond(q->cond);

}
//音频回调（解码）函数
static int audio_decode_frame(_VideoState* is)
{
	if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
		printf("Set audio thread priority to TIME_CRITICAL\n");
	}
	else {
		printf("Failed to set audio thread priority: %d\n", GetLastError());
	}
	AVPacket* pkt = nullptr;
	pkt = av_packet_alloc();
	int ret = 0;
	int len2 = 0;
	int data_size = 0;

	while (1)
	{
		if (packet_queue_get(is->audio_queue, pkt, 1) < 0)
		{
			return -1;
		}

		ret = avcodec_send_packet(is->ctx_a, pkt);
		if (ret < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "Failed to send pkt to audio device\n");
			return -1;
		}

		while (ret >= 0)
		{
			ret = avcodec_receive_frame(is->ctx_a, is->frame_a);
			if (ret == AVERROR(EAGAIN)) // 没有足够的数据解码出一帧，或者还没有解码成功，两种可能！！！
			{
				break;
			}
			else if (ret == AVERROR_EOF)
			{
				break;
			}
			else if (ret < 0)
			{
				av_log(NULL, AV_LOG_ERROR, "Failed to recevie frame from audio decoder\n");
				return -1;
			}

			//这里用来决定 swr_ctx 是否被分配内存（看采样格式）
			if (!is->swr_ctx && (is->ctx_a->sample_fmt != AV_SAMPLE_FMT_S16))
			{
				AVChannelLayout in_ch_layout, out_ch_layout;
				av_channel_layout_copy(&in_ch_layout, &is->ctx_a->ch_layout);
				av_channel_layout_copy(&out_ch_layout, &is->ctx_a->ch_layout);

				swr_alloc_set_opts2(&is->swr_ctx,
					&out_ch_layout,
					AV_SAMPLE_FMT_S16,
					is->ctx_a->sample_rate,
					&in_ch_layout,
					is->ctx_a->sample_fmt,
					is->ctx_a->sample_rate,
					0,
					NULL);
				swr_init(is->swr_ctx);
			}

			//音频采样格式不一致时需要“重采样”
			if (is->swr_ctx)
			{
				int out_size = av_samples_get_buffer_size(NULL, is->frame_a->ch_layout.nb_channels, is->frame_a->nb_samples + 512, AV_SAMPLE_FMT_S16, 1);
				av_fast_malloc(&is->audio_buf, &is->audio_buf_size, out_size); // 高效的分配空间的函数

				len2 = swr_convert(is->swr_ctx, 
					(uint8_t **) &is->audio_buf,
					is->frame_a->nb_samples + 512, // 采样个数最多为1.5倍
					(const uint8_t**)is->frame_a->extended_data,
					is->frame_a->nb_samples);
				//计算重采样后的大小（也是函数返回值）
				data_size = len2 * is->frame_a->ch_layout.nb_channels
					* av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
			}
			else // 如果不需要重采样的话，就需要单独给 is->audio_buf 填充数据
			{
				is->audio_buf = is->frame_a->data[0];
				data_size = av_samples_get_buffer_size(NULL,
					is->ctx_a->ch_layout.nb_channels,
					is->frame_a->nb_samples,
					AV_SAMPLE_FMT_S16,
					1);// 1 是不对齐
			}
			if (pkt)
			{
				printf("pkt is valid: %p\n", pkt);
				av_packet_unref(pkt);
			}
			av_frame_unref(is->frame_a);

			return data_size;
		}//循环2

	}//循环1

}
//音频回调函数
static void sdl_audio_callback(void* userdata, uint8_t* stream, int len)
{

	_VideoState* is = (_VideoState*)userdata;

	int audio_size = 0;

	int len1 = 0;

	while (len > 0)
	{
		//audio_buf中已经没有东西可读了
		if (is->audio_buf_index >= is->audio_buf_size)
		{
			//那么再次解码（返回解码数）
			audio_size = audio_decode_frame(is);
			if (audio_size < 0)
			{
				is->audio_buf_size = AUDIO_BUFFER_SIZE;
				is->audio_buf = nullptr;
			}
			else
			{
				is->audio_buf_size = audio_size; // 再次设置音频数据的大小
			}

			is->audio_buf_index = 0; // 重置索引为0
		}
		//如果还有数据，计算（还有多少没读，并给出能读的大小）
		len1 = is->audio_buf_size - is->audio_buf_index;
		if (len1 > len)
		{
			len1 = len;
		}

		if (is->audio_buf)
		{
			memcpy(stream, is->audio_buf + is->audio_buf_index, len1);
		}
		else // 给静音
		{
			memset(stream, 0, len1);
		}

		len -= len1;
		stream += len1;
		is->audio_buf_index += len1;
	}

}

int main(int argc, char* argv[])
{
	av_log_set_level(AV_LOG_DEBUG);
	char err_buf[AV_ERROR_MAX_STRING_SIZE];

	int ret = -1;
	char* src = nullptr;
	int idx_v = -1, idx_a = -1;//找到视频、音频流的索引
	int idx_video, idx_audio;

	AVFormatContext* fmtCtx = nullptr;
	const AVCodec* codec_d_v = nullptr;
	const AVCodec* codec_d_a = nullptr;

	AVCodecContext* ctx_d_v = nullptr;
	AVCodecContext* ctx_d_a = nullptr;

	AVPacket* pkt_v = nullptr;
	AVFrame* frame_v = nullptr;
	AVPacket* pkt_a = nullptr;
	AVFrame* frame_a = nullptr;
	AVPacket* pkt = nullptr;

	AVStream* instream_v = nullptr;
	AVStream* instream_a = nullptr;

	SDL_Texture* texture = nullptr;
	SDL_Event event;

	SDL_AudioSpec* spec = nullptr, *SPEC = nullptr;

	//1.判断输入参数
	if (argc < 2)
	{
		av_log(NULL, AV_LOG_ERROR, "the arguments is must more than 1\n");
		return ret;
	}

	src = argv[1];

	//2.初始化SDL，并创建窗口和Render
	ret = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
	if (ret != 0)
	{
		SDL_Log("Failed to initialize SDL--video!\n");
		return -1;
	}

	win = SDL_CreateWindow("Sample Player",
		SDL_WINDOWPOS_UNDEFINED,
		SDL_WINDOWPOS_UNDEFINED,
		w_width, w_height,
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

	//4.查找最好的（目标）视频流、音频流，不一定是最好的
	for (int i = 0; i < fmtCtx->nb_streams; i++)
	{
		if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO
			&& idx_v < 0)
		{
			idx_v = 0;
			idx_video = i;
		}
		else if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO
			&& idx_a < 0)
		{
			idx_a = 0;
			idx_audio = i;
		}

		if (!idx_a && !idx_v) // 如果都找到了，退出
		{
			break;
		}
	}

	if (idx_v == -1)
	{
		av_log(NULL, AV_LOG_ERROR, "could not find video stream!\n");
		goto _EXIT;
	}

	if (idx_a == -1)
	{
		av_log(NULL, AV_LOG_ERROR, "could not find audio stream!\n");
		goto _EXIT;
	}

	instream_v = fmtCtx->streams[idx_video];
	instream_a = fmtCtx->streams[idx_audio];

	//5.根据codec_id，获得解码器
	
	codec_d_v = avcodec_find_decoder(instream_v->codecpar->codec_id);
	if (!codec_d_v)
	{
		av_log(NULL, AV_LOG_ERROR, "%s\n", "Could not find video codec!");
		goto _EXIT;
	}

	codec_d_a = avcodec_find_decoder(instream_a->codecpar->codec_id);
	if (!codec_d_a)
	{
		av_log(NULL, AV_LOG_ERROR, "%s\n", "Could not find audio codec!");
		goto _EXIT;
	}

	//6.创建视频解码器上下文
	ctx_d_v = avcodec_alloc_context3(codec_d_v);
	if (!ctx_d_v)
	{
		av_log(NULL, AV_LOG_ERROR, "%s\n", "Failed to alloc ctx_d_v,no memory!");
		goto _EXIT;
	}

	//7.从视频流中拷贝解码器参数到解码器上下文中
	ret = avcodec_parameters_to_context(ctx_d_v, instream_v->codecpar);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(ctx_d_v, AV_LOG_ERROR, "%s\n", err_buf);
		goto _EXIT;
	}

	//8.将解码器和其上下文绑定
	ret = avcodec_open2(ctx_d_v, codec_d_v, NULL);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(ctx_d_v, AV_LOG_ERROR, "%s\n", err_buf);
		goto _EXIT;
	}
	///////////////////////////////////////

	//666.创建视频解码器上下文
	ctx_d_a = avcodec_alloc_context3(codec_d_a);
	if (!ctx_d_a)
	{
		av_log(NULL, AV_LOG_ERROR, "%s\n", "Failed to alloc ctx_d_v,no memory!");
		goto _EXIT;
	}

	//777.从视频流中拷贝解码器参数到解码器上下文中
	ret = avcodec_parameters_to_context(ctx_d_a, instream_a->codecpar);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(ctx_d_a, AV_LOG_ERROR, "%s\n", err_buf);
		goto _EXIT;
	}

	//888.将（音频）解码器和其上下文绑定
	ret = avcodec_open2(ctx_d_a, codec_d_a, NULL);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(ctx_d_a, AV_LOG_ERROR, "%s\n", err_buf);
		goto _EXIT;
	}

	///////////////////////////////////////

	//9.根据“视频”的宽/高创建纹理
	texture = SDL_CreateTexture(renderer,
		SDL_PIXELFORMAT_IYUV, // 像素格式
		SDL_TEXTUREACCESS_STREAMING,// 纹理访问模式--pattern
		ctx_d_v->width,
		ctx_d_v->height
	);

	//10.从多媒体文件中读取数据，进行解码
	pkt = av_packet_alloc();

	pkt_v = av_packet_alloc();
	frame_v = av_frame_alloc();

	pkt_a = av_packet_alloc();
	frame_a = av_frame_alloc();

	//11.给类的成员初始化
	decodepar = _VideoState(ctx_d_v, pkt_v, frame_v, texture,
		ctx_d_a, pkt_a, frame_a);

	//初始化（音频）队列
	decodepar.audio_queue = &p_queue;
	packet_queue_init(decodepar.audio_queue);

	spec = (SDL_AudioSpec*)malloc(sizeof(SDL_AudioSpec));
	SPEC = (SDL_AudioSpec*)malloc(sizeof(SDL_AudioSpec));
	if (!spec)
	{
		SDL_Log("Failed to alloc to SDL_AudioSpec* spec!\n");
		goto _EXIT;
	}
	spec->freq = ctx_d_a->sample_rate;
	spec->format = AUDIO_S16SYS; // 16位的有符号整型
	spec->channels = ctx_d_a->ch_layout.nb_channels;
	spec->samples = 1024; // 采样大小
	spec->callback = sdl_audio_callback;
	spec->userdata = (void*)(&decodepar);
	spec->silence = 0; //有介绍

	//音频设备初始化
	ret = SDL_OpenAudio(spec, SPEC); //这个函数有讲解：
	std::cout << ret << std::endl;
	printf("看这儿看这儿！！！spec->freq:%d,%d,%d", SPEC->freq, SPEC->format, SPEC->channels);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to open --- SDL_OpenAudio() function!\n");
		goto _EXIT;
	}

	SDL_PauseAudio(0); // 立即开始播放（从队列中取数据）

	while (av_read_frame(fmtCtx, pkt) >= 0)
	{
		if (pkt->stream_index == idx_video)
		{
			//12.对解码后的视频帧进行渲染
			decodepar.pkt_v = pkt;
			ret = decode(decodepar);
			if (ret < 0)
			{
				return -1;
			}
		}
		else if (pkt->stream_index == idx_audio)
		{
			packet_queue_put(decodepar.audio_queue, pkt);
		}
		//13.处理SDL事件
		SDL_PollEvent(&event);
		switch (event.type)
		{
		case SDL_QUIT:
			goto _QUIT; // 正常退出
			break;
		default:
			break;
		}

		if (pkt)
		{
			av_packet_unref(pkt);
		}
	}
	//显示最后一帧数据
	decodepar.pkt_v = nullptr;
	decode(decodepar);

	//14.释放相应资源
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

	if (frame_v)
	{
		av_frame_free(&frame_v);
	}

	if (frame_a)
	{
		av_frame_free(&frame_a);
	}

	if (pkt)
	{
		av_packet_free(&pkt);
	}

	if (pkt_v)
	{
		av_packet_free(&pkt_v);
	}

	if (pkt_a)
	{
		av_packet_free(&pkt_a);
	}

	if (ctx_d_v)
	{
		avcodec_free_context(&ctx_d_v);
	}

	if (ctx_d_a)
	{
		avcodec_free_context(&ctx_d_a);
	}

	if (fmtCtx)
	{
		avformat_close_input(&fmtCtx);
	}

	SDL_Quit();
	
	return 0;
}
/*
* （1）这里的类成员变量是指针是吧，为什么在前面初始化完成后，也就是刚刚将pkt、frame、ctx初始化为nullptr,然后用他们来初始化成员变量，
* 那么后续对主函数中的各个变量做出的修改是否会更新到类的成员变量上面去？
* 不会，因为同一级指针赋值，其实储存的是对方指向区域的地址，并且这个地址不能变（变了的话，操作的就不是同一片内存地址了），
* 但是现在的关键是：他们指向 nullptr，是空的，也就是说，后续的赋值中，操作的，绝对不是同一块内存地址，也就不会更新了，所以
* ：初始化的时机应该是，主函数中的各个指针，指向一个确定地址之后，给到成员变量才可以，懂了吗？
* 是否会更新的关键是：：：操作的是不是同一片内存地址？懂？
* 
* （2）silence 这个成员变量，意思是，当音频缓冲区没有数据时，会自动填充该变量的值，大多数时候需要的是“静音”
* 并且，这个值好像可以自动推导，不需要手动设置，但是对于 16位深或以上的数据，0就是静音，所以有时候手动设置比较合理
* 
* （3）SDL_OpenAudio()的第二个参数，会返回实际的设置参数，也就是说，咱们传进去的 spec的各个参数，可能会和实际音频设备的要求不一致，那么函数就会自动修改
* spec中的字段来保证音频设备的正确运行，并且会将修改后的、能够使得设备正常打开的字段信息，全部设置进 第二个参数中，也就是获取了实际参数
* 当然，如果你不关心 被修改之后的参数详细信息是什么，就直接 NULL 就好了
* 
* （4）AVERROR(EAGAIN) 接受解码数据时，返回这样的字段，有两种可能：1、通过send送往解码器的数据不够解码为“一帧”
*															   2、还没有解码完成
* 一般来说，解码音频时间在 0.1 --- 3ms之间，所以调试的时候，根本就不需要担心解码不完，一旦用户能够察觉到解码慢了，那一定是出问题了
* 
* （5）
* 
*/