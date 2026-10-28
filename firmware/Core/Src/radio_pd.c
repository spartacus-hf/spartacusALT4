#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
#include "radio_pd.h"
#include "radio_pd_policy.h"
#include "main.h"
#include "radio.h"
#include "radio_clocks.h"
#include "board_rev1.h"
#include "modulator.h"
#include "usbpd_core.h"
#include "usbpd_hw_if.h"
#include "usbpd_timersserver.h"
#include "usbpd_trace.h"
#include <string.h>
#include <stdlib.h>

/* ST's core allocates only during initialization. Keep its storage bounded and
 * separate from the general heap; no allocator is used by realtime callbacks. */
/* Pinned CM4 CONFIG_1 archive: one port allocates 12 + 1200 + 312
 * bytes (1528 with alignment). GetMemoryConsumption reports ALL THREE
 * library ports (4572), not our one initialized port. Allocation is bounded
 * so a future larger archive fails initialization without overrunning RAM:
 * __wrap_malloc returns NULL, USBPD_CAD_Init or USBPD_PE_Init fails, and
 * RADIO_PdInit reports PD INIT ERROR at boot rather than corrupting anything.
 * The archive is pinned (its md5 is ST's G4 1.6.2 file), so the arena is
 * the measured figure rounded to the 8-byte grain; it was 1792, a 264-byte
 * guard of RAM that nothing could use. `pd raw` prints what the archive
 * took (arena used/size). */
static uint8_t pd_arena[1536] __attribute__((aligned(8)));
static size_t pd_used;
static uint8_t allocating, initialized, attached, contract, failed;
static uint8_t accepted, pending, retries, shutdown_requested, discovery_sent, pps_status_sent;
static uint32_t source_pdo[7], source_count, renewed_at, attempted_at, config_key;
static uint32_t last_event, failures, resets, resyncs;
static uint8_t cc_off_state, cc_on_state, probe, supply;
static uint8_t rearm_count;
static uint16_t ceiling_key;
/* A source that refused three times can start offering the target later --
 * another load released, a PPS source warmed up -- so re-arm slowly instead
 * of giving up for the rest of the power cycle. */
#define PD_RETRY_REARM_MS    30000U
#define PD_RETRY_REARM_MAX   8U
static radio_pd_request_t request, active_request;
static uint8_t source_ext[24], source_status[4], identity_valid, status_valid;
static USBPD_ParamsTypeDef params;

/* ---- Event log --------------------------------------------------------------
 * What the attach supervisor and ST's stack did, in order, with a millisecond
 * tick and the LPTIM microsecond counter. It lives in .noinit, so a reset (a
 * fault, the watchdog, a firmware install, the debugger) keeps the record of
 * what preceded it; a power loss does not. Each boot adds a marker carrying
 * the reset cause, and `pd log` prints it oldest first. */
#define PD_LOG_N     64U
#define PD_LOG_MAGIC 0x50444C31UL
static struct {
    uint32_t magic;
    uint16_t seq;
    radio_pd_log_t e[PD_LOG_N];
} pd_log __attribute__((section(".noinit")));
enum { PDL_SENSE = RADIO_PD_LOG_OWN, PDL_ATTACH, PDL_DETACH, PDL_HARDRESET, PDL_CAD,
       PDL_NOTIFY, PDL_EVAL, PDL_VBUS, PDL_HOLD, PDL_CAPS, PDL_SOFTRESET, PDL_BOOT };
static void pd_log_add(uint8_t kind, uint8_t a, uint16_t b, uint16_t c)
{
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    radio_pd_log_t *e = &pd_log.e[pd_log.seq % PD_LOG_N];
    e->ms = HAL_GetTick(); e->us = RADIO_PdMicros();
    e->kind = kind; e->a = a; e->b = b; e->c = c;
    ++pd_log.seq;
    __set_PRIMASK(pm);
}
uint16_t RADIO_PdLogCount(void) { return pd_log.seq < PD_LOG_N ? pd_log.seq : (uint16_t)PD_LOG_N; }
uint8_t RADIO_PdLogGet(uint16_t age, radio_pd_log_t *out)
{
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    const uint8_t ok = age < RADIO_PdLogCount();
    if (ok) *out = pd_log.e[(uint16_t)(pd_log.seq - 1U - age) % PD_LOG_N];
    __set_PRIMASK(pm);
    return ok;
}
/* ST's archive calls USBPD_Trace from USBPD_PE_Notification with NO null
 * check (8001566: ldr r6,[USBPD_Trace]; 8001572: blx r6), so leaving the
 * pointer null is a branch to address 0 on the very first policy-engine
 * notification -- INVSTATE, forced HardFault, and this board's fault handler
 * resets, so it presents as a reset loop with a charger attached. The same
 * sink is handed to USBPD_PE_SetTrace, which gives the log the PE state
 * changes and the header of every message in and out. */
static void pd_trace(TRACE_EVENT type, uint8_t port, uint8_t sop, uint8_t *ptr, uint32_t size)
{
    (void)port;
    uint16_t b = 0U;
    if (ptr && size >= 2U) b = (uint16_t)(ptr[0] | ((uint16_t)ptr[1] << 8U));
    pd_log_add((uint8_t)type, sop, b, (uint16_t)size);
}
/* Nothing else in the image allocates -- dr_mp3 runs from its own arena
 * and USB from static blocks -- so there is no heap to fall back on, and
 * newlib's allocator stays out of the link. */
void *__wrap_malloc(size_t size)
{
    if (!allocating) return NULL;
    if (size > sizeof(pd_arena) - pd_used) return NULL;
    size_t aligned = (size + 7U) & ~(size_t)7U;
    if (aligned > sizeof(pd_arena) - pd_used) return NULL;
    void *p = pd_arena + pd_used;
    pd_used += aligned;
    return p;
}

static USBPD_SettingsTypeDef settings = {
    .PE_SupportedSOP = USBPD_SUPPORTED_SOP_SOP,
    .PE_SpecRevision = USBPD_SPECIFICATION_REV3,
    .PE_DefaultRole = USBPD_PORTPOWERROLE_SNK,
    .PE_CapscounterSupport = USBPD_TRUE,
    .PE_PD3_Support.d.Is_SrcCapaExt_Supported = 1U,
    .PE_PD3_Support.d.Is_GetPPSStatus_Supported = 1U,
    /* All role swaps, VCONN, FRS, VDM and optional extended replies disabled. */
};

/* The port is a sink and never runs the source state machine, which ST's
 * archive reaches only through USBPD_PE_StateMachine_DRP -- a function this
 * firmware never calls. Defining the symbol here keeps the archive's 3.7 KB
 * usbpd_pe_src.o out of the link. */
uint32_t USBPD_PE_StateMachine_SRC(uint8_t PortNum) { (void)PortNum; return 0U; }

static void wake_port(uint8_t port) { (void)port; }
/* The archive proved it does not null-check every hook it owns, so no member
 * of the callback struct is left null even when the feature is unused. */
static USBPD_StatusTypeDef power_setup_done(uint8_t port) { (void)port; return USBPD_OK; }
static USBPD_StatusTypeDef reject_src_request(uint8_t port, USBPD_CORE_PDO_Type_TypeDef *object)
{ (void)port; (void)object; return USBPD_REJECT; }
static void ignore_extended(uint8_t port, USBPD_ExtendedMsg_TypeDef id, uint8_t *data, uint16_t size)
{ (void)port; (void)id; (void)data; (void)size; }
static void ignore_prswap(uint8_t port, USBPD_PortPowerRole_TypeDef role, USBPD_PRS_Status_TypeDef status)
{ (void)port; (void)role; (void)status; }
static void wake_cad(void) { }
static USBPD_StatusTypeDef reject(uint8_t port) { (void)port; return USBPD_REJECT; }
static USBPD_StatusTypeDef reject_vconn(uint8_t port, USBPD_FunctionalState enable)
{ (void)enable; return reject(port); }
static uint8_t transmitting(void)
{
    return (uint8_t)(RADIO_State() == RADIO_TRANSMIT || RADIO_State() == RADIO_TX_PREPARE);
}
static void power_inhibit(void)
{
    contract = 0U;
    pending = accepted = 0U;
    if (transmitting()) RADIO_Trip(RADIO_FAULT_VIN);
}
static void recovery(uint8_t port)
{
    power_inhibit();
    USBPD_CAD_EnterErrorRecovery(port);
}

/* ---- Attach supervisor -------------------------------------------------------
 * With the external R3/R4 Rd the UCPD pulls stay off (CCENABLE=00), and so
 * does the Type-C detector: ST's CAD can see neither a plug, nor an unplug,
 * nor a source coming back after a hard reset. v8.01 looked once, at boot,
 * and only while VBUS sat inside vSafe5V, which left three holes:
 *   - an MCU reset under a live contract (fault, watchdog, firmware install,
 *     the debugger) found 9 V on the rail, skipped PD and showed a warning;
 *   - a source plugged in, or returning, after boot was never seen;
 *   - a startup that failed was never tried again.
 * The supervisor below does the CAD's job from the rail and the
 * one-millisecond CC sense. Detached, it senses when VBUS appears and then
 * every PD_SENSE_MS, and attaches on a source in the sensed orientation.
 * Attached without capabilities it sends Soft_Reset (pd_send_soft_reset):
 * at once when VBUS is above vSafe5V -- a source holding a contract never
 * advertises on its own -- and after PD_WAIT_SR_MS otherwise. Still nothing
 * by PD_WAIT_MAX_MS and it detaches, ahead of ST's 500 ms SinkWaitCap hard
 * reset: a hard reset removes VBUS for a second or more, VBUS is the MCU's
 * only supply, and once the MCU is down Q13 lets go of the Q2 power switch,
 * so the radio would switch itself off. It then retries with a back-off. */
#define PD_VBUS_PRESENT_MV 4400U  /* vSafe5V minimum */
#define PD_VSAFE5V_MAX_MV  5500U
#define PD_VBUS_LOST_MV    3670U  /* ST's sink-detach threshold */
#define PD_VBUS_LOST_MS    20U
#define PD_SENSE_MS        1000U  /* detached with VBUS: look for a source this often */
#define PD_SENSE_SETTLE_MS 2U     /* Type-C detector refresh with the PHY on */
#define PD_KEPT_SR_MS      5U     /* PE is in WAIT_FOR_CAPABILITIES after one pass */
/* A fresh source repeats capabilities every 100-200 ms; one that has gone
 * quiet took 100 ms to answer Soft_Reset on REV1 (a cold power-up 30 s after
 * the plug), so leave it 300 ms before PD_WAIT_MAX_MS. */
#define PD_WAIT_SR_MS      150U
#define PD_WAIT_MAX_MS     450U   /* ST's PE hard-resets at 500 ms */
/* Bench only: an MCU still running with VIN below 3 V is fed from outside
 * (a debugger's 3.3 V), which back-feeds through Q2 onto VBUS and holds it
 * above vSafe0V, and a source will not re-apply VBUS until it sees vSafe0V.
 * Open Q2 for a while so it can, then look again. */
#define PD_BACKFEED_MV     3000U
#define PD_BACKFEED_MS     300U
#define PD_HOLD_OPEN_MS    1500U
static const uint16_t pd_backoff_ms[] = { 1000U, 2000U, 5000U, 10000U, 30000U, 60000U };
static uint8_t sense_phase, soft_reset_sent, kept, tries, vbus_absent = 1U, hold_open;
static uint32_t sense_at, wait_since, next_try_at, lost_since, low_since, hold_at;

/* A source pulls one CC line up through its Rp; in sink coding the Type-C
 * detector reports 00 for that line only when it sees vRa or an open. */
static uint8_t cc_source_present(uint8_t state)
{
    return (uint8_t)((state & 3U) != 0U || ((state >> 2U) & 3U) != 0U);
}
static uint8_t external_rd(void) { return (uint8_t)!RADIO_Config()->pd_internal_rd; }

/* Soft_Reset sent past ST's policy engine. The PE only honours a DPM
 * Soft_Reset request in SNK_READY; in SNK_WAIT_FOR_CAPABILITIES it holds the
 * request and hard-resets after 500 ms instead. A source that already holds
 * a contract (the MCU restarted under it) sends no capabilities of its own,
 * so the clean way back is to ask: Soft_Reset makes it answer Accept and then
 * Source_Capabilities, which the waiting PE handles as a normal attach, and
 * VBUS never moves (measured on REV1 against a 9 V contract). The frame is a
 * control message, type 0x0D, no data objects, MessageID 0 as R3.1 6.8.1
 * requires, sink/UFP, revision 3.0. ST's PHY refuses it while a frame is
 * being received or its DMA is busy; the transmit-complete callback is
 * borrowed for this one frame so the protocol layer never sees a
 * transmission it did not start. */
static uint8_t soft_reset_frame[2] = { 0x8DU, 0x00U };
static volatile uint8_t soft_reset_tx;
static void soft_reset_done(uint8_t port, uint32_t status)
{
    (void)port;
    soft_reset_tx = (uint8_t)(status == 0U ? 1U : 2U);
}
static uint8_t pd_send_soft_reset(void)
{
    void (*const saved)(uint8_t, uint32_t) = Ports[0].cbs.USBPD_HW_IF_TxCompleted;
    soft_reset_tx = 0U;
    Ports[0].cbs.USBPD_HW_IF_TxCompleted = soft_reset_done;
    if (USBPD_HW_IF_SendBuffer(0U, USBPD_SOPTYPE_SOP, soft_reset_frame, sizeof(soft_reset_frame)) == USBPD_OK) {
        /* About 0.6 ms on the wire; the UCPD interrupt outranks this context. */
        const uint16_t t0 = RADIO_PdMicros();
        while (!soft_reset_tx && (uint16_t)(RADIO_PdMicros() - t0) < 3000U) { }
    }
    Ports[0].cbs.USBPD_HW_IF_TxCompleted = saved;
    ++resyncs;
    pd_log_add(PDL_SOFTRESET, soft_reset_tx, 0U, RADIO_PdVbusMv());
    return (uint8_t)(soft_reset_tx == 1U);
}

static void hard_reset(uint8_t port, USBPD_PortPowerRole_TypeDef role, USBPD_HR_Status_TypeDef status)
{
    (void)port;
    pd_log_add(PDL_HARDRESET, (uint8_t)status, (uint16_t)role, RADIO_PdVbusMv());
    ++resets;
    source_count = 0U;
    /* The source is cycling VBUS; a Soft_Reset now would only be noise. */
    wait_since = HAL_GetTick();
    soft_reset_sent = 1U;
    power_inhibit();
}
static void cad_event(uint8_t port, USBPD_CAD_EVENT event, CCxPin_TypeDef cc)
{
    params.ActiveCCIs = cc & 3U;
    pd_log_add(PDL_CAD, (uint8_t)event, (uint16_t)cc, RADIO_PdVbusMv());
    if (event == USBPD_CAD_EVENT_ATTACHED || event == USBPD_CAD_EVENT_ATTEMC) {
        attached = 1U; contract = 0U; source_count = 0U;
        pending = retries = discovery_sent = pps_status_sent = identity_valid = status_valid = 0U;
        rearm_count = 0U;
        wait_since = HAL_GetTick();
        memset(&request, 0, sizeof(request));
        memset(&active_request, 0, sizeof(active_request));
        /* IsCableConnected performs the PE reset/entry transition itself.
         * ST's DPM examples do not precede it with StateMachine_Reset. */
        USBPD_PE_IsCableConnected(port, 1U);
    } else if (event == USBPD_CAD_EVENT_DETACHED || event == USBPD_CAD_EVENT_EMC) {
        power_inhibit(); attached = 0U; source_count = 0U;
        identity_valid = status_valid = 0U;
        USBPD_PE_IsCableConnected(port, 0U);
        USBPD_PE_StateMachine_Stop(port);
        params.PE_SwapOngoing = USBPD_FALSE;
        params.PE_Power = USBPD_POWER_NO;
    }
}
static void notify_pd(uint8_t port, USBPD_NotifyEventValue_TypeDef event)
{
    (void)port;
    last_event = (uint32_t)event; /* ST's own trace already logs it */
    if (event == USBPD_NOTIFY_POWER_SNK_READY ||
        event == USBPD_NOTIFY_POWER_EXPLICIT_CONTRACT ||
        event == USBPD_NOTIFY_STATE_SNK_READY) {
        /* CONFIG_1 reaches STATE_SNK_READY after a valid Request/Accept/PS_RDY
         * exchange but does not emit POWER_SNK_READY on every source. Any of
         * these notifications commits the RDO selected during capability
         * evaluation. Attachment and object position reject stale events. */
        if (!attached || !request.position) return;
        active_request = request;
        contract = 1U;
        pending = accepted = 0U;
        retries = 0U;
        pps_status_sent = 0U;
        renewed_at = HAL_GetTick();
        probe = RADIO_PD_PROBE_WON;
        tries = 0U;
    } else if (event == USBPD_NOTIFY_REQUEST_ACCEPTED) {
        accepted = 1U;
    } else if (event == USBPD_NOTIFY_REQUEST_REJECTED || event == USBPD_NOTIFY_REQUEST_WAIT ||
               event == USBPD_NOTIFY_REQUEST_ERROR || event == USBPD_NOTIFY_REQUEST_CANCELED ||
               event == USBPD_NOTIFY_REQUEST_DISCARDED) {
        pending = accepted = 0U;
        ++failures;
        attempted_at = HAL_GetTick();
    } else if (event == USBPD_NOTIFY_POWER_SNK_STOP) {
        power_inhibit();
    }
}
static void get_data(uint8_t port, USBPD_CORE_DataInfoType_TypeDef id, uint8_t *data, uint32_t *size)
{
    (void)port; *size = 0U;
    if (id == USBPD_CORE_DATATYPE_SNK_PDO) {
        /* Safe 5 V sink capability; no data communications on U18. */
        uint32_t pdo = (100UL << 10U) | (RADIO_Config()->pd_current_ma / 10U);
        memcpy(data, &pdo, 4U); *size = 4U;
    } else if (id == USBPD_CORE_DATATYPE_REQ_VOLTAGE) {
        uint16_t mv = request.mv;
        memcpy(data, &mv, sizeof(mv)); *size = sizeof(mv);
    }
}
static void set_data(uint8_t port, USBPD_CORE_DataInfoType_TypeDef id, uint8_t *data, uint32_t size)
{
    (void)port;
    if (id == USBPD_CORE_DATATYPE_RCV_SRC_PDO) {
        uint32_t old_count = source_count;
        uint8_t changed;
        if (!data || !size || size > sizeof(source_pdo) || size % 4U) {
            source_count = 0U;
            pending = accepted = 0U;
            if (contract) {
                contract = 0U;
                if (transmitting()) RADIO_Trip(RADIO_FAULT_VIN);
            }
            ++failures;
            return;
        }
        pd_log_add(PDL_CAPS, (uint8_t)(size / 4U), 0U, RADIO_PdVbusMv());
        changed = (uint8_t)(old_count != size / 4U || memcmp(source_pdo, data, size) != 0);
        source_count = 0U;
        memcpy(source_pdo, data, size); source_count = size / 4U;
        retries = rearm_count = 0U;
        if (changed && (contract || pending)) {
            /* Capabilities are authoritative for the next request. Keep a
             * running receiver alive while we renegotiate, but force RF off
             * immediately if the source changed during transmission. */
            contract = 0U;
            pending = accepted = 0U;
            if (transmitting()) RADIO_Trip(RADIO_FAULT_VIN);
        }
    } else if (id == USBPD_CORE_EXTENDED_CAPA && data && size >= sizeof(source_ext)) {
        memcpy(source_ext, data, sizeof(source_ext)); identity_valid = 1U;
    } else if (id == USBPD_CORE_PPS_STATUS && data && size == sizeof(source_status)) {
        memcpy(source_status, data, sizeof(source_status)); status_valid = 1U;
    }
}
/* What to ask for and how far above it the policy may go. Shutdown and a
 * disabled policy both pin the pair to the mandatory 5 V. */
static void pd_window(const radio_config_t *c, uint16_t *target, uint16_t *ceiling)
{
    if (!c->pd_enable || shutdown_requested) { *target = *ceiling = 5000U; return; }
    *target = c->pd_target_mv;
    *ceiling = c->pd_max_mv;
}
static void evaluate(uint8_t port, uint32_t *rdo, USBPD_CORE_PDO_Type_TypeDef *type)
{
    (void)port;
    const radio_config_t *c = RADIO_Config();
    uint16_t mv, ceiling;
    pd_window(c, &mv, &ceiling);
    /* Freeze voltage during RF. A changed source capability that cannot retain
     * the supply inhibits TX before its new request is sent. */
    const uint8_t tx = transmitting();
    if (contract && tx) mv = ceiling = active_request.mv;
    uint16_t ma = contract && tx ? active_request.ma : c->pd_current_ma;
    radio_pd_request_t next;
    if (!RADIO_PdChoose(source_pdo, source_count, mv, ceiling, ma,
                        c->pd_enable && !shutdown_requested, &next)) {
        *rdo = 0U; *type = USBPD_CORE_PDO_TYPE_FIXED;
        power_inhibit(); return;
    }
    if (contract && tx && (next.mv != active_request.mv || next.ma < active_request.ma))
        RADIO_Trip(RADIO_FAULT_VIN);
    pending = 1U;
    attempted_at = HAL_GetTick();
    request = next;
    pd_log_add(PDL_EVAL, request.position, request.mv, request.ma);
    *rdo = request.rdo;
    *type = request.pps ? USBPD_CORE_PDO_TYPE_APDO : USBPD_CORE_PDO_TYPE_FIXED;
}
static USBPD_FunctionalState power_ready(uint8_t port, USBPD_VSAFE_StatusTypeDef safe)
{
    return USBPD_PWR_IF_GetVBUSStatus(port, safe == USBPD_VSAFE_0V ? USBPD_PWR_BELOWVSAFE0V : USBPD_PWR_VSAFE5V)
        ? USBPD_ENABLE : USBPD_DISABLE;
}
static const USBPD_CAD_Callbacks cad_callbacks = { cad_event, wake_cad };
static const USBPD_PE_Callbacks callbacks = {
    .USBPD_PE_HardReset = hard_reset, .USBPD_PE_EvaluatPRSwap = reject,
    .USBPD_PE_Notify = notify_pd, .USBPD_PE_GetDataInfo = get_data,
    .USBPD_PE_SetDataInfo = set_data, .USBPD_PE_SNK_EvaluateCapabilities = evaluate,
    .USBPD_PE_WakeupCallback = wake_port, .USBPD_PE_EvaluateVconnSwap = reject,
    .USBPD_PE_VconnPwr = reject_vconn, .USBPD_PE_EnterErrorRecovery = recovery,
    .USBPD_PE_DataRoleSwap = reject, .USBPD_PE_IsPowerReady = power_ready,
    .USBPD_PE_RequestSetupNewPower = power_setup_done,
    .USBPD_PE_SRC_EvaluateRequest = reject_src_request,
    .USBPD_PE_ExtendedMessage = ignore_extended,
    .USBPD_PE_PowerRoleSwap = ignore_prswap,
};

/* Read the Type-C detector with the CC analog PHY off and again with it on.
 *
 * With R3/R4 fitted and PD INTERNAL RD off, CCENABLE is 00, the PHY is down
 * and both lines read vOPEN (or whatever the detector last latched) whatever
 * is plugged in. The only way to see the source is to ask for the internal
 * Rd, which parallels R3/R4 down to 2.55 k and is out of Type-C spec -- and
 * reads a source one current step low (a 3 A Rp as 1.5 A).
 *
 * A source debounces a CC change for tCCDebounce (100-200 ms) before it
 * re-evaluates the attachment, so holding the pulls for a millisecond or two
 * is invisible to it: this measurement cannot cost VBUS. Its result supplies
 * the orientation for the attach. Nothing here is persisted. The boot copy
 * waits in place; RADIO_PdTick splits the same thing over two ticks. */
static void pd_sense_done(void)
{
    cc_on_state = RADIO_PdCcState();
    RADIO_PdCcSenseEnd();
    pd_log_add(PDL_SENSE, cc_off_state, cc_on_state, RADIO_PdVbusMv());
    if (!attached)
        probe = cc_source_present(cc_on_state) ? (uint8_t)RADIO_PD_PROBE_NEEDED
                                               : (uint8_t)RADIO_PD_PROBE_NO_SOURCE;
}
static void pd_cc_sense(void)
{
    cc_off_state = cc_on_state = RADIO_PdCcState();
    if (!external_rd()) return; /* the PHY is already on */
    RADIO_PdCcSenseBegin();
    HAL_Delay(PD_SENSE_SETTLE_MS);
    pd_sense_done();
}

static void pd_attach(uint32_t now, uint16_t mv)
{
    /* R3/R4 already provide the correct 5.1 k Rd; the sense supplied the
     * orientation. Start the BMC path directly without keeping UCPD's second
     * 5.1 k pull enabled (2.55 k makes this source remove VBUS). */
    const CCxPin_TypeDef cc = (cc_on_state & 3U) ? CC1 : CC2;
    kept = (uint8_t)(mv > PD_VSAFE5V_MAX_MV);
    probe = RADIO_PD_PROBE_ACTIVE;
    soft_reset_sent = 0U;
    lost_since = 0U;
    pd_log_add(PDL_ATTACH, (uint8_t)cc, kept, mv);
    HW_SignalAttachement(0U, cc);
    cad_event(0U, USBPD_CAD_EVENT_ATTACHED, cc);
    wait_since = now;
}
static void pd_detach(uint8_t outcome)
{
    probe = outcome;
    pd_log_add(PDL_DETACH, outcome, 0U, RADIO_PdVbusMv());
    HW_SignalDetachment(0U);
    cad_event(0U, USBPD_CAD_EVENT_DETACHED, CCNONE);
}
static void pd_backoff(uint32_t now)
{
    const unsigned n = sizeof(pd_backoff_ms) / sizeof(pd_backoff_ms[0]);
    next_try_at = now + pd_backoff_ms[tries < n ? tries : n - 1U];
    if (tries < 255U) ++tries;
}

static void pd_classify(void)
{
    if (attached) { supply = contract ? RADIO_SUPPLY_USB_PD : RADIO_SUPPLY_USB_C; return; }
    if (cc_source_present(cc_on_state)) { supply = RADIO_SUPPLY_USB_C; return; }
    /* No source on CC, so the rail decides. A USB-C source cannot exceed
     * 5.5 V without a contract; anything above that is the DC jack. */
    uint16_t mv = RADIO_PdVbusMv();
    if (mv > PD_VSAFE5V_MAX_MV) supply = RADIO_SUPPLY_DC_JACK;
    else if (mv >= PD_VBUS_PRESENT_MV) supply = RADIO_SUPPLY_USB_5V;
    else supply = RADIO_SUPPLY_UNKNOWN;
}

void RADIO_PdInit(void)
{
    USBPD_Trace = pd_trace;
    USBPD_PE_SetTrace(pd_trace, 0U);
    if (initialized || pd_used) return; /* Do not leak/reinitialize a live core. */
    if (pd_log.magic != PD_LOG_MAGIC) { memset(&pd_log, 0, sizeof(pd_log)); pd_log.magic = PD_LOG_MAGIC; }
    {
        const uint32_t csr = MOD_GetResetCsr();
        pd_log_add(PDL_BOOT, (uint8_t)(csr >> 24U), 0U, RADIO_PdVbusMv());
    }
    if (!USBPD_PE_CheckLIB(_LIB_ID)) { failed = 1U; return; }
    USBPD_TIM_Init();
    if (!RADIO_PdTimerReady()) { failed = 1U; return; }
    params.PE_PowerRole = USBPD_PORTPOWERROLE_SNK;
    params.PE_SpecRevision = USBPD_SPECIFICATION_REV3;
    allocating = 1U;
    USBPD_CAD_StatusTypeDef cad = USBPD_CAD_Init(0U, &cad_callbacks, &settings, &params);
    USBPD_StatusTypeDef pe = cad == USBPD_CAD_OK ? USBPD_PE_Init(0U, &settings, &params, &callbacks) : USBPD_ERROR;
    allocating = 0U;
    if (cad != USBPD_CAD_OK || pe != USBPD_OK) { failed = 1U; RADIO_PdStop(); return; }
    params.DPM_Initialized = USBPD_TRUE;
    /* Sense BEFORE the CAD is allowed to run: RADIO_PdTick returns while
     * `initialized` is clear, so USBPD_CAD_Process never samples the CC lines
     * during the millisecond the internal Rd is applied. It used to run after,
     * and the CAD latched HW_Attachment out of that window -- then the PHY
     * went back off, the sink could no longer receive Source_Capabilities, and
     * the PE hard-reset for ever. */
    pd_cc_sense();
    /* Join at once, while a fresh source's capability repeats are still in
     * flight (this one gives up and hard-resets after about 3.5 s of silence),
     * or with a Soft_Reset on the next pass when the rail says a contract
     * survived the restart. Internal Rd remains off. */
    const uint16_t mv = RADIO_PdVbusMv();
    if (external_rd() && cc_source_present(cc_on_state) && mv >= PD_VBUS_PRESENT_MV)
        pd_attach(HAL_GetTick(), mv);
    vbus_absent = (uint8_t)(mv < PD_VBUS_PRESENT_MV);
    next_try_at = HAL_GetTick() + PD_SENSE_MS;
    initialized = 1U;
    /* With external R3/R4 CCENABLE=0 freezes the Type-C detector and the
     * supervisor owns attachment; the CAD runs only with the internal Rd. */
    if (!external_rd()) USBPD_CAD_PortEnable(0U, USBPD_CAD_ENABLE);
}

/* Bench back-feed recovery; see PD_BACKFEED_MV. Returns while it holds Q2. */
static uint8_t pd_backfeed(uint32_t now, uint16_t mv)
{
    if (mv >= PD_BACKFEED_MV || RADIO_State() == RADIO_OFF || shutdown_requested) {
        low_since = 0U;
        if (hold_open) { hold_open = 0U; RADIO_PdPowerHold(1U); pd_log_add(PDL_HOLD, 1U, 0U, mv); }
        return 0U;
    }
    if (hold_open) {
        if (now - hold_at >= PD_HOLD_OPEN_MS) {
            hold_open = 0U;
            RADIO_PdPowerHold(1U);
            low_since = now;
            pd_log_add(PDL_HOLD, 1U, 0U, mv);
        }
        return 1U;
    }
    if (!low_since) { low_since = now; return 0U; }
    if (now - low_since >= PD_BACKFEED_MS) {
        hold_open = 1U;
        hold_at = now;
        RADIO_PdPowerHold(0U);
        pd_log_add(PDL_HOLD, 0U, 0U, mv);
        return 1U;
    }
    return 0U;
}

static void pd_supervise(uint32_t now)
{
    const uint16_t mv = RADIO_PdVbusMv();
    if (sense_phase) {
        if (now - sense_at < PD_SENSE_SETTLE_MS) return;
        sense_phase = 0U;
        pd_sense_done();
        if (cc_source_present(cc_on_state) && mv >= PD_VBUS_PRESENT_MV) pd_attach(now, mv);
        else next_try_at = now + PD_SENSE_MS;
        return;
    }
    if (attached) {
        if (mv < PD_VBUS_LOST_MV) {
            /* Unplugged, or a hard reset cycling VBUS: let go, and look
             * again when it returns. */
            if (!lost_since) lost_since = now;
            else if (now - lost_since >= PD_VBUS_LOST_MS) {
                pd_detach(source_count ? RADIO_PD_PROBE_NONE : RADIO_PD_PROBE_BROWNOUT);
                vbus_absent = 1U; tries = 0U;
            }
            return;
        }
        lost_since = 0U;
        if (source_count) return;
        const uint32_t waited = now - wait_since;
        if (!soft_reset_sent && waited >= (kept ? PD_KEPT_SR_MS : PD_WAIT_SR_MS)) {
            soft_reset_sent = 1U;
            (void)pd_send_soft_reset();
        }
        if (waited >= PD_WAIT_MAX_MS) {
            pd_detach(RADIO_PD_PROBE_TIMEOUT);
            pd_backoff(now);
        }
        return;
    }
    if (mv < PD_VBUS_PRESENT_MV) {
        vbus_absent = 1U;
        (void)pd_backfeed(now, mv);
        return;
    }
    (void)pd_backfeed(now, mv);
    if (vbus_absent) {
        /* A plug, or VBUS back after a hard reset: the source is advertising
         * now, so look straight away. */
        vbus_absent = 0U;
        tries = 0U;
        next_try_at = now + PD_SENSE_SETTLE_MS;
    }
    if ((int32_t)(now - next_try_at) < 0) return;
    cc_off_state = RADIO_PdCcState();
    RADIO_PdCcSenseBegin();
    sense_phase = 1U;
    sense_at = now;
}

void RADIO_PdTick(void)
{
    if (!initialized) return;
    uint32_t now = HAL_GetTick();
    USBPD_PE_TimerCounter(0U);
    USBPD_PRL_TimerCounter(0U);
    {
        static uint8_t band = 255U;
        const uint16_t mv = RADIO_PdVbusMv();
        const uint8_t b = mv < 800U ? 0U : mv < PD_VBUS_LOST_MV ? 1U : mv < PD_VBUS_PRESENT_MV ? 2U :
                          mv <= PD_VSAFE5V_MAX_MV ? 3U : mv < 7500U ? 4U : mv < 10500U ? 5U : 6U;
        if (b != band) { band = b; pd_log_add(PDL_VBUS, b, 0U, mv); }
    }
    RADIO_PdPortPoll();
    (void)USBPD_CAD_Process();
    if (external_rd()) pd_supervise(now);
    else if (attached && !source_count && !soft_reset_sent && now - wait_since >= PD_WAIT_SR_MS) {
        /* The CAD attached; a contract that outlived an MCU reset still needs
         * asking for. Detaching is the CAD's business in this mode. */
        soft_reset_sent = 1U;
        (void)pd_send_soft_reset();
    }
    if (!attached) return;
    (void)USBPD_PE_StateMachine_SNK(0U);
    const radio_config_t *c = RADIO_Config();
    uint16_t target, ceiling;
    pd_window(c, &target, &ceiling);
    uint32_t key = ((uint32_t)target << 16U) | c->pd_current_ma;
    if (key != config_key || ceiling != ceiling_key) {
        config_key = key; ceiling_key = ceiling; retries = rearm_count = 0U;
    }
    if (retries >= 3U && rearm_count < PD_RETRY_REARM_MAX && now - attempted_at >= PD_RETRY_REARM_MS) {
        retries = 0U; ++rearm_count;
    }
    if (pending && now - attempted_at > 1500U) {
        pending = 0U; ++failures;
        if (accepted) { power_inhibit(); RADIO_Trip(RADIO_FAULT_VIN); }
    }
    if (!contract || pending) return;
    radio_pd_request_t desired;
    if (!RADIO_PdChoose(source_pdo, source_count, target, ceiling, c->pd_current_ma,
                         c->pd_enable && !shutdown_requested, &desired)) return;
    uint8_t change = desired.rdo != active_request.rdo;
    uint8_t renew = active_request.pps && now - renewed_at >= 7000U;
    uint8_t idle = !transmitting();
    if ((renew || (change && idle && retries < 3U)) && now - attempted_at >= 500U) {
        if (USBPD_PE_Request_DataMessage(0U, USBPD_DATAMSG_REQUEST, NULL) == USBPD_OK) {
            pending = 1U;
            attempted_at = now;
            ++retries;
        }
        return;
    }
    /* Optional discovery is rate-limited and never delays a voltage request.
     * Unsupported/timeout responses leave identity explicitly unknown. One
     * attempt each: a busy PE is not asked again every millisecond. */
    if (idle && !change && now - renewed_at >= 500U && params.PE_SpecRevision == USBPD_SPECIFICATION_REV3) {
        if (!discovery_sent) {
            discovery_sent = 1U;
            (void)USBPD_PE_Request_CtrlMessage(0U, USBPD_CONTROLMSG_GET_SRC_CAPEXT, USBPD_SOPTYPE_SOP);
        } else if (active_request.pps && !pps_status_sent && now - renewed_at >= 1000U) {
            pps_status_sent = 1U;
            (void)USBPD_PE_Request_CtrlMessage(0U, USBPD_CONTROLMSG_GET_PPS_STATUS, USBPD_SOPTYPE_SOP);
        }
    }
}
void RADIO_PdStop(void)
{
    initialized = 0U; attached = 0U; contract = 0U;
    sense_phase = 0U;
    RADIO_PdSetCcForce(0U); /* back to the stored termination before CR is written */
    NVIC_DisableIRQ(UCPD1_IRQn);
    UCPD1->IMR = 0U;
    /* Keep Rd presented: external R3/R4 always are; internal Rd stays enabled
     * so the source keeps VBUS until PB10 is released. */
    UCPD1->CR = UCPD_CR_ANAMODE | (RADIO_Config()->pd_internal_rd ? UCPD_CR_CCENABLE : 0U);
    DMA1_Channel5->CCR = 0U; DMA1_Channel6->CCR = 0U;
}
void UCPD1_IRQHandler(void) { USBPD_PORT0_IRQHandler(); RADIO_RefSofIsrReturn(); /* radio_clocks.c */ }
uint16_t RADIO_PdVoltage(void) { return contract ? active_request.mv : 0U; }
uint16_t RADIO_PdCurrent(void) { return contract ? active_request.ma : 0U; }
uint8_t RADIO_PdPowerStable(void)
{
    /* With PD policy enabled, a measured rail alone is insufficient: the
     * source must have advertised and acknowledged the exact contract. When
     * PD is disabled the same connector may be fed by the documented DC
     * input, so the ADC rail is the available authority. */
    if (!RADIO_Config()->pd_enable) return !pending;
    /* No USB-C source attached: the radio runs from the DC input (the CAD
     * cannot attach while ADC_VIN is above vSafe5V) and the rail checks in
     * RADIO_Service decide. An attached source must be negotiated. */
    if (!attached) return !pending;
    if (!source_count) return 0U;
    return contract && (!pending || request.rdo == active_request.rdo);
}
void RADIO_PdPrepareShutdown(void)
{
    shutdown_requested = 1U;
    uint32_t start = HAL_GetTick();
    while (contract && active_request.mv != 5000U && HAL_GetTick() - start < 1000U) {
        /* SysTick owns RADIO_PdTick() and wakes this WFI once per millisecond;
         * keeping a single PD execution context avoids re-entering the ST
         * state machine from the shutdown path. */
        __WFI();
        APP_WatchdogProgress();
    }
    RADIO_PdStop();
}
void RADIO_PdSnapshot(radio_pd_info_t *s)
{
    /* Derived outside the critical section: both read the policy tables and
     * the ADC rail, neither of which belongs under a disabled interrupt. */
    const uint8_t sup = (uint8_t)RADIO_PdSupply();
    const uint8_t why = (uint8_t)RADIO_PdWhyCode();
    uint16_t target, ceiling;
    pd_window(RADIO_Config(), &target, &ceiling);
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    memset(s, 0, sizeof(*s));
    s->supply = sup; s->why = why; s->probe = probe;
    s->target_mv = target; s->max_mv = ceiling;
    memcpy(s->pdo, source_pdo, sizeof(source_pdo));
    s->count = (uint8_t)source_count; s->attached = attached; s->contract = contract; s->pending = pending;
    s->mv = RADIO_PdVoltage(); s->ma = RADIO_PdCurrent(); s->pps = active_request.pps;
    s->requested_mv = request.mv; s->requested_ma = request.ma;
    s->cc = (uint8_t)params.ActiveCCIs; s->revision = (uint8_t)params.PE_SpecRevision + 1U;
    s->rp = (uint8_t)params.SNKExposedRP_AtAttach;
    s->last_event = last_event; s->failures = failures; s->resets = resets; s->resyncs = resyncs;
    s->identity_valid = identity_valid; s->status_valid = status_valid;
    s->vid = (uint16_t)(source_ext[0] | ((uint16_t)source_ext[1] << 8U));
    s->pid = (uint16_t)(source_ext[2] | ((uint16_t)source_ext[3] << 8U));
    s->xid = (uint32_t)source_ext[4] | ((uint32_t)source_ext[5] << 8U) |
             ((uint32_t)source_ext[6] << 16U) | ((uint32_t)source_ext[7] << 24U);
    s->fw_revision = source_ext[8]; s->hw_revision = source_ext[9]; s->source_watts = source_ext[23];
    uint16_t voltage = (uint16_t)(source_status[0] | ((uint16_t)source_status[1] << 8U));
    s->source_mv = voltage == 0xFFFFU ? 0U : (uint16_t)(voltage * 20U);
    s->source_ma = source_status[2] == 0xFFU ? 0U : (uint16_t)(source_status[2] * 50U);
    __set_PRIMASK(pm);
}
void RADIO_PdRawSnapshot(radio_pd_raw_t *raw)
{
    memset(raw, 0, sizeof(*raw));
    const uint32_t cr = UCPD1->CR, sr = UCPD1->SR;
    raw->cr = cr;
    raw->cfg1 = UCPD1->CFG1;
    raw->sr = sr;
    raw->imr = UCPD1->IMR;
    raw->cc_enable = (uint8_t)((cr & UCPD_CR_CCENABLE) >> UCPD_CR_CCENABLE_Pos);
    raw->anamode = (uint8_t)((cr & UCPD_CR_ANAMODE) ? 1U : 0U);
    raw->tcdis1 = (uint8_t)((cr & UCPD_CR_CC1TCDIS) ? 1U : 0U);
    raw->tcdis2 = (uint8_t)((cr & UCPD_CR_CC2TCDIS) ? 1U : 0U);
    raw->phy_rx_enabled = (uint8_t)((cr & UCPD_CR_PHYRXEN) ? 1U : 0U);
    raw->cc1_state = (uint8_t)((sr & UCPD_SR_TYPEC_VSTATE_CC1) >> UCPD_SR_TYPEC_VSTATE_CC1_Pos);
    raw->cc2_state = (uint8_t)((sr & UCPD_SR_TYPEC_VSTATE_CC2) >> UCPD_SR_TYPEC_VSTATE_CC2_Pos);
    raw->enabled = (uint8_t)((UCPD1->CFG1 & UCPD_CFG1_UCPDEN) ? 1U : 0U);
    raw->dead_battery_disabled = (uint8_t)((PWR->CR3 & PWR_CR3_UCPD_DBDIS) ? 1U : 0U);
    raw->active_cc = (uint8_t)params.ActiveCCIs;
    raw->internal_rd = RADIO_Config()->pd_internal_rd;
    raw->initialized = initialized;
    raw->failed = failed;
    raw->arena_used = (uint16_t)pd_used;
    raw->arena_size = (uint16_t)sizeof(pd_arena);
    raw->cc_off_state = cc_off_state;
    raw->cc_on_state = cc_on_state;
    raw->probe = probe;
    raw->cc_forced = RADIO_PdCcForced();
}

const char *RADIO_PdStatus(void)
{
    if (failed) return "PD INIT ERROR";
    /* The LCD font folds case, so the console's own token renders fine here. */
    if (!attached) return RADIO_PdSupplyName();
    if (!source_count) return "PD PROBING";
    if (!contract) return "PD WAIT";
    if (pending) return "PD CHANGING";
    if (retries >= 3U) return "PD REJECTED";
    return active_request.pps ? "PPS" : "PD FIXED";
}

radio_supply_t RADIO_PdSupply(void)
{
    pd_classify();
    return (radio_supply_t)supply;
}
radio_pd_probe_t RADIO_PdProbe(void) { return (radio_pd_probe_t)probe; }

/* Look again now: the console's `pd fix` and the menu's PD FIX. */
uint8_t RADIO_PdProbeStart(void)
{
    if (!initialized || attached || !external_rd()) return 0U;
    if (RADIO_PdVbusMv() < PD_VBUS_PRESENT_MV) return 0U;
    tries = 0U;
    next_try_at = HAL_GetTick();
    return 1U;
}

radio_pd_why_t RADIO_PdWhyCode(void)
{
    const radio_config_t *c = RADIO_Config();
    uint16_t target, ceiling;
    pd_window(c, &target, &ceiling);
    if (!c->pd_enable) return RADIO_PD_WHY_DISABLED;
    if (contract)
        return active_request.mv >= target ? RADIO_PD_WHY_OK : RADIO_PD_WHY_BELOW_TARGET;
    if (attached) {
        if (!source_count) return RADIO_PD_WHY_PROBING;
        if (pending) return RADIO_PD_WHY_NEGOTIATING;
        /* Distinguish "the source has nothing we can use" from "it refused
         * what we asked for": only the first is the charger's fault. */
        radio_pd_request_t best;
        if (!RADIO_PdChoose(source_pdo, source_count, target, ceiling,
                            c->pd_current_ma, 1U, &best) || best.mv < target)
            return RADIO_PD_WHY_NO_OFFER;
        return RADIO_PD_WHY_REJECTED;
    }
    if (probe == RADIO_PD_PROBE_BROWNOUT) return RADIO_PD_WHY_BROWNOUT;
    const radio_supply_t supply_now = RADIO_PdSupply();
    if (supply_now == RADIO_SUPPLY_DC_JACK) return RADIO_PD_WHY_DC;
    if (cc_source_present(cc_on_state))
        return probe == RADIO_PD_PROBE_TIMEOUT ? RADIO_PD_WHY_SILENT : RADIO_PD_WHY_NO_ATTACH;
    if (supply_now == RADIO_SUPPLY_UNKNOWN) return RADIO_PD_WHY_NO_SUPPLY;
    return RADIO_PD_WHY_NO_CC;
}

static const char *const supply_names[] = {
    "unknown", "dc-jack", "usb-5v", "usb-c", "usb-pd",
};
static const char *const why_names[] = {
    "ok", "below-target", "disabled", "dc", "no-cc", "no-supply", "silent",
    "probing", "brownout", "no-attach", "no-caps", "no-offer", "negotiating",
    "rejected",
};
static const char *const probe_names[] = {
    "none", "no-source", "seen", "active", "won", "brownout", "timeout",
};
static const char *const why_details[] = {
    "TARGET OK",
    "BELOW TARGET",
    "OFF",
    "DC",
    "NO CC",
    "NO VIN",
    "SILENT: REPLUG",
    "PROBING",
    "VBUS DROPPED",
    "NO ATTACH",
    "NO CAPS",
    "NO OFFER",
    "NEGOTIATING",
    "REJECTED",
};

const char *RADIO_PdSupplyName(void)
{
    const unsigned i = (unsigned)RADIO_PdSupply();
    return i < (sizeof(supply_names) / sizeof(supply_names[0])) ? supply_names[i] : "unknown";
}
const char *RADIO_PdWhyName(void)
{
    const unsigned i = (unsigned)RADIO_PdWhyCode();
    return i < (sizeof(why_names) / sizeof(why_names[0])) ? why_names[i] : "unknown";
}
const char *RADIO_PdProbeName(void)
{
    const unsigned i = (unsigned)probe;
    return i < (sizeof(probe_names) / sizeof(probe_names[0])) ? probe_names[i] : "none";
}
const char *RADIO_PdWhyDetail(void)
{
    const unsigned i = (unsigned)RADIO_PdWhyCode();
    if (i >= (sizeof(why_details) / sizeof(why_details[0]))) return "";
    return why_details[i];
}

