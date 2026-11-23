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

#include "app.h"
#include "options.h"

#include <objbase.h>
#include <io.h>

#include <cstdio>
#include <stdexcept>

namespace
{
class BenchmarkOutputRedirect
{
public:
    explicit BenchmarkOutputRedirect(bool enabled)
    {
        if (enabled)
        {
            fflush(stdout);
            m_savedStdout = _dup(_fileno(stdout));
            if (m_savedStdout == -1 || _dup2(_fileno(stderr), _fileno(stdout)) == -1)
            {
                if (m_savedStdout != -1)
                {
                    _close(m_savedStdout);
                }
                throw std::runtime_error("Could not redirect benchmark diagnostics to stderr.");
            }
        }
    }

    ~BenchmarkOutputRedirect()
    {
        if (m_savedStdout != -1)
        {
            fflush(stdout);
            _dup2(m_savedStdout, _fileno(stdout));
            _close(m_savedStdout);
        }
    }

private:
    int m_savedStdout = -1;
};
} // namespace

int main(int argc, char** argv)
{
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
    {
        fprintf(stderr, "CoInitializeEx failed.\n");
        return 1;
    }

    int exitCode = 0;
    try
    {
        Options options = Options::Parse(argc, argv);
        if (options.helpRequested)
        {
            CoUninitialize();
            return 0;
        }
        const bool benchmark = options.benchmarkFrames != 0;
        std::string benchmarkJson;
        {
            BenchmarkOutputRedirect redirect(benchmark);
            App app(std::move(options));
            exitCode = app.Run();
            if (benchmark && exitCode == 0)
            {
                benchmarkJson = app.BenchmarkJson();
            }
        }
        if (benchmark && exitCode == 0)
        {
            printf("%s\n", benchmarkJson.c_str());
        }
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "error: %s\n", error.what());
        exitCode = 1;
    }

    CoUninitialize();
    return exitCode;
}
