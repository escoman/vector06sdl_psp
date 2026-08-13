#pragma once

#include <string>

/*
 * Flat "key = value" config file (config.ini) lying next to EBOOT.PBP.
 * Lines starting with '#' or ';' are comments.
 *
 * Currently known keys:
 *   border = true|false   show the Vector-06C screen border
 *   fps    = true|false   show the FPS counter in the top-left corner
 *
 * Missing file: defaults are applied and the file is created.
 * Unknown or malformed entries are ignored.
 */
std::string config_load(const char * argv0);
