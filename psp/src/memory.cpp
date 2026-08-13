#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <vector>

#include "memory.h"
#include "i8080.h"
#include <fstream>
#include "debuglog.h"

using namespace std;

Memory::Memory() : mode_stack(false), mode_map(false), page_map(0),
    page_stack(0)
{
    memset(bytes, 0, sizeof(bytes));
    printf("memory init\n");
}

// Barkar extensions:
//
// 1 = enable
//
// 7              screen 0    0xe000-0xffff  8k
//  6             screen 3    0x8000-0x9fff  8k
//   5            screen 1-2  0xa000-0xdfff  16k  default Kishinev version
//    4           stack
//     32         stack page
//       10       screen page
void Memory::control_write(uint8_t w8)
{
    this->mode_stack = (w8 & 0x10) != 0;
    this->mode_map = w8 & 0xe0;

    this->page_map = ((w8 & 3) + 1) << 16;
    this->page_stack = (((w8 & 0xc) >> 2) + 1) << 16;

    //printf("memory: raw=%02x mode_stack=%x mode_map=%02x page_map=%x page_stack=%x\n",
    //        w8, this->mode_stack, this->mode_map, this->page_map, this->page_stack);
}

/* bigram_select/tobank/read/get_byte/write are defined inline in
 * memory.h: they run on every CPU memory access */

void Memory::init_from_vector(const vector<uint8_t> & from, uint32_t start_addr)
{
    // clear the main ram because otherwise switching roms is a pain
    // but leave the kvaz alone
    if (start_addr < 65536) {
        memset(this->bytes, 0, 65536);
    }
    else {
        memset(this->bytes + start_addr, 0, sizeof(bytes) - start_addr);
    }
    for (unsigned i = 0; i < from.size(); ++i) {
        int addr = start_addr + i;
        //this->write(addr, from[i], false);
        uint32_t phys = this->tobank(addr);
        if (phys < sizeof(this->bytes)) {
            this->bytes[phys] = from[i];
        }
    }
}

void Memory::attach_boot(vector<uint8_t> boot)
{
    this->bootbytes = boot;
}

void Memory::detach_boot()
{
    this->bootbytes.clear();
}

uint8_t * Memory::buffer() 
{
    return bytes;
}

#include "serialize.h"

void Memory::serialize(std::vector<uint8_t> &to) {
    std::vector<uint8_t> tmp;
    tmp.push_back((uint8_t)mode_stack);
    tmp.push_back((uint8_t)mode_map);
    tmp.push_back((uint8_t)(page_map>>16));
    tmp.push_back((uint8_t)(page_stack>>16));
    tmp.push_back(sizeof(this->bytes)/65536); // normally 1+4, but we could get many ramdisks later
    tmp.insert(std::end(tmp), this->bytes, this->bytes + sizeof(this->bytes));
    tmp.insert(std::end(tmp), std::begin(this->bootbytes), std::end(this->bootbytes));

    SerializeChunk::insert_chunk(to, SerializeChunk::MEMORY, tmp);
}

void Memory::deserialize(std::vector<uint8_t>::iterator it, uint32_t size)
{
    auto begin = it;
    this->mode_stack = (bool) *it++;
    this->mode_map = (uint8_t) *it++;
    this->page_map = ((uint32_t) *it++) << 16;
    this->page_stack = ((uint32_t) *it++) << 16;
    uint32_t stored_ramsize = 65536 * *it++;
    size_t nbytes = std::min(stored_ramsize, (uint32_t)sizeof(this->bytes));
    std::copy(it, it + nbytes, this->bytes);
    it += stored_ramsize;

    this->bootbytes.clear();
    this->bootbytes.assign(it, begin + size);
}

void Memory::cool_off_heatmap()
{
    for (auto it = heatmap.begin(); it < heatmap.end(); ++it) {
        //if (*it > 0) printf("%04x %02x\n ", it - heatmap.begin(), *it);
        int i = *it;
        if (i > 64) {
            *it -= 10;
        }
        else {
            int v = static_cast<int>(*it) - 5;
            *it = (v < 0) ? 0 : (v > 255) ? 255 : v;
        }
    }
}

void Memory::export_bytes(uint8_t * dst, uint32_t addr, uint32_t size) const
{
    for (uint32_t i = 0; i < size; ++i) {
        dst[i] = this->bytes[this->tobank(addr + i)];
    }
}


auto Memory::get_mode_stack() const -> const bool
{
    return mode_stack;
}

auto Memory::get_mode_map() const -> const uint8_t
{
    return mode_map;
}

auto Memory::get_page_map() const -> const uint32_t
{
    return page_map>>16 - 1;
}

auto Memory::get_page_stack() const -> const uint32_t
{
    return page_stack>>16 - 1;
}

bool Memory::save_dump(const std::string& path) const
{
    uint8_t dump[0x10000];

    // Dump CPU-visible address space 0000h-FFFFh.
    // Use read() so that the current memory bank mapping is respected.
    for (uint32_t addr = 0; addr < 0x10000; ++addr) {
        dump[addr] = read(addr, false);
    }

    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        dbglog("Failed to save memory dump: %s\n", path.c_str());
        printf("Failed to save memory dump: %s\n", path.c_str());
        return false;
    }

    const size_t written = std::fwrite(dump, 1, sizeof(dump), file);
    std::fclose(file);

    if (written != sizeof(dump)) {
        dbglog("Incomplete memory dump: %s (%lu/%lu bytes)\n",
               path.c_str(),
               static_cast<unsigned long>(written),
               static_cast<unsigned long>(sizeof(dump)));
        return false;
    }

    dbglog("Memory dump saved: %s (65536 bytes)\n", path.c_str());
    printf("Memory dump saved: %s (65536 bytes)\n", path.c_str());

    return true;
}