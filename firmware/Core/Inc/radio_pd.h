#ifndef RADIO_PD_H
#define RADIO_PD_H
#include <stdint.h>
#include "radio_pd_policy.h"

/* What the radio is actually plugged into. U18 (USB-C power) and the DC jack
 * share one rail that the ADC cannot separate, so this comes from the Type-C
 * detector first and the rail only as a tie-break: a USB-C source cannot put
 * more than 5.5 V on VBUS before a contract, so anything higher is the jack. */
typedef enum {
    RADIO_SUPPLY_UNKNOWN,
    RADIO_SUPPLY_DC_JACK,
    RADIO_SUPPLY_USB_5V,  /* Type-C VBUS, no source on CC: charge-only cable or dumb supply */
    RADIO_SUPPLY_USB_C,   /* a source is on CC, no contract */
    RADIO_SUPPLY_USB_PD,  /* a source is on CC and a contract is live */
} radio_supply_t;

/* Why the rail is not at PD REQUEST MV. Derived on demand, never latched, so
 * it cannot disagree with the rest of the snapshot. */
typedef enum {
    RADIO_PD_WHY_OK,           /* contract at or above the target */
    RADIO_PD_WHY_BELOW_TARGET, /* below it, and it was the best the source offered */
    RADIO_PD_WHY_DISABLED,     /* PD ENABLE off */
    RADIO_PD_WHY_DC,           /* running from the DC jack; nothing to negotiate */
    RADIO_PD_WHY_NO_CC,        /* VBUS present but both CC lines open even with the PHY on */
    RADIO_PD_WHY_NO_SUPPLY,    /* rail below vSafe5V: debug power, or nothing plugged in */
    RADIO_PD_WHY_SILENT,       /* a source is on CC but sent no capabilities, even after Soft_Reset */
    RADIO_PD_WHY_PROBING,      /* attached, waiting for Source_Capabilities */
    RADIO_PD_WHY_BROWNOUT,     /* VBUS collapsed before capabilities arrived */
    RADIO_PD_WHY_NO_ATTACH,    /* a source is on CC and an attach is scheduled */
    RADIO_PD_WHY_NO_CAPS,      /* attached, no Source_Capabilities received */
    RADIO_PD_WHY_NO_OFFER,     /* caps received, nothing usable up to PD MAX MV */
    RADIO_PD_WHY_NEGOTIATING,  /* a request is in flight */
    RADIO_PD_WHY_REJECTED,     /* the source refused the request */
} radio_pd_why_t;

/* What the attach supervisor last did; see radio_pd.c. */
typedef enum {
    RADIO_PD_PROBE_NONE,      /* nothing yet, a clean detach, or internal Rd (CAD owns attach) */
    RADIO_PD_PROBE_NO_SOURCE, /* both CC lines open even with the analog PHY on */
    RADIO_PD_PROBE_NEEDED,    /* source seen in the CC sense; attach scheduled */
    RADIO_PD_PROBE_ACTIVE,    /* attached, waiting for capabilities (Soft_Reset if needed) */
    RADIO_PD_PROBE_WON,       /* reached a contract */
    RADIO_PD_PROBE_BROWNOUT,  /* VBUS collapsed before capabilities arrived */
    RADIO_PD_PROBE_TIMEOUT,   /* no capabilities; detached before ST's hard reset, retrying */
} radio_pd_probe_t;

typedef struct {
    uint32_t pdo[7], last_event, failures, resets, resyncs;
    uint16_t mv, ma, requested_mv, requested_ma, vid, pid;
    uint32_t xid;
    uint16_t source_mv, source_ma;
    uint8_t count, attached, contract, pending, pps, cc, revision, rp;
    uint8_t identity_valid, status_valid, fw_revision, hw_revision, source_watts;
    uint16_t target_mv, max_mv;
    uint8_t supply, why, probe;
} radio_pd_info_t;
void RADIO_PdSnapshot(radio_pd_info_t *info);

/* The UCPD peripheral as it actually stands. RM0440 46.4.6 and Table 440 give
 * sink mode no coding that enables the CC analog PHY without also applying the
 * internal Rd, so with PD INTERNAL RD off (CCENABLE = 00) the Type-C detector
 * reads 00 on both lines whatever is plugged in -- ST's CAD_Check_HW_SNK only
 * attaches when one line is not vOPEN, so the policy engine never runs and the
 * source sits at its default 5 V. That was measured on REV1 rather than
 * guessed at, and `cc_off_state` / `cc_on_state` are the measurement: the
 * detector read with the PHY off and again with it on.
 *
 * `arena_used` is what ST's core actually allocated out of pd_arena, so the
 * guard on that buffer is observable instead of being a number in a comment. */
typedef struct {
    uint32_t cr, cfg1, sr, imr;
    uint8_t cc_enable;      /* CR CCENABLE, 0 = both analog PHYs off */
    uint8_t anamode;        /* 0 source, 1 sink */
    uint8_t cc1_state, cc2_state; /* SR TYPEC_VSTATE_CCx, sink coding */
    uint8_t tcdis1, tcdis2; /* Type-C detectors disabled */
    uint8_t dead_battery_disabled; /* PWR CR3 UCPD1_DBDIS */
    uint8_t enabled, phy_rx_enabled, active_cc, internal_rd;
    uint8_t initialized, failed;
    uint16_t arena_used, arena_size;
    uint8_t cc_off_state, cc_on_state; /* boot sense: bits 1:0 CC1, bits 3:2 CC2 */
    uint8_t probe, cc_forced;
} radio_pd_raw_t;
void RADIO_PdRawSnapshot(radio_pd_raw_t *raw);
uint8_t RADIO_PdPowerStable(void);
void RADIO_PdPrepareShutdown(void);
void RADIO_PdInit(void);
void RADIO_PdTick(void);
void RADIO_PdStop(void);
/* Re-apply the configured CC termination after a settings change. */
void RADIO_PdApplyCcTermination(void);
/* RAM-only CC termination override used by the one-millisecond sense. */
void RADIO_PdSetCcForce(uint8_t on);
/* Bracket the sense window so it cannot be mistaken for an attachment. */
void RADIO_PdCcSenseBegin(void);
void RADIO_PdCcSenseEnd(void);
uint8_t RADIO_PdCcForced(void);
/* Both Type-C detector fields in one byte: bits 1:0 CC1, bits 3:2 CC2. */
uint8_t RADIO_PdCcState(void);
uint16_t RADIO_PdVbusMv(void);
uint16_t RADIO_PdVoltage(void);
uint16_t RADIO_PdCurrent(void);
const char *RADIO_PdStatus(void);
radio_supply_t RADIO_PdSupply(void);
radio_pd_why_t RADIO_PdWhyCode(void);
radio_pd_probe_t RADIO_PdProbe(void);
/* Ask the attach supervisor to look for a source now instead of at its next
 * back-off step; the console's `pd fix` and the menu's PD FIX. */
uint8_t RADIO_PdProbeStart(void);
/* PB10 power hold, for the bench back-feed recovery in radio_pd.c. */
void RADIO_PdPowerHold(uint8_t on);
/* Short lowercase tokens for the diagnostic console; the LCD font folds case,
 * so the UI prints the same strings. */
const char *RADIO_PdSupplyName(void);
const char *RADIO_PdWhyName(void);
const char *RADIO_PdProbeName(void);
/* One actionable sentence for the self test and the POWER SUPPLY INFO page. */
const char *RADIO_PdWhyDetail(void);

/* PD event log, newest first. `kind` below RADIO_PD_LOG_OWN is ST's
 * TRACE_EVENT (a = its sop/state argument, b = first two payload bytes,
 * c = payload size); from RADIO_PD_LOG_OWN up it is one of ours. */
typedef struct { uint32_t ms; uint16_t us; uint8_t kind, a; uint16_t b, c; } radio_pd_log_t;
#define RADIO_PD_LOG_OWN 100U
uint8_t RADIO_PdLogGet(uint16_t age, radio_pd_log_t *out);
uint16_t RADIO_PdLogCount(void);
uint16_t RADIO_PdMicros(void);
#endif
