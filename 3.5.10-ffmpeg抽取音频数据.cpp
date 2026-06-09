#include <iostream>
extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavutil/avutil.h>

}

int main(int argc, char* argv[]) // �����д������������������
{
	std::cout << "����ʼ�ˣ�" << std::endl;
	int ret = 0;
	// src ����Ƶ��ID
	int idx = 0;

	// 1. ����һЩ����
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

	// 2. �򿪶�ý���ļ�
	ret = avformat_open_input(&pFmtCtx, src, NULL, NULL);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(NULL, AV_LOG_ERROR, "%s\n", err_buf);
		memset(err_buf, 0, sizeof(err_buf));

		goto _ERROR;
	}

	// 3. �� src �ļ����ҵ���Ƶ�� Id
	idx = av_find_best_stream(pFmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (idx < 0)
	{
		av_strerror(idx, err_buf, sizeof(err_buf));
		av_log(pFmtCtx, AV_LOG_ERROR, "%s\n", err_buf);
		goto _ERROR;
	}

	// 4. ��Ŀ���ļ���������
	oFmtCtx = avformat_alloc_context();
	if (!oFmtCtx)
	{
		av_log(oFmtCtx, AV_LOG_ERROR, "no memory!\n");
		goto _ERROR;
	}
	//���������ʽ
	outFmt = av_guess_format(NULL, dst, NULL);
	oFmtCtx->oformat = outFmt;

	// 5. ΪĿ���ļ�������һ���µ���Ƶ��
	outStream = avformat_new_stream(oFmtCtx, NULL);

	//��
	ret = avio_open2(&oFmtCtx->pb, dst, AVIO_FLAG_WRITE, NULL, NULL);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(oFmtCtx, AV_LOG_ERROR, "%s\n", err_buf);
		memset(err_buf, 0, sizeof(err_buf));

		goto _ERROR;
	}

	// 6. ���������Ƶ����
	inStream = pFmtCtx->streams[idx];
	avcodec_parameters_copy(outStream->codecpar, inStream->codecpar); // �м����--������ ����������˼
	outStream->codecpar->codec_tag = 0; // ����Ϊ 0 ���Զ�ʶ���ý���ļ��ĸ�ʽ��ȷ����װ�������ͣ�����㹻��Ϥ����������ΪĿ��ֵ

	// 7. д��ý���ļ�ͷ��Ŀ���ļ�
	ret = avformat_write_header(oFmtCtx, NULL);
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(oFmtCtx, AV_LOG_ERROR, "%s\n", err_buf);
		memset(err_buf, 0, sizeof(err_buf));

		goto _ERROR;
	}

	// 8. ��Դ��ý���ļ���ȡ��Ƶ���ݵ�Ŀ���ļ���
	while (av_read_frame(pFmtCtx, &pkt) >= 0)
	{
		if (pkt.stream_index == idx) // �ж϶�ȡ���ǲ�����Ƶ�����ݣ��е�ʱ���ǣ���Ϊ�޸ģ����ݣ�����Ƶ�ļ���ʱ�򣬸�ʽת���������⣬������Ƶһ�����Ƶ��ϴ��ݣ��õİ��е�ʱ����ͬһ�������߳�
		{
			// ת��pkt�ĸ���ʱ�������ֶ�
			av_packet_rescale_ts(&pkt, inStream->time_base, outStream->time_base);
			// ��ȡ�������ݰ���һ��������FFmpeg����֪������ video��audio ��һ�������������Ҫ��������
			pkt.stream_index = outStream->index;
			// д���ļ� IO
			ret = av_interleaved_write_frame(oFmtCtx, &pkt); // ��һ���Ǻ���д�������ͨ��oFmtCtx��Ϊý���м��壬������д�� dst ָ����ļ�
			if (ret < 0)
			{
				av_packet_unref(&pkt);
				break;
			}
		}

		// �ͷž���Դ
		av_packet_unref(&pkt);
	}

	// 9. д��ý���ļ�β���ļ���
	av_write_trailer(oFmtCtx);

	// 10. ���������Դ�ͷ�
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
1.packet buffer -- oFmtCtx->pb ����д

2.(AVRounding)(AV_ROUND_PASS_MINMAX | AV_ROUND_UP) ���ﱾ����Ӧ����ǿ������ת���ģ����Ǹ������У�Ӧ�û��Զ�������������ڲ�ͬ�ı��������������ȣ�
Ҳ�����������Ľ����ǿ��ת�����㲻��������������ڲ�Ҳ������ֻ���������ĸ�������һ��

3.pkt.dts = pkt.pts; // �����ݰ�Ӧ�ñ������롱�����ʱ��㣨ʱ�����һ����ʱ������
	����� pts ����ʾʱ�����Ҳ���Ǹ����ݰ�Ӧ�ñ����ֵ�ʱ����ʱ��㣩

4.ʱ������Ǳ�׼��timebase = {1,1000},���ǽ�1s ����1000�� �ֿ�������
*/