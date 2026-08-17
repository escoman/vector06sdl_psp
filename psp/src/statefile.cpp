#include "statefile.h"
#include "util.h"
#include "debuglog.h"

#include <pspiofilemgr.h>

#include <cstdio>
#include <cstring>

/*
 * SAVE/LOAD STATE storage, see statefile.h. The state file layout:
 *
 *   [0..3]   magic "V06S"
 *   [4..7]   STATE_VERSION, little-endian u32
 *   [8..15]  save timestamp (unix seconds), little-endian u64
 *   [16..]   the unchanged Board::serialize() payload
 *
 * Overwriting goes through a temp file first (§19): a failed write
 * never destroys the previous state.
 */

namespace StateFile
{
    static const char SAVES_DIR[] = "ms0:/PSP/GAME/VECTOR06C/SAVES";
    static const char MAGIC[4] = { 'V', '0', '6', 'S' };

    static void put32(std::vector<uint8_t> & v, uint32_t x)
    {
        v.push_back((uint8_t)(x & 0xff));
        v.push_back((uint8_t)((x >> 8) & 0xff));
        v.push_back((uint8_t)((x >> 16) & 0xff));
        v.push_back((uint8_t)((x >> 24) & 0xff));
    }

    static void put64(std::vector<uint8_t> & v, uint64_t x)
    {
        put32(v, (uint32_t)(x & 0xffffffffu));
        put32(v, (uint32_t)(x >> 32));
    }

    static uint32_t get32(const uint8_t * p)
    {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
             | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }

    static uint64_t get64(const uint8_t * p)
    {
        return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
    }

    std::string rom_dir(const std::string & rom_base)
    {
        return std::string(SAVES_DIR) + "/" + rom_base;
    }

    std::string bin_path(const std::string & dir, int slot)
    {
        char name[32];
        snprintf(name, sizeof(name), "/state%d.bin", slot);
        return dir + name;
    }

    std::string shot_path(const std::string & dir, int slot)
    {
        char name[32];
        snprintf(name, sizeof(name), "/state%d.tga", slot);
        return dir + name;
    }

    /* sceIoMkdir fails when the directory already exists, so the
     * stat check decides between "already there" and a real error. */
    static bool mkdir_ok(const char * path)
    {
        if (sceIoMkdir(path, 0777) >= 0)
            return true;

        SceIoStat st;
        memset(&st, 0, sizeof(st));
        return sceIoGetstat(path, &st) >= 0
            && (st.st_mode & FIO_S_IFDIR) != 0;
    }

    bool ensure_dir(const std::string & dir)
    {
        if (!mkdir_ok(SAVES_DIR))
            return false;
        return mkdir_ok(dir.c_str());
    }

    bool exists(const std::string & path)
    {
        SceIoStat st;
        memset(&st, 0, sizeof(st));
        return sceIoGetstat(path.c_str(), &st) >= 0;
    }

    bool read_header(const std::string & bin_path, uint64_t & out_ts)
    {
        FILE * f = std::fopen(bin_path.c_str(), "rb");
        if (f == nullptr)
            return false;

        uint8_t hdr[HEADER_SIZE];
        const size_t got = std::fread(hdr, 1, sizeof(hdr), f);
        std::fclose(f);

        if (got != sizeof(hdr)
                || memcmp(hdr, MAGIC, sizeof(MAGIC)) != 0
                || get32(hdr + 4) != (uint32_t)STATE_VERSION) {
            return false;
        }
        out_ts = get64(hdr + 8);
        return true;
    }

    bool save(const std::string & dir, int slot,
              const std::vector<uint8_t> & payload, uint64_t timestamp)
    {
        std::vector<uint8_t> buf;
        buf.reserve(HEADER_SIZE + payload.size());
        buf.insert(buf.end(), MAGIC, MAGIC + sizeof(MAGIC));
        put32(buf, (uint32_t)STATE_VERSION);
        put64(buf, timestamp);
        buf.insert(buf.end(), payload.begin(), payload.end());

        const std::string dst = bin_path(dir, slot);
        const std::string tmp = dst + ".tmp";

        const int written = util::save_binfile(tmp, buf);
        if (written != (int)buf.size()) {
            std::remove(tmp.c_str());
            dbglog("StateFile: write failed: %s (%d of %lu)\n",
                   tmp.c_str(), written, (unsigned long)buf.size());
            return false;
        }

        /* Replace the old state only with a complete new one. */
        if (util::careful_rename(tmp, dst) != 0) {
            std::remove(tmp.c_str());
            dbglog("StateFile: rename failed: %s -> %s\n",
                   tmp.c_str(), dst.c_str());
            return false;
        }

        dbglog("StateFile: saved %s (%lu bytes)\n",
               dst.c_str(), (unsigned long)buf.size());
        return true;
    }

    bool load(const std::string & dir, int slot,
              std::vector<uint8_t> & out_payload, uint64_t & out_ts)
    {
        const std::string path = bin_path(dir, slot);
        std::vector<uint8_t> data = util::load_binfile(path);
        if (data.size() < HEADER_SIZE
                || memcmp(data.data(), MAGIC, sizeof(MAGIC)) != 0) {
            dbglog("StateFile: bad magic / truncated: %s\n", path.c_str());
            return false;
        }
        if (get32(data.data() + 4) != (uint32_t)STATE_VERSION) {
            dbglog("StateFile: unknown version %lu: %s\n",
                   (unsigned long)get32(data.data() + 4), path.c_str());
            return false;
        }

        out_ts = get64(data.data() + 8);
        out_payload.assign(data.begin() + HEADER_SIZE, data.end());
        return true;
    }
}
