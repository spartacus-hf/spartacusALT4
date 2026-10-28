#ifndef RADIO_PD_POLICY_H
#define RADIO_PD_POLICY_H
#include <stdint.h>
#define RADIO_PD_MAX_MV 14000U
typedef enum { RADIO_PD_FIXED, RADIO_PD_BATTERY, RADIO_PD_VARIABLE, RADIO_PD_PPS, RADIO_PD_UNSUPPORTED } radio_pd_type_t;
typedef struct {
    uint16_t min_mv, max_mv, max_ma;
    uint32_t max_mw;
    radio_pd_type_t type;
    uint8_t power_limited, selectable;
} radio_pd_offer_t;
uint8_t RADIO_PdDecode(uint32_t pdo, radio_pd_offer_t *offer);
typedef struct {
    uint32_t rdo;
    uint16_t mv, ma;
    uint8_t pps, position;
} radio_pd_request_t;
/* Objects are host-endian USB-PD SPR PDOs. Unsupported supply types are
 * skipped. `target_mv` is what the board asks for; `ceiling_mv` is how far
 * above that it may go when no offer reaches the target, for a source whose
 * ladder steps straight past it. Both are clamped to RADIO_PD_MAX_MV. */
uint8_t RADIO_PdChoose(const uint32_t *pdo, unsigned count, uint16_t target_mv,
                       uint16_t ceiling_mv, uint16_t limit_ma, uint8_t allow_pps,
                       radio_pd_request_t *out);
#endif
