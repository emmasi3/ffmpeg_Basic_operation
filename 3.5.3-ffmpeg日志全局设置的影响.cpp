#include <iostream>
extern"C"
{
#include <libavutil/log.h>

}

int main()
{
	av_log_set_level(AV_LOG_DEBUG); // 1、这里是设置全局日志级别的函数

	/*av_log_set_level(AV_LOG_ERROR);*/

	// 2、这4个函数是指定日志级别并打印的函数（实用）
	av_log(NULL, AV_LOG_DEBUG, "%s\n", "AV_LOG_DEBUG");

	av_log(NULL, AV_LOG_INFO, "%s\n", "AV_LOG_INFO");

	av_log(NULL, AV_LOG_WARNING, "%s\n", "AV_LOG_WARNING");

	av_log(NULL, AV_LOG_ERROR, "%s\n", "AV_LOG_ERROR");

	// 3、当级别为 DEBUG 时，这四句话全部会被打印
	//    当级别为 INFO 时，DEBUG 级别的日志不会被打印，
	//    当级别为 WARNING 时，WARNING 级别的日志和ERROR会被打印，所以，看懂了吗？

	// 4、av_log_set_level(AV_LOG_ERROR); 当不写这个函数时，也就是不指定 ffmpeg 的日志级别的时候，默认设置为 INFO 级别，也就是正常的

	// 5、av_log_set_level(AV_LOG_ERROR); 这个函数（在同一个作用域下）可以多次使用，这个函数并没有采用“单例模式”，call_once


	return 0;
}