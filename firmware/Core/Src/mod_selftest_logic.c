/* Size over speed: nothing here runs in the RF or audio path. */
#pragma GCC optimize ("Os")

#include "mod_selftest_logic.h"
#include "card_text.h"
#include <stddef.h>

#define ST_NAME(id, group, name, unit, low, high) name,
static const char *const selftest_names[SELFTEST_COUNT] = { SELFTEST_TABLE(ST_NAME) };
#undef ST_NAME

#define ST_UNIT(id, group, name, unit, low, high) unit,
static const char *const selftest_units[SELFTEST_COUNT] = { SELFTEST_TABLE(ST_UNIT) };
#undef ST_UNIT

#define ST_GRP(id, group, name, unit, low, high) (uint8_t)(group),
static const uint8_t selftest_groups[SELFTEST_COUNT] = { SELFTEST_TABLE(ST_GRP) };
#undef ST_GRP

/* Packed as one array of pairs: two relocation-free words per test in flash. */
#define ST_LIM(id, group, name, unit, low, high) {(low), (high)},
static const int32_t selftest_limits[SELFTEST_COUNT][2] = { SELFTEST_TABLE(ST_LIM) };
#undef ST_LIM

void MOD_SelfTestLimits(uint8_t id, int32_t *low, int32_t *high)
{
    const int32_t lo = id < SELFTEST_COUNT ? selftest_limits[id][0] : 0;
    const int32_t hi = id < SELFTEST_COUNT ? selftest_limits[id][1] : 0;
    if (low) *low = lo;
    if (high) *high = hi;
}

const char *MOD_SelfTestName(uint8_t id)
{
    return id < SELFTEST_COUNT ? selftest_names[id] : "?";
}

const char *MOD_SelfTestUnit(uint8_t id)
{
    return id < SELFTEST_COUNT ? selftest_units[id] : "";
}

uint8_t MOD_SelfTestGroup(uint8_t id)
{
    return id < SELFTEST_COUNT ? selftest_groups[id] : 0U;
}

const char *MOD_SelfTestGroupName(uint8_t group)
{
    switch (group) {
        case ST_GROUP_POWER:    return "POWER";
        case ST_GROUP_CLOCK:    return "CLOCK";
        case ST_GROUP_CONTROLS: return "CONTROLS";
        case ST_GROUP_STORAGE:  return "STORAGE";
        case ST_GROUP_FILTERS:  return "FILTERS";
        case ST_GROUP_RX:       return "RX";
        case ST_GROUP_ANALOG:   return "ANALOG";
        default:                return "?";
    }
}

const char *MOD_SelfTestVerdictName(uint8_t verdict)
{
    switch (verdict) {
        case ST_PENDING: return "----";
        case ST_RUNNING: return "RUN ";
        case ST_PASS:    return "PASS";
        case ST_WARN:    return "WARN";
        case ST_FAIL:    return "FAIL";
        case ST_SKIP:    return "SKIP";
        default:         return "?";
    }
}

/* ASCII case-insensitive compare; the console never sees a locale. */
static uint8_t name_is(const char *a, const char *b)
{
    for (;; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0U;
        if (!x) return 1U;
    }
}

uint8_t MOD_SelfTestGroupFromName(const char *name)
{
    if (!name) return 0U;
    if (name_is(name, "power"))    return ST_GROUP_POWER;
    if (name_is(name, "clock"))    return ST_GROUP_CLOCK;
    if (name_is(name, "controls")) return ST_GROUP_CONTROLS;
    if (name_is(name, "storage"))  return ST_GROUP_STORAGE;
    if (name_is(name, "filters"))  return ST_GROUP_FILTERS;
    if (name_is(name, "rx"))       return ST_GROUP_RX;
    if (name_is(name, "analog"))   return ST_GROUP_ANALOG;
    if (name_is(name, "all"))      return ST_GROUP_ALL;
    if (name_is(name, "default"))  return ST_GROUP_DEFAULT;
    return 0U;
}

uint8_t MOD_SelfTestRange(int32_t value, int32_t low, int32_t high)
{
    if (low > high) return ST_FAIL; /* an inverted window must never pass */
    return (value >= low && value <= high) ? (uint8_t)ST_PASS : (uint8_t)ST_FAIL;
}

uint8_t MOD_SelfTestRangeWarn(int32_t value, int32_t low, int32_t high)
{
    if (low > high) return ST_FAIL;
    return (value >= low && value <= high) ? (uint8_t)ST_PASS : (uint8_t)ST_WARN;
}

selftest_lpf_state_t MOD_SelfTestLpfState(int32_t bias_mv)
{
    if (bias_mv < ST_LPF_SHORT_MV)    return ST_LPF_SHORTED;
    if (bias_mv <= ST_LPF_CONDUCT_MV) return ST_LPF_CONDUCTING;
    if (bias_mv >= ST_LPF_OPEN_MV)    return ST_LPF_OPEN;
    return ST_LPF_MARGINAL;
}

uint8_t MOD_SelfTestLpfVerdict(int32_t bias_mv, uint8_t expect_conducting, uint8_t fitted)
{
    if (!fitted) return ST_SKIP;
    const selftest_lpf_state_t state = MOD_SelfTestLpfState(bias_mv);
    if (state == ST_LPF_SHORTED) return ST_FAIL; /* a short is a fault either way */
    if (expect_conducting) return state == ST_LPF_CONDUCTING ? (uint8_t)ST_PASS
                                : (state == ST_LPF_MARGINAL ? (uint8_t)ST_WARN : (uint8_t)ST_FAIL);
    return state == ST_LPF_OPEN ? (uint8_t)ST_PASS
                                : (state == ST_LPF_MARGINAL ? (uint8_t)ST_WARN : (uint8_t)ST_FAIL);
}

uint16_t MOD_SelfTestLpfDetail(int32_t bias_mv, uint8_t expect_conducting, uint8_t fitted)
{
    if (!fitted) return TXT_ST_LPF_NOT_FITTED;
    switch (MOD_SelfTestLpfState(bias_mv)) {
        case ST_LPF_SHORTED:
            return TXT_ST_LPF_SHORTED;
        case ST_LPF_CONDUCTING:
            return expect_conducting ? TXT_ST_LPF_CONDUCTS : TXT_ST_LPF_STUCK_ON;
        case ST_LPF_MARGINAL:
            return expect_conducting ? TXT_ST_LPF_PARTIAL : TXT_ST_LPF_SLOW_RISE;
        default:
            return expect_conducting ? TXT_ST_LPF_OPEN : TXT_ST_LPF_OPEN_EXPECTED;
    }
}

uint8_t MOD_SelfTestOneHot(uint32_t bits, uint8_t expected_slot)
{
    bits &= 31UL;
    if (expected_slot >= 5U) return bits == 0UL ? (uint8_t)ST_PASS : (uint8_t)ST_FAIL;
    return bits == (1UL << expected_slot) ? (uint8_t)ST_PASS : (uint8_t)ST_FAIL;
}

void MOD_SelfTestFit(const uint16_t *dac_code, const uint16_t *measured_mv,
                     unsigned count, selftest_fit_t *out)
{
    if (!out) return;
    out->counts_per_volt = 0.0f;
    out->r2_ppt = 0;
    out->intercept_mv = 0;
    out->valid = 0U;
    if (!dac_code || !measured_mv || count < 3U) return;
    float n = (float)count, sx = 0.0f, sy = 0.0f, sxx = 0.0f, sxy = 0.0f;
    for (unsigned i = 0U; i < count; ++i) {
        const float x = (float)dac_code[i], y = (float)measured_mv[i];
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    const float denominator = n * sxx - sx * sx;
    if (denominator <= 0.0f) return; /* every point at the same code */
    const float slope = (n * sxy - sx * sy) / denominator; /* mV per count */
    const float intercept = (sy - slope * sx) / n;
    if (!(slope > 0.0f)) return; /* a falling or flat transfer is a wiring fault */
    float ss_total = 0.0f, ss_residual = 0.0f;
    const float mean = sy / n;
    for (unsigned i = 0U; i < count; ++i) {
        const float x = (float)dac_code[i], y = (float)measured_mv[i];
        const float d = y - (slope * x + intercept), m = y - mean;
        ss_residual += d * d;
        ss_total += m * m;
    }
    float r2 = ss_total > 0.0f ? 1.0f - ss_residual / ss_total : 0.0f;
    if (r2 < 0.0f) r2 = 0.0f;
    if (r2 > 1.0f) r2 = 1.0f;
    out->counts_per_volt = 1000.0f / slope;
    out->r2_ppt = (int32_t)(r2 * 1000.0f + 0.5f);
    out->intercept_mv = (int32_t)(intercept >= 0.0f ? intercept + 0.5f : intercept - 0.5f);
    out->valid = 1U;
}

uint8_t MOD_SelfTestRollup(const selftest_result_t *results, unsigned count)
{
    if (!results || !count) return ST_SKIP;
    uint8_t seen_fail = 0U, seen_warn = 0U, seen_pass = 0U;
    for (unsigned i = 0U; i < count; ++i) {
        switch (results[i].verdict) {
            case ST_FAIL: seen_fail = 1U; break;
            case ST_WARN: seen_warn = 1U; break;
            case ST_PASS: seen_pass = 1U; break;
            default: break; /* PENDING, RUNNING and SKIP never stand in for a result */
        }
    }
    if (seen_fail) return ST_FAIL;
    if (seen_warn) return ST_WARN;
    return seen_pass ? (uint8_t)ST_PASS : (uint8_t)ST_SKIP;
}

void MOD_SelfTestTally(const selftest_result_t *results, unsigned count,
                       unsigned *pass, unsigned *warn, unsigned *fail, unsigned *skip)
{
    unsigned p = 0U, w = 0U, f = 0U, s = 0U;
    for (unsigned i = 0U; results && i < count; ++i) {
        switch (results[i].verdict) {
            case ST_PASS: ++p; break;
            case ST_WARN: ++w; break;
            case ST_FAIL: ++f; break;
            case ST_SKIP: ++s; break;
            default: break;
        }
    }
    if (pass) *pass = p;
    if (warn) *warn = w;
    if (fail) *fail = f;
    if (skip) *skip = s;
}
