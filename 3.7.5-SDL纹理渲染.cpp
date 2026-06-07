#define SDL_MAIN_HANDLED
#include <iostream>
extern"C"
{
#include <SDL.h>

}


int main() {

    int quit = 1;

    SDL_Window* window = nullptr;
    SDL_Renderer* render = nullptr;
    SDL_Texture* texture = nullptr;
    //方块
    SDL_Rect rect;
    rect.w = 30;
    rect.h = 30;

    SDL_Init(SDL_INIT_VIDEO);

    //设置窗口参数
    window = SDL_CreateWindow("SDL2 Window",
        200,
        200,
        640,
        480,
        SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS
    );

    if (!window)
    {
        std::cerr << "the window is failure!\n";
        goto _EXIT;
    }

    //创建渲染器（绑定渲染器和窗口）
    render = SDL_CreateRenderer(window, -1, 0);
    if (!render)
    {
        SDL_Log("Failed to Create Render!");
        goto _DWINDOW;
    }

    /*SDL_SetRenderDrawColor(render, 255, 0, 0, 255);

    SDL_RenderClear(render);// 清理渲染器的残留信息（上一次）

    SDL_RenderPresent(render);*/

    //创建纹理
    texture = SDL_CreateTexture(render,
        SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_TARGET, // 纹理可作为渲染对象
        640,
        480
    );

    if (!texture)
    {
        SDL_Log("Failed to Create Texture!");
        goto _RANDER;
    }

    do {
        SDL_Event event;

        SDL_PollEvent(&event);

        switch (event.type)
        {
        case SDL_KEYDOWN:
            if(event.key.keysym.sym == SDLK_q)
            {
                quit = 0;
            }
            break;
        default:
            SDL_Log("event type is %d", event.type);
        }
        
        //平面坐标，采取随机数（未设置随机数种子）
        rect.x = rand() % 600;
        rect.y = rand() % 450;

        //设置渲染目标，设置渲染颜色，用Clear渲染到整个界面
        /*
        * SDL_SetRenderTarget(render, texture);
        •	将渲染器的目标切换为给定的 texture。之后所有的绘制命令都会画到这个纹理上，而不是默认的窗口帧缓冲。
        •	只有当 texture 是用 SDL_TEXTUREACCESS_TARGET 创建时才能作为目标。
        • SDL_SetRenderDrawColor(render, 0, 0, 0, 0);
        •	设置渲染器的“绘制颜色”为 RGBA(0,0,0,0)（黑色且透明）。这个颜色会被用于后续的清屏或绘制点/线/矩形等操作。
        • SDL_RenderClear(render);
        •	用当前的绘制颜色清空当前渲染目标（此处为上面指定的 texture）。即把纹理内容填充为刚设置的 RGBA 值（在这里是透明黑）。
        */
        SDL_SetRenderTarget(render, texture);
        SDL_SetRenderDrawColor(render, 0, 0, 0, 0);
        SDL_RenderClear(render);

        //填充小方块
        SDL_RenderDrawRect(render, &rect);
        SDL_SetRenderDrawColor(render, 255, 0, 0, 0);
        SDL_RenderFillRect(render, &rect);

        //设置渲染目标（默认：窗口）
        SDL_SetRenderTarget(render, NULL);
        //送去给 GPU 负责计算（而不是CPU）
        SDL_RenderCopy(render, texture, NULL, NULL);
        //这一步才会让 GPU 显示（更新“屏幕”操作）
        SDL_RenderPresent(render);

        SDL_RenderClear(render);

    } while (quit);

_RANDER:
    SDL_DestroyTexture(texture);

_DWINDOW:
    SDL_DestroyWindow(window);

_EXIT:
    SDL_Quit();
    return 0;
}
/*
* （1）纹理：YUV 纹理 = YUV 数据在 GPU 显存中的存储方式，本质上 数据没有变，只是存放的位置不同：
? YUV 数据：存在 CPU 内存（RAM）里，纯粹的数值存储，不能直接显示。
? YUV 纹理：原封不动地上传到 GPU 显存，等待 GPU 进行渲染。
* 区别只是存储位置，而数据本质是一样的！
* 最终 GPU 会把 YUV 纹理转换成 RGB，才能显示在人眼前。
*   这就是纹理的概念，但是为什么要有纹理这一个东西呢？这个参考音视频基础中的“纹理详解”
* 
* （2）上述的步骤大题可以分为：
* 1、初始化SDL库(Library) SDL_Init，这里应该为初始化 SDL 的一些组件和属性
* 2、创建窗口 SDL_CreateWindow() 
* 3、创建渲染器render（绑定窗口window）
* 4、创建纹理 SDL_CreateTexture
*   对纹理做一些操作，但是要指定操作的目标 SDL_SetRenderTarget(render,texture);
*   操作完成后，接下来是渲染到窗口的操作，也就是让显卡显示画面到指定窗口
* 5、SDL_SetRenderTarget(render,NULL) 这里选择NULL，就是指定为默认窗口，也就是window
* 6、将纹理交给显卡处理（计算） SDL_RenderCopy(render,texture);
* 7、更新窗口（显示GPU刚刚计算完的RGB数据） SDL_RenderPresent(render);
* 8、释放资源（固定API）
* 这里的第7、步是关键，通过 SDL_RenderCopy计算完之后，必须刷新窗口，才会显示新的换面，否则无效
* 
*/