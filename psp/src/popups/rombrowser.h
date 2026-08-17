#pragma once

#include <atomic>
#include <inttypes.h>
#include "popup.h"

/*
 * ROM Browser: the PSP UI window opened from the MAIN MENU's
 * "Load ROM" item (Stage 2). UI state machine of this port:
 *
 *     enum class UIState { GAME, MAIN_MENU, ROM_BROWSER };
 *
 *   ROM_BROWSER - the machine stays frozen via the Emulator pause
 *                 flag (paused == true for the whole lifetime of the
 *                 window: no CPU/Memory/IO/sound work, no frames
 *                 published), the last Vector frame is the backdrop,
 *                 the window is drawn above the dim overlay in the
 *                 480x272 UI coordinate space.
 *
 * Transitions (all in the worker thread, see main.cpp handle_input):
 *
 *   MAIN_MENU --X on Load ROM--> ROM_BROWSER   (menu closes, still paused)
 *   ROM_BROWSER --O/START------> MAIN_MENU     (focus back on Load ROM)
 *   ROM_BROWSER --X on a ROM---> GAME          (ROM loaded, resumed)
 *
 * Threading, same split as the MAIN MENU; the texture / repaint
 * machinery is shared through the Popup base class:
 *
 *   Worker thread: open()/close()/update() mutate the state; open()
 *   rescans the ROMS folder every time so the list is always fresh.
 *
 *   Main/display thread: needs_repaint()/paint() rasterize the
 *   window; TV::render() presents it as one overlay quad.
 *
 * The scanning itself reuses the existing FileList::listRoms()
 * (same path handling, extension filter and sort order); the actual
 * ROM loading goes through Emulator::load_rom(). This class never
 * touches CPU/Memory directly.
 *
 * Preview (Stage 4): next to the ROM list the window shows a static
 * picture of the selected ROM: <base>.tga in the same directory
 * (FileList::findPreview), decoded by tgaload into a private RGBA
 * texture and presented by TV as a second quad over the right pane.
 * The preview updates together with the selection and is cached:
 * the file is decoded once per ROM (a missing/broken file is cached
 * as "no preview" too, never retried every frame), released on
 * close(). Decode errors never surface to the user.
 */

/* Normalized pad state passed to RomBrowser::update(): which
 * buttons are currently held. Keeps rombrowser free of pspctrl.h. */
enum {
    RB_PAD_UP   = 0x01,
    RB_PAD_DOWN = 0x02,
};

class RomBrowser : public Popup
{
public:
    /* Window size, PSP UI coordinate space (480x272); the renderer
     * centers it, leaving 20/26 px margins around. */
    static const int PANEL_W = 440;
    static const int PANEL_H = 220;

    /* Layout constants. */
    static const int PAD_X = 8;       /* window left/right padding */
    static const int PAD_Y = 8;       /* window top/bottom padding */
    static const int TITLE_H = 16;    /* header row, 8x8 font at 2x */
    static const int HDR_GAP = 4;     /* gap around the header divider */
    static const int ROW_H = 20;      /* one ROM row */
    static const int VISIBLE_ROWS = 8;/* rows fitting into the list area */
    static const int FOOTER_H = 16;   /* bottom hint/status row */

    /* Left pane: the ROM list. Right pane: the preview of the
     * selected ROM (Stage 4), its bounds derived from the panel. */
    static const int LIST_W = 208;    /* row width of the ROM list */
    static const int DIV_GAP = 8;     /* list -> divider -> preview */
    static const int PREVIEW_X = PAD_X + LIST_W + DIV_GAP;
    static const int PREVIEW_W = PANEL_W - PAD_X - PREVIEW_X;
    static const int PREVIEW_Y = PAD_Y + TITLE_H + HDR_GAP + 1 + HDR_GAP;
    static const int PREVIEW_H = PANEL_H - PAD_Y - FOOTER_H - PREVIEW_Y;

    /* Preview texture: power-of-two GE dimensions; the decoded image
     * occupies its top-left preview_w x preview_h window. */
    static const int PREVIEW_TEX_W = 256;
    static const int PREVIEW_TEX_H = 256;

    /* Fixed name storage instead of a shared std::vector: the worker
     * fills it in open(), the display thread reads it in paint(). */
    static const int MAX_ROMS = 256;
    static const int NAME_LEN = 64;

    RomBrowser();

    /* Atomic: written by the worker, read by the display thread. */
    bool is_open() const { return this->open_flag.load(std::memory_order_acquire); }

    /* Worker thread: rescan the ROMS folder (fresh list on every
     * open), reset the selection to the first ROM. */
    void open(const char * rom_dir);
    /* Worker thread: ROM_BROWSER -> MAIN_MENU or GAME. */
    void close();

    /* One input step; called by the worker thread (~50 Hz) while
     * open. UP/DOWN navigate cyclically, firing on the keyup edge
     * (one press = one row), and scroll
     * the window; with an empty list every key is a no-op. X/O/START
     * edges are handled by the caller (load / back). */
    void update(unsigned pad);

    bool has_items() const { return this->count > 0; }
    /* Worker thread only: file name of the selected ROM. */
    const char * selected_name() const;

    /* Worker thread: status line shown in the footer (load error),
     * cleared on the next open(). */
    void set_error(const char * msg);

    /* Preview access for TV::draw_preview_quad(). Main thread reads
     * only; preview_w == 0 means "no preview". */
    bool has_preview() const { return this->preview_w > 0; }
    const uint32_t * preview_tex_data() const { return preview_tex; }
    int get_preview_w() const { return this->preview_w; }
    int get_preview_h() const { return this->preview_h; }
    /* Panel-local destination rectangle of the preview quad: the
     * picture is stretched over the whole right pane (full pane
     * height). */
    void get_preview_rect(int * x, int * y, int * w, int * h) const
    {
        *x = fit_x; *y = fit_y; *w = fit_w; *h = fit_h;
    }
    /* True once after a new image was decoded: the GE must see the
     * texture in main memory before sampling. Consumed by TV. */
    bool consume_preview_upload()
    {
        bool v = preview_upload;
        preview_upload = false;
        return v;
    }

    /* Rasterize the window into the popup texture. Main thread
     * only; a state change arriving from the worker while painting
     * forces one more pass. */
    void paint();

private:
    /* Worker thread: decode the preview of the selected ROM unless
     * its result (image or "no preview") is already cached. */
    void update_preview();

    std::atomic<bool> open_flag;

    /* Worker thread writes in open()/update(); the display thread
     * only reads while is_open(). */
    int count;
    int selected;
    int top;            /* first visible row (scrolling) */
    bool dir_ok;        /* ROMS folder opened successfully */
    char names[MAX_ROMS][NAME_LEN];
    char message[NAME_LEN]; /* footer status line, empty = none */
    char rom_dir[128];      /* stored by open() for the preview search */

    /* Preview state. Worker thread writes in open()/update(),
     * the display thread reads the texture and the fit rect. */
    char preview_for[NAME_LEN]; /* ROM name the cache entry belongs to;
                                 * covers negative results too, so a
                                 * missing image is never re-probed */
    int preview_w, preview_h;   /* decoded image size, 0 = none */
    int fit_x, fit_y, fit_w, fit_h; /* panel-local quad rectangle */
    bool preview_upload;        /* new image: cache writeback needed */

    alignas(16) uint32_t preview_tex[PREVIEW_TEX_W * PREVIEW_TEX_H];
};
