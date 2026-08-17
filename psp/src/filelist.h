#pragma once

#include <string>
#include <vector>

/* Pure file-system helper of the ROM selection UI: no PSP pad, no
 * textures, no knowledge of the popup windows. The UI window that
 * shows the result lives in rombrowser.h. */
namespace FileList
{
    /* List Vector-06C ROM files (ROM_EXTENSIONS) from the given
     * directory, sorted alphabetically case-insensitively. Returns
     * false when the directory cannot be opened (files is empty
     * then), true after a successful scan (files may still be
     * empty). */
    bool listRoms(const std::string &dir, std::vector<std::string> &files);

    /* Find the preview image of a ROM in the same directory: the
     * ROM's base name (everything before the last dot) plus ".tga",
     * matched case-insensitively so "PUTUP.ROM" finds "putup.tga"
     * regardless of how the file is actually stored. On success the
     * full path is built with the name as stored in the directory
     * and true is returned; false means "no preview". */
    bool findPreview(const std::string &dir, const std::string &rom_name,
                     std::string &out_path);
}
