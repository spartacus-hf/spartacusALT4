#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
#include "radio_pd_policy.h"
#include <string.h>

uint8_t RADIO_PdDecode(uint32_t p, radio_pd_offer_t *o)
{
    if (!o) return 0U;
    memset(o, 0, sizeof(*o));
    unsigned type = p >> 30U;
    o->type = (radio_pd_type_t)type;
    if (type == 3U) {
        if ((p >> 28U) & 3U) { o->type = RADIO_PD_UNSUPPORTED; return 0U; }
        o->min_mv = (uint16_t)(((p >> 8U) & 255U) * 100U);
        o->max_mv = (uint16_t)(((p >> 17U) & 255U) * 100U);
        o->max_ma = (uint16_t)((p & 127U) * 50U);
        o->power_limited = (uint8_t)((p >> 27U) & 1U);
        if (o->min_mv < 3300U || o->max_mv > 21000U || (p & 0x06010080UL)) return 0U;
    } else {
        o->min_mv = (uint16_t)(((p >> 10U) & 1023U) * 50U);
        o->max_mv = type ? (uint16_t)(((p >> 20U) & 1023U) * 50U) : o->min_mv;
        if (type == 1U) o->max_mw = (p & 1023U) * 250U;
        else o->max_ma = (uint16_t)((p & 1023U) * 10U);
        if (o->min_mv < 5000U || o->max_mv > 20000U) return 0U;
    }
    if (o->min_mv > o->max_mv || o->max_ma > 5000U || (!o->max_ma && !o->max_mw)) return 0U;
    if (!o->max_mw) o->max_mw = (uint32_t)o->max_mv * o->max_ma / 1000U;
    /* Variable/battery ranges are informational: their RDO cannot select a
     * voltage, so they must not be represented as adjustable supplies. */
    o->selectable = (type == 0U || type == 3U) && o->min_mv <= RADIO_PD_MAX_MV && o->max_mv >= 5000U;
    return 1U;
}

/* The request one object would produce when asked for `want_mv`: a fixed
 * object's own voltage, or the highest PPS step at or below the request but
 * never below the range floor. `rdo` comes back without the object position,
 * which only the caller knows. Returns 0 when the object cannot serve a
 * request at all. Asking a PPS range that starts above `want_mv` yields its
 * floor rather than nothing, so the ceiling pass below can see it. */
static uint8_t pd_candidate(uint32_t p, uint16_t limit_ma, uint8_t allow_pps,
                            uint32_t available_mw, uint16_t want_mv, radio_pd_request_t *c)
{
    radio_pd_offer_t offer;
    uint32_t mv, ma, rdo;
    if (!RADIO_PdDecode(p, &offer) || !offer.selectable) return 0U;
    uint8_t pps = (p >> 30U) == 3U;
    if ((p >> 30U) == 0U) {
        mv = ((p >> 10U) & 1023U) * 50U;
        ma = (p & 1023U) * 10U;
        if (ma > limit_ma) ma = limit_ma;
        ma = ma / 10U * 10U;
        rdo = ((ma / 10U) << 10U) | (ma / 10U);
    } else if (pps && allow_pps && ((p >> 28U) & 3U) == 0U) {
        uint32_t min_mv = ((p >> 8U) & 255U) * 100U;
        uint32_t max_mv = ((p >> 17U) & 255U) * 100U;
        if (min_mv > max_mv || min_mv < 3300U || max_mv > 21000U) return 0U;
        mv = want_mv < max_mv ? want_mv : max_mv;
        mv = mv / 20U * 20U;
        /* Programmable floors are whole 100 mV, so this stays on a 20 mV step. */
        if (mv < min_mv) mv = min_mv;
        if (mv > max_mv) return 0U;
        ma = (p & 127U) * 50U;
        if (ma > limit_ma) ma = limit_ma;
        if (offer.power_limited && ma > available_mw * 1000U / mv)
            ma = available_mw * 1000U / mv;
        ma = ma / 50U * 50U;
        rdo = ((mv / 20U) << 9U) | (ma / 50U);
    } else return 0U;
    if (mv < 5000U || mv > RADIO_PD_MAX_MV || ma < 100U) return 0U;
    memset(c, 0, sizeof(*c));
    c->mv = (uint16_t)mv; c->ma = (uint16_t)ma; c->pps = pps; c->rdo = rdo;
    return 1U;
}

/* No USB suspend: this power connector has no USB data function. */
static void pd_commit(radio_pd_request_t *dst, const radio_pd_request_t *src, unsigned index)
{
    *dst = *src;
    dst->position = (uint8_t)(index + 1U);
    dst->rdo = ((uint32_t)(index + 1U) << 28U) | (1UL << 24U) | src->rdo;
}

uint8_t RADIO_PdChoose(const uint32_t *pdo, unsigned count, uint16_t target_mv,
                       uint16_t ceiling_mv, uint16_t limit_ma, uint8_t allow_pps,
                       radio_pd_request_t *out)
{
    if (!out) return 0U;
    memset(out, 0, sizeof(*out));
    if (!pdo || !count || count > 7U || target_mv < 5000U || target_mv > RADIO_PD_MAX_MV ||
        limit_ma < 100U || limit_ma > 3000U) return 0U;
    if (ceiling_mv < target_mv) ceiling_mv = target_mv;
    if (ceiling_mv > RADIO_PD_MAX_MV) ceiling_mv = RADIO_PD_MAX_MV;
    /* First source PDO must be a valid fixed 5 V offer (USB-PD SPR). Decode
     * it through the same validation used for every later PDO so a malformed
     * first object cannot be silently skipped in favour of an unsafe higher
     * voltage object. */
    radio_pd_offer_t first;
    if (!RADIO_PdDecode(pdo[0], &first) || first.type != RADIO_PD_FIXED ||
        first.min_mv != 5000U || first.max_mv != 5000U || first.max_ma < 100U) return 0U;
    uint32_t available_mw = 0U;
    for (unsigned i = 0U; i < count; ++i) {
        radio_pd_offer_t offer;
        if (RADIO_PdDecode(pdo[i], &offer) && offer.type == RADIO_PD_FIXED && offer.max_mw > available_mw)
            available_mw = offer.max_mw;
    }
    /* At or below the target, highest wins; a fixed object beats a PPS range
     * offering the same voltage. */
    for (unsigned i = 0U; i < count; ++i) {
        radio_pd_request_t c;
        if (!pd_candidate(pdo[i], limit_ma, allow_pps, available_mw, target_mv, &c)) continue;
        if (c.mv > target_mv) continue;
        if (out->position && (c.mv < out->mv || (c.mv == out->mv && (c.pps || !out->pps)))) continue;
        pd_commit(out, &c, i);
    }
    if (out->position && out->mv > 5000U) return 1U;
    /* Nothing above the mandatory 5 V reaches the target -- a source offering
     * 5/12/20 V against a 9 V target, say. Take the LOWEST offer above the
     * target instead, bounded by the ceiling, because every excess volt is
     * dissipated in the linear regulator. A 5 V-only result is kept when the
     * ceiling holds nothing either. */
    radio_pd_request_t best;
    memset(&best, 0, sizeof(best));
    for (unsigned i = 0U; i < count; ++i) {
        radio_pd_request_t c;
        if (!pd_candidate(pdo[i], limit_ma, allow_pps, available_mw, target_mv, &c)) continue;
        if (c.mv <= target_mv || c.mv > ceiling_mv) continue;
        if (best.position && (c.mv > best.mv || (c.mv == best.mv && (c.pps || !best.pps)))) continue;
        pd_commit(&best, &c, i);
    }
    if (best.position) *out = best;
    return out->position != 0U;
}
