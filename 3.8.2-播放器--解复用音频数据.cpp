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
//�����룩��������
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

	uint8_t* audio_buf; // ��ʱ�����Ƶ����
	unsigned int audio_buf_size;// ��ŵĴ�С
	int audio_buf_index;// ����Ƶ�豸��ȡ����λ�ã�������

	SDL_Texture* texture;

	AVCodecContext* ctx_a;
	AVPacket* pkt_a;
	AVFrame* frame_a;

	PacketQueue* audio_queue; // ����ֱ�ӽ��������Ϊ��һ����ĳ�Ա������ֻ����ָ�룬��Ϊ�ڼ�⵽PacketQueueʱ����Ҫ�ҵ����Ĺ��캯�������壩�������ڲ�û���ҵ������������ڴ�ռ��ǿ��Է���ģ����Դ���ָ��û����

}decodepar;
//�����ඨ�壨PacketQueue��
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

	SDL_mutex* mutex; // ������
	SDL_cond* cond; // �����������л���һ��
}p_queue;
//�м��̣���׬��ۡ��������ߣ�
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
	//�á���ͼ��ɫ�������ǰ��ȾĿ�꣬û����Ĭ��Ϊ�ڴ���
	SDL_RenderClear(renderer);
	//�͵�GPU�����㣩
	SDL_RenderCopy(renderer, par.texture, NULL, NULL);
	//ˢ�´��� ---- ��ʾһ֡ͼ��
	SDL_RenderPresent(renderer);
}

static int decode(_VideoState& par)
{
	int ret = -1;
	char buf[1024];

	//��ȥ����
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
			return -1; // �˳�
		}

		//��Ⱦ
		render(par);
	}

}
//���г�ʼ��
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
//���루���������
static int packet_queue_put_priv(PacketQueue* q, AVPacket* pkt)
{
	MyPacketEle mypkt;
	int ret;

	mypkt.pkt = pkt;

	ret = av_fifo_write(q->pkts, &mypkt.pkt, 1); // ����洢��mypkt�ĵ�ַ����mypkt�ĵ�һ����Ա�ĵ�ַһ��
	if (ret < 0)
	{
		return ret;
	}
	q->nb_packets++;
	q->size += mypkt.pkt->size + sizeof(mypkt); // ����ΪʲôҪ�� �����֣���ָ��+��ʵ���ݡ�һ���������٣�����
	q->duration = mypkt.pkt->duration;

	SDL_CondSignal(q->cond);

	return 0;
}
//���루���У�
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
//��ȡ����
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
			av_packet_move_ref(pkt, mypkt.pkt); // ��������ƶ����壬��Ч����ȡָ��
			av_packet_free(&mypkt.pkt); // ����������ȡһ�����ݰ�������ֱ���ͷ�������
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
//��ն���
static void packet_queue_clear(PacketQueue* q)
{
	MyPacketEle mypkt;

	SDL_LockMutex(q->mutex);
	//����һ��һ��ȡ���������ٵĲ����˷�ʱ�䣬������������ָ�룬����Ҳֻ�������ˣ���ȻҲ����ѡ���̣߳������߳��и���Ҫ������ȥ�����Լ۱Ȳ���
	while (av_fifo_read(q->pkts, &mypkt, 1) > 0)
	{
		av_packet_free(&mypkt.pkt);
	}

	q->nb_packets = 0;
	q->size = 0;
	q->duration = 0;

	SDL_UnlockMutex(q->mutex);
}
//���ٶ���
static void packet_queue_destroy(PacketQueue* q)
{
	packet_queue_clear(q);
	av_fifo_freep2(&q->pkts);
	SDL_DestroyMutex(q->mutex);
	SDL_DestroyCond(q->cond);

}
//��Ƶ�ص������룩����
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
			if (ret == AVERROR(EAGAIN)) // û���㹻�����ݽ����һ֡�����߻�û�н���ɹ������ֿ��ܣ�����
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

			//������������ swr_ctx �Ƿ񱻷����ڴ棨��������ʽ��
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

			//��Ƶ������ʽ��һ��ʱ��Ҫ���ز�����
			if (is->swr_ctx)
			{
				int out_size = av_samples_get_buffer_size(NULL, is->frame_a->ch_layout.nb_channels, is->frame_a->nb_samples + 512, AV_SAMPLE_FMT_S16, 1);
				av_fast_malloc(&is->audio_buf, &is->audio_buf_size, out_size); // ��Ч�ķ���ռ�ĺ���

				len2 = swr_convert(is->swr_ctx, 
					(uint8_t **) &is->audio_buf,
					is->frame_a->nb_samples + 512, // �����������Ϊ1.5��
					(const uint8_t**)is->frame_a->extended_data,
					is->frame_a->nb_samples);
				//�����ز�����Ĵ�С��Ҳ�Ǻ�������ֵ��
				data_size = len2 * is->frame_a->ch_layout.nb_channels
					* av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
			}
			else // �������Ҫ�ز����Ļ�������Ҫ������ is->audio_buf �������
			{
				is->audio_buf = is->frame_a->data[0];
				data_size = av_samples_get_buffer_size(NULL,
					is->ctx_a->ch_layout.nb_channels,
					is->frame_a->nb_samples,
					AV_SAMPLE_FMT_S16,
					1);// 1 �ǲ�����
			}
			if (pkt)
			{
				printf("pkt is valid: %p\n", pkt);
				av_packet_unref(pkt);
			}
			av_frame_unref(is->frame_a);

			return data_size;
		}//ѭ��2

	}//ѭ��1

}
//��Ƶ�ص�����
static void sdl_audio_callback(void* userdata, uint8_t* stream, int len)
{

	_VideoState* is = (_VideoState*)userdata;

	int audio_size = 0;

	int len1 = 0;

	while (len > 0)
	{
		//audio_buf���Ѿ�û�ж����ɶ���
		if (is->audio_buf_index >= is->audio_buf_size)
		{
			//��ô�ٴν��루���ؽ�������
			audio_size = audio_decode_frame(is);
			if (audio_size < 0)
			{
				is->audio_buf_size = AUDIO_BUFFER_SIZE;
				is->audio_buf = nullptr;
			}
			else
			{
				is->audio_buf_size = audio_size; // �ٴ�������Ƶ���ݵĴ�С
			}

			is->audio_buf_index = 0; // ��������Ϊ0
		}
		//����������ݣ����㣨���ж���û�����������ܶ��Ĵ�С��
		len1 = is->audio_buf_size - is->audio_buf_index;
		if (len1 > len)
		{
			len1 = len;
		}

		if (is->audio_buf)
		{
			memcpy(stream, is->audio_buf + is->audio_buf_index, len1);
		}
		else // ������
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
	int idx_v = -1, idx_a = -1;//�ҵ���Ƶ����Ƶ��������
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

	//1.�ж��������
	if (argc < 2)
	{
		av_log(NULL, AV_LOG_ERROR, "the arguments is must more than 1\n");
		return ret;
	}

	src = argv[1];

	//2.��ʼ��SDL�����������ں�Render
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

	//3.�򿪶�ý���ļ�����ȡ����Ϣ
	ret = avformat_open_input(&fmtCtx, src, NULL, NULL);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(NULL, AV_LOG_ERROR, "%s\n", err_buf);
		goto _EXIT;
	}

	ret = avformat_find_stream_info(fmtCtx, NULL); // ��������ܹ���ȡ����Ϣ�������� fmtCtx->streams ��
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(NULL, AV_LOG_ERROR, "%s\n", err_buf);
		goto _EXIT;
	}

	//4.������õģ�Ŀ�꣩��Ƶ������Ƶ������һ������õ�
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

		if (!idx_a && !idx_v) // ������ҵ��ˣ��˳�
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

	//5.����codec_id����ý�����
	
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

	//6.������Ƶ������������
	ctx_d_v = avcodec_alloc_context3(codec_d_v);
	if (!ctx_d_v)
	{
		av_log(NULL, AV_LOG_ERROR, "%s\n", "Failed to alloc ctx_d_v,no memory!");
		goto _EXIT;
	}

	//7.����Ƶ���п�����������������������������
	ret = avcodec_parameters_to_context(ctx_d_v, instream_v->codecpar);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(ctx_d_v, AV_LOG_ERROR, "%s\n", err_buf);
		goto _EXIT;
	}

	//8.�����������������İ�
	ret = avcodec_open2(ctx_d_v, codec_d_v, NULL);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(ctx_d_v, AV_LOG_ERROR, "%s\n", err_buf);
		goto _EXIT;
	}
	///////////////////////////////////////

	//666.������Ƶ������������
	ctx_d_a = avcodec_alloc_context3(codec_d_a);
	if (!ctx_d_a)
	{
		av_log(NULL, AV_LOG_ERROR, "%s\n", "Failed to alloc ctx_d_v,no memory!");
		goto _EXIT;
	}

	//777.����Ƶ���п�����������������������������
	ret = avcodec_parameters_to_context(ctx_d_a, instream_a->codecpar);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(ctx_d_a, AV_LOG_ERROR, "%s\n", err_buf);
		goto _EXIT;
	}

	//888.������Ƶ�����������������İ�
	ret = avcodec_open2(ctx_d_a, codec_d_a, NULL);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(ctx_d_a, AV_LOG_ERROR, "%s\n", err_buf);
		goto _EXIT;
	}

	///////////////////////////////////////

	//9.���ݡ���Ƶ���Ŀ�/�ߴ�������
	texture = SDL_CreateTexture(renderer,
		SDL_PIXELFORMAT_IYUV, // ���ظ�ʽ
		SDL_TEXTUREACCESS_STREAMING,// �������ģʽ--pattern
		ctx_d_v->width,
		ctx_d_v->height
	);

	//10.�Ӷ�ý���ļ��ж�ȡ���ݣ����н���
	pkt = av_packet_alloc();

	pkt_v = av_packet_alloc();
	frame_v = av_frame_alloc();

	pkt_a = av_packet_alloc();
	frame_a = av_frame_alloc();

	//11.����ĳ�Ա��ʼ��
	decodepar = _VideoState(ctx_d_v, pkt_v, frame_v, texture,
		ctx_d_a, pkt_a, frame_a);

	//��ʼ������Ƶ������
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
	spec->format = AUDIO_S16SYS; // 16λ���з�������
	spec->channels = ctx_d_a->ch_layout.nb_channels;
	spec->samples = 1024; // ������С
	spec->callback = sdl_audio_callback;
	spec->userdata = (void*)(&decodepar);
	spec->silence = 0; //�н���

	//��Ƶ�豸��ʼ��
	ret = SDL_OpenAudio(spec, SPEC); //��������н��⣺
	std::cout << ret << std::endl;
	printf("����������������spec->freq:%d,%d,%d", SPEC->freq, SPEC->format, SPEC->channels);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to open --- SDL_OpenAudio() function!\n");
		goto _EXIT;
	}

	SDL_PauseAudio(0); // ������ʼ���ţ��Ӷ�����ȡ���ݣ�

	while (av_read_frame(fmtCtx, pkt) >= 0)
	{
		if (pkt->stream_index == idx_video)
		{
			//12.�Խ�������Ƶ֡������Ⱦ
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
		//13.����SDL�¼�
		SDL_PollEvent(&event);
		switch (event.type)
		{
		case SDL_QUIT:
			goto _QUIT; // �����˳�
			break;
		default:
			break;
		}

		if (pkt)
		{
			av_packet_unref(pkt);
		}
	}
	//��ʾ���һ֡����
	decodepar.pkt_v = nullptr;
	decode(decodepar);

	//14.�ͷ���Ӧ��Դ
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
* ��1����������Ա������ָ���ǰɣ�Ϊʲô��ǰ���ʼ����ɺ�Ҳ���Ǹոս�pkt��frame��ctx��ʼ��Ϊnullptr,Ȼ������������ʼ����Ա������
* ��ô�������������еĸ��������������޸��Ƿ����µ���ĳ�Ա��������ȥ��
* ���ᣬ��Ϊͬһ��ָ�븳ֵ����ʵ������ǶԷ�ָ������ĵ�ַ�����������ַ���ܱ䣨���˵Ļ��������ľͲ���ͬһƬ�ڴ��ַ�ˣ���
* �������ڵĹؼ��ǣ�����ָ�� nullptr���ǿյģ�Ҳ����˵�������ĸ�ֵ�У������ģ����Բ���ͬһ���ڴ��ַ��Ҳ�Ͳ�������ˣ�����
* ����ʼ����ʱ��Ӧ���ǣ��������еĸ���ָ�룬ָ��һ��ȷ����ַ֮�󣬸�����Ա�����ſ��ԣ�������
* �Ƿ����µĹؼ��ǣ������������ǲ���ͬһƬ�ڴ��ַ������
* 
* ��2��silence �����Ա��������˼�ǣ�����Ƶ������û������ʱ�����Զ����ñ�����ֵ�������ʱ����Ҫ���ǡ������
* ���ң����ֵ��������Զ��Ƶ�������Ҫ�ֶ����ã����Ƕ��� 16λ������ϵ����ݣ�0���Ǿ����������ʱ���ֶ����ñȽϺ���
* 
* ��3��SDL_OpenAudio()�ĵڶ����������᷵��ʵ�ʵ����ò�����Ҳ����˵�����Ǵ���ȥ�� spec�ĸ������������ܻ��ʵ����Ƶ�豸��Ҫ��һ�£���ô�����ͻ��Զ��޸�
* spec�е��ֶ�����֤��Ƶ�豸����ȷ���У����һὫ�޸ĺ�ġ��ܹ�ʹ���豸�����򿪵��ֶ���Ϣ��ȫ�����ý� �ڶ��������У�Ҳ���ǻ�ȡ��ʵ�ʲ���
* ��Ȼ������㲻���� ���޸�֮��Ĳ�����ϸ��Ϣ��ʲô����ֱ�� NULL �ͺ���
* 
* ��4��AVERROR(EAGAIN) ���ܽ�������ʱ�������������ֶΣ������ֿ��ܣ�1��ͨ��send���������������ݲ�������Ϊ��һ֡��
*															   2����û�н������
* һ����˵��������Ƶʱ���� 0.1 --- 3ms֮�䣬���Ե��Ե�ʱ�򣬸����Ͳ���Ҫ���Ľ��벻�꣬һ���û��ܹ�������������ˣ���һ���ǳ�������
* 
* ��5��
* 
*/