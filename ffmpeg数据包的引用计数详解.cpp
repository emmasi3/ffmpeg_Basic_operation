/*
* 参考：https://zhuanlan.zhihu.com/p/498090314#:~:text=%E5%AF%B9%E4%BA%8E%E5%A4%9A%E4%B8%AAAVPacket%E5%85%B1%E4%BA%AB%E5%90%8C%E4%B8%80%E4%B8%AA%E7%BC%93%E5%AD%98%E7%A9%BA%E9%97%B4%EF%BC%8CFFmpeg%E4%BD%BF%E7%94%A8%E7%9A%84%E5%BC%95%E7%94%A8%E8%AE%A1%E6%95%B0%E7%9A%84%E6%9C%BA%E5%88%B6%EF%BC%88reference-count%EF%BC%89%EF%BC%9A%20%E5%88%9D%E5%A7%8B%E5%8C%96%E5%BC%95%E7%94%A8%E8%AE%A1%E6%95%B0%E4%B8%BA0%EF%BC%8C%E5%8F%AA%E6%9C%89%E7%9C%9F%E6%AD%A3%E5%88%86%E9%85%8DAVBuffer%E7%9A%84%E6%97%B6%E5%80%99%EF%BC%8C%E5%BC%95%E7%94%A8%E8%AE%A1%E6%95%B0%E5%88%9D%E5%A7%8B%E5%8C%96%E4%B8%BA1%3B,%E5%BD%93%E6%9C%89%E6%96%B0%E7%9A%84Packet%E5%BC%95%E7%94%A8%E5%85%B1%E4%BA%AB%E7%9A%84%E7%BC%93%E5%AD%98%E7%A9%BA%E9%97%B4%E6%97%B6%EF%BC%8C%E5%B0%B1%E5%B0%86%E5%BC%95%E7%94%A8%E8%AE%A1%E6%95%B0%2B1%EF%BC%9B%20%E5%BD%93%E9%87%8A%E6%94%BE%E4%BA%86%E5%BC%95%E7%94%A8%E5%85%B1%E4%BA%AB%E7%A9%BA%E9%97%B4%E7%9A%84Packet%EF%BC%8C%E5%B0%B1%E5%B0%86%E5%BC%95%E7%94%A8%E8%AE%A1%E6%95%B0-1%EF%BC%9B%E5%BC%95%E7%94%A8%E8%AE%A1%E6%95%B0%E4%B8%BA0%E6%97%B6%EF%BC%8C%E5%B0%B1%E9%87%8A%E6%94%BE%E6%8E%89%E5%BC%95%E7%94%A8%E7%9A%84%E7%BC%93%E5%AD%98%E7%A9%BA%E9%97%B4AVBuffer%E3%80%82%20AVFrame%E4%B9%9F%E6%98%AF%E9%87%87%E7%94%A8%E5%90%8C%E6%A0%B7%E7%9A%84%E6%9C%BA%E5%88%B6
*       https://blog.csdn.net/shulianghan/article/details/143897046
*       https://blog.csdn.net/weixin_44977283/article/details/140119345
*       https://blog.csdn.net/a6484594/article/details/149857675#:~:text=%E7%8E%B0%E4%BB%A3%20FFmpeg%20%E4%B8%AD%EF%BC%8C%E6%B0%B8%E8%BF%9C%E4%B8%8D%E8%A6%81%E6%89%8B%E5%8A%A8%E8%B0%83%E7%94%A8%20av_init_packet%28%29%EF%BC%8C%E4%BD%BF%E7%94%A8%20av_packet_unref%28%29%20%E6%88%96%20av_packet_move_ref%28%29%20%E5%8D%B3%E5%8F%AF,NULL%3B%20%2F%2F%20av_packet_alloc%28%29%E6%B2%A1%E6%9C%89%E5%BF%85%E8%A6%81%EF%BC%8C%E5%9B%A0%E4%B8%BAav_packet_clone%E5%86%85%E9%83%A8%E6%9C%89%E8%B0%83%E7%94%A8%20av_packet_alloc%20AVPacket%20%2Apkt2%20%3D%20NULL%3B
* 1、在进行 QT 音视频播放器代码的研读时，发现一个现象：
*	（1）在调用 av_read_frame() 从 网络/本地文件 中读取一帧数据时，所用的 AVPacket* pkt 包
*		首先被初始化引用计数 + 1，也就是当前 pkt 所读取到的数据(堆上)，引用计数为 1，是这个 av_read_frame()
*		API 赋予的
*	（2）读取到之后，自然是要放入音视频原始队列中了，代码如下：
*		void AvPacketQueue::enqueue(AVPacket *packet)
        {
             AVPacket pkt;
            // 增加引用计数，防止外部直接释放原始数据
            av_packet_ref(&pkt, packet);

             SDL_LockMutex(mutex);

            queue.enqueue(pkt);

            SDL_CondSignal(cond);
         SDL_UnlockMutex(mutex);
        }
* 
* 2、av_packet_unref(pkt); 会将 pkt 一些字段置为 0，并减少 pkt 持有的引用计数 - 1，pkt 不再持有资源
*   如果再次调用 av_packet_unref(pkt); 释放同一个 pkt，由于前面 pkt 已经被 unref 了一遍，所以此时 pkt 没有持有资源，所以
*   av_packet_unref 内部会检查 pkt，不会对 pkt 做任何修改（因为它本身没有引用资源，也就没什么好修改的）
*   （1）av_packet_ref(pkt, src); 这一条语句，是用来增加引用计数的，在 pkt 上，如果连续多次对一个 pkt 调用该方法，
*       即使为 2 次，也会引发内存泄漏，因为 av_packet_unref(pkt) 第一次就会将 pkt 的引用计数 -1，并且不再持有资源，所以
*       第 2 次调用 av_packet_unref(pkt); 不会成功，那么你之前 av_packet_ref 多增加的几次引用计数，永远不会减少
*       调用 av_packet_unref() 也没有，所以 “内存泄漏”
*       
* 3、
* 
*/

#include <iostream>
#include <string>

extern "C"
{
#include <libavutil/avutil.h>
#include <libavutil/fifo.h>
#include <libavutil/time.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
}

#define MEM_ITEM_SIZE (20 * 1024 * 102)
#define AVPACKET_LOOP_COUNT 1000

static void av_packet_test_6()
{
    AVPacket* packet = nullptr;
    AVPacket* packet2 = nullptr;

    packet = av_packet_alloc();
    int ret = av_new_packet(packet, MEM_ITEM_SIZE);
    _memccpy(packet->data, (void*)&av_packet_test_6, 1, MEM_ITEM_SIZE);

    packet2 = av_packet_alloc(); // 必须先 alloc，否则那玩意是个 nullptr
    *packet2 = *packet;          // 有点类似 packet 可以重新分配内存，这个就字段来看，把字段全部复制过来了
    if (packet->buf)
        av_log(NULL, AV_LOG_INFO, "[%s] -- packet->buf != NULL -- line:%d\n", __FUNCTION__, __LINE__);
    else
        av_log(NULL, AV_LOG_INFO, "[%s] -- packet->buf == NULL -- line:%d\n", __FUNCTION__, __LINE__);

    if (packet2->buf)
        av_log(NULL, AV_LOG_INFO, "[%s] -- packet2->buf != NULL -- line:%d\n", __FUNCTION__, __LINE__);
    else
        av_log(NULL, AV_LOG_INFO, "[%s] -- packet2->buf == NULL -- line:%d\n", __FUNCTION__, __LINE__);

    //av_init_packet(packet);

    av_packet_free(&packet);
    // 这一步会报错，因为释放的是同一块 buf，报错
    av_packet_free(&packet2);
}

static void test_copy_AVPacket()
{
    AVPacket* pkt = av_packet_alloc();
    //AVPacket* pkt2 = av_packet_alloc();
    if (!pkt)
    {
        av_log(NULL, AV_LOG_ERROR, "AVPacket* pkt = av_packet_alloc() failed!\n");
        return;
    }

    // 模拟 av_read_frame() 内部对 pkt 做的引用计数效果
    int ret = av_new_packet(pkt, MEM_ITEM_SIZE);
    if (pkt->buf)
    {
        printf("count(pkt) = %d\n",
            av_buffer_get_ref_count(pkt->buf));  // count：1
    }
    else
    {
        /*
        * 其实从这里可以看出，pkt 分配内存后，并没有初始化 “引用计数”，直到 av_new_packet() 才开始引用计数
        */
        std::cout << "pkt->buf is nullptr!\n";
    }
    _memccpy(pkt->data, (void*)&test_copy_AVPacket, 1, MEM_ITEM_SIZE);

    /*
    * （1）测试通过普通的结构体拷贝，是否能够触发 AVPacket 的引用计数 + 1？还是一定要通过调用 av_packet_ref 系列函数才行
    *   结果：依旧打印 1，引用计数没加，结构体拷贝，仅仅是浅拷贝而已，没有触发引用计数增加
    */ 
    {
        AVPacket pkt2(*pkt);

        if (pkt->buf)
        {
            printf("count(pkt) = %d\n",
                av_buffer_get_ref_count(pkt->buf)); // count：1
        }
    }

    /*
    * （2）那么队列中的 queue<AVPacket> 也就同理了，依旧不会增加引用计数，引用计数仅仅在调用 av_packet_ref 系列函数时才增加或者减少
    *   av_read_frame() 中应该也调用了相关方法，嗯嗯！
    *   那么测试 av_packet_ref 看看效果
    */
    {
        AVPacket* pkt2 = av_packet_alloc();
        av_packet_ref(pkt2, pkt);

        if (pkt->buf)
        {
            printf("count(pkt) = %d\n",
                av_buffer_get_ref_count(pkt->buf)); // count：2
        }

        // 释放之后再看一下
        av_packet_unref(pkt2);
        if (pkt->buf)
        {
            printf("count(pkt) = %d\n",
                av_buffer_get_ref_count(pkt->buf)); // count：2
        }
        // 释放 AVPacket 本身的堆内存
        av_packet_free(&pkt2);
    }
    

    std::cout << "------------------\n";
    /*
    * （3）AVPacket 引用计数的机制：参考：https://blog.csdn.net/shulianghan/article/details/143897046，尤其是他的图片：
    *   拷贝下来了：ffmpeg_AVPacket_引用计数.png
    *   数据结构设计：AVPacket::AVBufferRef* buf; 每一个 AVpacket 都拥有一个 AVBuffer 的引用
    *               pkt1->buf != pkt2->buf; （在上面的参考中：尤其是 零声教育 那个视频中的图，是错误的，不是 pkt1->buf == pkt2->buf）
    *               真正的缓冲区数据在 AVBuffer 中，这是 “共享内存” 中的资源，只有一份，而 pkt->buf 也就是 AVBufferRef，每一个 AVPacket 对象
    *               都有一份，内部有指针指向 “共享内存 AVBuffer”
    *               
    *               av_packet_ref(pkt2, pkt); 会处理 pkt2 中的 buf，引用计数会  +1
    */
    {
        AVPacket* pkt2 = av_packet_alloc();

        std::cout << " &(pkt2->buf): " << &pkt2->buf << '\n';

        av_packet_ref(pkt2, pkt);

        std::cout << " ref 之后 ,&(pkt2->buf): " << &pkt2->buf << '\n';
        std::cout << " &(pkt->buf): " << &pkt->buf << '\n';

        // 释放之后再看一下
        av_packet_unref(pkt2);
        // 释放 AVPacket 本身的堆内存
        av_packet_free(&pkt2);
    }
    std::cout << "---------------------\n";

    /*
    * （4）avcodec_send_packet() / avcodec_receive_frame() 对于 AVPacket / AVFrame 的引用计数如何？
    *   根据之前编程的经验：每一次调用这俩 API 后，都要进行 av_packet_unref()，再结合这俩方法的官方注释：
    *   may 可以创建新的引用对于数据包(前提是要开始引用计数)，他没有保证，用的是这个单词，···
    *   结合来看，大部分情况下，都会创建新的引用，也就是引用计数 + 1，所以记得调用 av_packet_unref
    */


    // 释放引用计数
    av_packet_unref(pkt);        
    if (pkt->buf)
    {
        printf("count(pkt) = %d\n",
            av_buffer_get_ref_count(pkt->buf)); // count：0
    }
    else
    {
        std::cout << "count(pkt) = 0\n";
    }
    // 释放 AVPacket 结构体本身的 “堆内存”（内部首先会调用 av_packet_unref，但是不影响，内部会检查是否有效）
    av_packet_free(&pkt);
}

int main()
{
    //av_packet_test_6();

    test_copy_AVPacket();


    return 0;
}