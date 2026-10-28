/* Production PD integration; only ST core/interrupt boundaries are mocked. */
#include "radio_pd.h"
#include "radio.h"
#include "board_rev1.h"
#include "usbpd_core.h"
#include "usbpd_hw_if.h"
#include "usbpd_timersserver.h"
#include "usbpd_trace.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define __get_PRIMASK() 0U
#define __disable_irq() ((void)0)
#define __set_PRIMASK(x) ((void)(x))
#undef __WFI
#define __WFI() ((void)0)
#undef NVIC_DisableIRQ
#define NVIC_DisableIRQ(x) ((void)(x))
#include "../../Core/Src/radio_pd.c"

TRACE_ENTRY_POINT USBPD_Trace;
static radio_config_t test_config;
static radio_state_t test_state;
static uint32_t tick, trips, sends;
static uint8_t connected;
static unsigned hw_attaches, hw_detaches;
static CCxPin_TypeDef hw_cc;
static const USBPD_CAD_Callbacks *cad_cb;
static const USBPD_PE_Callbacks *pe_cb;
uint32_t HAL_GetTick(void) { return tick; }
void HAL_Delay(uint32_t ms) { tick += ms; }
void APP_WatchdogProgress(void) { }
const radio_config_t *RADIO_Config(void) { return &test_config; }
radio_state_t RADIO_State(void) { return test_state; }
void RADIO_Trip(uint32_t bits) { trips |= bits; test_state = RADIO_FAULT; }
void *__real_malloc(size_t n) { return malloc(n); }
void USBPD_TIM_Init(void) { }
uint8_t RADIO_PdTimerReady(void) { return 1U; }
void RADIO_PdPortPoll(void) { }
void RADIO_RefSofIsrReturn(void) { }
uint16_t RADIO_PdMicros(void) { return (uint16_t)(tick * 1000U); }
uint32_t MOD_GetResetCsr(void) { return 0x14000000UL; }
void USBPD_PE_SetTrace(TRACE_ENTRY_POINT p, uint8_t debug) { (void)p; (void)debug; }
static uint8_t power_hold = 1U;
static unsigned hold_changes;
void RADIO_PdPowerHold(uint8_t on) { power_hold = on; ++hold_changes; }
/* The raw Soft_Reset goes out through ST's PHY with the transmit-complete
 * callback borrowed; the PHY here completes it on the spot. */
USBPD_PORT_HandleTypeDef Ports[USBPD_PORT_COUNT];
static unsigned soft_resets;
static uint8_t phy_busy;
static void prl_tx_completed(uint8_t p, uint32_t status) { (void)p; (void)status; assert(0 && "PRL saw a foreign frame"); }
USBPD_StatusTypeDef USBPD_HW_IF_SendBuffer(uint8_t p, USBPD_SOPType_TypeDef t, uint8_t *b, uint32_t n)
{
    assert(p == 0U && t == USBPD_SOPTYPE_SOP && n == 2U && b[0] == 0x8DU && b[1] == 0x00U);
    if (phy_busy) return USBPD_ERROR;
    ++soft_resets;
    Ports[0].cbs.USBPD_HW_IF_TxCompleted(0U, 0U);
    return USBPD_OK;
}
/* The CC termination and the rail, modelled the way REV1 behaves: the Type-C
 * detector reads vOPEN on both lines unless the analog PHY is enabled, which
 * on this board means asking for the internal Rd. */
static uint8_t test_cc_state, test_cc_force;
static uint16_t test_vbus_mv = 5000U;
void RADIO_PdSetCcForce(uint8_t on) { test_cc_force = on ? 1U : 0U; }
void RADIO_PdCcSenseBegin(void) { RADIO_PdSetCcForce(1U); }
void RADIO_PdCcSenseEnd(void) { RADIO_PdSetCcForce(0U); }
uint8_t RADIO_PdCcForced(void) { return test_cc_force; }
uint8_t RADIO_PdCcState(void) { return test_cc_force ? test_cc_state : 0U; }
uint16_t RADIO_PdVbusMv(void) { return test_vbus_mv; }
void USBPD_PORT0_IRQHandler(void) { }
uint8_t USBPD_PWR_IF_GetVBUSStatus(uint8_t p, USBPD_VBUSPOWER_STATUS s) { (void)p; (void)s; return 1U; }
uint32_t USBPD_PE_CheckLIB(uint32_t id) { return id == _LIB_ID; }
uint32_t USBPD_PE_GetMemoryConsumption(void) { return 4572U; }
USBPD_CAD_StatusTypeDef USBPD_CAD_Init(uint8_t p, const USBPD_CAD_Callbacks *cb,
                                     const USBPD_SettingsTypeDef *s, USBPD_ParamsTypeDef *params_)
{
    (void)p; (void)params_;
    assert(s->PE_DefaultRole == USBPD_PORTPOWERROLE_SNK && !s->PE_RoleSwap && !s->PE_VDMSupport);
    assert(!s->PE_PD3_Support.d.PE_FastRoleSwapSupport);
    assert(__wrap_malloc(12U) != NULL);
    cad_cb = cb; return USBPD_CAD_OK;
}
USBPD_StatusTypeDef USBPD_PE_Init(uint8_t p, USBPD_SettingsTypeDef *s,
                                USBPD_ParamsTypeDef *params_, const USBPD_PE_Callbacks *cb)
{
    (void)p; (void)s; (void)params_;
    assert(__wrap_malloc(1200U) != NULL);
    assert(__wrap_malloc(312U) != NULL);
    pe_cb = cb; return USBPD_OK;
}
void USBPD_CAD_PortEnable(uint8_t p, USBPD_CAD_activation s) { (void)p; (void)s; }
void USBPD_CAD_EnterErrorRecovery(uint8_t p) { (void)p; }
void USBPD_PE_IsCableConnected(uint8_t p, uint8_t on) { (void)p; connected = on; }
void USBPD_PE_StateMachine_Reset(uint8_t p) { (void)p; }
void USBPD_PE_StateMachine_Stop(uint8_t p) { (void)p; }
void USBPD_PE_TimerCounter(uint8_t p) { (void)p; }
void USBPD_PRL_TimerCounter(uint8_t p) { (void)p; }
void HW_SignalAttachement(uint8_t p, CCxPin_TypeDef cc)
{ assert(p == 0U); hw_cc = cc; ++hw_attaches; }
void HW_SignalDetachment(uint8_t p) { assert(p == 0U); ++hw_detaches; }
uint32_t USBPD_CAD_Process(void) { return 1U; }
uint32_t USBPD_PE_StateMachine_SNK(uint8_t p) { (void)p; return 1U; }
USBPD_StatusTypeDef USBPD_PE_Request_DataMessage(uint8_t p, USBPD_DataMsg_TypeDef msg, uint32_t *data)
{
    (void)data;
    assert(p == 0U && msg == USBPD_DATAMSG_REQUEST);
    uint32_t rdo;
    USBPD_CORE_PDO_Type_TypeDef type;
    pe_cb->USBPD_PE_SNK_EvaluateCapabilities(p, &rdo, &type);
    assert(rdo && request.mv <= RADIO_PD_MAX_MV && request.ma <= 3000U);
    ++sends; return USBPD_OK;
}
USBPD_StatusTypeDef USBPD_PE_Request_CtrlMessage(uint8_t p, USBPD_ControlMsg_TypeDef m, USBPD_SOPType_TypeDef sop)
{ (void)p; (void)m; assert(sop == USBPD_SOPTYPE_SOP); return USBPD_OK; }
static uint32_t fixed(unsigned mv, unsigned ma) { return (mv / 50U << 10U) | ma / 10U; }
static void source_caps(const uint32_t *pdos, unsigned n)
{
    pe_cb->USBPD_PE_SetDataInfo(0U, USBPD_CORE_DATATYPE_RCV_SRC_PDO, (uint8_t *)pdos, n * 4U);
    uint32_t rdo;
    USBPD_CORE_PDO_Type_TypeDef type;
    pe_cb->USBPD_PE_SNK_EvaluateCapabilities(0U, &rdo, &type);
    assert(rdo);
}
static void ready(void)
{
    pe_cb->USBPD_PE_Notify(0U, USBPD_NOTIFY_REQUEST_ACCEPTED);
    /* CONFIG_1 emitted STATE_SNK_READY (32), not POWER_SNK_READY (18), on
     * the source used for the REV1 live qualification. */
    pe_cb->USBPD_PE_Notify(0U, USBPD_NOTIFY_STATE_SNK_READY);
}
/* The CC sense and supply classification, before RADIO_PdInit. */
static void probe_test(void)
{
    /* Nothing on CC and a rail no USB-C source could produce without a
     * contract: the DC jack. */
    test_cc_state = 0U; test_vbus_mv = 9000U;
    pd_cc_sense();
    assert(probe == RADIO_PD_PROBE_NO_SOURCE && !test_cc_force);
    assert(RADIO_PdProbeStart() == 0U); /* not initialized */
    assert(RADIO_PdSupply() == RADIO_SUPPLY_DC_JACK);
    assert(RADIO_PdWhyCode() == RADIO_PD_WHY_DC);

    /* VBUS at 5 V with no source on CC either: a charge-only cable. */
    test_vbus_mv = 5000U; probe = RADIO_PD_PROBE_NONE;
    pd_cc_sense();
    assert(RADIO_PdSupply() == RADIO_SUPPLY_USB_5V);
    assert(RADIO_PdWhyCode() == RADIO_PD_WHY_NO_CC);

    /* The REV1 topology: external R3/R4 let the source power the board, but
     * the Type-C detector sees it only while the internal Rd is applied. */
    test_cc_state = 0x01U; probe = RADIO_PD_PROBE_NONE;
    pd_cc_sense();
    assert(cc_off_state == 0U && cc_on_state == 0x01U);
    assert(probe == RADIO_PD_PROBE_NEEDED && !test_cc_force);
    assert(RADIO_PdSupply() == RADIO_SUPPLY_USB_C);
    assert(RADIO_PdWhyCode() == RADIO_PD_WHY_NO_ATTACH);

    /* The stored setting already asks for the internal Rd: nothing to sense. */
    test_config.pd_internal_rd = 1U;
    probe = RADIO_PD_PROBE_NONE;
    pd_cc_sense();
    assert(probe == RADIO_PD_PROBE_NONE && !test_cc_force);
    test_config.pd_internal_rd = 0U;

    /* Leave the module as RADIO_PdInit will find it. */
    probe = RADIO_PD_PROBE_NONE; test_cc_force = 0U;
    attached = contract = pending = accepted = connected = 0U; source_count = 0U;
    cc_off_state = cc_on_state = 0U; test_cc_state = 0U; test_vbus_mv = 5000U;
}

static void run_until(uint32_t t)
{
    while (tick < t) { ++tick; RADIO_PdTick(); }
}

/* The attach supervisor that replaced v8.01's boot-only probe. Runs after
 * the negotiation tests on the initialized module. */
static void supervisor_test(void)
{
    uint32_t pdos[] = {fixed(5000,3000), fixed(9000,3000)};
    test_config.pd_enable = 1U; test_config.pd_target_mv = 9000U; test_config.pd_max_mv = 12000U;
    test_state = RADIO_RECEIVE;
    Ports[0].cbs.USBPD_HW_IF_TxCompleted = prl_tx_completed;

    /* Unplugged: VBUS falls away and the port lets go after 20 ms. */
    assert(attached);
    test_vbus_mv = 3000U;
    unsigned detaches = hw_detaches;
    run_until(tick + 10U); assert(attached);
    run_until(tick + 15U); assert(!attached && hw_detaches == detaches + 1U && !connected);

    /* Plugged back in: seen within a few milliseconds of VBUS, with a real
     * CC sense (the internal Rd held for PD_SENSE_SETTLE_MS, then dropped). */
    test_vbus_mv = 5000U; test_cc_state = 0x08U;
    unsigned attaches = hw_attaches;
    run_until(tick + 1U);
    assert(!attached);
    run_until(tick + 4U);
    assert(attached && hw_attaches == attaches + 1U && hw_cc == CC2 && !test_cc_force && connected);
    assert(RADIO_PdWhyCode() == RADIO_PD_WHY_PROBING && !RADIO_PdPowerStable());

    /* A fresh source that stays silent: Soft_Reset at PD_WAIT_SR_MS, and a
     * detach before ST's 500 ms SinkWaitCap hard reset could run. */
    const uint32_t start = wait_since;
    unsigned resets_before = soft_resets;
    run_until(start + PD_WAIT_SR_MS - 1U); assert(soft_resets == resets_before);
    run_until(start + PD_WAIT_SR_MS); assert(soft_resets == resets_before + 1U);
    assert(Ports[0].cbs.USBPD_HW_IF_TxCompleted == prl_tx_completed); /* handed back */
    run_until(start + PD_WAIT_MAX_MS - 1U); assert(attached);
    run_until(start + PD_WAIT_MAX_MS); assert(!attached && probe == RADIO_PD_PROBE_TIMEOUT);
    assert(soft_resets == resets_before + 1U && RADIO_PdWhyCode() == RADIO_PD_WHY_SILENT);
    /* It tries again after a back-off, and answers Source_Capabilities
     * normally this time. */
    attaches = hw_attaches;
    run_until(tick + pd_backoff_ms[0] - 10U); assert(!attached && hw_attaches == attaches);
    run_until(tick + 20U); assert(attached && hw_attaches == attaches + 1U);
    source_caps(pdos, 2U); ready();
    run_until(tick + 1000U);
    assert(attached && contract && RADIO_PdVoltage() == 9000U && probe == RADIO_PD_PROBE_WON);
    assert(RADIO_PdWhyCode() == RADIO_PD_WHY_OK && RADIO_PdSupply() == RADIO_SUPPLY_USB_PD);

    /* The MCU restarted under a live 9 V contract: attach at once and ask for
     * the capabilities with Soft_Reset on the next pass, instead of skipping
     * PD as v8.01 did (or hard-resetting VBUS away as ST's PE would). */
    cad_cb->USBPD_CAD_CallbackEvent(0U, USBPD_CAD_EVENT_DETACHED, CCNONE);
    test_vbus_mv = 8800U;
    resets_before = soft_resets;
    pd_attach(tick, test_vbus_mv);
    assert(attached && kept);
    run_until(tick + PD_KEPT_SR_MS); assert(soft_resets == resets_before + 1U);
    source_caps(pdos, 2U); ready();
    run_until(tick + PD_WAIT_MAX_MS + 10U);
    assert(attached && contract && RADIO_PdVoltage() == 9000U);
    /* A busy PHY refuses the frame; the port still leaves before 500 ms. */
    cad_cb->USBPD_CAD_CallbackEvent(0U, USBPD_CAD_EVENT_DETACHED, CCNONE);
    phy_busy = 1U; resets_before = soft_resets;
    pd_attach(tick, test_vbus_mv);
    run_until(tick + PD_WAIT_MAX_MS);
    assert(!attached && soft_resets == resets_before && probe == RADIO_PD_PROBE_TIMEOUT);
    phy_busy = 0U;

    /* A hard reset cycles VBUS; no Soft_Reset is sent into it. */
    tries = 0U; next_try_at = tick; run_until(tick + 5U); assert(attached);
    source_caps(pdos, 2U); ready();
    resets_before = soft_resets;
    pe_cb->USBPD_PE_HardReset(0U, USBPD_PORTPOWERROLE_SNK, USBPD_HR_STATUS_START_ACK);
    run_until(tick + PD_WAIT_SR_MS + 10U);
    assert(soft_resets == resets_before && attached);

    /* Bench: VBUS gone but the MCU still running, so VIN is a debugger's
     * back-feed. Q2 is opened so the source can reach vSafe0V, and closed
     * again to look. Never while the radio is powering itself off. */
    cad_cb->USBPD_CAD_CallbackEvent(0U, USBPD_CAD_EVENT_DETACHED, CCNONE);
    test_vbus_mv = 1900U; hold_changes = 0U; power_hold = 1U;
    run_until(tick + 2U); low_since = 0U;
    run_until(tick + PD_BACKFEED_MS + 2U);
    assert(!power_hold && hold_changes == 1U);
    run_until(tick + PD_HOLD_OPEN_MS + 1U);
    assert(power_hold && hold_changes == 2U);
    test_vbus_mv = 5000U;
    run_until(tick + 10U);
    assert(power_hold && attached);
    cad_cb->USBPD_CAD_CallbackEvent(0U, USBPD_CAD_EVENT_DETACHED, CCNONE);
    test_state = RADIO_OFF; test_vbus_mv = 1900U; hold_changes = 0U;
    run_until(tick + PD_BACKFEED_MS * 4U);
    assert(power_hold && hold_changes == 0U);
    test_state = RADIO_RECEIVE; test_vbus_mv = 5000U;

    /* The operator's PD FIX looks straight away. */
    run_until(tick + 10U);
    if (attached) cad_cb->USBPD_CAD_CallbackEvent(0U, USBPD_CAD_EVENT_DETACHED, CCNONE);
    next_try_at = tick + 60000U;
    assert(RADIO_PdProbeStart() == 1U);
    run_until(tick + 5U); assert(attached);
    assert(RADIO_PdProbeStart() == 0U); /* already attached */

    /* The event log keeps a boot marker and reads back newest first. */
    radio_pd_log_t e;
    assert(RADIO_PdLogCount() > 0U && RADIO_PdLogGet(0U, &e));
    assert(!RADIO_PdLogGet(RADIO_PdLogCount(), &e));
}

int main(void)
{
    RADIO_ConfigDefaults(&test_config);
    assert(test_config.pd_enable && test_config.pd_target_mv == 9000U && !test_config.pd_internal_rd);
    assert(test_config.pd_max_mv == 12000U);
    probe_test();
    /* Without a USB-C source the DC input powers the radio; PD must not
     * block TX by itself (the 5 V rail check does). */
    assert(RADIO_PdPowerStable());
    test_cc_state = 0x08U; /* boot source on CC2 */
    test_config.pd_enable = 0U; /* start at the mandatory 5 V contract */
    test_state = RADIO_RECEIVE;
    RADIO_PdInit();
    assert(cad_cb && pe_cb && initialized && pd_used == 1528U);
    assert(connected && attached && probe == RADIO_PD_PROBE_ACTIVE && hw_cc == CC2 && !test_cc_force);
    assert(pd_log.magic == PD_LOG_MAGIC && RADIO_PdLogCount() > 0U);
    RADIO_PdInit(); assert(pd_used == 1528U);
    assert(pe_cb->USBPD_PE_EvaluatPRSwap(0U) == USBPD_REJECT);
    assert(pe_cb->USBPD_PE_DataRoleSwap(0U) == USBPD_REJECT);
    assert(pe_cb->USBPD_PE_VconnPwr(0U, USBPD_ENABLE) == USBPD_REJECT);
    uint32_t pdos[] = {fixed(5000,3000), fixed(9000,3000), fixed(15000,3000),
        (3UL<<30U) | (210U<<17U) | (33U<<8U) | 60U};
    source_caps(pdos, 4U);
    assert(RADIO_PdVoltage() == 0U && !RADIO_PdPowerStable());
    test_config.pd_enable = 1U; assert(!RADIO_PdPowerStable()); /* attached, no contract */
    test_config.pd_enable = 0U;
    ready(); assert(RADIO_PdVoltage() == 5000U);
    test_config.pd_enable = 1U; test_config.pd_target_mv = 14000U;
    tick = 600U; RADIO_PdTick();
    assert(sends == 1U && !RADIO_PdPowerStable());
    assert(RADIO_PdVoltage() == 5000U); /* Requested is not yet accepted. */
    ready(); assert(RADIO_PdVoltage() == 14000U && RADIO_PdPowerStable());
    radio_pd_info_t info; RADIO_PdSnapshot(&info);
    assert(info.count == 4U && info.cc == CC2 && info.pps && info.contract && info.mv == 14000U);
    test_config.pd_target_mv = 9000U;
    tick += 600U; RADIO_PdTick();
    pe_cb->USBPD_PE_Notify(0U, USBPD_NOTIFY_REQUEST_REJECTED);
    assert(RADIO_PdVoltage() == 14000U); /* Rejection preserves old contract. */
    tick += 600U; RADIO_PdTick();
    pe_cb->USBPD_PE_Notify(0U, USBPD_NOTIFY_REQUEST_WAIT);
    tick += 600U; RADIO_PdTick();
    pe_cb->USBPD_PE_Notify(0U, USBPD_NOTIFY_REQUEST_REJECTED);
    unsigned prior = sends;
    tick += 600U; RADIO_PdTick(); assert(sends == prior); /* Bounded attempts. */
    test_state = RADIO_TRANSMIT;
    test_config.pd_target_mv = 10000U;
    tick += 1000U; RADIO_PdTick(); assert(sends == prior);
    source_caps(pdos, 4U); assert(request.mv == 14000U && !trips); ready();
    test_state = RADIO_RECEIVE;
    tick += 7001U; RADIO_PdTick(); assert(sends > prior); /* PPS renewal. */
    ready(); assert(RADIO_PdVoltage() == 10000U);
    test_config.pd_target_mv = 12000U;
    tick += 600U; RADIO_PdTick();
    pe_cb->USBPD_PE_Notify(0U, USBPD_NOTIFY_REQUEST_ACCEPTED);
    tick += 1501U; RADIO_PdTick();
    assert(trips && !contract); /* Accepted transition without PS_RDY is unsafe. */
    cad_cb->USBPD_CAD_CallbackEvent(0U, USBPD_CAD_EVENT_DETACHED, CCNONE);
    RADIO_PdSnapshot(&info); assert(!connected && !info.count && !info.contract && !info.identity_valid);
    test_state = RADIO_RECEIVE; test_config.pd_enable = 0U;
    cad_cb->USBPD_CAD_CallbackEvent(0U, USBPD_CAD_EVENT_ATTACHED, CC1);
    source_caps(pdos, 4U); ready(); assert(RADIO_PdVoltage() == 5000U);
    RADIO_PdSnapshot(&info); assert(info.cc == CC1);
    /* A changed capability list invalidates the old contract and starts a
     * fresh negotiation even while the receiver remains active. */
    test_config.pd_enable = 1U;
    uint32_t changed[] = {fixed(5000,3000), fixed(12000,2000)};
    source_caps(changed, 2U);
    assert(!RADIO_PdPowerStable());
    ready();
    assert(RADIO_PdVoltage() == 12000U);
    RADIO_PdSnapshot(&info);
    assert(info.supply == RADIO_SUPPLY_USB_PD && info.why == RADIO_PD_WHY_OK);
    assert(info.target_mv == 12000U && info.max_mv == test_config.pd_max_mv);

    /* The ceiling. This source steps straight past a 9 V target, which is
     * what a laptop charger does, and 5 V is not enough for the regulator. */
    test_config.pd_target_mv = 9000U;
    tick += 600U; RADIO_PdTick();
    assert(RADIO_PdVoltage() == 12000U && RADIO_PdWhyCode() == RADIO_PD_WHY_OK);
    unsigned before_ceiling = sends;
    test_config.pd_max_mv = 9000U; /* no headroom: 12 V is no longer allowed */
    tick += 600U; RADIO_PdTick();
    assert(sends > before_ceiling);
    ready();
    assert(RADIO_PdVoltage() == 5000U && RADIO_PdWhyCode() == RADIO_PD_WHY_BELOW_TARGET);

    /* Three refusals stop the sink, but no longer for the rest of the power
     * cycle: a source can start offering the target later. */
    test_config.pd_max_mv = 12000U;
    for (unsigned n = 0U; n < 3U; ++n) {
        tick += 600U; RADIO_PdTick();
        pe_cb->USBPD_PE_Notify(0U, USBPD_NOTIFY_REQUEST_REJECTED);
    }
    unsigned stuck = sends;
    tick += 600U; RADIO_PdTick(); assert(sends == stuck && retries >= 3U);
    tick += PD_RETRY_REARM_MS; RADIO_PdTick();
    assert(sends > stuck && rearm_count == 1U);

    /* The re-arm is itself bounded, so a source that always refuses is not
     * retried for ever. */
    for (unsigned guard = 0U; guard < 128U && rearm_count < PD_RETRY_REARM_MAX; ++guard) {
        pe_cb->USBPD_PE_Notify(0U, USBPD_NOTIFY_REQUEST_REJECTED);
        tick += PD_RETRY_REARM_MS; RADIO_PdTick();
    }
    assert(rearm_count == PD_RETRY_REARM_MAX);
    for (unsigned n = 0U; n < 3U; ++n) {
        tick += 600U; RADIO_PdTick();
        pe_cb->USBPD_PE_Notify(0U, USBPD_NOTIFY_REQUEST_REJECTED);
    }
    unsigned capped = sends;
    tick += PD_RETRY_REARM_MS * 4U; RADIO_PdTick();
    assert(sends == capped && RADIO_PdWhyCode() == RADIO_PD_WHY_BELOW_TARGET);

    supervisor_test();
    puts("REV1 PD CC sense, supply classification, attach, orientation, negotiation, "
         "14V PPS, ceiling, retries and re-arm, TX freeze and reconnect, attach supervisor "
         "(replug, Soft_Reset resync, silent source, VBUS loss, bench back-feed): PASS");
    return 0;
}
