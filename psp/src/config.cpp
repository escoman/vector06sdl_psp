#include "config.h"

#include <cstring>
#include <cstdlib>
#include <vector>

#include "options.h"
#include "util.h"
#include "debuglog.h"

static const char DEFAULT_CONFIG_PATH[] =
    "ms0:/PSP/GAME/VECTOR06C/config.ini";

/* Derive "<eboot dir>/config.ini" from argv[0]. */
static std::string config_path(const char * argv0)
{
    if (argv0 != nullptr) {
        std::string p(argv0);
        size_t slash = p.find_last_of('/');
        if (slash != std::string::npos) {
            return p.substr(0, slash + 1) + "config.ini";
        }
    }
    return DEFAULT_CONFIG_PATH;
}

static std::string trim(const std::string & s)
{
    const char * ws = " \t\r\n";
    size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) {
        return "";
    }
    size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

static bool parse_bool(const std::string & v, bool & out)
{
    std::string t = trim(v);
    if (t == "true" || t == "1" || t == "yes" || t == "on") {
        out = true;
        return true;
    }
    if (t == "false" || t == "0" || t == "no" || t == "off") {
        out = false;
        return true;
    }
    return false;
}

/* Priority value: decimal or 0x-prefixed hex, clamped to the PSP
 * user-thread range. Returns false when the value is not a number. */
static bool parse_priority(const std::string & v, int & out)
{
    const std::string t = trim(v);
    if (t.empty()) {
        return false;
    }
    char * end = nullptr;
    const long n = strtol(t.c_str(), &end, 0);
    if (end == t.c_str() || *end != '\0') {
        return false;
    }
    if (n < 0x08) {
        out = 0x08;
    } else if (n > 0x77) {
        out = 0x77;
    } else {
        out = (int)n;
    }
    return true;
}

/* Sound buffer target in milliseconds, clamped to a sane range. */
static bool parse_buffer_ms(const std::string & v, int & out)
{
    const std::string t = trim(v);
    if (t.empty()) {
        return false;
    }
    char * end = nullptr;
    const long n = strtol(t.c_str(), &end, 10);
    if (end == t.c_str() || *end != '\0' || n <= 0) {
        return false;
    }
    out = (n > 150) ? 150 : (int)n;
    return true;
}

static void apply_line(const std::string & line)
{
    size_t eq = line.find('=');
    if (eq == std::string::npos) {
        return;
    }

    std::string key = trim(line.substr(0, eq));
    std::string val = trim(line.substr(eq + 1));
    bool b;
    int prio;
    int ms;

    if (key == "border" && parse_bool(val, b)) {
        Options.show_border = b;
    } else if (key == "fps" && parse_bool(val, b)) {
        Options.show_fps = b;
    } else if (key == "fast_framebuffer" && parse_bool(val, b)) {
        Options.fast_framebuffer = b;
    } else if (key == "sound_record" && parse_bool(val, b)) {
        Options.sound_record = b;
    } else if (key == "sound_buffer_ms" && parse_buffer_ms(val, ms)) {
        Options.sound_buffer_ms = ms;
    } else if (key == "worker_priority" && parse_priority(val, prio)) {
        Options.worker_priority = prio;
    } else if (key == "main_priority" && parse_priority(val, prio)) {
        Options.main_priority = prio;
    }
}

static void parse(const std::vector<uint8_t> & data)
{
    std::string line;
    for (size_t i = 0; i <= data.size(); ++i) {
        char c = (i < data.size()) ? (char)data[i] : '\n';
        if (c == '\n' || c == '\r' || i == data.size()) {
            std::string t = trim(line);
            line.clear();
            if (!t.empty() && t[0] != '#' && t[0] != ';') {
                apply_line(t);
            }
        } else {
            line += c;
        }
    }
}

/* Write the defaults so the user can find and edit the file. */
static void create_default(const std::string & path)
{
    static const char TEXT[] =
        "# Vector-06C PSP configuration\n"
        "\n"
        "# Show the screen border (true/false)\n"
        "border = true\n"
        "\n"
        "# Show the FPS counter in the top-left corner (true/false)\n"
        "fps = false\n"
        "\n"
        "# Build each frame in one pass after the machine frame instead\n"
        "# of emulating the raster beam. Faster, but palette changes\n"
        "# made mid-frame are not reproduced (true/false)\n"
        "fast_framebuffer = false\n"
        "\n"
        "# Diagnostic: record the sound pipeline to WAV files\n"
        "# (psp_internal.wav = generated sound, psp_callback.wav =\n"
        "# what the audio callback actually feeds to the PSP)\n"
        "sound_record = false\n"
        "\n"
        "# Target sound buffer fill in milliseconds (default 40).\n"
        "# Lower = less audio latency vs gameplay, higher risk of\n"
        "# dropouts; higher = safer playback, more latency.\n"
        "sound_buffer_ms = 40\n"
        "\n"
        "# Thread priorities, hex, lower = higher priority (0x08..0x77).\n"
        "# worker = emulation, main = display. When a heavy game drives\n"
        "# the worker to 100% CPU, the lower-priority display thread\n"
        "# shows almost nothing; raising main_priority above the worker\n"
        "# trades emulation pacing for visible frames.\n"
        "worker_priority = 0x18\n"
        "main_priority = 0x20\n";

    std::vector<uint8_t> d(TEXT, TEXT + sizeof(TEXT) - 1);
    util::save_binfile(path, d);
    dbglog("config: created default %s\n", path.c_str());
}

std::string config_load(const char * argv0)
{
    /* Defaults; the file overrides them. */
    Options.show_border = true;
    Options.show_fps = false;
    Options.fast_framebuffer = false;
    Options.sound_record = false;
    Options.sound_buffer_ms = 40;
    Options.worker_priority = 0x18;
    Options.main_priority = 0x20;

    const std::string path = config_path(argv0);
    std::vector<uint8_t> data = util::load_binfile(path);

    if (data.empty()) {
        create_default(path);
    } else {
        parse(data);
    }

    dbglog("config: %s border=%d fps=%d fastfb=%d sndrec=%d sndbuf=%dms wrk_prio=0x%02x main_prio=0x%02x\n",
           path.c_str(), (int)Options.show_border, (int)Options.show_fps,
           (int)Options.fast_framebuffer, (int)Options.sound_record,
           Options.sound_buffer_ms,
           Options.worker_priority, Options.main_priority);
    return path;
}
