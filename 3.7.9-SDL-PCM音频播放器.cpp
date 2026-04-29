#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
extern "C"
{
#include<SDL.h>

}

#define BLOCK_SIZE 4096000
static size_t buffer_len = 0;
static uint8_t* audio_buf = nullptr; //全局可用
static uint8_t* audio_pos = nullptr;


static void read_audio_data(void *udata,uint8_t *stream,int len)
{
	if (buffer_len == 0)
	{
		return;
	}

	//clean data guoqu
	SDL_memset(stream, 0, len);

	len = (len < buffer_len) ? len : buffer_len;
	// 拷贝音频数据到声卡的缓冲区中
	SDL_MixAudio(stream, audio_pos, len, SDL_MIX_MAXVOLUME);

	audio_pos += len;
	buffer_len -= len;
}


int main(int argc,char* argv[])
{
	int ret = -1;
	char* path = nullptr;
	FILE* audio_fd = nullptr;
	size_t buffer_len2 = 0; // 记录每一次读取的数据总大小（因为全局的 buffer_len 在回调函数中会被改变，所以记录一下，方便使用）
	double wait_time_ms = 0;


	SDL_AudioSpec spec;

	if (SDL_Init(SDL_INIT_AUDIO) < 0)
	{
		SDL_Log("Failed to initialize the SDL!\n");
		return ret;
	}

	path = argv[1];
	audio_fd = fopen(path, "rb");
	if (!audio_fd)
	{
		SDL_Log("Failed to open the pcm file: %s\n", path);
		goto _EXIT;
	}

	//储存 PCM 数据
	audio_buf = (uint8_t*)malloc(BLOCK_SIZE * sizeof(uint8_t));
	if (!audio_buf)
	{
		SDL_Log("Failed to alloc memory!\n");
		goto _EXIT;
	}

	//1.打开音频设备
	spec.freq = 44100;
	spec.channels = 2;
	spec.format = AUDIO_S16SYS;
	spec.callback = read_audio_data; // 设置回调函数（音频设备触发）
	spec.userdata = NULL;

	if (SDL_OpenAudio(&spec, NULL) != 0)
	{
		SDL_Log("Failed to open the Audio device!\n");
		goto _EXIT;
	}

	SDL_PauseAudio(0); // 开始播放音频

	do
	{
		buffer_len = fread(audio_buf, 1, BLOCK_SIZE, audio_fd);

		if (buffer_len == 0)
		{
			break;
		}

		audio_pos = audio_buf;
		buffer_len2 = buffer_len;
		while (audio_pos < (audio_buf + buffer_len2))
		{
			SDL_Delay(1);
		}

	} while (true);

	//计算最后一个包送完后应该等待播放完毕的时间
	//wait_time_ms = (double)BLOCK_SIZE / (spec.freq * 2 * spec.channels) * 1000;
	//SDL_Delay((int)wait_time_ms);

	SDL_CloseAudio(); // 关闭音频设备
	ret = 0;
_EXIT:

	SDL_Quit();

	if (audio_fd)
	{
		fclose(audio_fd);
		audio_fd = nullptr;
	}
	
	if (audio_buf)
	{
		free(audio_buf);
		audio_buf = nullptr;
	}

	return ret;
}
/*
* （1）这里你明白一点：就是 audio_buf 内的数据量大多数情况下，是远远大于声卡缓冲区的
* 如果你去调试这个回调函数，你就会发现，函数执行一次只有21万字节的数据被拷贝到缓冲区，还剩下20多倍的数据没有拷贝完，所以
* 并不是 audio_buf 整个包一次性被拷贝进声卡缓冲区，而是一段一段来，这个时间间隔是比较短的，所以播放起来是流畅的
* 但是：：：fread()这个操作时，播放器正在播放上一个 audio_buf 中的最后一点数据（大概20万字节），不到 0.5s 就播放完了，但是fread()却要在这个时间内
* 读取 4096000，也就是 4MB 大小的数据，这是非常耗时的，也就是0.5s过后，播放器没有声音输出的空白时间过长，人耳可以很明显地察觉到，这也就是问题所在
* 
* 现在提供我的思路：
* 1、既然本质是 fread() 读取时间过长，那咱们就一次性读完，将所有的pcm数据储存在一个 audio_buf 里，具体操作：读取pcm的大小，直接创建一个对应的
* 大小对应的 audio_buf 来一次性储存
* 2、创建至少两个 audio_buf 来储存pcm数据的数组，开子线程，异步往没被读取的数组中填充数据，等到某一个数组被处理完，另一个就衔接上，避开了fread()执行的
* 时间，这个方法也可以不开线程，毕竟是本地播放
* 3、如果是网络在线播放并且pcm播放器在本地的话，就采用第二种方法，一直读取，用到的时候直接顶上去，防止卡顿
* 
*/