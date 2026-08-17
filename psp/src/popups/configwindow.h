#pragma once

#include <atomic>
#include <functional>
#include <inttypes.h>
#include "popup.h"

/*
 * CONFIG WINDOW: the PSP UI window opened from the MAIN MENU's
 * "Config" item (Stage 3). UI state machine of this port:
 *
 *     enum class UIState { GAME, MAIN_MENU, ROM_BROWSER, CONFIG };
 *
 *   CONFIG - the machine stays frozen via the Emulator pause flag
 *            (paused == true for the whole lifetime of the window:
 *            no CPU/Memory/IO/sound work, no frames published), the
 *            last Vector frame is the backdrop, the window is drawn
 *            above the dim overlay in the 480x272 UI coordinate
 *            space, VKBD is locked hidden (SELECT is ignored).
 *
 * Transitions (all in the worker thread, see main.cpp handle_input):
 *
 *   MAIN_MENU --X on Config----> CONFIG   (menu closes, still paused)
 *   CONFIG ----O/START---------> MAIN_MENU (focus back on Config)
 *
 * The window itself knows nothing about concrete parameters: it
 * renders a list of typed descriptors (ConfigParam) and applies the
 * per-type editing rules. Every value change immediately
 *   1. goes through the descriptor's set() into Options and the
 *      owning subsystem (runtime application, wired in main.cpp);
 *   2. is written back to config.ini through on_save (bound to
 *      config_set_value, which preserves comments and other keys).
 *
 * Adding a new config.ini parameter is one more ConfigParam entry in
 * the table built by main.cpp; no window logic changes.
 *
 * Threading, same split as the other popups; the texture / repaint
 * machinery comes from the Popup base class. Worker thread:
 * open()/close()/update(); main thread: needs_repaint()/paint().
 */

/* Normalized pad state passed to ConfigWindow::update(): which
 * buttons are currently held. Keeps configwindow free of pspctrl.h. */
enum {
    CFG_PAD_UP    = 0x01,
    CFG_PAD_DOWN  = 0x02,
    CFG_PAD_LEFT  = 0x04,
    CFG_PAD_RIGHT = 0x08,
};

/* Editing rules of one parameter; the window applies them without
 * knowing what the parameter actually controls. */
enum class CfgType : int {
    BOOL,       /* LEFT -> false, RIGHT -> true (clamped, no wrap) */
    LIST,       /* LEFT/RIGHT cycle through values[] */
    INTEGER,    /* LEFT/RIGHT move by step, clamped to [min, max] */
};

struct ConfigParam
{
    const char * name;    /* display name ("Border") */
    const char * key;     /* config.ini key ("border") */
    CfgType type;

    /* Current value, bound straight to Options: BOOL = 0/1,
     * LIST = index into values[], INTEGER = the raw number. */
    std::function<int()> get;
    /* Apply a new value: Options and, when the architecture allows,
     * the running subsystem (soundnik, filler, thread priority). */
    std::function<void(int)> set;

    /* LIST only: the allowed values in their exact config.ini
     * spelling, order defines the LEFT/RIGHT sequence. */
    const char * const * values;
    int value_count;

    /* INTEGER only; never left/entered outside [min, max]. */
    int min;
    int max;
    int step;             /* default 1 */
    bool hex;             /* display/save as 0xNN (priorities) */
};

class ConfigWindow : public Popup
{
public:
    /* Window size, PSP UI coordinate space (480x272); the renderer
     * centers it, leaving 16/26 px margins around. */
    static const int PANEL_W = 480;
    static const int PANEL_H = 240;

    /* Layout constants. */
    static const int PAD_X = 8;       /* window left/right padding */
    static const int PAD_Y = 8;       /* window top/bottom padding */
    static const int TITLE_H = 16;    /* header row, 8x8 font at 2x */
    static const int HDR_GAP = 4;     /* gap around the header divider */
    static const int ROW_H = 20;      /* one parameter row */
    static const int VISIBLE_ROWS = 8;/* rows fitting into the list area */
    static const int FOOTER_H = 16;   /* bottom hint row */

    static const int MAX_PARAMS = 16;
    static const int VALUE_LEN = 32;  /* formatted "< VALUE >" buffer */

    ConfigWindow();

    /* Atomic: written by the worker, read by the display thread. */
    bool is_open() const { return this->open_flag.load(std::memory_order_acquire); }

    /* main(): the parameter table lives outside the window (owned
     * by main.cpp, which binds Options and the subsystems); set once
     * before the first open(). */
    void set_params(const ConfigParam * params, int count);

    /* Worker thread: open over the current Options values,
     * selection always starts on the first parameter. */
    void open();
    /* Worker thread: CONFIG -> MAIN_MENU. */
    void close();

    /* One input step; called by the worker thread (~50 Hz) while
     * open. UP/DOWN navigate the parameters cyclically, LEFT/RIGHT
     * change the value of the selected one; every direction fires on
     * the keyup edge (one press = one step), then saves immediately.
     * O/START edges are handled by the caller (close). */
    void update(unsigned pad);

    /* Worker thread: called by update() right after every applied
     * change with the config.ini key and its new value string;
     * main.cpp binds this to config_set_value. */
    std::function<void(const char *, const char *)> on_save;

    /* Rasterize the window into the popup texture. Main thread
     * only; a state change arriving from the worker while painting
     * forces one more pass. */
    void paint();

private:
    /* Apply one LEFT (-1) / RIGHT (+1) step to the selected
     * parameter per its type rules, then save. */
    void change_value(int dir);
    /* Format the current value of p for display ("TRUE", "gaussian",
     * "0x18") into out. */
    void format_value(const ConfigParam & p, char * out, int out_len) const;
    /* Format the value string written to config.ini ("true",
     * "gaussian", "0x18", "40"). */
    void format_config_value(const ConfigParam & p, char * out, int out_len) const;

    std::atomic<bool> open_flag;

    /* Set by set_params() before the first open(); the window only
     * reads it afterwards (worker in update, display thread in
     * paint). */
    const ConfigParam * params;
    int count;

    int selected;         /* worker thread only */
    int top;              /* first visible row (scrolling) */
};
