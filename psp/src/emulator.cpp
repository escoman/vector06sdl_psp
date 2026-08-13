#include "emulator.h"
#include <cstring>
#include "util.h"

Emulator::Emulator(Board & borat) : board(borat)
{
}

Emulator::~Emulator()
{
}

int Emulator::execute_frame()
{
    for (int i = 0; i < N_SCANCODES; ++i) {
        if (this->keydowns[i]) {
            SDL_KeyboardEvent ev;
            ev.keysym.scancode = this->keydowns[i];
            board.handle_keydown(ev);
        }
        if (this->keyups[i]) {
            SDL_KeyboardEvent ev;
            ev.keysym.scancode = this->keyups[i];
            board.handle_keyup(ev);
        }
        this->keydowns[i] = this->keyups[i] = 0;
    }
    int executed;
    if (Options.vsync && Options.vsync_enable) {
        executed = board.execute_frame_with_cadence(true, true);
    }
    else {
        executed = board.execute_frame_with_cadence(true, false);
    }
    return executed ? 1 : 0;
}

void Emulator::export_pixel_bytes(uint8_t * dst)
{
    memcpy(dst, board.get_tv().pixels(), pixel_bytes_size());
}

void Emulator::export_audio_frame(float * dst, size_t framesize)
{
    /* PSP version: audio is handled by the PSP audio callback thread.
     * This function is not used in the PSP main loop. */
    (void)dst;
    (void)framesize;
}

size_t Emulator::pixel_bytes_size() {
    return (size_t) (Options.screen_width * Options.screen_height * 4);
}

void Emulator::keydown(int scancode) {
    for (int i = 0; i < N_SCANCODES; ++i) {
        if (this->keydowns[i] == 0 || this->keydowns[i] == scancode) {
            this->keydowns[i] = scancode;
            break;
        }
    }
}

void Emulator::keyup(int scancode) {
    for (int i = 0; i < N_SCANCODES; ++i) {
        if (this->keyups[i] == 0 || this->keyups[i] == scancode) {
            this->keyups[i] = scancode;
        }
    }
}

void Emulator::start_emulator_thread()
{
}

void Emulator::save_state(vector <uint8_t> &to) {
    this->board.serialize(to);
}

bool Emulator::restore_state(vector <uint8_t> &from) {
    return this->board.deserialize(from);
}
