#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <psptypes.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspaudiolib.h>
#include <pspaudio.h>
#include <psppower.h>
#include <pspthreadman.h>

#ifdef PROFILE
#include <pspprof.h>
#endif

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <ctime>

#include "memory.h"
#include "vio.h"
#include "tv.h"
#include "board.h"
#include "emulator.h"
#include "options.h"
#include "config.h"
#include "keyboard.h"
#include "vkbd.h"
#include "mainmenu.h"
#include "rombrowser.h"
#include "configwindow.h"
#include "statewindow.h"
#include "mapwindow.h"
#include "keymap.h"
#include "statefile.h"
#include "tgaload.h"
#include "8253.h"
#include "sound.h"
#include "ay.h"
#include "wav.h"
#include "util.h"
#include "debuglog.h"

#ifdef AUTOSELECT_ROM
#include "i8080.h"
#endif

PSP_MODULE_INFO("VECTOR06C", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(16 * 1024);

static int exitRequest = 0;
static std::string statusMessage;

/* Machine РУС/ЛАТ mode latch (IO::PC bit 3): the ROM toggles it on
 * every РУС/ЛАТ press; lit = Russian input mode. Written by the
 * worker thread via IO::onruslat, read by the main thread for the
 * VKBD LED. */
static bool vector_ruslat = false;

static const char ROM_DIR[] = "ms0:/PSP/GAME/VECTOR06C/ROMS";

/* Sound Mode list values, exactly as spelled in config.ini and as
 * implemented by sound_filters (SoundMode enum order). */
static const char * const SOUND_MODE_VALUES[] = {
    "none", "cubic", "gaussian", "sinc",
};

/* The Config window edits main_priority from the worker thread, but
 * only a thread itself may change its own priority: the main loop
 * applies the new value on its next pass. */
static std::atomic<bool> main_prio_pending(false);

int exitCallback(int arg1, int arg2, void *common)
{
    exitRequest = 1;
    sceKernelExitGame();
    return 0;
}

int callbackThread(SceSize args, void *argp)
{
    int cbid = sceKernelCreateCallback("Exit Callback", exitCallback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

int setupCallbacks(void)
{
    int thid = sceKernelCreateThread("update_thread", callbackThread,
                                     0x11, 0xFA0, 0, 0);
    if (thid >= 0)
    {
        sceKernelStartThread(thid, 0, 0);
    }
    return thid;
}

/* PSP buttons -> normalized VKBD pad state (which buttons are held) */
static unsigned vkbd_padmask(uint32_t buttons)
{
    unsigned pad = 0;
    if (buttons & PSP_CTRL_LEFT)   pad |= VKBD_PAD_LEFT;
    if (buttons & PSP_CTRL_RIGHT)  pad |= VKBD_PAD_RIGHT;
    if (buttons & PSP_CTRL_UP)     pad |= VKBD_PAD_UP;
    if (buttons & PSP_CTRL_DOWN)   pad |= VKBD_PAD_DOWN;
    if (buttons & PSP_CTRL_CROSS)  pad |= VKBD_PAD_PRESS;
    return pad;
}

/* PSP buttons -> normalized MAIN MENU pad state (which buttons are
 * held) */
static unsigned menu_padmask(uint32_t buttons)
{
    unsigned pad = 0;
    if (buttons & PSP_CTRL_UP)     pad |= MENU_PAD_UP;
    if (buttons & PSP_CTRL_DOWN)   pad |= MENU_PAD_DOWN;
    if (buttons & PSP_CTRL_CROSS)  pad |= MENU_PAD_PRESS;
    return pad;
}

/* PSP buttons -> normalized ROM Browser pad state (which buttons
 * are held) */
static unsigned rb_padmask(uint32_t buttons)
{
    unsigned pad = 0;
    if (buttons & PSP_CTRL_UP)   pad |= RB_PAD_UP;
    if (buttons & PSP_CTRL_DOWN) pad |= RB_PAD_DOWN;
    return pad;
}

/* PSP buttons -> normalized Config window pad state (which buttons
 * are held) */
static unsigned cfg_padmask(uint32_t buttons)
{
    unsigned pad = 0;
    if (buttons & PSP_CTRL_UP)    pad |= CFG_PAD_UP;
    if (buttons & PSP_CTRL_DOWN)  pad |= CFG_PAD_DOWN;
    if (buttons & PSP_CTRL_LEFT)  pad |= CFG_PAD_LEFT;
    if (buttons & PSP_CTRL_RIGHT) pad |= CFG_PAD_RIGHT;
    return pad;
}

/* PSP buttons -> normalized State Browser pad state (which buttons
 * are held) */
static unsigned sb_padmask(uint32_t buttons)
{
    unsigned pad = 0;
    if (buttons & PSP_CTRL_UP)    pad |= SB_PAD_UP;
    if (buttons & PSP_CTRL_DOWN)  pad |= SB_PAD_DOWN;
    if (buttons & PSP_CTRL_LEFT)  pad |= SB_PAD_LEFT;
    if (buttons & PSP_CTRL_RIGHT) pad |= SB_PAD_RIGHT;
    return pad;
}

/* PSP buttons -> normalized Map Keys pad state (which buttons are
 * held) */
static unsigned mk_padmask(uint32_t buttons)
{
    unsigned pad = 0;
    if (buttons & PSP_CTRL_UP)   pad |= MK_PAD_UP;
    if (buttons & PSP_CTRL_DOWN) pad |= MK_PAD_DOWN;
    return pad;
}

/* The current PSP input expressed as KeyMap source bits: the 10
 * digital buttons plus the analog nub digitized into 4 independent
 * digital directions (deadzone 64 around the 128 center, the same
 * "never flicker" philosophy the rest of the port uses). */
static unsigned psp_source_mask(uint32_t buttons, const SceCtrlData & pad)
{
    unsigned m = 0;
    if (buttons & PSP_CTRL_UP)       m |= 1u << MAP_SRC_UP;
    if (buttons & PSP_CTRL_DOWN)     m |= 1u << MAP_SRC_DOWN;
    if (buttons & PSP_CTRL_LEFT)     m |= 1u << MAP_SRC_LEFT;
    if (buttons & PSP_CTRL_RIGHT)    m |= 1u << MAP_SRC_RIGHT;
    if (buttons & PSP_CTRL_CROSS)    m |= 1u << MAP_SRC_CROSS;
    if (buttons & PSP_CTRL_CIRCLE)   m |= 1u << MAP_SRC_CIRCLE;
    if (buttons & PSP_CTRL_TRIANGLE) m |= 1u << MAP_SRC_TRIANGLE;
    if (buttons & PSP_CTRL_SQUARE)   m |= 1u << MAP_SRC_SQUARE;
    if (buttons & PSP_CTRL_LTRIGGER) m |= 1u << MAP_SRC_L;
    if (buttons & PSP_CTRL_RTRIGGER) m |= 1u << MAP_SRC_R;

    const int dx = (int)pad.Lx - 128;
    const int dy = (int)pad.Ly - 128;
    if (dy <= -64) m |= 1u << MAP_SRC_ANA_UP;
    if (dy >=  64) m |= 1u << MAP_SRC_ANA_DOWN;
    if (dx <= -64) m |= 1u << MAP_SRC_ANA_LEFT;
    if (dx >=  64) m |= 1u << MAP_SRC_ANA_RIGHT;
    return m;
}

/* Drop the disabled sources: keep only what the effective mapping
 * table actually turns into a Vector key. */
static unsigned mapped_source_mask(unsigned src_mask)
{
    unsigned m = 0;
    for (int src = 0; src < MAP_SRC_COUNT; ++src) {
        if ((src_mask & (1u << src)) && KeyMap::effective_key(src) >= 0)
            m |= 1u << src;
    }
    return m;
}

/* Release every PSP source currently held as its mapped Vector key.
 * Used whenever the pad stops reaching the machine (VKBD opens, MAIN
 * MENU opens), or that key would stay pressed forever. */
static void release_held_vector_keys(Emulator & lator, unsigned src_mask)
{
    const unsigned fed = mapped_source_mask(src_mask);
    for (int src = 0; src < MAP_SRC_COUNT; ++src) {
        if (fed & (1u << src))
            lator.keyup(KeyMap::effective_key(src));
    }
}

/* SAVE flow (Stage 5): serialize the paused machine into the
 * selected slot (stateN.bin, safe tmp+rename overwrite) and write
 * the screenshot of the frame currently on screen next to it
 * (stateN.tga). Runs in the worker thread while the machine is
 * paused, so the Board cannot change mid-serialize (§35). The
 * window stays open; the slot is refreshed in place. */
static void state_save_action(Emulator & lator, TV & tv, StateWindow & sb)
{
    /* The Vector frame as 0xAABBGGRR pixels: the pure machine
     * picture, no UI layer ever reaches these buffers (§9). Sized
     * for the full 576x288 frame (Options.screen_width/height). */
    static uint32_t shot[576 * 288];

    const std::string dir = StateFile::rom_dir(lator.get_rom_base());
    const int slot = sb.selected_slot();

    if (!StateFile::ensure_dir(dir)) {
        sb.set_error("Cannot create saves dir");
        dbglog("UI: save failed, cannot create %s\n", dir.c_str());
        return;
    }

    /* The file IO below is visibly slow on real hardware; show the
     * in-progress status while it runs. The display thread repaints
     * the footer independently of this blocked worker thread; every
     * exit path below replaces the message (result or error). */
    sb.set_status("Saving...");

    std::vector<uint8_t> payload;
    lator.save_state(payload);

    const uint64_t now = (uint64_t)time(nullptr);
    if (!StateFile::save(dir, slot, payload, now)) {
        sb.set_error("Save failed");
        return;
    }

    const int fw = Options.screen_width;
    const int fh = Options.screen_height;
    tv.copy_latest_rgb(shot);
    if (!tga_save(StateFile::shot_path(dir, slot).c_str(), shot, fw, fh))
        dbglog("UI: screenshot write failed (slot %d)\n", slot);

    /* The state itself is already saved; a missing screenshot is
     * not fatal (the slot simply shows no picture next time). */
    sb.after_save(slot, now, shot, fw, fh);
    dbglog("UI: state saved into slot %d\n", slot);
}

/* LOAD flow: restore the selected slot into the paused machine.
 * Every refusal (empty slot, missing/corrupt file, unknown version,
 * Board rejecting the payload) leaves the machine untouched and the
 * window open with a footer message (§23). True on success; the
 * caller then closes everything and resumes. */
static bool state_load_action(Emulator & lator, StateWindow & sb)
{
    if (!sb.is_selected_occupied()) {
        sb.set_error("Empty slot");
        return false;
    }

    const std::string dir = StateFile::rom_dir(lator.get_rom_base());
    std::vector<uint8_t> payload;
    uint64_t ts = 0;
    if (!StateFile::load(dir, sb.selected_slot(), payload, ts)) {
        sb.set_error("Invalid state");
        return false;
    }

    if (!lator.restore_state(payload)) {
        sb.set_error("Invalid state");
        dbglog("UI: restore refused (slot %d)\n", sb.selected_slot());
        return false;
    }
    return true;
}

/* SAVE PREVIEW flow: write the frame currently on screen next to
 * the loaded ROM file ("<rom base>.tga", the exact name the ROM
 * Browser preview lookup uses, FileList::findPreview). Same pure
 * machine picture as the state screenshots: the UI layers exist
 * only in the GE list, copy_latest_rgb never sees them. Runs in the
 * worker thread while the machine is paused. The boot loader has no
 * ROM file behind it (rom_path empty), so for it the item is a
 * no-op. */
static void save_preview_action(Emulator & lator, TV & tv, MainMenu & menu)
{
    const std::string & rom_path = lator.get_rom_path();
    if (rom_path.empty()) {
        dbglog("UI: Save Preview skipped, no ROM loaded\n");
        return;
    }

    /* "<dir>/<base>.tga": the directory and the base name of the
     * ROM file, extension dropped (same rule as findPreview). */
    const size_t slash = rom_path.find_last_of('/');
    const std::string dir = (slash == std::string::npos)
        ? std::string(".") : rom_path.substr(0, slash);
    std::string base = (slash == std::string::npos)
        ? rom_path : rom_path.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0)
        base = base.substr(0, dot);
    const std::string preview = dir + "/" + base + ".tga";

    static uint32_t shot[576 * 288];
    const int fw = Options.screen_width;
    const int fh = Options.screen_height;
    tv.copy_latest_rgb(shot);

    /* The TGA write is visibly slow on real hardware; show the
     * in-progress status in the menu title while it runs. The
     * display thread repaints the panel independently of this
     * blocked worker thread. */
    menu.set_status("Saving...");

    if (tga_save(preview.c_str(), shot, fw, fh))
        dbglog("UI: preview saved: %s\n", preview.c_str());
    else
        dbglog("UI: preview write failed: %s\n", preview.c_str());

    menu.set_status("");
}

/* "<dir>/<base>.map": next to the ROM file, extension dropped — the
 * same derivation rule as the preview (save_preview_action). */
static std::string map_path_for_rom(const std::string & rom_path)
{
    const size_t slash = rom_path.find_last_of('/');
    const std::string dir = (slash == std::string::npos)
        ? std::string(".") : rom_path.substr(0, slash);
    std::string base = (slash == std::string::npos)
        ? rom_path : rom_path.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0)
        base = base.substr(0, dot);
    return dir + "/" + base + ".map";
}

/* ROM isolation: every ROM load starts from the Default Mapping and
 * then applies the ROM's own .map on top; a ROM without a file
 * simply runs the defaults. Malformed files never fail the load,
 * unknown lines are ignored. */
static void apply_rom_mapping(const std::string & rom_path)
{
    KeyMap::reset_to_default();
    if (rom_path.empty())
        return;
    const std::string path = map_path_for_rom(rom_path);
    if (KeyMap::load_file(path))
        dbglog("UI: key mapping applied from %s\n", path.c_str());
}

/* MAP_KEYS -> MAIN_MENU: save only the differences next to the ROM
 * (fully default -> the .map is deleted), return the focus to the
 * Map Keys item. The VKBD is only visible during the assignment
 * mode, so normally it is already hidden here; guard anyway.
 * The boot loader has no ROM file behind it (rom_path empty): its
 * edits stay session-local and never reach the disk. */
static void mapkey_close_action(Emulator & lator, MapWindow & mapk,
                                VirtualKeyboard & vkbd, MainMenu & menu,
                                unsigned vkbd_pad)
{
    const std::string & rom_path = lator.get_rom_path();
    if (!rom_path.empty())
        KeyMap::save_or_cleanup(map_path_for_rom(rom_path));
    if (vkbd.is_visible())
        vkbd.hide(vkbd_pad);
    mapk.close();
    menu.open(MainMenu::ITEM_MAP_KEYS);
    dbglog("UI: Map Keys closed, back to MAIN MENU\n");
}

/* Map PSP buttons to Vector-06C keycodes and drive the PSP UI layer
 * (MAIN MENU / ROM Browser / Config / State Browser / VKBD). Runs in the worker
 * thread (Emulator::on_frame_input) once per machine frame, so
 * rendering stalls in the display thread cannot delay button
 * handling; while the machine is paused the worker calls this at the
 * same rate, so the UI stays operable.
 *
 * UI state (independent of the Board state):
 *   GAME        - pad feeds the Vector / VKBD; START opens the menu.
 *   MAIN_MENU   - machine frozen via the pause flag; pad drives the
 *                 menu only; START/O close and resume; X on Load ROM
 *                 opens the ROM Browser, X on Config the Config
 *                 window, X on Exit requests a clean shutdown.
 *   ROM_BROWSER - machine frozen via the pause flag; pad drives the
 *                 list; X loads the selected ROM and resumes, O/START
 *                 return to the MAIN MENU.
 *   CONFIG      - machine frozen via the pause flag; pad edits the
 *                 config.ini parameters; O/START return to the MAIN
 *                 MENU (focus back on Config).
 *   STATE_BROWSER - machine frozen via the pause flag; pad moves the
 *                 slot selection (clamped grid); X saves into /
 *                 restores the selected slot per the window mode;
 *                 O/START return to the MAIN MENU (focus back on
 *                 Save/Load State).
 *   MAP_KEYS    - machine frozen via the pause flag; pad moves the
 *                 source selection; X starts the assignment mode
 *                 (the always-visible VKBD becomes the key picker,
 *                 O cancels), TRIANGLE writes an explicit NONE;
 *                 O/START save the .map next to the ROM and return
 *                 to the MAIN MENU (focus back on Map Keys). */
void handle_input(Emulator & lator, Keyboard & keyboard,
                  VirtualKeyboard & vkbd, MainMenu & menu,
                  RomBrowser & browser, ConfigWindow & cfg,
                  StateWindow & sb, MapWindow & mapk, TV & tv)
{
    SceCtrlData pad;
    sceCtrlReadBufferPositive(&pad, 1);

    static uint32_t oldButtons = 0;
    /* GAME only: which mapping sources fed their Vector key in the
     * previous frame (edge detection for the mapping-driven feed).
     * Every popup branch clears it: leaving GAME always releases the
     * held keys first. */
    static unsigned old_mapped = 0;
    uint32_t buttons = pad.Buttons;
    uint32_t pressed = buttons & ~oldButtons;
    uint32_t released = oldButtons & ~buttons;

    dbglog("buttons=%08X pressed=%08X\n", buttons, pressed);

    if (sb.is_open()) {
        /* STATE_BROWSER state: the D-pad moves the slot selection
         * around the grid (clamped at the edges, never wraps), X
         * saves into / restores the selected slot per the window
         * mode, O/START return to the MAIN MENU with the focus back
         * on the item the window was opened from. The machine stays
         * paused the whole time, so the serialize/deserialize in
         * the save/load actions cannot race with a running frame
         * (§35); SELECT is ignored, the VKBD stays locked and
         * hidden. */
        sb.update(sb_padmask(buttons));

        if (pressed & (PSP_CTRL_START | PSP_CTRL_CIRCLE)) {
            const int focus = (sb.mode() == StateWindow::MODE_SAVE)
                ? MainMenu::ITEM_SAVE_STATE : MainMenu::ITEM_LOAD_STATE;
            sb.close();
            menu.open(focus);
            dbglog("UI: State Browser closed, back to MAIN MENU\n");
        } else if ((pressed & PSP_CTRL_CROSS)
                && sb.mode() == StateWindow::MODE_SAVE) {
            state_save_action(lator, tv, sb);
        } else if ((pressed & PSP_CTRL_CROSS)
                && state_load_action(lator, sb)) {
            /* Restored successfully: straight to GAME (resumed),
             * never back through the MAIN MENU. */
            sb.close();
            menu.close();
            lator.resume();
            dbglog("UI: state restored, GAME resumed\n");
        }

        old_mapped = 0;
        oldButtons = buttons;
        return;
    }

    if (cfg.is_open()) {
        /* CONFIG state: UP/DOWN select the parameter (cyclic, with
         * autorepeat), LEFT/RIGHT edit the selected value per its
         * type rules; every change lands in Options and config.ini
         * immediately. O/START return to the MAIN MENU, SELECT is
         * ignored so the VKBD stays locked and hidden, the machine
         * stays paused the whole time. */
        cfg.update(cfg_padmask(buttons));

        if (pressed & (PSP_CTRL_START | PSP_CTRL_CIRCLE)) {
            cfg.close();
            /* The menu focus returns to the Config item. */
            menu.open(MainMenu::ITEM_CONFIG);
            dbglog("UI: Config closed, back to MAIN MENU\n");
        }

        old_mapped = 0;
        oldButtons = buttons;
        return;
    }

    if (mapk.is_open()) {
        /* MAP_KEYS state: the machine stays paused the whole time.
         * Normal mode: UP/DOWN move the source selection, X starts
         * the assignment, TRIANGLE writes an explicit NONE, O/START
         * save the .map and return to the MAIN MENU. Assignment
         * mode: the pad drives the VKBD (the key picker); the sink
         * interception (main) lands the picked key, O cancels.
         * SELECT is a system button and is ignored (§32). */
        if (mapk.is_waiting()) {
            /* Assignment mode: the VKBD is visible exactly for its
             * lifetime and serves as the key picker. */
            vkbd.update(vkbd_padmask(buttons));
            if (pressed & PSP_CTRL_CIRCLE) {
                mapk.cancel_assign();
                vkbd.hide(vkbd_padmask(buttons));
                dbglog("UI: key assignment cancelled\n");
            } else if (!mapk.is_waiting()) {
                /* The VKBD sink landed the picked key this very
                 * frame; the picker is no longer needed. */
                vkbd.hide(vkbd_padmask(buttons));
            }
        } else {
            mapk.update(mk_padmask(buttons));

            if (pressed & (PSP_CTRL_START | PSP_CTRL_CIRCLE)) {
                mapkey_close_action(lator, mapk, vkbd, menu,
                                    vkbd_padmask(buttons));
            } else if (pressed & PSP_CTRL_CROSS) {
                mapk.start_assign();
                /* The VKBD appears only now, as the key picker
                 * (§14); the rest of the time it stays hidden and
                 * never covers the source list. */
                vkbd.show(vkbd_padmask(buttons));
                dbglog("UI: key assignment started (src %d)\n",
                       mapk.waiting_src());
            } else if (pressed & PSP_CTRL_TRIANGLE) {
                mapk.disable_selected();
                dbglog("UI: source disabled (NONE)\n");
            }
        }

        old_mapped = 0;
        oldButtons = buttons;
        return;
    }

    if (browser.is_open()) {
        /* ROM Browser state: UP/DOWN navigate the list (cyclic,
         * scrolled), X loads the selected ROM through
         * Emulator::load_rom and goes straight to GAME, O/START go
         * back to the MAIN MENU. The machine stays paused the whole
         * time; nothing reaches the Vector, SELECT does not open the
         * VKBD: it stays locked and hidden. */
        browser.update(rb_padmask(buttons));

        if (pressed & (PSP_CTRL_START | PSP_CTRL_CIRCLE)) {
            browser.close();
            /* The menu selection resets to the first item (Load
             * ROM) on every open. */
            menu.open();
            dbglog("UI: ROM Browser closed, back to MAIN MENU\n");
        } else if ((pressed & PSP_CTRL_CROSS) && browser.has_items()) {
            char path[256];
            snprintf(path, sizeof(path), "%s/%s",
                     ROM_DIR, browser.selected_name());
            if (lator.load_rom(path)) {
                /* ROM Browser -> GAME with the new ROM; the old
                 * board state was replaced by the LOADROM reset.
                 * The mapping starts from the defaults plus this
                 * ROM's own .map (ROM isolation). */
                apply_rom_mapping(path);
                browser.close();
                menu.close();
                lator.resume();
                dbglog("UI: ROM loaded, GAME resumed\n");
            } else {
                /* Keep the browser open with the error in the
                 * footer; the old ROM state is untouched. */
                browser.set_error("Failed to load ROM");
            }
        }

        old_mapped = 0;
        oldButtons = buttons;
        return;
    }

    if (menu.is_open()) {
        /* MAIN MENU state: the D-pad navigates the items (cyclic),
         * X on Load ROM opens the ROM Browser, START/O close the
         * menu and resume the machine. Nothing reaches the Vector,
         * SELECT does not open the VKBD: it stays locked until
         * GAME. */
        menu.update(menu_padmask(buttons));

        if (pressed & (PSP_CTRL_START | PSP_CTRL_CIRCLE)) {
            menu.close();
            lator.resume();
            dbglog("UI: MAIN MENU closed, machine resumed\n");
        } else if ((pressed & PSP_CTRL_CROSS)
                && menu.selected_item() == MainMenu::ITEM_LOAD_ROM) {
            /* MAIN MENU -> ROM Browser: the browser replaces the
             * menu (never stacked under it), the machine stays
             * paused, the VKBD stays hidden. */
            menu.close();
            browser.open(ROM_DIR);
            dbglog("UI: ROM Browser opened, machine stays paused\n");
        } else if ((pressed & PSP_CTRL_CROSS)
                && menu.selected_item() == MainMenu::ITEM_SAVE_PREVIEW) {
            /* Save the current screen as the ROM Browser preview of
             * the running ROM; the menu stays open, the machine
             * stays paused. Silently ignored for the boot loader. */
            save_preview_action(lator, tv, menu);
        } else if ((pressed & PSP_CTRL_CROSS)
                && menu.selected_item() == MainMenu::ITEM_SAVE_STATE) {
            /* MAIN MENU -> State Browser (SAVE mode): the window
             * replaces the menu, the machine stays paused, the VKBD
             * stays hidden. */
            menu.close();
            sb.open(StateWindow::MODE_SAVE,
                    StateFile::rom_dir(lator.get_rom_base()).c_str());
            dbglog("UI: State Browser opened (SAVE), machine stays paused\n");
        } else if ((pressed & PSP_CTRL_CROSS)
                && menu.selected_item() == MainMenu::ITEM_LOAD_STATE) {
            /* MAIN MENU -> State Browser (LOAD mode), same
             * replacement scheme. */
            menu.close();
            sb.open(StateWindow::MODE_LOAD,
                    StateFile::rom_dir(lator.get_rom_base()).c_str());
            dbglog("UI: State Browser opened (LOAD), machine stays paused\n");
        } else if ((pressed & PSP_CTRL_CROSS)
                && menu.selected_item() == MainMenu::ITEM_CONFIG) {
            /* MAIN MENU -> Config window, same replacement scheme:
             * the machine stays paused, the VKBD stays hidden. */
            menu.close();
            cfg.open();
            dbglog("UI: Config opened, machine stays paused\n");
        } else if ((pressed & PSP_CTRL_CROSS)
                && menu.selected_item() == MainMenu::ITEM_MAP_KEYS) {
            /* MAIN MENU -> Map Keys window: the machine stays
             * paused; the VKBD stays hidden until the assignment
             * mode starts, so it never covers the source list. */
            menu.close();
            const std::string & rom_path = lator.get_rom_path();
            std::string label = "BOOT LOADER";
            if (!rom_path.empty()) {
                const size_t slash = rom_path.find_last_of('/');
                label = (slash == std::string::npos)
                    ? rom_path : rom_path.substr(slash + 1);
            }
            mapk.open(label.c_str());
            dbglog("UI: Map Keys opened (%s), machine stays paused\n",
                   label.c_str());
        } else if ((pressed & PSP_CTRL_CROSS)
                && menu.selected_item() == MainMenu::ITEM_EXIT) {
            /* Exit: request a clean shutdown; the main loop breaks,
             * stops the worker, finalizes the diagnostic recordings
             * and calls sceKernelExitGame(). */
            exitRequest = 1;
            dbglog("UI: Exit selected, shutting down\n");
        }

        old_mapped = 0;
        oldButtons = buttons;
        return;
    }

    /* GAME state.
     * START is the system button: hide the VKBD if it is open, set
     * the pause flag and open the MAIN MENU over the current
     * picture. It never reaches the Vector and never exits the app;
     * exiting will go through the menu's Exit item later. */
    if (pressed & PSP_CTRL_START) {
        if (vkbd.is_visible()) {
            /* releases every active virtual key, incl. a held X;
             * the top/bottom position survives for the next
             * SELECT. */
            vkbd.hide(vkbd_padmask(buttons));
        } else {
            /* Whatever physical buttons are held right now must not
             * stay pressed while (and after) the pause. */
            release_held_vector_keys(lator, psp_source_mask(buttons, pad));
        }
        menu.open();
        lator.pause();
        dbglog("UI: MAIN MENU opened, machine paused\n");
        old_mapped = 0;
        oldButtons = buttons;
        return;
    }

    /* SELECT: toggle the on-screen keyboard (used to be reset). The
     * keyboard keeps its top/bottom position across hide/show. */
    if (pressed & PSP_CTRL_SELECT) {
        if (vkbd.is_visible()) {
            /* releases every active virtual key, incl. a held X */
            vkbd.hide(vkbd_padmask(buttons));
        } else {
            /* While the VKBD is open no PSP button reaches the
             * Vector; release whatever is held right now or that
             * key would stay pressed forever. */
            release_held_vector_keys(lator, psp_source_mask(buttons, pad));

            vkbd.show(vkbd_padmask(buttons));
        }
    }

    if (vkbd.is_visible()) {
        /* Nothing reaches the emulator while the VKBD is open.
         * The D-pad navigates the keyboard (with autorepeat) and X
         * presses the selected key. */
        vkbd.update(vkbd_padmask(buttons));

        /* O: move the keyboard top <-> bottom. */
        if (pressed & PSP_CTRL_CIRCLE) {
            vkbd.move();
        }
        old_mapped = 0;
    } else {
        /* Mapping-driven feed: every held source goes through the
         * effective table (defaults plus the ROM's .map); disabled
         * sources do nothing. Edges are detected per source, so a
         * changed mapping applies from the next frame with no ROM
         * reload, and several PSP buttons may feed one Vector key. */
        const unsigned mapped_now =
            mapped_source_mask(psp_source_mask(buttons, pad));
        const unsigned down_edges = mapped_now & ~old_mapped;
        const unsigned up_edges = old_mapped & ~mapped_now;
        for (int src = 0; src < MAP_SRC_COUNT; ++src) {
            if (down_edges & (1u << src))
                lator.keydown(KeyMap::effective_key(src));
            if (up_edges & (1u << src))
                lator.keyup(KeyMap::effective_key(src));
        }
        old_mapped = mapped_now;
    }

    oldButtons = buttons;
}

int main(int argc, char *argv[])
{
    dbglog_open();
    dbglog("=== VECTOR06C PSP start ===\n");

    pspDebugScreenInit();
    pspDebugScreenSetBackColor(0x00000000);
    pspDebugScreenSetTextColor(0xFFFFFFFF);
    pspDebugScreenClearLineDisable();

    /* Highest clock first; step down until the firmware accepts one.
     * The return value matters: when every attempt is rejected the
     * PSP silently stays at its default 222 MHz and the emulator
     * runs ~1.5x slower (PPSSPP accepts anything, so it looks fine
     * there). The readback below shows what actually took effect. */
    {
        static const struct { int cpu; int bus; } clocks[] = {
            { 333, 166 }, { 300, 150 }, { 266, 133 }, { 222, 111 },
        };
        int rc = -1;
        for (size_t i = 0; i < sizeof(clocks) / sizeof(clocks[0]); ++i) {
            rc = scePowerSetClockFrequency(
                clocks[i].cpu, clocks[i].cpu, clocks[i].bus);
            if (rc == 0) {
                break;
            }
            dbglog("clock %d/%d MHz rejected (rc=%d)\n",
                   clocks[i].cpu, clocks[i].bus, rc);
        }
        const int cpu_mhz = scePowerGetCpuClockFrequencyInt();
        const int bus_mhz = scePowerGetBusClockFrequencyInt();
        printf("CPU clock: %d MHz, bus %d MHz (set rc=%d)\n",
               cpu_mhz, bus_mhz, rc);
        dbglog("CPU clock: %d MHz, bus %d MHz (set rc=%d)\n",
               cpu_mhz, bus_mhz, rc);
    }

    dbglog("Vector-06c PSP starting...\n");

    /* config.ini next to the EBOOT (border / fps options); the path
     * is kept: the Config window writes every edited value straight
     * back into this file (config_set_value). */
    const std::string config_file = config_load(argv[0]);

    /* Display (main) thread priority from config.ini. Changing it
     * before anything spawns also covers the ROM browser phase. */
    {
        const int rc = sceKernelChangeThreadPriority(
            0, Options.main_priority);
        dbglog("main thread priority set to 0x%02x (rc=%d)\n",
               Options.main_priority, rc);
    }

    setupCallbacks();
    /* Analog mode: the digital buttons keep reporting exactly as
     * before, plus the nub becomes readable; the Map Keys module
     * digitizes it into 4 independent digital directions. */
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    /* No ROM browser at startup: the machine boots straight into the
     * boot ROM and the MAIN MENU opens over its picture after a
     * short delay, as if START was pressed (Emulator::on_first_frame
     * + the worker's auto-open timer). The ROMS scan itself is done
     * by the FileList helper (filelist.h) used by the RomBrowser. */
    char selected_file[128] = "";

#ifdef AUTOSELECT_ROM
    /* Test build skips the browser: autoselect.txt next to the EBOOT
     * holds the file name of the ROM to boot from ROMS/. */
    FILE * as = std::fopen("autoselect.txt", "r");
    if (as != nullptr) {
        if (std::fgets(selected_file, sizeof(selected_file), as)
                != nullptr) {
            /* fgets keeps the line ending; it would end up inside
             * the ROM path. */
            size_t n = std::strlen(selected_file);
            while (n > 0 &&
                   (selected_file[n - 1] == '\n' ||
                    selected_file[n - 1] == '\r' ||
                    selected_file[n - 1] == ' '  ||
                    selected_file[n - 1] == '\t')) {
                selected_file[--n] = '\0';
            }
        }
        std::fclose(as);
    }
#endif

#ifdef PROFILE
    gprof_start();
#endif

    /* --- Emulator objects initialization (after PSP env is ready) --- */
    /* NOTE: Large objects (Memory ~640KB, Debug ~9.4MB) must be allocated
     * on the heap, NOT on the stack (PSP stack is only ~256KB). */

    dbglog("Старт main()...\n");
    dbglog("Инициализирую Memory... ");
    Memory* memory = new Memory();
    dbglog("OK\n");

    dbglog("Инициализирую Debug... ");
    Debug* debug = new Debug(memory);
    dbglog("OK\n");

    dbglog("Инициализирую FD1793... ");
    FD1793* fdc = new FD1793();
    dbglog("OK\n");

    dbglog("Инициализирую Wav... ");
    Wav* wav = new Wav();
    dbglog("OK\n");

    dbglog("Инициализирую WavPlayer... ");
    WavPlayer* tape_player = new WavPlayer(*wav);
    dbglog("OK\n");

    dbglog("Инициализирую Keyboard... ");
    Keyboard* keyboard = new Keyboard();
    dbglog("OK\n");

    dbglog("Инициализирую I8253... ");
    I8253* timer = new I8253();
    dbglog("OK\n");

    dbglog("Инициализирую TimerWrapper... ");
    TimerWrapper* tw = new TimerWrapper(*timer);
    dbglog("OK\n");

    dbglog("Инициализирую AY... ");
    AY* ay = new AY();
    dbglog("OK\n");

    dbglog("Инициализирую AYWrapper... ");
    AYWrapper* aw = new AYWrapper(*ay);
    dbglog("OK\n");

    dbglog("Инициализирую Soundnik... ");
    Soundnik* soundnik = new Soundnik(*tw, *aw);
    dbglog("OK\n");

    dbglog("Инициализирую IO... ");
    IO* io = new IO(*memory, *keyboard, *timer, *fdc, *ay, *tape_player);
    dbglog("OK\n");

    /* Sound chip writes are queued as timestamped events and rendered in
     * batch by Soundnik::process_frame() at the end of each frame */
    io->sound_event = [soundnik](SoundEventType type, uint8_t addr,
        uint8_t value) {
        soundnik->push_event(type, addr, value);
    };

    /* The ROM flips this latch together with the input mode when the
     * РУС/ЛАТ key is pressed; the VKBD LED shows the latch. */
    io->onruslat = [](bool rus) { vector_ruslat = rus; };

    dbglog("Инициализирую TV... ");
    TV* tv = new TV();
    dbglog("OK\n");

    dbglog("Инициализирую PixelFiller... ");
    PixelFiller* filler = new PixelFiller(*memory, *io, *tv);
    dbglog("OK\n");

    dbglog("Инициализирую Board... ");
    Board* board = new Board(*memory, *io, *filler, *soundnik, *tv, *tape_player, *debug);
    dbglog("OK\n");

    dbglog("Инициализирую Emulator... ");
    Emulator* lator = new Emulator(*board);
    dbglog("OK\n");

    dbglog("Инициализирую экранную клавиатуру... ");
    /* On-screen Vector keyboard (UI layer of the main thread); its
     * virtual presses go through the same keydown/keyup queue as the
     * physical PSP buttons (the sinks are wired after the Map Keys
     * window exists, see below). */
    VirtualKeyboard* vkbd = new VirtualKeyboard();
    vkbd->prepare();
    /* The VKBD LED shows the machine РУС/ЛАТ mode latch (lit =
     * Russian input), not the keyboard's key level. */
    vkbd->set_ruslat_source(&vector_ruslat);
    dbglog("OK\n");

    dbglog("Инициализирую главное меню... ");
    /* MAIN MENU: the PSP UI layer over the machine picture. It is
     * state + texture only; the Emulator owns pause/resume and the
     * TV presents the layer. */
    MainMenu* menu = new MainMenu();
    dbglog("OK\n");

    dbglog("Инициализирую окно выбора ROM... ");
    /* ROM Browser (Stage 2): opened from the MAIN MENU's Load ROM
     * item; rescans ROMS on every open, loads through
     * Emulator::load_rom. Same state + texture split as the menu. */
    RomBrowser* browser = new RomBrowser();
    dbglog("OK\n");

    dbglog("Инициализирую окно Config... ");
    /* Config window (Stage 3): edits the config.ini parameters over
     * the paused machine picture. The window itself is
     * parameter-agnostic; the whole knowledge about the concrete
     * keys lives in this table (display order = table order):
     * get/set bind straight to Options and the owning subsystem, so
     * every change applies at runtime with no second settings copy.
     * Adding a new config.ini parameter = one more entry here. */
    ConfigWindow* cfg = new ConfigWindow();

    std::vector<ConfigParam> config_params = {
        { "Border", "border", CfgType::BOOL,
          []() { return Options.show_border ? 1 : 0; },
          [](int v) { Options.show_border = (v != 0); },
          nullptr, 0, 0, 0, 1, false },

        { "FPS", "fps", CfgType::BOOL,
          []() { return Options.show_fps ? 1 : 0; },
          [](int v) { Options.show_fps = (v != 0); },
          nullptr, 0, 0, 0, 1, false },

        { "Fast Framebuffer", "fast_framebuffer", CfgType::BOOL,
          []() { return Options.fast_framebuffer ? 1 : 0; },
          [filler](int v) {
              Options.fast_framebuffer = (v != 0);
              /* the filler caches the mode at init; push it live */
              filler->set_fast_mode(Options.fast_framebuffer);
          },
          nullptr, 0, 0, 0, 1, false },

        { "Sound Record", "sound_record", CfgType::BOOL,
          []() { return Options.sound_record ? 1 : 0; },
          [](int v) { Options.sound_record = (v != 0); },
          nullptr, 0, 0, 0, 1, false },

        { "Sound Buffer", "sound_buffer_ms", CfgType::INTEGER,
          []() { return Options.sound_buffer_ms; },
          [soundnik](int v) {
              Options.sound_buffer_ms = v;
              /* retune the playback ring target fill live */
              soundnik->set_buffer_ms(v);
          },
          nullptr, 0, 1, 150, 1, false },

        { "Sound Mode", "sound_mode", CfgType::LIST,
          []() { return (int)Options.sound_mode; },
          [soundnik](int v) {
              Options.sound_mode = (SoundMode)v;
              /* the resampler picks this enum on the next audio
               * callback; tables for every mode are prebuilt */
              soundnik->set_sound_mode((SoundMode)v);
          },
          SOUND_MODE_VALUES,
          (int)(sizeof(SOUND_MODE_VALUES) / sizeof(SOUND_MODE_VALUES[0])),
          0, 0, 1, false },

        { "Worker Priority", "worker_priority", CfgType::INTEGER,
          []() { return Options.worker_priority; },
          [](int v) {
              Options.worker_priority = v;
              /* handle_input runs in the worker thread: only a
               * thread may change its own priority, and this is
               * exactly that thread (no restart needed). */
              const int rc = sceKernelChangeThreadPriority(0, v);
              dbglog("worker thread priority set to 0x%02x (rc=%d)\n",
                     v, rc);
          },
          nullptr, 0, 0x08, 0x77, 1, true },

        { "Main Priority", "main_priority", CfgType::INTEGER,
          []() { return Options.main_priority; },
          [](int v) {
              Options.main_priority = v;
              /* the display thread applies its own new priority on
               * the next main loop pass (see main_prio_pending) */
              main_prio_pending.store(true, std::memory_order_release);
          },
          nullptr, 0, 0x08, 0x77, 1, true },
    };

    cfg->set_params(config_params.data(), (int)config_params.size());
    cfg->on_save = [config_file](const char * key, const char * value) {
        config_set_value(config_file, key, value);
    };
    dbglog("OK\n");

    dbglog("Инициализирую окно Save/Load State... ");
    /* State Browser (Stage 5): one window behind both the Save State
     * and the Load State menu items; rescans SAVES/<ROM>/ on every
     * open, saves/restores through StateFile + Emulator. Same
     * state + texture split as the other popups. */
    StateWindow* sb = new StateWindow();
    dbglog("OK\n");

    dbglog("Инициализирую окно Map Keys... ");
    /* Map Keys window (Stage 6): per-ROM assignment of PSP buttons
     * / analog directions to Vector keys over the paused machine
     * picture; the VKBD serves as the key picker. The mapping data
     * itself lives in the KeyMap module (keymap.h). */
    MapWindow* mapk = new MapWindow();
    dbglog("OK\n");

    /* VKBD virtual presses go through the same keydown/keyup queue
     * as the physical PSP buttons — except while the Map Keys
     * window waits for a key: then the first VKBD press becomes the
     * assignment instead of a machine input (§14). */
    vkbd->on_keydown = [lator, vkbd, mapk](int scancode) {
        if (mapk->is_open() && mapk->is_waiting()) {
            mapk->assign_selected(scancode);
            /* The picker key itself must not stay latched inside
             * the VKBD (sticky keys); the emitted keyups are
             * harmless no-ops for the machine, which never saw the
             * matching keydowns. */
            vkbd->release_all();
            return;
        }
        lator->keydown(scancode);
    };
    vkbd->on_keyup = [lator](int scancode) { lator->keyup(scancode); };

    /* PSP pad handling lives in the worker thread: one poll per
     * machine frame (50 Hz), independent of how fast the display
     * thread presents pictures. The same hook runs while paused so
     * the MAIN MENU / ROM Browser / Config / State Browser stay
     * operable. */
    lator->on_frame_input =
        [lator, keyboard, vkbd, menu, browser, cfg, sb, mapk, tv]() {
        handle_input(*lator, *keyboard, *vkbd, *menu, *browser, *cfg,
                     *sb, *mapk, *tv);
    };

    /* The boot ROM runs first; AUTO_MENU_OPEN_DELAY_US after the
     * worker start the MAIN MENU opens over its picture and the
     * machine freezes, as if START was pressed. No separate static
     * boot screen. The AUTOSELECT_ROM harness runs the machine
     * unattended for 60 s (perf.log / gmon.out) and must not freeze,
     * so the hook stays unwired there; the START toggle still works
     * in every build. */
#ifndef AUTOSELECT_ROM
    lator->on_auto_open_menu = [lator, menu]() {
        menu->open();
        lator->pause();
        dbglog("UI: MAIN MENU auto-opened (as if START was pressed)\n");
    };
#endif

    dbglog("Инициализирую эмулятор (options)...\n");

    /* Init options for PSP */
    options(0, NULL);

    /* Init components (like android_main.cpp) */
    filler->init();

    /* Diagnostic sound recording (config.ini: sound_record = true):
     * psp_internal.wav gets the samples right after Soundnik generates
     * them, psp_callback.wav gets what the PSP audio callback actually
     * feeds to the hardware. Files live in the working directory (the
     * game folder on the memory stick / under PPSSPP). */
    WavRecorder* rec_internal = nullptr;
    WavRecorder* rec_callback = nullptr;
    if (Options.sound_record && !Options.nosound) {
        rec_internal = new WavRecorder();
        rec_internal->init("psp_internal.wav");
        rec_callback = new WavRecorder();
        rec_callback->init("psp_callback.wav");
        printf("Sound recording enabled\n");
        printf("Internal: psp_internal.wav\n");
        printf("Callback: psp_callback.wav\n");
        dbglog("sound_record: internal=psp_internal.wav callback=psp_callback.wav\n");
    }

    soundnik->init(rec_internal, rec_callback);
    tv->init();
    board->init();
    fdc->init();
    io->yellowblue();

    keyboard->onreset = [board](bool blkvvod) {
        board->reset(blkvvod ?
                Board::ResetMode::BLKVVOD : Board::ResetMode::BLKSBR);
    };

    board->reset(Board::ResetMode::BLKVVOD);

    /* Load the selected ROM (AUTOSELECT_ROM test builds only; the
     * release build boots the boot ROM and loads through the ROM
     * Browser). Runs before the worker starts, so the Board is still
     * owned by the main thread here. */
    if (selected_file[0] != '\0') {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", ROM_DIR, selected_file);
        lator->load_rom(path);
        apply_rom_mapping(path);
    }

    /* ROM is loaded, Board is ready: start the emulation worker.
     * From here on the main thread only polls input, renders with GU
     * and waits for vblank; the Board itself belongs to the worker. */
    lator->start_emulator_thread();

    /* --- Main emulation loop --- */
    dbglog("Running...\n");
    dbglog("entering main loop\n");
    int dbg_frame = 0;

    /* Frame counter */
    unsigned int fps_frames = 0;
    unsigned int fps_last_time = sceKernelGetSystemTimeLow();
#ifdef AUTOSELECT_ROM
    unsigned int auto_start_us = 0;
#endif

    while (!exitRequest) {
        /* PSP pad handling runs in the worker thread now (see
         * lator->on_frame_input); the display thread only paints. */

        /* The Config window edits main_priority from the worker
         * thread, but only the display thread itself may change its
         * own priority: pick the pending value up here. */
        if (main_prio_pending.exchange(false)) {
            const int rc = sceKernelChangeThreadPriority(
                0, Options.main_priority);
            dbglog("main thread priority set to 0x%02x (rc=%d)\n",
                   Options.main_priority, rc);
        }

        /* Re-rasterize the VKBD overlay texture only when its visual
         * state changed (selection, pressed keys, РУС/LAT). Hidden
         * keyboard: zero cost. */
        if (vkbd->is_visible() && vkbd->needs_repaint()) {
            vkbd->paint();
        }

        /* MAIN MENU panel texture, same scheme: repaint only on a
         * selection change, otherwise the old texture is reused. */
        if (menu->is_open() && menu->needs_repaint()) {
            menu->paint();
        }

        /* ROM Browser window texture, same scheme: repaint only on
         * a visible state change (selection, scroll, error). */
        if (browser->is_open() && browser->needs_repaint()) {
            browser->paint();
        }

        /* Config window texture, same scheme: repaint only on a
         * visible state change (selection, edited value). */
        if (cfg->is_open() && cfg->needs_repaint()) {
            cfg->paint();
        }

        /* State Browser window texture, same scheme: repaint only
         * on a visible state change (selection, slot info,
         * footer message). */
        if (sb->is_open() && sb->needs_repaint()) {
            sb->paint();
        }

        /* Map Keys window texture, same scheme: repaint only on a
         * visible state change (selection, assignment mode,
         * mapping). */
        if (mapk->is_open() && mapk->needs_repaint()) {
            mapk->paint();
        }

        /* Present the newest ready frame via PSP GU; this call also
         * paces the loop at the LCD vblank. The machine frames
         * themselves run in the worker thread, independently.
         * Layers: Vector frame, dim backdrop + State Browser or
         * Config or ROM Browser or MAIN MENU (when open), VKBD
         * (when visible). */
        dbglog("frame %d: tv->render...\n", dbg_frame);
#ifdef AUTOSELECT_ROM
        unsigned perf_tr0 = sceKernelGetSystemTimeLow();
#endif
        tv->render(vkbd, menu, browser, cfg, sb, mapk);
#ifdef AUTOSELECT_ROM
        board->perf_render_us += sceKernelGetSystemTimeLow() - perf_tr0;
#endif
        dbglog("frame %d: tv->render done\n", dbg_frame);

        /*if (dbg_frame == 99)
            tv->save_frame( files[selected] + ".bmp" );
        if (dbg_frame == 100)
            memory->save_dump( files[selected] + ".dump");*/

#ifdef AUTOSELECT_ROM
        if (dbg_frame == 600) {
            tv->save_frame("frame_t600.bmp");
            memory->save_dump("dump_t600.bin");
        }
        if (dbg_frame % 50 == 0) {
            dbglog("TRACE pc=%04x sp=%04x\n", i8080cpu::i8080_pc(), i8080cpu::i8080_regs_sp());
        }
#endif
        ++dbg_frame;

#ifdef AUTOSELECT_ROM
        /* Auto-stop: run the machine for exactly 60 s so the host can
         * collect a reproducible gmon.out without touching the pad. */
        if (auto_start_us == 0)
            auto_start_us = sceKernelGetSystemTimeLow();
        if ((unsigned int)(sceKernelGetSystemTimeLow() - auto_start_us)
                >= 60000000) {
#ifdef PROFILE
            gprof_stop("gmon.out", 1);
#endif
            break;
        }
#endif

        ++fps_frames;
        unsigned int now = sceKernelGetSystemTimeLow();
        if ((unsigned int)(now - fps_last_time) >= 1000000) {
            dbglog("FPS: %u\n", fps_frames);

#ifdef AUTOSELECT_ROM
            {
                FILE* pf = fopen("perf.log", "a");
                if (pf) {
                    fprintf(pf,
                        "PERF loop=%u mach=%u exec=%u.%03u snd=%u.%03u "
                        "render=%u.%03u cpu=%u.%03u fill=%u.%03u "
                        "fastfb=%u.%03u "
                        "sync=%u.%03u vbl=%u.%03u flush=%u.%03u ms\n",
                        fps_frames, board->perf_frames,
                        board->perf_exec_us / 1000, board->perf_exec_us % 1000,
                        board->perf_snd_us / 1000, board->perf_snd_us % 1000,
                        board->perf_render_us / 1000,
                        board->perf_render_us % 1000,
                        board->perf_cpu_us / 1000, board->perf_cpu_us % 1000,
                        board->perf_fill_us / 1000, board->perf_fill_us % 1000,
                        board->perf_fastfb_us / 1000,
                        board->perf_fastfb_us % 1000,
                        tv->perf_sync_us / 1000, tv->perf_sync_us % 1000,
                        tv->perf_vbl_us / 1000, tv->perf_vbl_us % 1000,
                        tv->perf_flush_us / 1000, tv->perf_flush_us % 1000);
                    fprintf(pf,
                        "SND  ev=%u.%03u tmr=%u.%03u ay=%u.%03u mix=%u.%03u "
                        "samples=%u aysteps=%u\n",
                        board->snd_perf().perf_ev_us / 1000, board->snd_perf().perf_ev_us % 1000,
                        board->snd_perf().perf_tmr_us / 1000, board->snd_perf().perf_tmr_us % 1000,
                        board->snd_perf().perf_ay_us / 1000, board->snd_perf().perf_ay_us % 1000,
                        board->snd_perf().perf_mix_us / 1000, board->snd_perf().perf_mix_us % 1000,
                        board->snd_perf().perf_nsamples, board->snd_perf().perf_naysteps);
                    board->snd_perf().perf_ev_us = board->snd_perf().perf_tmr_us = 0;
                    board->snd_perf().perf_ay_us = board->snd_perf().perf_mix_us = 0;
                    board->snd_perf().perf_nsamples = board->snd_perf().perf_naysteps = 0;
                    tv->perf_sync_us = tv->perf_vbl_us = tv->perf_flush_us = 0;
                    fclose(pf);
                }
                board->perf_exec_us = board->perf_snd_us = 0;
                board->perf_render_us = board->perf_cpu_us = 0;
                board->perf_fill_us = 0;
                board->perf_fastfb_us = 0;
                board->perf_frames = 0;
            }
#endif

            fps_frames = 0;
            fps_last_time = now;
        }
    }

    /* Stop the worker before tearing the machine objects down. */
    lator->stop_emulator_thread();

    /* Finish the diagnostic sound recording: finalize the WAV headers
     * and report how much each side of the pipeline produced. */
    if (rec_internal != nullptr || rec_callback != nullptr) {
        uint32_t internal_frames = 0, callback_frames = 0;
        if (rec_internal != nullptr) {
            internal_frames = rec_internal->frames_written();
            rec_internal->close();
        }
        if (rec_callback != nullptr) {
            callback_frames = rec_callback->frames_written();
            rec_callback->close();
        }
        printf("Internal samples: %lu\n",
               (unsigned long)internal_frames);
        printf("Callback samples: %lu\n",
               (unsigned long)callback_frames);
        dbglog("sound_record done: internal=%lu callback=%lu frames\n",
               (unsigned long)internal_frames,
               (unsigned long)callback_frames);
    }

    /* Run-wide sound pipeline statistics (fill/step/rate_int/latency/
     * underrun/process_frame pacing) for the audio latency diagnosis. */
    soundnik->report_stats();

    dbglog_close();
    sceKernelExitGame();
    return 0;
}
