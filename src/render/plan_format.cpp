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

#include "render/plan_format.h"

#include "core/error.h"

#include <cstdio>

namespace
{
// Every description here is bounded: a short literal and a single number.
constexpr size_t kDescriptionSize = 128;

template<typename... Args>
std::string Format(const char* format, Args... arguments)
{
    char buffer[kDescriptionSize];
    if (std::snprintf(buffer, sizeof(buffer), format, arguments...) < 0)
    {
        Fatal("Failed to format a plan description.");
    }
    return buffer;
}
} // namespace

std::string FormatBytes(uint64_t bytes)
{
    constexpr const char* units[] = {"B", "KiB", "MiB", "GiB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0]))
    {
        value /= 1024.0;
        ++unit;
    }

    if (unit == 0)
    {
        return Format("%llu %s", static_cast<unsigned long long>(bytes), units[unit]);
    }
    return Format("%.2f %s", value, units[unit]);
}
