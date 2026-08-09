#pragma once

#include <string>
#include <functional>
#include <inttypes.h>
#include "globaldefs.h"
#include "event.h"
#include "options.h"

class TV
{
private:
    uint32_t * bmp;
    uint32_t * texbuf;   /* power-of-two texture buffer for PSP GU */
    int tex_width;
    int tex_height;
    int refresh_rate;

    uint32_t pixelformat;

public:
    TV();
    ~TV();
    int probe();
    void init();
    void toggle_fullscreen();
    void save_frame(std::string path);
    uint32_t* pixels() const;
    std::function<uint32_t(uint8_t,uint8_t,uint8_t)> get_rgb2pixelformat() const;
    /* executed: 1 if the frame was real, 0 if the frame is a skip frame */
    void render(int executed);
    int get_refresh_rate() const;
    void handle_window_event(SDL_Event & event);
};
