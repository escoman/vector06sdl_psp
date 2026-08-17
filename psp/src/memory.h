#pragma once

#include <inttypes.h>
#include <cstring>
#include <cstdio>
#include <array>
#include <vector>
#include <functional>
#include <string>

#define TOTAL_MEMORY (64 * 1024 + 256 * 1024)


class Memory {
public:
    typedef std::array<uint8_t, TOTAL_MEMORY> heatmap_t;

private:
    uint8_t bytes[TOTAL_MEMORY];
    bool mode_stack;
    uint8_t mode_map;
    uint32_t page_map;
    uint32_t page_stack;

    std::vector<uint8_t> bootbytes;

    heatmap_t heatmap;

    /* Hot path helpers: defined inline below because they run on
     * every CPU memory access */
    __attribute__((always_inline)) uint32_t tobank(uint32_t a) const
    {
        return (a & 0x78000) | ((a<<2)&0x7ffc) | ((a>>13)&3);
    }

public:
    __attribute__((always_inline)) uint32_t bigram_select(uint32_t addr, bool stackrq) const
    {
        if (!(this->mode_map || this->mode_stack)) {
            return addr;
        } else if (this->mode_stack && stackrq) {
            return addr + this->page_stack;
        } else if ((this->mode_map & 0x20) && (addr >= 0xa000) && (addr <= 0xdfff)) {
            return addr + this->page_map;
        } else if ((this->mode_map & 0x40) && (addr >= 0x8000) && (addr <= 0x9fff)) {
            return addr + this->page_map;
        } else if ((this->mode_map & 0x80) && (addr >= 0xe000) && (addr <= 0xffff)) {
            return addr + this->page_map;
        }
        return addr;
    }

    __attribute__((always_inline)) uint8_t get_byte(uint32_t addr, bool stackrq) const
    {
        uint8_t value;

        uint32_t bigaddr = this->bigram_select(addr & 0xffff, stackrq);
        if (this->bootbytes.size() && bigaddr < this->bootbytes.size()) {
            value = this->bootbytes[bigaddr];
        }
        else {
            value = this->bytes[this->tobank(bigaddr)];
        }

        return value;
    }
    /* virtual addr, physical addr, stackrq, value */
    std::function<void(uint32_t,uint32_t,bool,uint8_t)> onwrite;
    std::function<void(uint32_t,uint32_t,bool,uint8_t)> onread;

    std::function<void(const uint32_t, const uint8_t, const bool)> debug_onread;
    std::function<void(const uint32_t, const uint8_t)> debug_onwrite;

public:
    Memory();
    void control_write(uint8_t w8);

    /* Kvaz paging back to the power-on state (the constructor
     * values). Used by the ROM load path: init_from_vector() places
     * the bytes through the CURRENT mapping, so the mapping must be
     * the power-on default first, or the previous ROM's port 0x10
     * writes would land the new ROM in wrong banks. */
    void reset_paging()
    {
        mode_stack = false;
        mode_map = 0;
        page_map = 0;
        page_stack = 0;
    }

    __attribute__((always_inline)) uint8_t read(uint32_t addr, bool stackrq, const bool _is_opcode = false) const
    {
        uint8_t value;
        uint32_t phys = addr;

        uint32_t bigaddr = this->bigram_select(addr & 0xffff, stackrq);
        if (this->bootbytes.size() && bigaddr < this->bootbytes.size()) {
            value = this->bootbytes[bigaddr];
        }
        else {
            phys = this->tobank(bigaddr);
            value = this->bytes[phys];
        }

        if (this->onread) this->onread(addr, phys, stackrq, value);

        if (debug_onread)
        {
            debug_onread(bigaddr, value, _is_opcode);
        }

        return value;
    }

    __attribute__((always_inline)) void write(uint32_t addr, uint8_t w8, bool stackrq)
    {
        uint32_t bigaddr = this->bigram_select(addr & 0xffff, stackrq);
        uint32_t phys = this->tobank(bigaddr);
        if (this->onwrite) {
            this->onwrite(addr, phys, stackrq, w8);
        }
        this->bytes[phys] = w8;

        if (bigaddr < this->heatmap.size()) {
            //this->heatmap[phys] = std::clamp(this->heatmap[phys] + 64, 0, 255);
            this->heatmap[bigaddr] = 255;
        }

        if (debug_onwrite) debug_onwrite(bigaddr, w8);
    }
    void init_from_vector(const std::vector<uint8_t> & from, uint32_t start_addr);
    void attach_boot(std::vector<uint8_t> boot);
    void detach_boot();
    uint8_t * buffer();
    size_t buffer_size() const { return sizeof(bytes); }
    heatmap_t& get_heatmap() { return heatmap; }
    void cool_off_heatmap();
    void export_bytes(uint8_t * dst, uint32_t addr, uint32_t size) const;

    void serialize(std::vector<uint8_t> & to);
    void deserialize(std::vector<uint8_t>::iterator from, uint32_t size);
    
    auto get_mode_stack() const -> const bool;
    auto get_mode_map() const -> const uint8_t;
    auto get_page_map() const -> const uint32_t;
    auto get_page_stack() const -> const uint32_t;

    bool save_dump(const std::string& path) const;
};
