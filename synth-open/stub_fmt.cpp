// Minimal replacement for fmt::v9::vformat (normally from fmt/format.cc, 37 KB on M4 at -Os).
// Supports exactly what the synth GUI and SynthVoiceSDK formatters use:
//   "{}" "{:d}" "{:.Nf}" with an optional '+' sign flag, "{{" "}}" escapes, integers,
//   floats, strings. Anything else is emitted verbatim and ignored.
// Floats are formatted with integer arithmetic: newlib-nano printf has no float support here.
// Written from the public fmt API (MIT licence, util/3rdparty/fmt), not from the closed libs.
#include <fmt/format.h>

#include <cmath>
#include <cstdint>
#include <string>

namespace
{
struct Spec
{
    bool plus = false;
    int precision = -1;
    bool hasType = false;
};

void appendUnsigned(std::string& out, unsigned long long v)
{
    char tmp[24];
    int n = 0;
    do
    {
        tmp[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v != 0);
    while (n > 0)
    {
        out.push_back(tmp[--n]);
    }
}

void appendSigned(std::string& out, long long v, const Spec& spec)
{
    if (v < 0)
    {
        out.push_back('-');
        appendUnsigned(out, 0ull - static_cast<unsigned long long>(v));
    }
    else
    {
        if (spec.plus)
        {
            out.push_back('+');
        }
        appendUnsigned(out, static_cast<unsigned long long>(v));
    }
}

void appendFloat(std::string& out, double v, const Spec& spec)
{
    int precision = spec.precision >= 0 ? spec.precision : 6;
    if (precision > 9)
    {
        precision = 9;
    }
    if (std::isnan(v))
    {
        out += "nan";
        return;
    }
    bool neg = std::signbit(v);
    if (neg)
    {
        v = -v;
    }
    if (std::isinf(v))
    {
        out += neg ? "-inf" : "inf";
        return;
    }
    unsigned long long scale = 1;
    for (int i = 0; i < precision; i++)
    {
        scale *= 10;
    }
    double scaled = v * static_cast<double>(scale) + 0.5;
    if (scaled > 1.8e19)
    {
        scaled = 1.8e19;
    }
    auto fixedValue = static_cast<unsigned long long>(scaled);
    if (neg && fixedValue != 0)
    {
        out.push_back('-');
    }
    else if (spec.plus)
    {
        out.push_back('+');
    }
    appendUnsigned(out, fixedValue / scale);
    if (precision > 0)
    {
        out.push_back('.');
        std::string frac;
        appendUnsigned(frac, fixedValue % scale);
        out.append(static_cast<size_t>(precision) - frac.size(), '0');
        out += frac;
    }
}

struct Visitor
{
    std::string& out;
    const Spec& spec;

    void operator()(fmt::monostate) const {}
    void operator()(bool v) const { out += v ? "true" : "false"; }
    void operator()(char v) const { out.push_back(v); }
    void operator()(int v) const { appendSigned(out, v, spec); }
    void operator()(unsigned v) const { appendUnsigned(out, v); }
    void operator()(long long v) const { appendSigned(out, v, spec); }
    void operator()(unsigned long long v) const { appendUnsigned(out, v); }
#ifdef __SIZEOF_INT128__ // host test build only (x86_64 fmt has 128 bit args), the Cortex-M4 has none
    void operator()(__int128 v) const { appendSigned(out, static_cast<long long>(v), spec); }
    void operator()(unsigned __int128 v) const { appendUnsigned(out, static_cast<unsigned long long>(v)); }
#endif
    void operator()(float v) const { appendFloat(out, v, spec); }
    void operator()(double v) const { appendFloat(out, v, spec); }
    void operator()(long double v) const { appendFloat(out, static_cast<double>(v), spec); }
    void operator()(const char* v) const
    {
        if (v)
        {
            out += v;
        }
    }
    void operator()(fmt::string_view v) const { out.append(v.data(), v.size()); }
    void operator()(const void*) const {}
    void operator()(fmt::basic_format_arg<fmt::format_context>::handle) const {}
};
} // namespace

namespace fmt
{
inline namespace v9
{
std::string vformat(string_view fmt, format_args args)
{
    std::string out;
    size_t nextArg = 0;
    const char* p = fmt.data();
    const char* end = p + fmt.size();

    while (p < end)
    {
        char c = *p++;
        if (c != '{' && c != '}')
        {
            out.push_back(c);
            continue;
        }
        if (p < end && *p == c)
        {
            out.push_back(c);
            p++;
            continue;
        }
        if (c == '}')
        {
            continue;
        }

        size_t index = nextArg;
        bool explicitIndex = false;
        size_t parsed = 0;
        while (p < end && *p >= '0' && *p <= '9')
        {
            parsed = parsed * 10 + static_cast<size_t>(*p++ - '0');
            explicitIndex = true;
        }
        if (explicitIndex)
        {
            index = parsed;
        }
        else
        {
            nextArg++;
        }

        Spec spec;
        if (p < end && *p == ':')
        {
            p++;
            if (p < end && *p == '+')
            {
                spec.plus = true;
                p++;
            }
            if (p < end && *p == '.')
            {
                p++;
                spec.precision = 0;
                while (p < end && *p >= '0' && *p <= '9')
                {
                    spec.precision = spec.precision * 10 + (*p++ - '0');
                }
            }
            while (p < end && *p != '}')
            {
                spec.hasType = true;
                p++;
            }
        }
        if (p < end && *p == '}')
        {
            p++;
        }

        auto arg = args.get(static_cast<int>(index));
        if (arg)
        {
            visit_format_arg(Visitor{ out, spec }, arg);
        }
    }
    return out;
}
} // namespace v9
} // namespace fmt
