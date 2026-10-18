#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lab::dsp
{
    // The suite's host clock: the Zone's clock is FL's bar position. Filled by the EffectProcessor base for every
    // control block; chains read it, never the playhead.
    struct Transport
    {
        double bpm = 120.0;
        bool playing = false;          // host transport running and reporting a position
        double ppq = 0.0;              // quarter notes at the start of the control block (0 when stopped)
        double ppqPerSample = 0.0;     // quarter notes per sample at the current tempo
        int numerator = 4, denominator = 4;
        double sampleRate = 48000.0;

        double quartersPerBar() const noexcept { return (double) numerator * 4.0 / (double) std::max (1, denominator); }

        // Bar index of a quarter position, and the bar the control block starts in.
        int64_t barAt (double quarters) const noexcept { return (int64_t) std::floor (quarters / quartersPerBar() + 1.0e-9); }
        int64_t bar() const noexcept { return barAt (ppq); }

        // Samples per quarter note, and per cycle of a synced rate given in quarter notes.
        double samplesPerQuarter() const noexcept { return sampleRate * 60.0 / std::max (1.0, bpm); }
        double samplesFor (double quarters) const noexcept { return quarters * samplesPerQuarter(); }
    };
}
