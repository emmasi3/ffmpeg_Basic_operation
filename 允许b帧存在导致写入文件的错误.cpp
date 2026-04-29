/*
* 
* 1、问题复现：
*	在encode_DXGI_GPU.cpp 中，将 ctx->max_b_frames = 1; 也就是不为 0 时，编码出来的 pkt 通过
*	av_interleaved_write_frame() 送入指定的文件IO时，报错：
*	 pts < dts，这是不允许的？
*	最后查了 AI，表示可以通过：
*		AVDictionary* mux_opts = nullptr;
		av_dict_set(&mux_opts, "movflags", "+negative_cts_offsets", 0);

		// 5、写多媒体文件头
		ret = avformat_write_header(oFmtCtx, &mux_opts);
*	这样的方式，允许存在 pts < dts 的情况
*	但是，很遗憾，没什么卵用···
*	最后 AI 修改了 ctx->time_base{1, sylar::fps}; 这才正常
*	但是打印出来此时的 pkt 的pts和dts(编码器给出的)，严格保持 pts >= dts
*	···也就是还是不能够输入 pts < dts 的 pkt 帧，哎！
*	网上也搜不出来什么···哎，就这样吧，设置为 ctx->time_base{1, 1000000} 应该是将计算的误差放大了，导致出现了 pts < dts 的情况
* 
* 2、昨晚写的这第一条简见解，有着很大的问题 -- 就是 b 帧在相邻的帧之间的排序时，必须是 自身的 pts < dts 吗？
*	分析一下：b 帧要参考后一帧的数据，也就是它的后一帧数据需要解码出来，那么 b 帧能否顺利解码的关键在于 -- b 帧 dts > 下一帧 dts
*	只需要在 b 帧解码之前，将下一帧解码出来就行了，至于 pts，这仅仅是 dts 问题的延伸，这种说法还行吧？
*	由此来看，之前说的，b 帧自身的 pts < dts 的情况，不是唯一的结果，在实际情况中，是有 (pkt_0->pts >= pkt_0->dts) && (pkt_0->dts > pkt_1->dts) 的 
*	嗯嗯，我打印了该项目中，所有的接收到的帧的 pts 和 dts 情况，都满足上面的情况，没有一次出现 pts < dts 的情况，也就是没有文件写入错误
*	嗯嗯
*	
* 
*/