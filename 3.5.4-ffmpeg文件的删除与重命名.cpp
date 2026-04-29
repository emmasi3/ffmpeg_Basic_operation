#include <iostream>
#include <vector>
extern"C"
{
#include <libavformat/avformat.h>

}

int main()
{
	int ret = 0;

	/*ret = avpriv_io_delete("C:\Users\Sakura\Desktop\111.txt");*/

	// 这个函数时ffmepg的私有内部函数，我这个FFmpeg是直接下载官网上编译好的，至于为什么视频上老师能够使用，大概率是因为，是他自己编译好的，私有函数的
	// 外部接口交给了用户

	return 0;
}