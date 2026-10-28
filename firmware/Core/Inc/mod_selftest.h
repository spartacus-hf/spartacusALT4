#ifndef MOD_SELFTEST_H
#define MOD_SELFTEST_H

#include <stdint.h>
#include "mod_selftest_logic.h"

/* REV1 built-in self test.
 *
 * A bounded, non-blocking sequence serviced from the main loop, in the shape
 * of RADIO_SweepService. It reserves the hardware through RADIO_CalStart's
 * RADIO_CAL_SELFTEST mode, so it inherits that function's fault, emergency
 * latch and audio-source interlocks, and RADIO_CalStop abandons it cleanly.
 *
 * It never transmits. No call in this module reaches BOARD_WriteRf,
 * BOARD_PermitRf(1), BOARD_SetAux, HRTIM output enable for the TX pair, or a
 * write to radio_config_t.qualified_bands or .confirmed -- elf_policy_check
 * asserts that from the linked image, not from this comment.
 *
 * Groups that drive the RF path (FILTERS, RX, ANALOG) re-check
 * RADIO_ConfigRxBlock and skip themselves on a board whose assembly has not
 * been declared, so the supply, clock, control and storage groups still run
 * on a fresh install. */

/* Begin a run over the ST_GROUP_* bits in `mask`. Goes through RADIO_CalStart,
 * so it returns 0 when the radio will not release the hardware (a latched
 * fault, realtime audio, a local audio source, an active calibration).
 * ST_GROUP_ANALOG additionally needs MOD_SelfTestArmAnalog() beforehand. */
uint8_t MOD_SelfTestStart(uint8_t mask);
/* Stop a run and release the interlock (wraps RADIO_CalStop). */
void MOD_SelfTestStop(void);

/* Driven by radio.c only. Begin initialises a run whose mask MOD_SelfTestStart
 * has already latched; Service returns 1 while the run continues; Abort drops
 * a run whose interlock is being torn down under it. */
void    MOD_SelfTestBegin(uint32_t now);
uint8_t MOD_SelfTestService(uint32_t now);
void    MOD_SelfTestAbort(void);

uint8_t MOD_SelfTestActive(void);
uint8_t MOD_SelfTestMask(void);        /* groups the last run covered */
uint8_t MOD_SelfTestProgress(void);    /* 0..100 */
uint8_t MOD_SelfTestOverall(void);     /* MOD_SelfTestRollup of the run */
uint32_t MOD_SelfTestElapsedMs(void);
/* The row for a test id, or NULL. Rows for groups outside the run stay
 * ST_PENDING and are not reported. */
const selftest_result_t *MOD_SelfTestResult(uint8_t id);
/* A row's detail: its string, or its card text read into buf ("" with no
 * card) -- a card read each call, for a listing or an export. A page drawn
 * every frame takes MOD_SelfTestDetailShown (one row: the card text cache). */
const char *MOD_SelfTestDetail(uint8_t id, char *buf, uint16_t cap);
const char *MOD_SelfTestDetailShown(uint8_t id);
/* The id of the step currently running, or SELFTEST_COUNT when idle. */
uint8_t MOD_SelfTestCurrent(void);
/* Free-text line for the step in progress, written into the caller's buffer
 * so the engine holds no string storage of its own. */
void MOD_SelfTestStatus(char *out, uint16_t cap);
/* The window a row was judged against, with the two configuration-dependent
 * windows substituted. */
void MOD_SelfTestRowLimits(uint8_t id, int32_t *low, int32_t *high);

/* ---- ANALOG arming ----
 * The energised group applies PIN diode bias and drives the envelope DACs
 * into the PA drains. REV1's own bring-up order wants the JP10/JP11 drain
 * jumpers pulled first, so it never runs unless the operator has armed it
 * since the last run. Arming expires with the run. */
void    MOD_SelfTestArmAnalog(void);
uint8_t MOD_SelfTestAnalogArmed(void);

/* ---- Offered measurements ----
 * A row with .offers set carries a value some setting could take. Nothing is
 * applied until this is called, and applying still needs a settings save.
 * Returns 0 when the row offers nothing or the value is out of range. */
uint8_t MOD_SelfTestApply(uint8_t id);
/* One line describing what MOD_SelfTestApply(id) would change, or NULL. */
const char *MOD_SelfTestOffer(uint8_t id);

/* CONTROLS group: the panel poll feeds observed activity in. Called from the
 * UI button/encoder path while a run is waiting on the operator. */
void MOD_SelfTestNoteButton(uint8_t index);       /* 0..4, PD2..PD6 */
void MOD_SelfTestNoteEncoder(int8_t steps, uint8_t click, uint8_t long_click);

/* Write the finished run to EXPORTS/STnnn.CSV. Main loop only; returns 0
 * without a mounted card, in USB disk mode, or with no completed run. `name`
 * is a 24-byte buffer that receives the path. */
uint8_t MOD_SelfTestExport(char *name);
/* Path of the last run's export, or NULL when nothing was written. */
const char *MOD_SelfTestExportName(void);

#endif
