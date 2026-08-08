#pragma once

/* PSP-specific SDL event emulation
 * This is a minimal subset of SDL types needed by the emulator core
 */

typedef int SDL_Scancode;

/* PSP button mappings to Vector-06C keycodes */
enum {
    SDL_SCANCODE_F6 = 0x80,   /* RUS/LAT toggle */
    SDL_SCANCODE_F11 = 0x81,  /* Reset (BLKVVOD) */
    SDL_SCANCODE_F12 = 0x82,  /* Reset (BLKSBR) */
    SDL_SCANCODE_PAUSE = 0x83,
    SDL_SCANCODE_LSHIFT = 0x84,
    SDL_SCANCODE_RSHIFT = 0x85,
    SDL_SCANCODE_LCTRL = 0x86,
    SDL_SCANCODE_RCTRL = 0x87,
    SDL_SCANCODE_LGUI = -1,
    SDL_SCANCODE_LALT = 0x88,
    SDL_SCANCODE_SPACE = 0x20,
    SDL_SCANCODE_SEMICOLON = 0x3b,
    SDL_SCANCODE_1 = '1',
    SDL_SCANCODE_2 = '2',
    SDL_SCANCODE_3 = '3',
    SDL_SCANCODE_4 = '4',
    SDL_SCANCODE_5 = '5',
    SDL_SCANCODE_6 = '6',
    SDL_SCANCODE_7 = '7',
    SDL_SCANCODE_8 = '8',
    SDL_SCANCODE_9 = '9',
    SDL_SCANCODE_0 = '0',
    SDL_SCANCODE_MINUS = '-',
    SDL_SCANCODE_SLASH = '/',
    SDL_SCANCODE_A = 'A',
    SDL_SCANCODE_B = 'B',
    SDL_SCANCODE_C = 'C',
    SDL_SCANCODE_D = 'D',
    SDL_SCANCODE_E = 'E',
    SDL_SCANCODE_F = 'F',
    SDL_SCANCODE_G = 'G',
    SDL_SCANCODE_H = 'H',
    SDL_SCANCODE_I = 'I',
    SDL_SCANCODE_J = 'J',
    SDL_SCANCODE_K = 'K',
    SDL_SCANCODE_L = 'L',
    SDL_SCANCODE_M = 'M',
    SDL_SCANCODE_N = 'N',
    SDL_SCANCODE_O = 'O',
    SDL_SCANCODE_P = 'P',
    SDL_SCANCODE_Q = 'Q',
    SDL_SCANCODE_R = 'R',
    SDL_SCANCODE_S = 'S',
    SDL_SCANCODE_T = 'T',
    SDL_SCANCODE_U = 'U',
    SDL_SCANCODE_V = 'V',
    SDL_SCANCODE_W = 'W',
    SDL_SCANCODE_X = 'X',
    SDL_SCANCODE_Y = 'Y',
    SDL_SCANCODE_Z = 'Z',

    SDL_SCANCODE_GRAVE = 0xc0,
    SDL_SCANCODE_RIGHTBRACKET = ']',
    SDL_SCANCODE_BACKSLASH = '\\',
    SDL_SCANCODE_LEFTBRACKET = '[',
    SDL_SCANCODE_PERIOD = '.',
    SDL_SCANCODE_EQUALS = '=',
    SDL_SCANCODE_COMMA = ',',
    SDL_SCANCODE_APOSTROPHE = '\'',

    SDL_SCANCODE_F5 = 0x90,
    SDL_SCANCODE_F4 = 0x91,
    SDL_SCANCODE_F3 = 0x92,
    SDL_SCANCODE_F2 = 0x93,
    SDL_SCANCODE_F1 = 0x94,
    SDL_SCANCODE_ESCAPE = 0x95,
    SDL_SCANCODE_F8 = 0x96, /* СТР */
    SDL_SCANCODE_F7 = 0x97, /* ^\ ? */

    SDL_SCANCODE_DOWN = 0x98,
    SDL_SCANCODE_RIGHT = 0x99,
    SDL_SCANCODE_UP = 0x9a,
    SDL_SCANCODE_LEFT = 0x9b,
    SDL_SCANCODE_BACKSPACE = 0x9c,
    SDL_SCANCODE_RETURN = 0x9d,
    SDL_SCANCODE_RALT = 0x9e,
    SDL_SCANCODE_TAB = 0x9f,
};

enum SDL_PixelFormat {
    SDL_PIXELFORMAT_ARGB8888,
    SDL_PIXELFORMAT_RGB888,
    SDL_PIXELFORMAT_BGR888,
    SDL_PIXELFORMAT_ABGR8888,
};

enum SDL_EventType {
    SDL_KEYDOWN,
    SDL_KEYUP,
    SDL_WINDOWEVENT,
    SDL_QUIT,
    SDL_USEREVENT,

    SDL_WINDOWEVENT_RESIZED,
    SDL_WINDOWEVENT_EXPOSED,
    SDL_WINDOWEVENT_SIZE_CHANGED,
};

struct SDL_KeySym {
    SDL_Scancode scancode;
};

struct SDL_KeyboardEvent {
    SDL_KeySym keysym;
};

struct SDL_WindowEvent {
    SDL_EventType event;
};

struct SDL_Event {
    SDL_EventType type;
    union {
        SDL_KeyboardEvent key;
        SDL_WindowEvent window;
    };
};
