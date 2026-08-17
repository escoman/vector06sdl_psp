#include "configwindow.h"
#include "font.h"

#include <cstdio>
#include <cstring>
#include <cctype>

/*
 * CONFIG WINDOW input and rasterization, see configwindow.h. The
 * texture, palette and repaint machinery come from the Popup base
 * class; the window is repainted only when its visible state
 * changes, the recurring per-frame cost is one quad.
 *
 * The window is parameter-agnostic: all knowledge about concrete
 * config.ini entries lives in the ConfigParam table built by
 * main.cpp (get/set bound to Options + the owning subsystem).
 */

ConfigWindow::ConfigWindow() :
    open_flag(false),
    params(nullptr), count(0),
    selected(0), top(0)
{
    reset_input_state();

    panel_w = PANEL_W;
    panel_h = PANEL_H;
}

void ConfigWindow::set_params(const ConfigParam * p, int n)
{
    this->params = p;
    this->count = (n > MAX_PARAMS) ? MAX_PARAMS : n;
}

void ConfigWindow::open()
{
    if (open_flag.load(std::memory_order_relaxed))
        return;

    /* Values are read straight from Options through the get()
     * bindings while painting, so opening always shows the current
     * state. */
    selected = 0;         /* every open starts on the first parameter */
    top = 0;
    reset_input_state();
    mark_dirty();
    open_flag.store(true, std::memory_order_release);
}

void ConfigWindow::close()
{
    open_flag.store(false, std::memory_order_release);
}

/* One LEFT (-1) / RIGHT (+1) step per the type rules; every applied
 * change goes to Options through set() and straight into config.ini
 * through on_save — nothing is deferred until the window closes. */
void ConfigWindow::change_value(int dir)
{
    if (selected < 0 || selected >= count)
        return;
    const ConfigParam & p = params[selected];

    int v = p.get ? p.get() : 0;
    switch (p.type) {
        case CfgType::BOOL:
            /* LEFT -> false, RIGHT -> true; already at the limit:
             * stay. */
            v = (dir < 0) ? 0 : 1;
            break;
        case CfgType::LIST:
            if (p.value_count <= 0)
                return;
            v = (v + dir + p.value_count) % p.value_count;
            break;
        case CfgType::INTEGER:
            v += dir * ((p.step > 0) ? p.step : 1);
            if (v < p.min) v = p.min;
            if (v > p.max) v = p.max;
            break;
    }

    if (v == p.get())
        return;           /* clamped at the limit: nothing changed */

    if (p.set)
        p.set(v);

    if (on_save) {
        char buf[VALUE_LEN];
        format_config_value(p, buf, sizeof(buf));
        on_save(p.key, buf);
    }
    mark_dirty();
}

void ConfigWindow::update(unsigned pad)
{
    if (!open_flag.load(std::memory_order_relaxed))
        return;

    if (count <= 0) {
        prev_pad = pad;
        return;
    }

    /* Navigation and editing fire on the keyup edge: one press =
     * exactly one step, holding the button never repeats. */
    bool moved = false;
    if (keyup_edge(pad, CFG_PAD_DOWN)) {
        selected = (selected + 1) % count;
        moved = true;
    }
    if (keyup_edge(pad, CFG_PAD_UP)) {
        selected = (selected + count - 1) % count;
        moved = true;
    }
    if (moved) {
        /* Keep the selection inside the visible window. */
        if (selected < top)
            top = selected;
        if (selected >= top + VISIBLE_ROWS)
            top = selected - VISIBLE_ROWS + 1;

        mark_dirty();
    }

    if (keyup_edge(pad, CFG_PAD_LEFT))
        change_value(-1);
    if (keyup_edge(pad, CFG_PAD_RIGHT))
        change_value(+1);

    prev_pad = pad;
}

/* Display form of the current value: BOOL as TRUE/FALSE, LIST as
 * the mode name in upper case, INTEGER as 0xNN or decimal. */
void ConfigWindow::format_value(const ConfigParam & p,
                                char * out, int out_len) const
{
    const int v = p.get ? p.get() : 0;
    switch (p.type) {
        case CfgType::BOOL:
            snprintf(out, (size_t)out_len, "%s", v ? "TRUE" : "FALSE");
            break;
        case CfgType::LIST:
            if (v >= 0 && v < p.value_count) {
                snprintf(out, (size_t)out_len, "%s", p.values[v]);
                for (char * c = out; *c != '\0'; ++c)
                    *c = (char)toupper((unsigned char)*c);
            } else {
                out[0] = '\0';
            }
            break;
        case CfgType::INTEGER:
            if (p.hex)
                snprintf(out, (size_t)out_len, "0x%02X", v & 0xff);
            else
                snprintf(out, (size_t)out_len, "%d", v);
            break;
    }
}

/* config.ini spelling of the current value: keeps the exact key
 * value format the loader already accepts ("true", "gaussian",
 * "0x18", "40"). */
void ConfigWindow::format_config_value(const ConfigParam & p,
                                       char * out, int out_len) const
{
    const int v = p.get ? p.get() : 0;
    switch (p.type) {
        case CfgType::BOOL:
            snprintf(out, (size_t)out_len, "%s", v ? "true" : "false");
            break;
        case CfgType::LIST:
            if (v >= 0 && v < p.value_count)
                snprintf(out, (size_t)out_len, "%s", p.values[v]);
            else
                out[0] = '\0';
            break;
        case CfgType::INTEGER:
            if (p.hex)
                snprintf(out, (size_t)out_len, "0x%02x", v & 0xff);
            else
                snprintf(out, (size_t)out_len, "%d", v);
            break;
    }
}

void ConfigWindow::paint()
{
    /* Snapshot the sequence first: a state change arriving from the
     * worker while we rasterize must force one more pass. */
    const unsigned seq = snapshot_seq();

    const int row_w = PANEL_W - PAD_X * 2;
    const int list_y0 = PAD_Y + TITLE_H + HDR_GAP + 1 + HDR_GAP;
    const int footer_y = PANEL_H - PAD_Y - FOOTER_H;

    /* Window background only; the dim backdrop underneath already
     * separates the window from the game picture, no frame. */
    fill_rect(0, 0, PANEL_W, PANEL_H, C_PANEL_BG);

    /* Header: "Config" left, divider below. */
    print_text2x(PAD_X, PAD_Y, "Config", C_TEXT_WHITE);
    fill_rect(PAD_X, PAD_Y + TITLE_H + HDR_GAP, row_w, 1, C_PANEL_BORDER);

    /* Parameter rows: name on the left, "< VALUE >" right-aligned.
     * Same highlight scheme as the MAIN MENU. */
    for (int r = 0; r < VISIBLE_ROWS; ++r) {
        const int idx = top + r;
        if (idx >= count)
            break;
        const int y = list_y0 + r * ROW_H;
        const bool sel = (idx == selected);
        const ConfigParam & p = params[idx];

        fill_rect(PAD_X, y, row_w, ROW_H - 2,
                  sel ? C_ITEM_BG_SEL : C_ITEM_BG);

        const uint8_t fg = sel ? C_TEXT_BLACK : C_TEXT_WHITE;
        print_text2x(PAD_X + 4, y + (ROW_H - 2 - OVERLAY_FONT_H * 2) / 2,
                     p.name, fg);

        char shown[VALUE_LEN];
        format_value(p, shown, sizeof(shown));
        char line[VALUE_LEN + 4];
        snprintf(line, sizeof(line), "< %s >", shown);
        const int lw = (int)strlen(line) * OVERLAY_FONT_W * 2;
        print_text2x(PANEL_W - PAD_X - lw,
                     y + (ROW_H - 2 - OVERLAY_FONT_H * 2) / 2,
                     line, fg);
    }

    /* Footer hint. */
    {
        const char * hints = "O Back";
        const int hw = (int)strlen(hints) * OVERLAY_FONT_W * 2;
        print_text2x(PANEL_W - PAD_X - hw, footer_y, hints, C_TEXT_WHITE);
    }

    finish_paint(seq);
}
