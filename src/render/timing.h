//
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#pragma once

#include <algorithm>
#include <cstdint>

// Fixed-size circular buffer tracking the running sum, so Average() is O(1).
//
// The backend accumulates GPU timestamp results here, and the GUI plots the raw window, so the
// buffer is deliberately kept as a flat array indexed by `next` - that is the layout ImGui's
// PlotLines wants, and rotating it would cost more than it saves.
template<uint32_t Size>
struct RollingAverage
{
    static constexpr uint32_t kSize = Size;
    float samples[kSize] = {};
    float sum = 0.0f;
    uint32_t count = 0;
    uint32_t next = 0;

    void Add(float value)
    {
        sum += value - samples[next];
        samples[next] = value;
        next = (next + 1) % kSize;
        count = std::min(count + 1, kSize);
    }

    // The most recently added sample, i.e. the one just behind the write cursor.
    float Latest() const { return count > 0 ? samples[(next + kSize - 1) % kSize] : 0.0f; }

    float Average() const { return count > 0 ? sum / static_cast<float>(count) : 0.0f; }

    float Median() const
    {
        if (count == 0)
        {
            return 0.0f;
        }
        float sorted[kSize];
        std::copy(samples, samples + count, sorted);
        std::sort(sorted, sorted + count);
        const uint32_t mid = count / 2;
        return (count % 2 == 0) ? 0.5f * (sorted[mid - 1] + sorted[mid]) : sorted[mid];
    }
};

// One timing series as reported to the application: the window itself plus the statistics the
// GUI displays beside the plot.
struct TimingSnapshot
{
    static constexpr uint32_t kHistorySize = 64;

    float latest = 0.0f;
    float average = 0.0f;
    float median = 0.0f;
    RollingAverage<kHistorySize> history;

    void AddSample(float milliseconds)
    {
        history.Add(milliseconds);
        latest = history.Latest();
        average = history.Average();
        median = history.Median();
    }

    // Laid out for ImGui::PlotLines, which takes a base pointer and a starting offset.
    const float* Samples() const { return history.samples; }
    uint32_t Offset() const { return history.next; }
};

// Backends accumulate into a RollingAverage of their own and publish it through this.
template<uint32_t Size>
TimingSnapshot MakeTimingSnapshot(const RollingAverage<Size>& source)
{
    static_assert(Size == TimingSnapshot::kHistorySize,
                  "Backend timing history must match the window the application reports.");
    TimingSnapshot snapshot;
    snapshot.history = source;
    snapshot.latest = source.Latest();
    snapshot.average = source.Average();
    snapshot.median = source.Median();
    return snapshot;
}
