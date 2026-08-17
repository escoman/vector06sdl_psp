#pragma once

#include <string>
#include "event.h"

/*
 * PSP -> Vector-06C key mapping (Stage 6, "Map Keys").
 *
 * Two levels, exactly per spec:
 *
 *   Default Mapping  - the built-in table below, the single source
 *                      of truth for the factory assignments; it
 *                      works with no .map file at all and is never
 *                      modified by this module.
 *   ROM mapping      - an optional <rom>.map text file next to the
 *                      ROM; only the entries DIFFERENT from the
 *                      defaults are stored there (overrides and
 *                      explicit NONE disables).
 *
 *   Default Mapping -> <ROM>.map -> Effective Mapping
 *
 * Every source has one of three states:
 *   STATE_DEFAULT  - no entry in the .map, the default key applies;
 *   STATE_OVERRIDE - PSP_<SRC>=<KEY> in the .map;
 *   STATE_DISABLED - PSP_<SRC>=NONE in the .map (the button does
 *                    nothing for this ROM; the default must NOT
 *                    return until the entry is removed).
 *
 * The Vector key identifier is the same SDL scancode used by the
 * VKBD and Emulator::keydown()/keyup() - no second key table exists.
 * START and SELECT are system buttons and are never part of the
 * mapping. All functions run in the worker thread only.
 */

/* Mapping sources: the four face buttons, the D-pad, the shoulder
 * buttons and the analog nub digitized into four directions. */
enum {
    MAP_SRC_UP = 0,
    MAP_SRC_DOWN,
    MAP_SRC_LEFT,
    MAP_SRC_RIGHT,
    MAP_SRC_CROSS,
    MAP_SRC_CIRCLE,
    MAP_SRC_TRIANGLE,
    MAP_SRC_SQUARE,
    MAP_SRC_L,
    MAP_SRC_R,
    MAP_SRC_ANA_UP,
    MAP_SRC_ANA_DOWN,
    MAP_SRC_ANA_LEFT,
    MAP_SRC_ANA_RIGHT,
    MAP_SRC_COUNT
};

namespace KeyMap
{
    enum EntryState {
        STATE_DEFAULT = 0,
        STATE_OVERRIDE,
        STATE_DISABLED,
    };

    /* Back to the pure Default Mapping; the ROM-specific entries are
     * forgotten. First step of every ROM load. */
    void reset_to_default();

    /* The built-in assignment of a source (-1: the default itself
     * has none, which is true for the analog directions only). */
    int default_key(int src);

    /* The assignment in effect right now (-1: nothing happens when
     * the source is pressed). */
    int effective_key(int src);

    EntryState entry_state(int src);

    /* ROM-specific change; the effective table updates at once. */
    void assign(int src, int scancode);
    void disable(int src);

    /* True when at least one source differs from the default
     * (override or disabled): a .map is worth writing. */
    bool has_custom();
    int custom_count();

    /* Parse <rom>.map over the current (default) table; unknown and
     * malformed lines are ignored, never fatal. True when the file
     * existed. */
    bool load_file(const std::string & path);

    /* Write only the entries differing from the default; when there
     * are none, delete the file instead (the ROM is fully default
     * again). True when a file was written. */
    bool save_or_cleanup(const std::string & path);

    /* Names. source_label: short human label for the window list
     * ("PSP X", "STICK LEFT"); source_file_id: the .map key
     * ("PSP_X", "PSP_ANALOG_LEFT"); key_label: short display name
     * of a Vector key ("ENTER", "---" when none); key_file_name:
     * the .map value token ("ENTER", "NONE" is handled separately). */
    const char * source_label(int src);
    const char * source_file_id(int src);
    const char * key_label(int scancode);
    const char * key_file_name(int scancode);
    int key_by_file_name(const char * name);   /* -1 when unknown */
    bool is_assignable(int scancode);
}
