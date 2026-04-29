#include <iostream>
extern"C"
{
	#include <libavutil/log.h> // 1、记住，这个extern"C"必须有，讲解在此目录下

}

int main()
{
	av_log_set_level(AV_LOG_DEBUG);// 2、这个函数用来设置ffmpeg的日志级别，AV_LOG_DEBUG 是最低等级的，前方的 INFO、WARNING、ERROR，都会被打印出来

	av_log(NULL, AV_LOG_DEBUG, "Hello World!  %s\n","hello");

	return 0;
}
