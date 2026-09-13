/*
 * 8253 Differential Test: CounterUnit::Count() (clock-by-clock)
 * vs. Soundnik::integrate_timer() (batch)
 *
 * Standalone native test. Does NOT modify any production code.
 *
 * Build:
 *   g++ -std=c++17 -O2 -I../../src -o test_8253 test_8253.cpp && ./test_8253
 */

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

/* Access individual counter OUT via the public I8253 API.
 * I8253::Count(1, ch0, ch1, ch2) returns per-counter outputs. */
#include "8253.h"

/* ------------------------------------------------------------------
 * Batch timer — faithful copy of Soundnik::TimerChannel and
 * Soundnik::integrate_timer() from psp/src/sound.cpp.
 * ----------------------------------------------------------------*/
struct BatchTimer {
    int mode;
    int latch_mode;
    bool bcd;
    int write_state;
    uint8_t write_lsb;
    int loadvalue;
    bool enabled;
    int out;
    /* --- new fields matching CounterUnit semantics --- */
    int value;       /* current counter value */
    bool armed;      /* mode 0: transition pending */
    bool load;       /* new load value pending */
    /* --- legacy fields (kept for ABI) --- */
    int phase;
    int remain;
};

static void batch_reset(BatchTimer &ch) {
    ch.mode = 0; ch.latch_mode = 0; ch.bcd = false;
    ch.write_state = 0; ch.write_lsb = 0;
    ch.loadvalue = 0; ch.enabled = false;
    ch.out = 0; ch.value = 0;
    ch.armed = false; ch.load = false;
    ch.phase = 0; ch.remain = 0;
}

/* Mirror of Soundnik::apply_timer_write() for the data-path only
 * (no delay modelling — that is the whole point of the test). */
static void batch_set_mode(BatchTimer &ch, int mode, int latch_mode) {
    ch.mode = mode;
    ch.latch_mode = latch_mode;
    ch.bcd = false;
    ch.write_state = 0;
    ch.enabled = false;
    ch.out = (mode == 0) ? 0 : 1;
    ch.value = 0;
    ch.armed = (mode == 0);
    ch.load = false;
    ch.phase = 0;
    ch.remain = 0;
}

static void batch_load(BatchTimer &ch, int loadvalue) {
    ch.loadvalue = loadvalue;
    ch.load = true;
    /* integrate_timer will process the load (set enabled, value, etc.) */
}

/* Exact copy of Soundnik::integrate_timer() from psp/src/sound.cpp
 * (without delay handling — the test consumes dead clocks on the
 * reference side instead). */
static int integrate_timer(BatchTimer &ch, int dt) {
    if (!ch.enabled && !ch.load) return ch.out ? dt : 0;
    switch (ch.mode) {
    case 0: {
        /* Load and countdown happen in the SAME call, matching
         * CounterUnit::Count() where both occur in one invocation.
         * The reference returns out AFTER the transition. */
        if (ch.load) {
            ch.value = ch.loadvalue;
            ch.enabled = true;
            ch.armed = true;
            ch.out = 0;
            ch.load = false;
        }
        if (!ch.enabled) return 0;
        if (ch.out) return dt;

        int high = 0;
        int rem = dt;
        int prev = ch.value;
        ch.value -= rem;
        if (ch.value <= 0 && ch.armed && prev > 0) {
            ch.armed = false;
            ch.out = 1;
            ch.value += ch.bcd ? 10000 : 65536;
            high = (rem >= prev) ? (rem - prev + 1) : 0;
        }
        return high;
    }
    case 2:
        return dt;
    case 3: {
        /* Square wave generator.
         * Counter decrements by 2 per clock (by 1 or 3 at boundaries
         * for odd loadvalue). OUT toggles when value <= 0. */
        if (!ch.enabled && ch.load) {
            ch.value = ch.loadvalue;
            ch.enabled = true;
            ch.load = false;
        }
        if (!ch.enabled) return ch.out ? dt : 0;

        int load = ch.loadvalue ? ch.loadvalue : 65536;
        int high = 0;
        int rem = dt;

        while (rem > 0) {
            int dur;
            if (ch.out && ch.value == load && (load & 1))
                dur = (load + 1) / 2;
            else
                dur = (ch.value + 1) / 2;
            if (dur < 1) dur = 1;

            if (dur > rem) {
                if (ch.out) high += rem;
                ch.value -= 2 * rem;
                rem = 0;
            } else {
                if (ch.out) high += dur;
                rem -= dur;
                ch.out ^= 1;
                /* Post-toggle value: reference does value += load
                 * from the negative value.  For odd load after high:
                 * load-1; for even load after high: load;
                 * after low (any load): load. */
                if (ch.out) {
                    ch.value = load;
                } else {
                    ch.value = load - (load & 1);
                }
            }
        }
        return high;
    }
    default:
        return ch.out ? dt : 0;
    }
}

/* ------------------------------------------------------------------
 * Test infrastructure
 * ----------------------------------------------------------------*/
static int total_tests = 0;
static int passed_tests = 0;
static int failed_tests = 0;

struct TraceEntry {
    int clock;
    int ref_out;
    int batch_out;
    int ref_value;
    int batch_value;
    int ref_phase;
    int batch_phase;
    bool diff;
};

/* Compute the number of "dead" clocks before the reference CounterUnit
 * starts actual counting (delay + mode-specific settle). */
static int count_dead_clocks(int mode) {
    /* SetMode: delay = LATCH_DELAY (1)
     * write_value: delay = 3 (mode 0/2/3, !enabled)
     * Count consumes 2 clocks in delay before counting starts.
     * Mode 0 needs one extra clock to consume the load step. */
    return (mode == 0) ? 3 : 2;
}

/* Helper: configure counter 0 via the public I8253 API.
 * Control word format: counter_sel(2) | latch_mode(2) | mode(2) | bcd(1)
 * For counter 0, latch_mode=3 (LSB+MSB), bcd=0: cw = 0x30 | (mode<<1) */
static void ref_setup(I8253 &ref, int mode, int loadval) {
    uint8_t cw = (uint8_t)(0x30 | ((mode & 3) << 1));  /* counter 0, LSB+MSB */
    ref.write(3, cw);
    ref.write(0, (uint8_t)(loadval & 0xFF));
    ref.write(0, (uint8_t)((loadval >> 8) & 0xFF));
}

/* Helper: get counter 0 OUT via public API */
static int ref_count0(I8253 &ref) {
    int ch0, ch1, ch2;
    ref.Count(1, ch0, ch1, ch2);
    return ch0;
}

/* ----------------------------------------------------------------*/
static void run_scenario(const char *label, int mode, int loadval,
                         int total_clocks)
{
    total_tests++;
    printf("=== %s (mode=%d load=%d clocks=%d) ===\n",
           label, mode, loadval, total_clocks);

    /* Reference */
    I8253 ref;
    ref_setup(ref, mode, loadval);

    /* Batch */
    BatchTimer batch;
    batch_reset(batch);
    batch_set_mode(batch, mode, 3);
    batch_load(batch, loadval);

    int dead = count_dead_clocks(mode);

    /* Consume dead clocks on the reference */
    for (int i = 0; i < dead; i++) ref.Count(1);

    /* Per-clock comparison */
    std::vector<TraceEntry> trace;
    int first_diff = -1;
    int max_diffs = 30;
    int ndiffs = 0;

    int n = total_clocks - dead;
    for (int i = 0; i < n; i++) {
        int r_out = ref_count0(ref);
        int b_out = integrate_timer(batch, 1);

        /* Access internal state via the friend trick — not needed,
         * we use the public Count() return value and track state
         * indirectly. For the batch we have direct access. */
        TraceEntry e;
        e.clock = i + 1;
        e.ref_out = r_out;
        e.batch_out = b_out;
        e.diff = (r_out != b_out);
        trace.push_back(e);

        if (e.diff) {
            if (first_diff < 0) first_diff = i + 1;
            ndiffs++;
        }
    }

    if (first_diff < 0) {
        printf("  PASSED (%d clocks compared)\n\n", n);
        passed_tests++;
    } else {
        printf("  FAILED — first divergence at clock %d "
               "(%d diffs in %d clocks)\n", first_diff, ndiffs, n);
        printf("  %-8s %-10s %-10s\n", "clock", "ref_OUT", "batch_OUT");
        printf("  %-8s %-10s %-10s\n", "-----", "-------", "---------");

        /* Show 5 before first diff and 10 after */
        int show_from = (first_diff > 6) ? first_diff - 5 : 1;
        int show_to = first_diff + 10;
        if (show_to > n) show_to = n;
        int shown = 0;
        for (int i = show_from - 1; i < show_to && shown < 30; i++) {
            const auto &e = trace[i];
            printf("  %-8d %-10d %-10d%s\n",
                   e.clock, e.ref_out, e.batch_out,
                   e.diff ? "  <-- DIFF" : "");
            shown++;
        }
        printf("\n");
        failed_tests++;
    }
}

/* ----------------------------------------------------------------*/
static void run_batch_sum_test(const char *label, int mode, int loadval,
                               int batch_size, int n_batches)
{
    total_tests++;
    printf("=== %s (mode=%d load=%d batch=%d x%d) ===\n",
           label, mode, loadval, batch_size, n_batches);

    /* Reference: run clock-by-clock, sum OUT per batch window */
    I8253 ref;
    ref_setup(ref, mode, loadval);

    /* Batch */
    BatchTimer batch;
    batch_reset(batch);
    batch_set_mode(batch, mode, 3);
    batch_load(batch, loadval);

    int dead = count_dead_clocks(mode);
    for (int i = 0; i < dead; i++) ref.Count(1);

    int first_diff = -1;
    int total_compared = 0;

    for (int b = 0; b < n_batches; b++) {
        /* Reference: sum individual clocks */
        int ref_sum = 0;
        for (int c = 0; c < batch_size; c++)
            ref_sum += ref_count0(ref);

        /* Batch: single call */
        int batch_sum = integrate_timer(batch, batch_size);

        total_compared++;
        if (ref_sum != batch_sum) {
            if (first_diff < 0) first_diff = b;
            printf("  batch #%d: ref_sum=%d batch_sum=%d DIFF=%+d\n",
                   b, ref_sum, batch_sum, batch_sum - ref_sum);
            if (b - first_diff > 5) break;  /* enough diffs shown */
        }
    }

    if (first_diff < 0) {
        printf("  PASSED (%d batches, sum matched every window)\n\n",
               n_batches);
        passed_tests++;
    } else {
        printf("  FAILED — first sum divergence at batch #%d\n\n",
               first_diff);
        failed_tests++;
    }
}

/* ----------------------------------------------------------------*/
static void run_realistic_audio_test()
{
    total_tests++;
    printf("=== REALISTIC AUDIO: mode=3 load=1000, "
           "Bresenham 33/34 pattern ===\n");

    /* Reference */
    I8253 ref;
    ref_setup(ref, 3, 1000);

    /* Batch */
    BatchTimer batch;
    batch_reset(batch);
    batch_set_mode(batch, 3, 3);
    batch_load(batch, 1000);

    int dead = count_dead_clocks(3);
    for (int i = 0; i < dead; i++) ref.Count(1);

    /* Bresenham: 1497600 / 44100 ≈ 33.959...
     * cps_whole = 33, cps_frac_num = 1497600 % 44100 = 42300
     * dt = 33 or 34 depending on fractional accumulator. */
    int cps_whole = 33;
    int cps_frac_num = 42300;  /* SOUND_CLOCK_RATE % 44100 */
    int cps_frac_acc = 0;
    int sampleRate = 44100;

    int first_diff = -1;
    int nsamples = 200;  /* ~4 ms of audio */
    int total_ref_sum = 0, total_batch_sum = 0;

    printf("  %-8s %-6s %-10s %-10s %-6s\n",
           "sample", "dt", "ref_sum", "batch_sum", "diff");
    for (int s = 0; s < nsamples; s++) {
        int dt = cps_whole;
        cps_frac_acc += cps_frac_num;
        if (cps_frac_acc >= sampleRate) {
            cps_frac_acc -= sampleRate;
            dt += 1;
        }

        int ref_sum = 0;
        for (int c = 0; c < dt; c++)
            ref_sum += ref_count0(ref);

        int batch_sum = integrate_timer(batch, dt);

        total_ref_sum += ref_sum;
        total_batch_sum += batch_sum;

        if (ref_sum != batch_sum && first_diff < 0) {
            first_diff = s;
        }
        if (s < 20 || (first_diff >= 0 && s <= first_diff + 5)) {
            printf("  %-8d %-6d %-10d %-10d %-+d%s\n",
                   s, dt, ref_sum, batch_sum, batch_sum - ref_sum,
                   (ref_sum != batch_sum) ? " <--" : "");
        }
    }

    if (first_diff < 0) {
        printf("  PASSED (%d samples, all matched)\n\n", nsamples);
        passed_tests++;
    } else {
        printf("  ...\n  FAILED — first divergence at sample %d\n",
               first_diff);
        printf("  Total ref_sum=%d batch_sum=%d (diff=%+d over %d samples)\n\n",
               total_ref_sum, total_batch_sum,
               total_batch_sum - total_ref_sum, nsamples);
        failed_tests++;
    }
}

/* ----------------------------------------------------------------*/
static void run_reconfigure_test()
{
    total_tests++;
    printf("=== RECONFIGURE: mode=3 load=100 → 20 clocks → "
           "load=50 → 40 clocks ===\n");

    /* Reference */
    I8253 ref;
    ref_setup(ref, 3, 100);

    /* Batch */
    BatchTimer batch;
    batch_reset(batch);
    batch_set_mode(batch, 3, 3);
    batch_load(batch, 100);

    int dead = count_dead_clocks(3);
    for (int i = 0; i < dead; i++) ref.Count(1);

    /* Phase 1: 20 clocks with load=100 */
    int first_diff = -1;
    for (int i = 0; i < 20; i++) {
        int r = ref_count0(ref);
        int b = integrate_timer(batch, 1);
        if (r != b && first_diff < 0) first_diff = i + 1;
    }

    /* Reconfigure: load=50 (no new SetMode, just write new value) */
    ref.write(0, (uint8_t)50);
    ref.write(0, (uint8_t)0);
    /* In the batch, apply_timer_write sets enabled=true, loadvalue=50 */
    batch_load(batch, 50);

    /* Note: the reference has another delay from write_value().
     * Consume it. */
    dead = 2;  /* write_value when already enabled: delay=0 for mode 3?
                * Actually: write_value sets delay=3 only when !enabled.
                * Since enabled is already true, delay stays 0. */
    /* No extra dead clocks needed since enabled is already true. */

    /* Phase 2: 40 clocks with load=50 */
    for (int i = 0; i < 40; i++) {
        int r = ref_count0(ref);
        int b = integrate_timer(batch, 1);
        if (r != b && first_diff < 0) first_diff = 20 + i + 1;
    }

    if (first_diff < 0) {
        printf("  PASSED\n\n");
        passed_tests++;
    } else {
        printf("  FAILED — first divergence at clock %d\n\n", first_diff);
        failed_tests++;
    }
}

/* ----------------------------------------------------------------*/
static void run_mode0_test()
{
    total_tests++;
    printf("=== MODE 0 DETAIL: load=5, batch=1 per clock ===\n");

    I8253 ref;
    ref_setup(ref, 0, 5);

    BatchTimer batch;
    batch_reset(batch);
    batch_set_mode(batch, 0, 3);
    batch_load(batch, 5);

    int dead = 3;
    for (int i = 0; i < dead; i++) ref.Count(1);

    int first_diff = -1;
    int n = 20;
    printf("  %-8s %-10s %-10s\n", "clock", "ref_OUT", "batch_OUT");
    for (int i = 0; i < n; i++) {
        int r = ref_count0(ref);
        int b = integrate_timer(batch, 1);
        bool d = (r != b);
        if (d && first_diff < 0) first_diff = i + 1;
        printf("  %-8d %-10d %-10d%s\n", i + 1, r, b, d ? " <-- DIFF" : "");
    }

    if (first_diff < 0) {
        printf("  PASSED\n\n");
        passed_tests++;
    } else {
        printf("  FAILED — first divergence at clock %d\n\n", first_diff);
        failed_tests++;
    }
}

/* ----------------------------------------------------------------*/
static void run_load0_test()
{
    total_tests++;
    printf("=== LOAD=0 (65536) SEMANTICS: mode=3, 200 clocks ===\n");

    I8253 ref;
    ref_setup(ref, 3, 0);

    BatchTimer batch;
    batch_reset(batch);
    batch_set_mode(batch, 3, 3);
    batch_load(batch, 0);

    int dead = count_dead_clocks(3);
    for (int i = 0; i < dead; i++) ref.Count(1);

    int first_diff = -1;
    int n = 200;
    for (int i = 0; i < n; i++) {
        int r = ref_count0(ref);
        int b = integrate_timer(batch, 1);
        if (r != b && first_diff < 0) first_diff = i + 1;
    }

    if (first_diff < 0) {
        printf("  PASSED (%d clocks, load=0 treated as 65536)\n\n", n);
        passed_tests++;
    } else {
        printf("  FAILED — first divergence at clock %d\n\n", first_diff);
        failed_tests++;
    }
}

/* ================================================================== */
int main()
{
    printf("8253 DIFFERENTIAL TEST\n");
    printf("======================\n");
    printf("Reference: CounterUnit::Count(1) from src/8253.h\n");
    printf("Batch:     integrate_timer() from psp/src/sound.cpp\n");
    printf("Delay:     reference has SetMode+write delays that batch skips\n");
    printf("           (dead clocks consumed before comparison starts)\n\n");

    /* --- Section 5: Mode 0 --- */
    printf("--- MODE 0 (Interrupt on terminal count) ---\n");
    int loads0[] = {1,2,3,4,5,7,10,100,255,256,1000};
    for (int L : loads0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "M0 load=%d", L);
        run_scenario(buf, 0, L, L + 20);
    }

    /* --- Section 5: Mode 2 --- */
    printf("--- MODE 2 (Rate generator) ---\n");
    printf("NOTE: batch always returns dt (inaudible simplification)\n");
    int loads2[] = {1,2,3,5,10,100};
    for (int L : loads2) {
        char buf[128];
        snprintf(buf, sizeof(buf), "M2 load=%d", L);
        run_scenario(buf, 2, L, L * 3);
    }

    /* --- Section 5: Mode 3 (priority) --- */
    printf("--- MODE 3 (Square wave) — PRIORITY ---\n");
    int loads3[] = {1,2,3,4,5,7,9,10,100};
    for (int L : loads3) {
        char buf[128];
        snprintf(buf, sizeof(buf), "M3 load=%d", L);
        run_scenario(buf, 3, L, L * 6);
    }

    /* --- Section 6: load=0 --- */
    printf("--- LOAD=0 SEMANTICS ---\n");
    run_load0_test();

    /* --- Section 9: Different batch sizes --- */
    printf("--- BATCH SIZE VARIATIONS (mode=3 load=5) ---\n");
    int batches[] = {1,2,3,4,5,10,16,32,33,34,64,100};
    for (int B : batches) {
        char buf[128];
        snprintf(buf, sizeof(buf), "M3 load=5 batch=%d", B);
        run_batch_sum_test(buf, 3, 5, B, 20);
    }

    /* --- Section 12: Realistic audio --- */
    printf("--- REALISTIC AUDIO GENERATION ---\n");
    run_realistic_audio_test();

    /* --- Section 8: Reconfigure during run --- */
    printf("--- RECONFIGURE DURING RUN ---\n");
    run_reconfigure_test();

    /* --- Mode 0 detail --- */
    printf("--- MODE 0 DETAIL ---\n");
    run_mode0_test();

    /* --- Summary --- */
    printf("==============================\n");
    printf("SUMMARY: %d tests, %d PASSED, %d FAILED\n",
           total_tests, passed_tests, failed_tests);
    if (failed_tests == 0)
        printf("8253 batch implementation эквивалентна оригиналу\n");
    else
        printf("8253 batch implementation НЕ эквивалентна оригиналу\n");

    return failed_tests ? 1 : 0;
}
