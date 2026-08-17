#pragma once

#include <inttypes.h>
#include <string>
#include <vector>

/*
 * SAVE/LOAD STATE storage (Stage 5). Plain files in the game folder,
 * never the PSP SAVEDATA infrastructure:
 *
 *     ms0:/PSP/GAME/VECTOR06C/SAVES/
 *         <ROM base name>/            e.g. RISEOUT/
 *             state1.bin  state1.tga
 *             state2.bin  state2.tga
 *             ...
 *
 * stateN.bin is the real emulator state: a small header (magic,
 * version, save timestamp) followed by the unchanged
 * Board::serialize() payload. stateN.tga is only the visual preview
 * of the slot (Vector screenshot, written by tga_save); the machine
 * itself is never restored from it.
 *
 * The directory name is the whole ROM binding: states of other ROMs
 * are simply never scanned (the ROM base name comes from
 * Emulator::get_rom_base()). Missing directory = no states.
 *
 * Everything runs in the worker thread while the machine is paused;
 * file IO here is plain blocking stdio/sceIo.
 */
namespace StateFile
{
    /* Bumped whenever the serialized Board layout changes; a file
     * with an unknown version is refused as "Invalid state". */
    static const int STATE_VERSION = 1;
    /* magic[4] + version u32 LE + timestamp u64 LE (unix seconds,
     * the moment of saving; shown in the slot). */
    static const int HEADER_SIZE = 16;

    /* SAVES/<rom_base> */
    std::string rom_dir(const std::string & rom_base);
    /* stateN.bin / stateN.tga inside dir; slot is 1-based. */
    std::string bin_path(const std::string & dir, int slot);
    std::string shot_path(const std::string & dir, int slot);

    /* Create SAVES/ and dir when missing; true when dir exists (or
     * was created) afterwards. */
    bool ensure_dir(const std::string & dir);

    bool exists(const std::string & path);
    /* Read only the header of an existing stateN.bin: false when the
     * file is missing, too short, has a wrong magic or an unknown
     * version. */
    bool read_header(const std::string & bin_path, uint64_t & out_ts);

    /* Safe overwrite (§19): header + payload go into a temp file
     * first; only after a complete write the old state is replaced.
     * A failed write leaves the previous state intact. */
    bool save(const std::string & dir, int slot,
              const std::vector<uint8_t> & payload, uint64_t timestamp);
    /* The reverse: full file, header checked, payload stripped into
     * out; false on any missing/corrupt/unknown-version file, the
     * Board is never touched then. */
    bool load(const std::string & dir, int slot,
              std::vector<uint8_t> & out_payload, uint64_t & out_ts);
}
