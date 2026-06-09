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

    //������Ⱦ��
    render = SDL_CreateRenderer(window, -1, 0);
    if (!render)
    {
        SDL_Log("Failed to Create Render!");
        goto _DWINDOW;
    }

    SDL_SetRenderDrawColor(render, 255, 0, 0, 255);

    SDL_RenderClear(render);// ������Ⱦ���Ĳ�����Ϣ����һ�Σ�

    SDL_RenderPresent(render);

    do {
        SDL_Event event;

        SDL_WaitEvent(&event);

        switch (event.type)
        {
        case SDL_QUIT:
            quit = 0;
            break;
        defult:
            SDL_Log("event type is %d", event.type);
        }
    
    } while (quit);

    SDL_DestroyWindow(window);

_DWINDOW:
    SDL_DestroyWindow(window);

_EXIT:
    SDL_Quit();
    return 0;
}
