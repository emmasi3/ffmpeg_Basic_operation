#define SDL_MAIN_HANDLED
#include <iostream>
extern"C"
{
#include <SDL.h>

}


int main() {

    SDL_Window* window = nullptr;
    SDL_Renderer* render = nullptr;

    SDL_Init(SDL_INIT_VIDEO);

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

    //创建渲染器
    render = SDL_CreateRenderer(window, -1, 0);
    if (!render)
    {
        SDL_Log("Failed to Create Render!");
        goto _DWINDOW;
    }

    SDL_SetRenderDrawColor(render, 255, 0, 0, 255);

    SDL_RenderClear(render);// 清理渲染器的残留信息（上一次）

    SDL_RenderPresent(render);

    SDL_Delay(3000);

    SDL_DestroyWindow(window);

_DWINDOW:
    SDL_DestroyWindow(window);

_EXIT:
    SDL_Quit();
    return 0;
}
