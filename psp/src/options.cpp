#include "globaldefs.h"
#include "options.h"

_options Options =
{
    .bootromfile = "",
    .romfile = "",
    .rom_org = 256,
    .pc = 0,
    .wavfile = "",
    .eddfile = {},
    .max_frame = -1,
    .vsync = false,
    .novideo = true,
    .screen_width = DEFAULT_SCREEN_WIDTH,
    .screen_height = DEFAULT_SCREEN_HEIGHT,
    .border_width = DEFAULT_BORDER_WIDTH,
    .center_offset = DEFAULT_CENTER_OFFSET,

    // timer, beeper, ay, covox, global
    .volume = {0.1f, 0.1f, 0.1f, 0.1f, 1.5f},
    .enable = {/* timer ch0..2 */ true, true, true,
               /* ay ch0..2 */    true, true, true},
    .nofilter = false,
};

void options(int argc, char ** argv)
{
    /* PSP version: simplified, no command line parsing needed */
    Options.gl.use_shader = false;
    Options.gl.default_shader = false;
    Options.gl.filtering = false;
    Options.nosound = false;
    Options.nofdc = false;
    Options.bootpalette = true;
    Options.novideo = false;  /* PSP GU rendering is enabled */

    /* Turn on cadence (6:5 pullup at 60 Hz, see TV::init): this locks
     * the machine to 50 frames per wall-clock second. Emulator::
     * execute_frame() only uses cadence when both flags are set. */
    Options.vsync = true;
    Options.vsync_enable = true;
}

void _options::parse_log(const std::string & opt)
{
    /* Not used on PSP */
}

std::string _options::path_for_frame(int n)
{
    return "frame" + std::to_string(n);
}

void _options::load(const std::string & filename)
{
    /* Not used on PSP */
}

void _options::save(const std::string & filename)
{
    /* Not used on PSP */
}

std::string _options::get_config_path(void)
{
    return "v06x.conf";
}
