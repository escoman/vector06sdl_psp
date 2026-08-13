#include "config.h"

#include <cstring>
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

static void apply_line(const std::string & line)
{
    size_t eq = line.find('=');
    if (eq == std::string::npos) {
        return;
    }

    std::string key = trim(line.substr(0, eq));
    std::string val = trim(line.substr(eq + 1));
    bool b;

    if (key == "border" && parse_bool(val, b)) {
        Options.show_border = b;
    } else if (key == "fps" && parse_bool(val, b)) {
        Options.show_fps = b;
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
        "fps = false\n";

    std::vector<uint8_t> d(TEXT, TEXT + sizeof(TEXT) - 1);
    util::save_binfile(path, d);
    dbglog("config: created default %s\n", path.c_str());
}

std::string config_load(const char * argv0)
{
    /* Defaults; the file overrides them. */
    Options.show_border = true;
    Options.show_fps = false;

    const std::string path = config_path(argv0);
    std::vector<uint8_t> data = util::load_binfile(path);

    if (data.empty()) {
        create_default(path);
    } else {
        parse(data);
    }

    dbglog("config: %s border=%d fps=%d\n",
           path.c_str(), (int)Options.show_border, (int)Options.show_fps);
    return path;
}
