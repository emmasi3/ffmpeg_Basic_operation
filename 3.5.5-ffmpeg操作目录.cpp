#include <iostream>
#include <sys/stat.h>
extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}

int main()
{
	// 检查目录是否存在
	struct stat st;
	if (stat("./new", &st) != 0) {
		av_log(NULL, AV_LOG_ERROR, "Directory './new' does not exist or is not accessible\n");
		return -1;
	}

	av_log_set_level(AV_LOG_INFO);
	int ret = 0;
	AVIODirContext* ctx = nullptr;
	AVIODirEntry* entry = nullptr;
	char err_buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };

	ret = avio_open_dir(&ctx, "./new", NULL); // 好了，不用看了，这个函数在windows上没有实现，用不了，我想可能是官网上的编译好的ffmpeg是面向大众的，没有启用相
	// 关的文件

	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf));
		av_log(NULL, AV_LOG_ERROR, "con't open dir:%s\n",err_buf);
		return - 1;
	}
	else
	{
		av_log(NULL, AV_LOG_INFO, "enable open dir\n");
		avio_close_dir(&ctx);
	}

	return 0;
}