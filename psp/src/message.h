#pragma once

#include <atomic>
#include <inttypes.h>

/*
 * Universal modal message dialog with a text message and YES/NO or
 * Close buttons.  Rasterized into its own small indexed texture and
 * drawn by TV as a separate layer above all popup windows.
 *
 * Threading: same split as Popup — the worker thread calls show(),
 * dismiss() and update(); the display thread calls needs_repaint()
 * and paint().
 *
 * Usage:
 *   dialog.show("LOAD ROM?", MessageDialog::YES_NO);
 *   // ... later, after dialog.dismisses() ...
 *   if (dialog.result() == MessageDialog::RESULT_YES) { ... }
 */
class MessageDialog
{
public:
    enum Style { YES_NO, CLOSE };
    enum Result { RESULT_NONE = -1, RESULT_YES = 0, RESULT_NO = 1,
                  RESULT_CLOSE = 0 };

    /* Texture dimensions (power-of-two for GE). */
    static const int DLG_TEX_W = 256;
    static const int DLG_TEX_H = 128;

    /* Dialog box within the texture. */
    static const int DLG_W = 200;
    static const int DLG_H = 80;
    static const int DLG_X = (DLG_TEX_W - DLG_W) / 2;   /* 28 */
    static const int DLG_Y = (DLG_TEX_H - DLG_H) / 2;   /* 24  */

    MessageDialog();

    /* Worker thread: show the dialog with a message and button
     * style.  Resets result to RESULT_NONE. */
    void show(const char * message, Style style);
    /* Worker thread: dismiss with a specific result. */
    void dismiss(Result r);

    bool is_active() const
    {
        return active.load(std::memory_order_acquire);
    }
    Result result() const
    {
        return static_cast<Result>(
            result_val.load(std::memory_order_relaxed));
    }

    /* Worker thread: one input step (~50 Hz).  LEFT/RIGHT move the
     * selection (YES_NO only), X confirms, O/BACK cancels. */
    void update(unsigned pad);

    /* Display thread: repaint machinery (same pattern as Popup). */
    bool needs_repaint() const
    {
        return paint_seq.load(std::memory_order_relaxed) != painted_seq;
    }
    bool consume_tex_upload()
    {
        bool v = tex_upload;
        tex_upload = false;
        return v;
    }
    const uint8_t * tex_data() const { return tex; }
    const uint32_t * clut_data() const { return clut; }
    void paint();

    /* Pad masks for update(), same convention as GameCenter. */
    enum {
        PAD_LEFT  = 0x10,
        PAD_RIGHT = 0x20,
        PAD_PRESS = 0x04,  /* X button */
        PAD_BACK  = 0x08,  /* CIRCLE / START */
    };

private:
    /* Palette indices (local to this dialog). */
    enum Color : uint8_t {
        C_DIM = 0,       /* semi-transparent black (dim + padding) */
        C_BG,            /* dialog box background */
        C_BORDER,        /* dialog box border */
        C_TEXT_WHITE,    /* normal text */
        C_TEXT_BLACK,    /* selected button text */
        C_BTN_SEL,       /* selected button highlight */
    };

    void fill_rect(int x, int y, int w, int h, uint8_t color);
    void print_text2x(int x, int y, const char * text, uint8_t color);

    /* Keyup edge detection (same pattern as Popup). */
    bool keyup_edge(unsigned pad, unsigned mask) const
    {
        return (pad & mask) == 0 && (prev_pad & mask) != 0;
    }

    alignas(16) uint8_t tex[DLG_TEX_W * DLG_TEX_H];
    alignas(16) uint32_t clut[256];

    std::atomic<bool> active;
    std::atomic<int>  result_val;

    /* Repaint handshake (same as Popup). */
    std::atomic<unsigned> paint_seq;
    unsigned painted_seq;
    bool tex_upload;

    /* Input state (worker thread only). */
    unsigned prev_pad;
    int selection;       /* 0 = left button, 1 = right button */
    Style style;
    char message[128];
};
