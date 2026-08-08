#pragma once

#include <string>
#include <vector>

namespace FileBrowser
{
    /* List *.rom files from given directory */
    void listRoms(const std::string &dir, std::vector<std::string> &files);
}
