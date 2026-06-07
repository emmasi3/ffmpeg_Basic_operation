#define _CRT_SECURE_NO_WARNINGS
#define SDL_MAIN_HANDLED
#include <iostream>
#include<memory>
#include <atomic>
extern"C"
{
#include <SDL.h>

}
//event message
#define REFRESH_EVENT (SDL_USEREVENT + 1)
#define QUIT_EVENT (SDL_USEREVENT +2)

//bool thread_exit = false;
std::atomic<bool> thread_exit = false;

int refresh_video_timer(void* udata)
{
	thread_exit = false;

	while (!thread_exit)
	{
		SDL_Event event;
		event.type = REFRESH_EVENT;
		SDL_PushEvent(&event);
		SDL_Delay(33); // 间隔33ms触发一次刷新事件
	}

	//push quit evnet
	SDL_Event event;
	event.type = QUIT_EVENT;
	SDL_PushEvent(&event); // 添加事件到事件队列queue

	return 0;
}

int main(int argc,char* argv[])
{
	FILE* video_fd = nullptr;

	SDL_Rect rect;
	SDL_Event event;

	uint32_t pixformat = 0;

	SDL_Window* win = nullptr;
	SDL_Renderer* renderer = nullptr;
	SDL_Texture* texture = nullptr;

	SDL_Thread* timer_thread = nullptr;

	//int w_width = 544, w_height = 960;
	int w_width = 544, w_height = 960;
	const int video_width = 544, video_height = 960;

	uint8_t* video_pos = nullptr, * video_end = nullptr;

	unsigned int remain_len = 0;
	size_t video_buff_len = 0;
	size_t blank_space_len = 0;
	uint8_t* video_buf = nullptr;
	//std::unique_ptr<uint8_t[]> video_buf = nullptr;

	const char* path = argv[1];

	//一帧的数据量（字节数）
	const unsigned int yuv_frame_len =
		video_width * video_height * 12 / 8; // 这里为什么要 *1.5，而不是3？ 因为YUV420p，描述一个像素点所占用的内存是 1.5字节，也就是y-1，u+v-0.5

	unsigned int tmp_yuv_frame_len = yuv_frame_len;

	//视频里说的是对齐,但是其实我感觉没多大影响，测试过
	if (yuv_frame_len & 0xF)
	{
		tmp_yuv_frame_len = (yuv_frame_len & 0xFFF0) + 0X10;
	}

	//initialize SDL 初始化SDL
	if (SDL_Init(SDL_INIT_VIDEO))
	{
		fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
		return -1;
	}

	//create window from SDL
	win = SDL_CreateWindow("YUV Player",
		SDL_WINDOWPOS_UNDEFINED, // 窗口位置坐标
		SDL_WINDOWPOS_UNDEFINED,
		//w_width, w_height, // 窗口大小
		544,960,
		SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE // 设置窗口的类型，比如：可以被openGL渲染，可以调节窗口大小
	);

	if (!win)
	{
		fprintf(stderr, "Failed to create window,%s\n", SDL_GetError());
		goto _FAIL;
	}


	renderer = SDL_CreateRenderer(win, -1, 0);

	//IYUV:Y + U + V (3 planes)
	//YUV2:Y + V + U (3 planes)
	pixformat = SDL_PIXELFORMAT_IYUV; // 这就是 YUV420P，的枚举

	//create texture for reder
	texture = SDL_CreateTexture(renderer,pixformat,
		SDL_TEXTUREACCESS_STREAMING,video_width,video_height);//这里的第三个参数决定了纹理的访问方式，流式访问，常用于纹理需要不断更新的场景，播放YUV显示适用于这种情况，该目录下有三种访问模式的表格，可以看看

	//alloc space
	//video_buf = std::make_unique<uint8_t[]>(tmp_yuv_frame_len);
	video_buf = (uint8_t*)malloc(tmp_yuv_frame_len * sizeof(uint8_t));

	if (!video_buf)
	{
		fprintf(stderr, "Failed to alloc yuv frame space to unique_ptr\n");
		goto _FAIL;
	}

	//open yuv file
	video_fd = fopen(path, "rb");
	if (!video_fd)
	{
		fprintf(stderr, "Failed to open yuv file\n");
		goto _FAIL;
	}

	if ((video_buff_len = fread(video_buf, 1, yuv_frame_len, video_fd)) <= 0)
	{
		fprintf(stderr, "Failed to read data from yuv file!\n");
		goto _FAIL;
	}

	//set video position，在当前逻辑下，该 video_pos 可以去掉，因为 buf 和 pos 始终指向的是同一块内存，不涉及 指针的 修改
	video_pos = video_buf;
	
	/*video_end = video_buf + video_buff_len;
	blank_space_len = BLOCK_SIZE - video_buff_len;*/

	timer_thread = SDL_CreateThread(refresh_video_timer,
								NULL, NULL);

	do {
		//Wait
		SDL_WaitEvent(&event);
		if (event.type == REFRESH_EVENT)
		{
			SDL_UpdateTexture(texture, NULL, video_pos, video_width); //这里是更新纹理，为什么只需要提供宽度？这个和具体的像素格式有关，有时间再去看
			
			//FIX:IF window is resize
			rect.x = 0;
			rect.y = 0;
			rect.w = w_width;
			rect.h = w_height;

			//SDL_RenderClear(renderer)
			SDL_RenderCopy(renderer, texture, NULL, &rect); //给到 GPU 进行计算
			SDL_RenderPresent(renderer); // 刷新窗口，也就是让 GPU 渲染画面到指定窗口

			//read block data
			//std::memset(video_buf, 0, yuv_frame_len); 没用
			if ((video_buff_len = fread(video_buf, 1, yuv_frame_len, video_fd)) <= 0)
			{
				if (feof(video_fd)) {
					// 文件结束，停止读取
					printf("End of file reached.\n");
					thread_exit = true;
				}
				else if (ferror(video_fd)) {
					// 出现错误
					perror("Error reading from YUV file");
					thread_exit = true;
				}
				thread_exit = true;
				continue;
			}
		}
		else if (event.type == SDL_WINDOWEVENT)
		{
			//if Resize
			SDL_GetWindowSize(win, &w_width, &w_height); // 这个函数会 “更新” 后两个参数，根据 win 的参数情况，但是需要警惕：
														 // 若该方法本身发生异常，那么 w_width, w_height 这两个局部变量，将会被置为 0
		}
		else if (event.type == SDL_QUIT)
		{
			thread_exit = true;
		}
		else if (event.type == QUIT_EVENT)
		{
			break;
		}
	} while (true);
	
	SDL_Delay(3000);

_FAIL:
	if (video_buf)
	{
		free(video_buf);
		video_buf = nullptr;
	}
	//close file
	if (video_fd)
	{
		fclose(video_fd);
		video_fd = nullptr;
	}

	if (texture)
	{
		SDL_DestroyTexture(texture);
		texture = nullptr;
	}

	if (renderer)
	{
		SDL_DestroyRenderer(renderer);
		renderer = nullptr;
	}

	if (win)
	{
		SDL_DestroyWindow(win);
		win = nullptr;
	}

	SDL_Quit();

	return 0;
}
/*
* 1、这里来探讨一下SDL的事件处理机制（API），
* （1）注册事件（放入队列）----SDL_PushEvent(&event);，注意：这个队列是全局共享的
* （2）取出事件（pop 队列）----SDL_PollEvent(&event);或者 SDL_WaitEvent(&event)，两者的区别是一个阻塞一个不阻塞，这个函数会给event.type按照队列
*		中的事件先后顺序赋值，
* （3）后序处理就是用户自己的事情了
* 
* 问题：SDL_Event event全部共享吗？
*		不共享，只有队列是共享的，所以在不同的作用域内，调用 SDL_PollEvent(&event);或者 SDL_WaitEvent(&event) API时，会给event.type赋值
* 
* 2、一个重要的问题，在使用 fopen(path,"r"); 的时候，仅仅执行了一次 fread函数就读取到了文件末尾
*		这是为什么呢？
* 在文本模式下，也就是 r ，不同的操作系统会将文本内容转换为本操作系统的一些字符，所以极有可能转换导致 EOF 凭空出现
* 但是，b 也就是二进制模式，是最原始的数据形式，不会做任何改变，所以也就避免了这样的事情
* 当然，这个问题最初的表现形式是：只显示一张图片@@@，这就让我纳闷了
* 
* 3、如果要播放“裸”YUV数据，必须手动设置2个东西：（1）分辨率、（2）像素格式（420p、422、444）
*	必须与原数据一致，否则解析的时候，直接彩色乱码
* 
* 4、这个代码有个问题，我们做播放器在一般情况下，都是为了播放原视频，也就是观看效果要和原视频一致，但是在我的代码中，通过 SDL_Delay(40)，去控制
*	刷新纹理的速度，进而影响到了 GPU 渲染图片到窗口的速度，也就是实际帧率被改变了，照片更换速度被改变，也就意味着视频播放速度变了
*	现在有两种解决方案：
*	（1）通过获取 YUV 原视频的实际帧率来动态控制帧率，保证和原片一样。（但是这必须建立在有原视频的情况下，YUV实际就不是视频，而是一堆图片的集合，一般
*		没有描述帧率等视频信息的东西）
*	（2）设置常用的帧率，60fps就够了，毕竟这东西就是一个 YUV数据 ，他本来就没有帧率啊。
*/