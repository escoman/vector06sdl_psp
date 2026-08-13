// vector06sdl PSP port
// Event-based sound pipeline
//
// The CPU core no longer steps the sound chips clock by clock after
// every instruction. Instead, writes to the sound chips (8253 timer,
// AY-3-8910, PIA tape out, Covox) are captured here as timestamped
// events, and Soundnik renders audio in one batch per frame
// (Soundnik::process_frame()).
#pragma once

#include <stdint.h>

/* Sound clock timebase: 1.5 MHz (CPU T-states / 2), the same scale the
 * old soundSteps() received. One frame = 1497600 / 50 = 29952 clocks. */

enum class SoundEventType : uint8_t {
    TimerReg = 0,   /* 8253 write: addr = ~port & 3 (0-2 counters, 3 = CW) */
    AyReg,          /* AY write: addr = port & 1 (0 = data, 1 = register) */
    TapeOut,        /* PIA1 PC bit0 level: value = 0/1 */
    TapeIn,         /* tape player level: value = 0/1 */
    Covox,          /* PPI2 PA2 DAC byte: value = PA2 */
};

struct SoundEvent {
    uint64_t clock;     /* absolute 1.5 MHz sound clock at commit time */
    SoundEventType type;
    uint8_t addr;
    uint8_t value;
};

/* Fixed-size ring buffer. Producer and consumer are both the emulation
 * thread (writes happen during OUT commits, draining happens once per
 * frame), so no locking is required. */
class SoundEventQueue
{
public:
    static const int CAPACITY = 8192;   /* power of two */

    SoundEventQueue() : dropped(0), head(0), tail(0) {}

    void clear()
    {
        head = tail = 0;
    }

    bool empty() const
    {
        return head == tail;
    }

    /* Drops the event if the queue is full. */
    void push(const SoundEvent & e)
    {
        int next = (head + 1) & (CAPACITY - 1);
        if (next == tail) {
            ++dropped;
            return;
        }
        events[head] = e;
        head = next;
    }

    const SoundEvent & peek() const
    {
        return events[tail];
    }

    void pop()
    {
        tail = (tail + 1) & (CAPACITY - 1);
    }

    int dropped;

private:
    SoundEvent events[CAPACITY];
    int head;
    int tail;
};
