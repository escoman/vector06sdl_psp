#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <pspiofilemgr_dirent.h>

#include <cstring>
#include <cstdio>
#include <cctype>
#include <algorithm>

#include "filebrowser.h"

namespace FileBrowser
{
    bool hasRomExtension(const std::string &name)
    {
        /* Accept .rom, .ROM, .bin, .BIN */
        size_t dot = name.rfind('.');
        if (dot == std::string::npos)
        {
            return false;
        }
        std::string ext = name.substr(dot);
        for (size_t i = 0; i < ext.size(); ++i)
        {
            ext[i] = (char)tolower(ext[i]);
        }
        return ext == ".rom" || ext == ".bin" || ext == ".r0m";
    }

    void listRoms(const std::string &dir, std::vector<std::string> &files)
    {
        files.clear();

        SceUID fd = sceIoDopen(dir.c_str());
        if (fd < 0)
        {
            return;
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

        /* Simple alphabetical sort */
        std::sort(files.begin(), files.end());
    }
}
