#pragma once

#include <cmath>
#include <cstdio>
#include <string>

namespace lab::dsp
{
    // The report of a DSP header's selfTest(): one "selftest_<case>=<value>" pair per case plus
    // "selftest_<case>_ok=<0|1>", in the key=value form test/run_tests.py parses from the harness's --selftest (T0).
    // Off the audio thread only (it builds strings).
    struct SelfTestReport
    {
        std::string text;
        int cases = 0, failures = 0;

        bool add (const char* name, double value, bool ok)
        {
            char buffer[160];
            std::snprintf (buffer, sizeof (buffer), "selftest_%s=%.6g selftest_%s_ok=%d\n", name, std::isfinite (value) ? value : -1.0e30, name, ok ? 1 : 0);
            text += buffer;
            ++cases;
            if (! ok)
                ++failures;
            return ok;
        }

        void merge (const SelfTestReport& other)
        {
            text += other.text;
            cases += other.cases;
            failures += other.failures;
        }
    };
}
