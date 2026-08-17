#pragma once

#include <atomic>
#include <inttypes.h>
#include "popup.h"

/*
 * Map Keys window (Stage 6): per-ROM assignment of PSP buttons /
 * D-pad / analog directions to Vector-06C keys, opened from the
 * MAIN MENU "Map Keys" item.
 *
 *   MAIN_MENU --X on Map Keys--> MAP_KEYS   (menu closes, still paused)
 *   MAP_KEYS  --O/START--------> MAIN_MENU  (focus back on Map Keys;
 *                                            on close the .map is
 *                                            saved next to the ROM)
 *
 * The window lists every mapping source with its CURRENT effective
 * assignment and origin tag (DEFAULT / ROM / DISABLED). The VKBD
 * serves as the key picker and is visible only while the assignment
 * mode is active: X on a source enters it ("PRESS VECTOR KEY"), the
 * next VKBD key press becomes the assignment (handled by the caller
 * through the VKBD sink), O cancels. TRIANGLE writes an explicit
 * NONE (disabled) entry. SELECT/START are system buttons and never
 * appear as sources.
 *
 * The mapping data itself lives in the KeyMap module (keymap.h):
 * this window only edits it and shows the state. Threading is the
 * usual popup split: the worker mutates, the display thread paints.
 */

/* Normalized pad state passed to MapWindow::update(): which
 * buttons are currently held. Keeps mapwindow free of pspctrl.h. */
enum {
    MK_PAD_UP   = 0x01,
    MK_PAD_DOWN = 0x02,
};

class MapWindow : public Popup
{
public:
    /* Window size, PSP UI coordinate space (480x272); centered by
     * the renderer, leaving 16 px above and below. */
    static const int PANEL_W = 480;
    static const int PANEL_H = 240;

    /* Layout constants. */
    static const int PAD_X = 8;        /* window left/right padding */
    static const int PAD_Y = 8;        /* window top/bottom padding */
    static const int TITLE_H = 16;     /* header row, 8x8 font at 2x */
    static const int ROW_H = 19;       /* one mapping row */
    static const int VISIBLE_ROWS = 9; /* rows fitting the list area */
    static const int FOOTER_H = 16;    /* bottom hint row */

    MapWindow();

    /* Atomic: written by the worker, read by the display thread. */
    bool is_open() const { return this->open_flag.load(std::memory_order_acquire); }

    /* Worker thread: open the window for the ROM named rom_label
     * ("PUTUP.ROM"; "BOOT LOADER" when no ROM is loaded - editing
     * still works, only the .map save is skipped). */
    void open(const char * rom_label);
    /* Worker thread: MAP_KEYS -> MAIN_MENU. */
    void close();

    /* One input step; called by the worker thread (~50 Hz) while
     * open and NOT in the assignment mode (during the assignment
     * the caller feeds the pad to the VKBD instead, which is the
     * only time the VKBD is visible here). UP/DOWN move the source
     * selection cyclically, firing on the keyup edge. */
    void update(unsigned pad);

    /* Assignment mode (§14, §34): X on the selected source starts
     * it, the next VKBD key press ends it, O cancels. */
    bool is_waiting() const { return this->wait_src >= 0; }
    int waiting_src() const { return this->wait_src; }
    void start_assign();
    void cancel_assign();
    /* Worker thread: the VKBD sink reports the picked key here. */
    void assign_selected(int scancode);

    /* TRIANGLE: explicit NONE entry for the selected source (§20);
     * the default does NOT return on its own afterwards. */
    void disable_selected();

    /* Rasterize the window into the popup texture. Main thread
     * only; a state change arriving from the worker while painting
     * forces one more pass. */
    void paint();

private:
    std::atomic<bool> open_flag;

    int selected;       /* highlighted source row (worker only) */
    int top;            /* first visible row (scrolling) */
    int wait_src;       /* source waiting for a VKBD key, -1 none */
    char rom_label[64]; /* shown in the header */
};
