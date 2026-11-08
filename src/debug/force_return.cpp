#include "debug/force_return.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "return_hook.h"
#include "scene_timing.h"

namespace rg::debug
{
namespace
{

enum class Op
{
    Set,
    AtLeast,
    AtMost,
};

struct Rule
{
    uint32_t returnTo = 0u; // 0: every call
    Op op = Op::Set;
    int32_t value = 0;
    unsigned uses = 0u;
};

struct Forced
{
    uint32_t address = 0u;
    PS2Runtime::RecompiledFunction original = nullptr;
    std::vector<Rule> rules;
};

// The functions one run can force, each with a wrapper of its own.
constexpr size_t kSlots = 16u;
// The first uses of each rule that are logged.
constexpr unsigned kLoggedUses = 4u;

// Filled before the game runs; after, only the game's thread, which runs all
// guest code, touches them (the use counts).
std::array<Forced, kSlots> g_forced;
size_t g_used = 0u;

// The rule for a call returning to returnTo: its own, else the one for every
// call, else none.
Rule *ruleFor(Forced &forced, uint32_t returnTo)
{
    Rule *any = nullptr;
    for (Rule &rule : forced.rules)
    {
        if (rule.returnTo == returnTo)
        {
            return &rule;
        }
        if (rule.returnTo == 0u && !any)
        {
            any = &rule;
        }
    }
    return any;
}

// A value as typed: decimal, and in hex too when large.
std::string shown(int32_t value)
{
    char text[40];
    if (value >= -0xFFFF && value <= 0xFFFF)
    {
        std::snprintf(text, sizeof(text), "%d", value);
    }
    else
    {
        std::snprintf(text, sizeof(text), "%d (0x%08X)", value, static_cast<uint32_t>(value));
    }
    return text;
}

// Once the forced function has returned to its caller: pc is the return
// address, v0 the function's answer.
void afterForced(uint8_t *, R5900Context *ctx, PS2Runtime *, uint32_t slot)
{
    const uint32_t returnTo = ctx->pc;
    if (returnTo == 0u)
    {
        return; // its thread ended: no caller reads the answer
    }
    Forced &forced = g_forced[slot];
    Rule *rule = ruleFor(forced, returnTo);
    if (!rule)
    {
        return;
    }
    const int32_t was = static_cast<int32_t>(getRegU32(ctx, 2));
    int32_t now = rule->value;
    if (rule->op == Op::AtLeast)
    {
        now = std::max(was, rule->value);
    }
    else if (rule->op == Op::AtMost)
    {
        now = std::min(was, rule->value);
    }
    setReturnS32(ctx, now);
    if (rule->uses++ < kLoggedUses)
    {
        std::printf("[force-return] 0x%08X returning to 0x%08X: %s becomes %s\n", forced.address, returnTo,
                    shown(was).c_str(), shown(now).c_str());
    }
}

void callForced(size_t slot, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    Forced &forced = g_forced[slot];
    const uint32_t returnTo = getRegU32(ctx, 31);
    if (runtime->hasFunction(returnTo))
    {
        // A call unwound at a safe point gets its answer when its caller
        // resumes at returnTo (return_hook.h).
        rg::hooks::callThen(forced.original, rdram, ctx, runtime, &afterForced, static_cast<uint32_t>(slot));
        return;
    }
    // Nowhere to wait for an unwound call (callThen would then run `after`
    // where the function stopped, v0 still its own): only a call that returns
    // at once is forced.
    forced.original(rdram, ctx, runtime);
    if (ctx->pc == returnTo)
    {
        afterForced(rdram, ctx, runtime, static_cast<uint32_t>(slot));
    }
}

template <size_t Slot>
void forcedReturn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    callForced(Slot, rdram, ctx, runtime);
}

template <size_t... Slots>
constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(Slots)> makeWrappers(std::index_sequence<Slots...>)
{
    return {&forcedReturn<Slots>...};
}

constexpr auto kWrappers = makeWrappers(std::make_index_sequence<kSlots>{});

// A number in hex (0x...) or decimal, never octal, of at most 32 bits, with
// a '-' only when `sign`; p moves past it.
bool parseNumber(const char *&p, bool sign, long long &out)
{
    const char *q = p;
    const bool negative = sign && *q == '-';
    if (negative)
    {
        ++q;
    }
    int base = 10;
    if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X'))
    {
        base = 16;
        q += 2;
    }
    const unsigned char first = static_cast<unsigned char>(*q);
    if (base == 16 ? !std::isxdigit(first) : !std::isdigit(first))
    {
        return false;
    }
    if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X'))
    {
        return false; // strtoull would take a second prefix
    }
    char *end = nullptr;
    errno = 0;
    const unsigned long long magnitude = std::strtoull(q, &end, base);
    if (end == q || errno == ERANGE || magnitude > UINT32_MAX)
    {
        return false;
    }
    out = negative ? -static_cast<long long>(magnitude) : static_cast<long long>(magnitude);
    p = end;
    return true;
}

// A code address: not 0, a whole instruction.
bool parseAddress(const char *&p, uint32_t &address)
{
    long long value = 0;
    if (!parseNumber(p, false, value) || value == 0 || (value & 3) != 0)
    {
        return false;
    }
    address = static_cast<uint32_t>(value);
    return true;
}

// ADDR[@RA](=|>=|<=)VALUE. VALUE is signed for >= and <=, which compare as
// signed; = also takes a 32-bit pattern up to 0xFFFFFFFF.
bool parseRule(const std::string &text, uint32_t &address, Rule &rule)
{
    const char *p = text.c_str();
    if (!parseAddress(p, address))
    {
        return false;
    }
    if (*p == '@')
    {
        ++p;
        if (!parseAddress(p, rule.returnTo))
        {
            return false;
        }
    }
    if (p[0] == '>' && p[1] == '=')
    {
        rule.op = Op::AtLeast;
        p += 2;
    }
    else if (p[0] == '<' && p[1] == '=')
    {
        rule.op = Op::AtMost;
        p += 2;
    }
    else if (p[0] == '=')
    {
        rule.op = Op::Set;
        p += 1;
    }
    else
    {
        return false;
    }
    long long value = 0;
    const long long highest = rule.op == Op::Set ? static_cast<long long>(UINT32_MAX) : INT32_MAX;
    if (!parseNumber(p, true, value) || *p != '\0' || value < INT32_MIN || value > highest)
    {
        return false;
    }
    rule.value = static_cast<int32_t>(static_cast<uint32_t>(value));
    return true;
}

// Why returnTo cannot be where a call in the main ELF returns to, or null
// when it can: the generated code resumes there, and the instruction before
// the delay slot calls through ra (jal, jalr to ra, or a branch and link).
const char *notAReturn(PS2Runtime &runtime, uint32_t returnTo)
{
    if (!runtime.hasFunction(returnTo))
    {
        return "is not a place the main ELF's code resumes at (a caller in an overlay cannot be checked before it "
               "loads)";
    }
    const uint32_t word = runtime.memory().read32(returnTo - 8u);
    const uint32_t op = word >> 26;
    const uint32_t rt = (word >> 16) & 0x1Fu;
    const uint32_t rd = (word >> 11) & 0x1Fu;
    const bool call = op == 3u || (op == 0u && (word & 0x3Fu) == 9u && rd == 31u) ||
                      (op == 1u && rt >= 0x10u && rt <= 0x13u);
    return call ? nullptr : "is not right after a call";
}

const char *opText(Op op)
{
    switch (op)
    {
    case Op::AtLeast:
        return "at least ";
    case Op::AtMost:
        return "at most ";
    case Op::Set:
        break;
    }
    return "";
}

Forced *findSlot(uint32_t address)
{
    for (size_t i = 0; i < g_used; ++i)
    {
        if (g_forced[i].address == address)
        {
            return &g_forced[i];
        }
    }
    return nullptr;
}

// A new slot forcing address, hooked; null when there is none left or the
// address is not a function.
Forced *hookSlot(PS2Runtime &runtime, uint32_t address)
{
    if (g_used == kSlots)
    {
        std::fprintf(stderr, "[force-return] at most %zu functions can be forced in one run\n", kSlots);
        return nullptr;
    }
    Forced &forced = g_forced[g_used];
    if (!rg::timing::hookFunction(runtime, address, kWrappers[g_used], forced.original))
    {
        return nullptr;
    }
    forced.address = address;
    ++g_used;
    return &forced;
}

} // namespace

void installForceReturn(PS2Runtime &runtime)
{
    const char *spec = std::getenv("RG_FORCE_RETURN");
    if (!spec || !*spec)
    {
        return;
    }
    std::string all(spec);
    all.erase(std::remove_if(all.begin(), all.end(), [](unsigned char c) { return std::isspace(c) != 0; }),
              all.end());
    for (size_t start = 0; start <= all.size();)
    {
        const size_t comma = std::min(all.find(',', start), all.size());
        const std::string text = all.substr(start, comma - start);
        start = comma + 1;
        if (text.empty())
        {
            continue;
        }
        uint32_t address = 0u;
        Rule rule;
        if (!parseRule(text, address, rule))
        {
            std::fprintf(stderr,
                         "[force-return] \"%s\" is not ADDR[@RA]=VALUE, >=VALUE or <=VALUE (addresses a multiple of "
                         "4, 0x for hex; a signed 32-bit VALUE for >= and <=): left out\n",
                         text.c_str());
            continue;
        }
        Forced *forced = findSlot(address);
        if (forced)
        {
            const Rule *before = ruleFor(*forced, rule.returnTo);
            if (before && before->returnTo == rule.returnTo)
            {
                std::fprintf(stderr,
                             "[force-return] \"%s\": that function and caller already have a rule, left out\n",
                             text.c_str());
                continue;
            }
        }
        else if (!(forced = hookSlot(runtime, address)))
        {
            std::fprintf(stderr, "[force-return] \"%s\" left out\n", text.c_str());
            continue;
        }
        if (rule.returnTo != 0u)
        {
            std::printf("[force-return] 0x%08X returns %s%s to the calls returning to 0x%08X\n", address,
                        opText(rule.op), shown(rule.value).c_str(), rule.returnTo);
            if (const char *why = notAReturn(runtime, rule.returnTo))
            {
                std::fprintf(stderr,
                             "[force-return] 0x%08X %s: RA is the address after the call's delay slot, the jal's "
                             "own + 8\n",
                             rule.returnTo, why);
            }
        }
        else
        {
            std::printf("[force-return] 0x%08X returns %s%s\n", address, opText(rule.op), shown(rule.value).c_str());
        }
        forced->rules.push_back(rule);
    }
}

} // namespace rg::debug
