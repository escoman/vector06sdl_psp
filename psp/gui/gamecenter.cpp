#include "gamecenter.h"
#include "netman.h"
#include "debuglog.h"
#include "globaldefs.h"
#include "options.h"
#include "stb_image.h"
#include "font.h"
#include "layer_draw.h"

#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <pspgu.h>
#include <pspkernel.h>

#include <cstdio>
#include <cstring>
#include <vector>


/*
 * Game Center input, catalog parsing and rasterization, see
 * gamecenter.h.  The texture, palette and repaint machinery come
 * from the Popup base class; the window is repainted only when
 * the visible state changes.
 */

/* Maximum download buffer for the catalog INI (128 KB). */
static const int CATALOG_BUF_SIZE = 128 * 1024;

/* Maximum download buffer for a preview image (256 KB). */
static const int PREVIEW_BUF_SIZE = 256 * 1024;

/* Decoding cap for preview images. */
static const int MAX_SRC_DIM = 768;

/* Convert RGBA (stb_image output) to PSP GE 0xAABBGGRR. */
static uint32_t rgba_to_ge(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return ((uint32_t)a << 24) | ((uint32_t)b << 16)
         | ((uint32_t)g << 8)  |  (uint32_t)r;
}

/* Box-filter shrink (same algorithm as imgload.cpp). */
static void box_shrink(const uint32_t * src, int sw, int sh,
                       uint32_t * dst, int tw, int th)
{
    for (int dy = 0; dy < th; ++dy) {
        const int sy0 = (int)((size_t)dy * sh / th);
        const int sy1 = (int)((size_t)(dy + 1) * sh / th);
        for (int dx = 0; dx < tw; ++dx) {
            const int sx0 = (int)((size_t)dx * sw / tw);
            const int sx1 = (int)((size_t)(dx + 1) * sw / tw);
            unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                const uint32_t * row = src + (size_t)sy * sw;
                for (int sx = sx0; sx < sx1; ++sx) {
                    const uint32_t p = row[sx];
                    r += p & 0xff;
                    g += (p >> 8) & 0xff;
                    b += (p >> 16) & 0xff;
                    a += (p >> 24) & 0xff;
                    ++n;
                }
            }
            if (n == 0) n = 1;
            dst[dy * tw + dx] =
                ((a / n) << 24) | ((b / n) << 16)
              | ((g / n) << 8)  |  (r / n);
        }
    }
}

/* Box-filter shrink directly from RGBA buffer to GE format. */
static void box_shrink_rgba(const uint8_t * rgba, int sw, int sh, int ch,
                            uint32_t * dst, int tw, int th)
{
    for (int dy = 0; dy < th; ++dy) {
        const int sy0 = (int)((size_t)dy * sh / th);
        const int sy1 = (int)((size_t)(dy + 1) * sh / th);
        for (int dx = 0; dx < tw; ++dx) {
            const int sx0 = (int)((size_t)dx * sw / tw);
            const int sx1 = (int)((size_t)(dx + 1) * sw / tw);
            unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                const uint8_t * row = rgba + (size_t)sy * sw * ch;
                for (int sx = sx0; sx < sx1; ++sx) {
                    const uint8_t * p = row + sx * ch;
                    r += p[0];
                    g += p[1];
                    b += p[2];
                    a += (ch >= 4) ? p[3] : 255;
                    ++n;
                }
            }
            if (n == 0) n = 1;
            dst[dy * tw + dx] = rgba_to_ge(r / n, g / n, b / n, a / n);
        }
    }
}

/* ---- Trim helper: strip leading/trailing whitespace and quotes ---- */

static void trim_value(char * dst, const char * src, int dst_len)
{
    /* Skip leading whitespace. */
    while (*src == ' ' || *src == '\t')
        ++src;

    /* Strip surrounding quotes if present. */
    int len = (int)strlen(src);
    if (len >= 2 && src[0] == '"' && src[len - 1] == '"') {
        ++src;
        len -= 2;
    }

    /* Trim trailing whitespace. */
    while (len > 0 && (src[len - 1] == ' '  || src[len - 1] == '\t'
                       || src[len - 1] == '\r' || src[len - 1] == '\n'))
        --len;

    if (len >= dst_len)
        len = dst_len - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* ---- Constructor ---- */

GameCenter::GameCenter() :
    open_flag(false),
    count(0), selected(0), top(0),
    catalog_ok(false),
    entries(nullptr),
    preview_for_index(-1),
    preview_w(0), preview_h(0),
    fit_x(0), fit_y(0), fit_w(0), fit_h(0),
    preview_upload(false),
    idle_frames(0),
    load_requested(false),
    rom_ready(false),
    preview_tex(nullptr)
{
    reset_input_state();
    panel_w = PANEL_W;
    panel_h = PANEL_H;
    rom_path[0] = '\0';
    message[0] = '\0';
}

/* ---- Open / Close ---- */

void GameCenter::open()
{
    if (open_flag.load(std::memory_order_relaxed))
        return;

    /* Allocate catalog and preview buffers on demand. */
    if (!entries)
        entries = new GameEntry[MAX_GAMES]();
    if (!preview_tex)
        preview_tex = new uint32_t[PREVIEW_TEX_W * PREVIEW_TEX_H]();

    selected = 0;
    top = 0;
    count = 0;
    catalog_ok = false;
    message[0] = '\0';
    preview_for_index = -1;
    preview_w = preview_h = 0;
    idle_frames = 0;
    reset_input_state();

    /* Step 1: WiFi hardware check. */
    if (!NetMan::wifi_available()) {
        snprintf(message, sizeof(message), "WiFi module not available");
        mark_dirty();
        open_flag.store(true, std::memory_order_release);
        return;
    }

    /* Step 2: Initialize network infrastructure. */
    set_status("Initializing network...");
    if (!NetMan::init()) {
        snprintf(message, sizeof(message), "Network init failed");
        mark_dirty();
        open_flag.store(true, std::memory_order_release);
        return;
    }

    /* Step 3: Connect to AP (system dialog). */
    set_status("Connecting...");
    if (!NetMan::connect()) {
        snprintf(message, sizeof(message), "Connection failed");
        mark_dirty();
        open_flag.store(true, std::memory_order_release);
        return;
    }

    /* Step 4: Download catalog. */
    set_status("Downloading catalog...");
    std::vector<uint8_t> catbuf(CATALOG_BUF_SIZE);
    int cat_len = NetMan::http_download(Options.catalog_url.c_str(),
                                        catbuf.data(), CATALOG_BUF_SIZE);
    if (cat_len <= 0) {
        snprintf(message, sizeof(message), "Catalog download failed");
        mark_dirty();
        open_flag.store(true, std::memory_order_release);
        return;
    }

    /* Null-terminate for the parser. */
    if (cat_len < CATALOG_BUF_SIZE)
        catbuf[cat_len] = '\0';
    else
        catbuf[CATALOG_BUF_SIZE - 1] = '\0';

    /* Step 5: Parse and display. */
    parse_catalog((const char *)catbuf.data(), cat_len);
    if (count == 0) {
        snprintf(message, sizeof(message), "Catalog is empty");
    } else {
        catalog_ok = true;
    }

    mark_dirty();
    open_flag.store(true, std::memory_order_release);
}

void GameCenter::close()
{
    NetMan::disconnect();
    NetMan::shutdown();

    preview_for_index = -1;
    preview_w = preview_h = 0;
    preview_upload = false;

    /* Release catalog and preview buffers. */
    delete[] entries;
    entries = nullptr;
    delete[] preview_tex;
    preview_tex = nullptr;

    open_flag.store(false, std::memory_order_release);
}

/* ---- Status ---- */

void GameCenter::set_status(const char * msg)
{
    snprintf(message, sizeof(message), "%s", msg);
    mark_dirty();
}

const char * GameCenter::selected_title() const
{
    if (count <= 0 || selected < 0 || selected >= count)
        return "";
    return entries[selected].title;
}

/* ---- INI parser ---- */

void GameCenter::parse_catalog(const char * data, int len)
{
    count = 0;
    GameEntry * cur = nullptr;

    const char * p = data;
    const char * end = data + len;

    while (p < end) {
        /* Find end of line. */
        const char * eol = p;
        while (eol < end && *eol != '\n')
            ++eol;

        /* Copy line into a temp buffer. */
        int line_len = (int)(eol - p);
        char line[512];
        if (line_len >= (int)sizeof(line))
            line_len = sizeof(line) - 1;
        memcpy(line, p, line_len);
        line[line_len] = '\0';
        p = eol + 1;

        /* Strip trailing CR. */
        if (line_len > 0 && line[line_len - 1] == '\r')
            line[--line_len] = '\0';

        /* Skip empty lines and comments. */
        const char * s = line;
        while (*s == ' ' || *s == '\t')
            ++s;
        if (*s == '\0' || *s == ';')
            continue;

        /* Section header: [name] */
        if (*s == '[') {
            const char * close = strchr(s, ']');
            if (close) {
                char section[128];
                int slen = (int)(close - s - 1);
                if (slen >= (int)sizeof(section))
                    slen = sizeof(section) - 1;
                memcpy(section, s + 1, slen);
                section[slen] = '\0';

                if (strcmp(section, "catalog") == 0) {
                    cur = nullptr;  /* metadata section, skip */
                } else if (count < MAX_GAMES) {
                    cur = &entries[count];
                    memset(cur, 0, sizeof(GameEntry));
                    snprintf(cur->key, TITLE_LEN, "%s", section);
                    ++count;
                } else {
                    cur = nullptr;
                }
            }
            continue;
        }

        /* Key = value pair. */
        if (cur != nullptr) {
            const char * eq = strchr(s, '=');
            if (eq) {
                char key[64];
                int klen = (int)(eq - s);
                while (klen > 0 && (s[klen - 1] == ' ' || s[klen - 1] == '\t'))
                    --klen;
                if (klen >= (int)sizeof(key))
                    klen = sizeof(key) - 1;
                memcpy(key, s, klen);
                key[klen] = '\0';

                const char * val = eq + 1;

                if (strcmp(key, "title") == 0) {
                    trim_value(cur->title, val, TITLE_LEN);
                } else if (strcmp(key, "description") == 0) {
                    trim_value(cur->description, val, sizeof(cur->description));
                } else if (strcmp(key, "author") == 0) {
                    trim_value(cur->author, val, sizeof(cur->author));
                } else if (strcmp(key, "genre") == 0) {
                    trim_value(cur->genre, val, sizeof(cur->genre));
                } else if (strcmp(key, "year") == 0) {
                    trim_value(cur->year, val, sizeof(cur->year));
                } else if (strcmp(key, "rom_file") == 0) {
                    trim_value(cur->rom_file, val, PATH_LEN);
                } else if (strcmp(key, "preview") == 0) {
                    char raw[PATH_LEN];
                    trim_value(raw, val, PATH_LEN);
                    /* Take only the first path (before ';'). */
                    char * semi = strchr(raw, ';');
                    if (semi)
                        *semi = '\0';
                    snprintf(cur->preview, PATH_LEN, "%s", raw);
                } else if (strcmp(key, "size_bytes") == 0) {
                    cur->size_bytes = atoi(val);
                }
            }
        }
    }
}

/* ---- Preview ---- */

void GameCenter::build_preview_url(char * url, int url_len,
                                   const char * preview_path)
{
    snprintf(url, url_len, "%s%s", Options.download_url.c_str(), preview_path);
}

void GameCenter::update_preview()
{
    if (selected == preview_for_index)
        return;  /* cache hit */

    preview_for_index = selected;
    preview_w = preview_h = 0;

    if (count <= 0 || selected < 0 || selected >= count)
        return;

    const char * prev = entries[selected].preview;
    if (prev[0] == '\0')
        return;  /* no preview for this entry */

    char url[PATH_LEN];
    build_preview_url(url, sizeof(url), prev);

    /* Download the preview image. */
    std::vector<uint8_t> imgbuf(PREVIEW_BUF_SIZE);
    int img_len = NetMan::http_download(url, imgbuf.data(), PREVIEW_BUF_SIZE);
    if (img_len <= 0)
        return;

    /* Decode from memory using stb_image (force 4 channels). */
    int w = 0, h = 0, ch = 0;
    uint8_t * rgba = stbi_load_from_memory(imgbuf.data(), img_len,
                                           &w, &h, &ch, 4);
    
    /* Free download buffer immediately to save memory. */
    imgbuf.clear();
    imgbuf.shrink_to_fit();
    
    if (rgba == nullptr)
        return;

    if (w == 0 || h == 0 || w > MAX_SRC_DIM || h > MAX_SRC_DIM) {
        stbi_image_free(rgba);
        return;
    }

    /* stbi with desired_channels=4 should return 4 channels.
     * But some versions may return actual channels, so use 4 always. */
    const int actual_ch = 4;

    /* Convert to GE format. */
    const size_t src_total = (size_t)w * (size_t)h;
    
    int tw, th;
    if (w <= PREVIEW_TEX_W && h <= PREVIEW_TEX_H) {
        /* Small image — convert directly to preview_tex. */
        for (size_t i = 0; i < src_total; ++i) {
            preview_tex[i] = rgba_to_ge(rgba[i * 4], rgba[i * 4 + 1],
                                        rgba[i * 4 + 2], rgba[i * 4 + 3]);
        }
        tw = w;
        th = h;
    } else {
        /* Large image — shrink directly from RGBA buffer. */
        /* Calculate target dimensions. */
        if ((size_t)PREVIEW_TEX_W * h <= (size_t)PREVIEW_TEX_H * w) {
            tw = PREVIEW_TEX_W;
            th = (int)((size_t)h * PREVIEW_TEX_W / w);
        } else {
            th = PREVIEW_TEX_H;
            tw = (int)((size_t)w * PREVIEW_TEX_H / h);
        }
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        
        box_shrink_rgba(rgba, w, h, actual_ch, preview_tex, tw, th);
    }
    
    stbi_image_free(rgba);
    preview_w = tw;
    preview_h = th;

    /* Stretched over the whole right pane. */
    fit_x = PREVIEW_X;
    fit_y = PREVIEW_Y;
    fit_w = PREVIEW_W;
    fit_h = PREVIEW_H;
    preview_upload = true;
}

void GameCenter::load_selected_rom()
{
    if (count <= 0 || selected < 0 || selected >= count)
        return;

    const GameEntry & entry = entries[selected];
    if (entry.rom_file[0] == '\0')
        return;

    /* Build download URL for ROM. */
    char url[PATH_LEN];
    snprintf(url, sizeof(url), "%s%s", Options.download_url.c_str(), entry.rom_file);

    /* Download ROM to buffer. */
    std::vector<uint8_t> rombuf(512 * 1024);  /* 512 KB max */
    int rom_len = NetMan::http_download(url, rombuf.data(), rombuf.size());
    if (rom_len <= 0)
        return;

    /* Extract filename from rom_file path. */
    const char * filename = strrchr(entry.rom_file, '/');
    if (filename)
        ++filename;
    else
        filename = entry.rom_file;

    /* Build local path. */
    snprintf(rom_path, sizeof(rom_path), "%s/%s", ROM_DIR, filename);

    /* Save ROM to file. */
    SceUID fd = sceIoOpen(rom_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0)
        return;
    sceIoWrite(fd, rombuf.data(), rom_len);
    sceIoClose(fd);

    /* Download preview if available. */
    if (entry.preview[0] != '\0') {
        char prev_url[PATH_LEN];
        snprintf(prev_url, sizeof(prev_url), "%s%s", Options.download_url.c_str(), entry.preview);

        std::vector<uint8_t> prevbuf(256 * 1024);  /* 256 KB max */
        int prev_len = NetMan::http_download(prev_url, prevbuf.data(), prevbuf.size());
        if (prev_len > 0) {
            /* Build preview path: same name as ROM but .png extension. */
            char prev_path[PATH_LEN];
            snprintf(prev_path, sizeof(prev_path), "%s/%.*s.png",
                     ROM_DIR,
                     (int)(strrchr(filename, '.') - filename),
                     filename);

            SceUID pfd = sceIoOpen(prev_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
            if (pfd >= 0) {
                sceIoWrite(pfd, prevbuf.data(), prev_len);
                sceIoClose(pfd);
            }
        }
    }

    rom_ready = true;
}

/* ---- Input ---- */

void GameCenter::update(unsigned pad)
{
    if (!open_flag.load(std::memory_order_relaxed))
        return;

    if (count <= 0) {
        prev_pad = pad;
        return;
    }

    bool moved = false;
    if (keyup_edge(pad, GC_PAD_DOWN)) {
        selected = (selected + 1) % count;
        moved = true;
    }
    if (keyup_edge(pad, GC_PAD_UP)) {
        selected = (selected + count - 1) % count;
        moved = true;
    }
    if (moved) {
        if (selected < top)
            top = selected;
        if (selected >= top + VISIBLE_ROWS)
            top = selected - VISIBLE_ROWS + 1;

        idle_frames = 0;  /* Reset idle counter on move. */
        mark_dirty();
    }

    /* Lazy preview loading: wait for idle before loading. */
    ++idle_frames;
    if (idle_frames == 12 && preview_for_index != selected) {
        /* ~200ms at 60fps — load preview now. */
        update_preview();
        mark_dirty();
    }

    /* X button - request ROM load (main.cpp shows the dialog). */
    if (keyup_edge(pad, GC_PAD_PRESS)) {
        load_requested = true;
        idle_frames = 0;  /* Reset idle counter to prevent preview reload. */
    }

    prev_pad = pad;
}

/* ---- Paint ---- */

void GameCenter::paint()
{
    const unsigned seq = snapshot_seq();

    const int header_w = PANEL_W - PAD_X * 2;
    const int list_y0 = PAD_Y + TITLE_H + HDR_GAP + 1 + HDR_GAP;
    const int footer_y = PANEL_H - PAD_Y - FOOTER_H;

    fill_rect(0, 0, PANEL_W, PANEL_H, C_PANEL_BG);

    /* Header: a status message ("Connecting...", "WiFi module not
     * available") replaces the title, same pattern as MainMenu
     * "Saving..." — centered on the panel background. */
    if (message[0] != '\0' && !catalog_ok) {
        const int tw = (int)strlen(message) * OVERLAY_FONT_W * 2;
        print_text2x((PANEL_W - tw) / 2, PAD_Y, message, C_TEXT_WHITE);
    } else {
        print_text2x(PAD_X, PAD_Y, "Game Center", C_TEXT_WHITE);
        char count_text[32];
        snprintf(count_text, sizeof(count_text), "Games: %d", count);
        const int cw = (int)strlen(count_text) * OVERLAY_FONT_W * 2;
        print_text2x(PANEL_W - PAD_X - cw, PAD_Y, count_text, C_TEXT_WHITE);
    }
    fill_rect(PAD_X, PAD_Y + TITLE_H + HDR_GAP, header_w, 1, C_PANEL_BORDER);

    /* Divider between list and preview. */
    fill_rect(PREVIEW_X - DIV_GAP / 2, list_y0, 1, footer_y - list_y0,
              C_PANEL_BORDER);

    if (!catalog_ok) {
        /* Error message is already in the header; leave the list
         * area empty. */
    } else if (count == 0) {
        const char * msg = "Catalog is empty";
        const int mw = (int)strlen(msg) * OVERLAY_FONT_W * 2;
        print_text2x((PANEL_W - mw) / 2,
                     list_y0 + (VISIBLE_ROWS * ROW_H) / 2 - OVERLAY_FONT_H,
                     msg, C_TEXT_WHITE);
    } else {
        const int max_chars = LIST_W / (OVERLAY_FONT_W * 2);
        for (int r = 0; r < VISIBLE_ROWS; ++r) {
            const int idx = top + r;
            if (idx >= count)
                break;
            const int y = list_y0 + r * ROW_H;
            const bool sel = (idx == selected);
            fill_rect(PAD_X, y, LIST_W, ROW_H - 2,
                      sel ? C_ITEM_BG_SEL : C_ITEM_BG);

            char shown[TITLE_LEN];
            snprintf(shown, sizeof(shown), "%s", entries[idx].title);
            shown[max_chars < TITLE_LEN ? max_chars : TITLE_LEN - 1] = '\0';
            print_text2x(PAD_X + 4, y + (ROW_H - 2 - OVERLAY_FONT_H * 2) / 2,
                         shown, sel ? C_TEXT_BLACK : C_TEXT_WHITE);
        }
    }

    /* Footer: key hints (the error/status message is already shown
     * in the list area when the catalog failed to load). */
    {
        const char * hints = "O Back";
        const int hw = (int)strlen(hints) * OVERLAY_FONT_W * 2;
        print_text2x(PANEL_W - PAD_X - hw, footer_y, hints, C_TEXT_WHITE);
    }

    finish_paint(seq);
}

void GameCenter::draw()
{
    /* Panel quad first. */
    UILayer::draw();
    /* Preview on top of the right pane. */
    draw_preview();
}

void GameCenter::draw_preview()
{
    if (!has_preview())
        return;

    if (consume_preview_upload()) {
        sceKernelDcacheWritebackInvalidateRange(
            (void *)preview_tex_data(),
            (unsigned)(PREVIEW_TEX_W * PREVIEW_TEX_H * sizeof(uint32_t)));
    }

    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, PREVIEW_TEX_W, PREVIEW_TEX_H, PREVIEW_TEX_W, preview_tex_data());
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);

    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);

    struct Vertex {
        float u, v;
        float x, y, z;
    };

    int fx, fy, fw, fh;
    get_preview_rect(&fx, &fy, &fw, &fh);
    const float x0 = ((float)LAYER_SCREEN_W - (float)get_width()) / 2.0f + (float)fx;
    const float y0 = ((float)LAYER_SCREEN_H - (float)get_height()) / 2.0f + (float)fy;
    const float uw = (float)get_preview_w();
    const float vh = (float)get_preview_h();

    Vertex * vertices = (Vertex *)layer_draw_alloc_vertices(sizeof(Vertex) * 4);
    vertices[0] = { 0.0f, 0.0f, x0,              y0,              0.0f };
    vertices[1] = { uw,   0.0f, x0 + (float)fw,  y0,              0.0f };
    vertices[2] = { uw,   vh,   x0 + (float)fw,  y0 + (float)fh,  0.0f };
    vertices[3] = { 0.0f, vh,   x0,              y0 + (float)fh,  0.0f };

    sceKernelDcacheWritebackInvalidateRange(vertices, sizeof(Vertex) * 4);

    sceGuDrawArray(
        GU_TRIANGLE_FAN,
        GU_TEXTURE_32BITF |
        GU_VERTEX_32BITF |
        GU_TRANSFORM_2D,
        4, 0, vertices);

    sceGuDisable(GU_BLEND);
}
