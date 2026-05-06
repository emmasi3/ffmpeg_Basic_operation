/*
* 1、使用 dshow 设备时，一直很疑惑，到底是从哪一步开始 硬件设备便开始录制 audio/video，到设备的缓存中？
*	AI 告诉我在我第一次调用 av_read_frame() 时，才开始
*	验证方法：
*	（1）在 SDL::Start() 方法中，在音频生产者线程(调用av_read_frame)开启之前 && avforamt_open_input() 之后，添加一个线程睡眠函数 5s
*		我们发现，原本视频和音频的同步延迟为 1s 左右，现在 sleep 5s 之后，延迟更大了，飙到了 7s 左右
*	（2）基于（1）中的发现，我们猜测在 avformat_open_input() 调用之后，便开始录制音频到缓冲区了，所以再次验证
*	（3）在 avforamt_open_input() 之前，sleep 5s，取消（1）中的睡眠操作，看看延迟是否回到 1s 左右
*	结果：如我所料，果然，在 avforamt_open_input() 之前先 sleep 5s，延迟回到了 1s 左右，嗯嗯
* 
*	结论：设备录制 video/audio 到设备的缓冲区中的开启时刻是 avforamt_open_input() 调用之后，而不是 av_read_frame() 第一次从设备上下文中
*		读取数据帧
* 
* 
* 
* 
* 
* 
* 
* 
* 
* 
* 
* 
* 
*/