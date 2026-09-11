#pragma once

#include <atomic>
#include <inttypes.h>
#include "popup.h"

/*
 * GAME CENTER: online ROM catalog browser opened from the MAIN MENU.
 *
 *     enum class UIState { ..., GAME_CENTER };
 *
 *   GAME_CENTER — the machine stays frozen via the Emulator pause
 *                 flag; the window is drawn above the dim overlay.
 *
 * Flow:
 *   1. open() checks WiFi, initialises the network, connects to an
 *      AP via the system dialog, downloads the INI catalog, parses
 *      it and shows the list.
 *   2. Navigation: UP/DOWN scroll the list; the first preview image
 *      of the selected entry is downloaded on demand (cached).
 *   3. close() disconnects the network and frees resources.
 *
 * Threading: same split as RomBrowser — the worker thread calls
 * open()/close()/update(), the display thread calls paint().
 */

/* Normalized pad state passed to GameCenter::update(). */
enum {
    GC_PAD_UP    = 0x01,
    GC_PAD_DOWN  = 0x02,
    GC_PAD_PRESS = 0x04,  /* X button - load ROM */
    GC_PAD_BACK  = 0x08,  /* CIRCLE/START - back */
    GC_PAD_LEFT  = 0x10,
    GC_PAD_RIGHT = 0x20,
};

class GameCenter : public Popup
{
public:
    /* Window size, same as RomBrowser. */
    static const int PANEL_W = 440;
    static const int PANEL_H = 220;

    /* Layout constants. */
    static const int PAD_X = 8;
    static const int PAD_Y = 8;
    static const int TITLE_H = 16;
    static const int HDR_GAP = 4;
    static const int ROW_H = 20;
    static const int VISIBLE_ROWS = 8;
    static const int FOOTER_H = 16;

    /* Two panes: list left, preview right. */
    static const int LIST_W = 208;
    static const int DIV_GAP = 8;
    static const int PREVIEW_X = PAD_X + LIST_W + DIV_GAP;
    static const int PREVIEW_W = PANEL_W - PAD_X - PREVIEW_X;
    static const int PREVIEW_Y = PAD_Y + TITLE_H + HDR_GAP + 1 + HDR_GAP;
    static const int PREVIEW_H = PANEL_H - PAD_Y - FOOTER_H - PREVIEW_Y;

    /* Preview texture: power-of-two GE dimensions. */
    static const int PREVIEW_TEX_W = 256;
    static const int PREVIEW_TEX_H = 256;

    /* Catalog limits. */
    static const int MAX_GAMES = 128;
    static const int TITLE_LEN = 64;
    static const int PATH_LEN = 256;

    GameCenter();

    bool is_open() const { return this->open_flag.load(std::memory_order_acquire); }

    /* Worker thread: check WiFi, connect, download catalog, show
     * list.  Sets status to an error message on failure. */
    void open();
    /* Worker thread: close window, disconnect network. */
    void close();

    /* One input step; called by the worker thread (~50 Hz). */
    void update(unsigned pad);

    bool has_items() const { return this->count > 0; }
    const char * selected_title() const;

    /* Footer status line. */
    void set_status(const char * msg);

    /* Preview access for TV::draw_gc_preview_quad(). */
    bool has_preview() const { return this->preview_w > 0 && !confirm_dialog; }
    const uint32_t * preview_tex_data() const { return preview_tex; }
    int get_preview_w() const { return this->preview_w; }
    int get_preview_h() const { return this->preview_h; }
    void get_preview_rect(int * x, int * y, int * w, int * h) const
    {
        *x = fit_x; *y = fit_y; *w = fit_w; *h = fit_h;
    }
    bool consume_preview_upload()
    {
        bool v = preview_upload;
        preview_upload = false;
        return v;
    }

    /* ROM download access. */
    bool has_rom_ready() const { return rom_ready; }
    const char * get_rom_path() const { return rom_path; }
    void clear_rom_ready() { rom_ready = false; rom_path[0] = '\0'; }
    bool is_confirm_dialog_active() const { return confirm_dialog; }

    /* Rasterize the window. Main thread only. */
    void paint();

private:
    struct GameEntry {
        char key[TITLE_LEN];
        char title[TITLE_LEN];
        char description[256];
        char author[64];
        char genre[32];
        char year[16];
        char rom_file[PATH_LEN];
        char preview[PATH_LEN];  /* first preview path only */
        int  size_bytes;
    };

    void parse_catalog(const char * data, int len);
    void update_preview();
    void build_preview_url(char * url, int url_len, const char * preview);
    void load_selected_rom();

    std::atomic<bool> open_flag;

    int count;
    int selected;
    int top;            /* first visible row (scrolling) */
    char message[64];
    bool catalog_ok;

    GameEntry entries[MAX_GAMES];

    /* Preview state (same scheme as RomBrowser). */
    int preview_for_index;  /* entry the cache belongs to; -1 = none */
    int preview_w, preview_h;
    int fit_x, fit_y, fit_w, fit_h;
    bool preview_upload;
    int idle_frames;        /* frames since last selection change */

    /* Confirmation dialog state. */
    bool confirm_dialog;
    int confirm_selection;  /* 0 = YES, 1 = NO */

    /* ROM download state. */
    char rom_path[256];     /* path to downloaded ROM, empty if none */
    bool rom_ready;         /* true when ROM is downloaded and ready to load */

    alignas(16) uint32_t preview_tex[PREVIEW_TEX_W * PREVIEW_TEX_H];
};
