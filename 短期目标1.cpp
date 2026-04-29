/*

1、完成 windows 桌面录制的高性能任务，也就是调用 DirectX Graphics Infrastructure (DXGI) 出色的屏幕抓取工具，秒杀 GDI
 -- 参考 ：https://zhuanlan.zhihu.com/p/1898859341521588602 -- 底部有作者的 GitHub 仓库代码
 or https://zhuanlan.zhihu.com/p/684396021  https://blog.csdn.net/ababab12345/article/details/102674601

 https://github.com/peilinok/screen-recorder 一个开源的项目，使用了 WGC 更加高性能的录屏工具，嗯嗯，底层还是 DXGI

2、将录屏录播推流到 Linux 服务器上

3、Linux 端将音视频数据保存为 mp4 格式文件或者其他格式文件

4、Linux 端将音视频数据再次推流并转发，在 windows 端通过 VLC 播放器拉流

5、测试性能

6、直播：
	| 特性     |					直播								| 点播                  |
	|          | ---------------------			 | ---------------------				|
	| 数据来源 | 实时生成（主播、摄像机）          | 存在完整文件 / 视频片段				|
	| 数据分发 | **只需要把流推给下游节点 / CDN**  | 可以随机访问任意部分，按请求拉取		|
	| 延迟要求 | 高（低延迟 <1s~5s）              | 不敏感（缓冲几秒或几分钟都行）			|
	| 数据缓存 | 临时缓冲						 | 文件 / 分片永久存储					|
	| 并发模式 | 一写多读（广播）                 | 一写多读，但可能是**多写多读**（点播）  |





*/