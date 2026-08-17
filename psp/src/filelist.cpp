#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <pspiofilemgr_dirent.h>

#include <cstring>
#include <cstdio>
#include <cctype>
#include <algorithm>

#include "filelist.h"

namespace FileList
{
    /* Extensions treated as Vector-06C ROMs; everything else
     * (.gif/.png/.bmp/.txt/.ini, ...) stays out of the list. */
    static const char * const ROM_EXTENSIONS[] = {
        ".rom",
        ".bin",
        ".r0m",
    };

    bool hasRomExtension(const std::string &name)
    {
        /* Accept .rom, .ROM, .bin, .BIN, ... */
        size_t dot = name.rfind('.');
        if (dot == std::string::npos)
            return false;

        std::string ext = name.substr(dot);
        for (size_t i = 0; i < ext.size(); ++i)
            ext[i] = (char)tolower(ext[i]);
        
        /* std::string compares against const char * directly. */
        for (const char * rom_ext : ROM_EXTENSIONS)
            if (ext == rom_ext)
                return true;
        return false;
    }

    /* Alphabetical, case-insensitive, stable. */
    static bool ci_less(const std::string &a, const std::string &b)
    {
        return std::lexicographical_compare(
            a.begin(), a.end(), b.begin(), b.end(),
            [](char ca, char cb) {
                return tolower((unsigned char)ca)
                     < tolower((unsigned char)cb);
            });
    }

    bool listRoms(const std::string &dir, std::vector<std::string> &files)
    {
        files.clear();

        SceUID fd = sceIoDopen(dir.c_str());
        if (fd < 0)
        {
            return false;
        }

        SceIoDirent entry;
        memset(&entry, 0, sizeof(entry));

        while (sceIoDread(fd, &entry) > 0)
        {
            if (entry.d_name[0] == '.')
            {
                continue;
            }
            if (entry.d_stat.st_mode & FIO_S_IFDIR)
            {
                continue;
            }
            std::string name(entry.d_name);
            if (hasRomExtension(name))
            {
                files.push_back(name);
            }
        }

        sceIoDclose(fd);

        std::stable_sort(files.begin(), files.end(), ci_less);
        return true;
    }

    /* Case-insensitive whole-name compare. */
    static bool ci_equal(const std::string &a, const std::string &b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
                return false;
        return true;
    }

    bool findPreview(const std::string &dir, const std::string &rom_name,
                     std::string &out_path)
    {
        /* Base name: everything before the last dot (the ROM
         * extension is never part of the preview name). */
        const size_t dot = rom_name.rfind('.');
        if (dot == std::string::npos || dot == 0)
            return false;
        const std::string base = rom_name.substr(0, dot);

        SceUID fd = sceIoDopen(dir.c_str());
        if (fd < 0)
            return false;

        SceIoDirent entry;
        memset(&entry, 0, sizeof(entry));

        while (sceIoDread(fd, &entry) > 0)
        {
            if (entry.d_name[0] == '.')
                continue;
            if (entry.d_stat.st_mode & FIO_S_IFDIR)
                continue;

            std::string name(entry.d_name);
            const size_t ndot = name.rfind('.');
            if (ndot == std::string::npos || ndot == 0)
                continue;
            /* base + ".tga", any case on both parts */
            if (ci_equal(name.substr(0, ndot), base)
                    && ci_equal(name.substr(ndot), ".tga")) {
                sceIoDclose(fd);
                out_path = dir + "/" + name;
                return true;
            }
        }

        sceIoDclose(fd);
        return false;
    }
}
