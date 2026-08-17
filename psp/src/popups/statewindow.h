#pragma once

#include <atomic>
#include <inttypes.h>
#include "popup.h"

/*
 * State Browser: one window behind both MAIN MENU items "Save
 * State" and "Load State" (Stage 5), opened in the corresponding
 * mode. UI state machine:
 *
 *     enum class UIState { ..., STATE_BROWSER };
 *
 *   STATE_BROWSER - the machine stays frozen via the Emulator pause
 *                   flag for the whole lifetime of the window; the
 *                   last Vector frame is the backdrop, the window is
 *                   drawn above the dim overlay like the other
 *                   popups.
 *
 * Transitions (all in the worker thread, see main.cpp handle_input):
 *
 *   MAIN_MENU --X on Save/Load State--> STATE_BROWSER (menu closes)
 *   STATE_BROWSER --O/START-----------> MAIN_MENU (focus back on the
 *                                       item it was opened from)
 *   STATE_BROWSER --X (SAVE mode)-----> serialize into the slot,
 *                                       stay in the browser
 *   STATE_BROWSER --X (LOAD mode)-----> restore the slot, close,
 *                                       straight to GAME (resumed)
 *
 * The slots form a STATE_GRID_COLS x STATE_GRID_ROWS grid; the whole
 * UI iterates over STATE_SLOTS entries only, so a different grid
 * size needs no layout rewrite. Storage lives in plain files
 * (SAVES/<ROM base name>/stateN.bin + stateN.tga, see statefile.h);
 * the directory is rescanned on every open.
 *
 * Slot thumbnails: the Vector screenshots (stateN.tga) are decoded
 * into one shared RGBA atlas (STATE_GRID_COLS x STATE_GRID_ROWS
 * tiles of THUMB_W x THUMB_H) and presented by TV as one quad per
 * occupied slot stretched over the whole cell, drawn UNDER the
 * panel quad: the panel texture keeps
 * transparent windows (Popup::C_HOLE) at the occupied cells, so the
 * picture shows through while the slot number and save date
 * rasterized into the panel land on top of the picture. The atlas
 * is rebuilt on open() and after every successful save.
 *
 * Threading split is the usual popup one: the worker mutates the
 * state (open/close/update/after_save), the display thread polls
 * needs_repaint() and paints; the thumbnail atlas follows the
 * RomBrowser preview handshake (upload flag consumed by TV).
 */

/* Grid dimensions (§1): the only constants the slot count and the
 * cell layout derive from. */
static const int STATE_GRID_COLS = 3;
static const int STATE_GRID_ROWS = 3;
static const int STATE_SLOTS = STATE_GRID_COLS * STATE_GRID_ROWS;

/* Normalized pad state passed to StateWindow::update(): which
 * buttons are currently held. Keeps statewindow free of pspctrl.h. */
enum {
    SB_PAD_UP    = 0x01,
    SB_PAD_DOWN  = 0x02,
    SB_PAD_LEFT  = 0x04,
    SB_PAD_RIGHT = 0x08,
};

class StateWindow : public Popup
{
public:
    /* Window size, PSP UI coordinate space (480x272); the renderer
     * centers it like the other popups. */
    static const int PANEL_W = 440;
    static const int PANEL_H = 220;

    /* Layout constants (same scheme as the ROM Browser). */
    static const int PAD_X = 8;       /* window left/right padding */
    static const int PAD_Y = 8;       /* window top/bottom padding */
    static const int TITLE_H = 16;    /* header row, 8x8 font at 2x */
    static const int HDR_GAP = 4;     /* gap around the header divider */
    static const int FOOTER_H = 16;   /* bottom hint/status row */
    static const int GRID_GAP = 4;    /* gap between the grid cells */

    static const int GRID_Y0 = PAD_Y + TITLE_H + HDR_GAP + 1 + HDR_GAP;
    static const int GRID_W = PANEL_W - PAD_X * 2;
    static const int GRID_H = PANEL_H - PAD_Y - FOOTER_H - GRID_Y0;
    static const int CELL_W =
        (GRID_W - (STATE_GRID_COLS - 1) * GRID_GAP) / STATE_GRID_COLS;
    static const int CELL_H =
        (GRID_H - (STATE_GRID_ROWS - 1) * GRID_GAP) / STATE_GRID_ROWS;

    /* One slot thumbnail: the Vector frame (576x288) box-shrunk 4x. */
    static const int THUMB_W = 144;
    static const int THUMB_H = 72;

    /* Atlas holding every slot tile; power-of-two GE dimensions
     * (STATE_GRID_COLS*THUMB_W x STATE_GRID_ROWS*THUMB_H fit). */
    static const int ATLAS_W = 512;
    static const int ATLAS_H = 256;

    enum Mode { MODE_SAVE, MODE_LOAD };

    StateWindow();

    /* Atomic: written by the worker, read by the display thread. */
    bool is_open() const { return this->open_flag.load(std::memory_order_acquire); }
    Mode mode() const { return this->open_mode; }

    /* Worker thread: rescan SAVES/<rom>/ (fresh slot info on every
     * open, §27); SAVE mode starts on slot 1, LOAD mode on the
     * first occupied slot (slot 1 when all are empty). */
    void open(Mode m, const char * rom_dir);
    /* Worker thread: STATE_BROWSER -> MAIN_MENU or GAME. */
    void close();

    /* One input step; called by the worker thread (~50 Hz) while
     * open. The D-pad moves the selection through the grid, firing
     * on the keyup edge and CLAMPED at the grid edges (§13: no row
     * wrap, slot 3 + RIGHT stays on slot 3). X/O/START edges are
     * handled by the caller. */
    void update(unsigned pad);

    /* 1-based slot number under the cursor. */
    int selected_slot() const { return this->selected + 1; }
    bool is_selected_occupied() const { return this->occupied[this->selected]; }

    /* Worker thread: status line shown in the footer (save/load
     * error), cleared on the next open(). */
    void set_error(const char * msg);

    /* Worker thread: refresh a slot right after a successful save
     * (new timestamp, new thumbnail box-shrunk from the just-shown
     * frame fw x fh) without rescanning the directory. */
    void after_save(int slot, uint64_t ts,
                    const uint32_t * frame, int fw, int fh);

    /* Thumbnail access for TV::draw_state_thumbs(). Main thread
     * reads only while is_open(). */
    const uint32_t * thumb_tex_data() const { return thumb_tex; }
    bool slot_has_thumb(int idx) const
    {
        return idx >= 0 && idx < STATE_SLOTS && this->thumb_w[idx] > 0;
    }
    /* Source tile of a slot inside the atlas (pixels). */
    void thumb_tile(int idx, int * u, int * v, int * w, int * h) const
    {
        *u = (idx % STATE_GRID_COLS) * THUMB_W;
        *v = (idx / STATE_GRID_COLS) * THUMB_H;
        *w = this->thumb_w[idx];
        *h = this->thumb_h[idx];
    }
    /* Panel-local destination rectangle of the slot quad: the
     * picture covers the whole cell (stretched by the GE), the slot
     * number and the save date are overlaid on top of it. */
    static void thumb_rect(int idx, int * x, int * y, int * w, int * h)
    {
        *x = PAD_X + (idx % STATE_GRID_COLS) * (CELL_W + GRID_GAP);
        *y = GRID_Y0 + (idx / STATE_GRID_COLS) * (CELL_H + GRID_GAP);
        *w = CELL_W;
        *h = CELL_H;
    }
    /* True once after the atlas was rebuilt: the GE must see it in
     * main memory before sampling. Consumed by TV. */
    bool consume_thumb_upload()
    {
        bool v = thumb_upload;
        thumb_upload = false;
        return v;
    }

    /* Rasterize the window into the popup texture. Main thread
     * only; a state change arriving from the worker while painting
     * forces one more pass. */
    void paint();

private:
    /* Worker thread: probe stateN.bin headers and decode the
     * stateN.tga thumbnails into the atlas. */
    void scan_slots();
    /* Worker thread: blit a packed tw x th image into the slot's
     * atlas tile (tga_load output is packed, the atlas has pitch). */
    void blit_tile(int idx, const uint32_t * src, int tw, int th);

    std::atomic<bool> open_flag;
    Mode open_mode;
    int selected;               /* 0-based grid index, worker only */
    char rom_dir[128];          /* SAVES/<rom> stored by open() */
    char message[64];           /* footer status line, empty = none */

    /* Slot info: worker writes in open()/after_save(), the display
     * thread reads it while is_open(). */
    bool occupied[STATE_SLOTS];
    uint64_t slot_ts[STATE_SLOTS];
    int thumb_w[STATE_SLOTS], thumb_h[STATE_SLOTS]; /* 0 = no picture */
    bool thumb_upload;          /* atlas rebuilt: cache writeback */

    alignas(16) uint32_t thumb_tex[ATLAS_W * ATLAS_H];
};
