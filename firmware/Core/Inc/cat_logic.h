#ifndef CAT_LOGIC_H
#define CAT_LOGIC_H

/* Kenwood TS-480 CAT as QMX speaks it (QMX CAT programming manual,
 * firmware 1_02_006), so WSJT-X, JS8Call and the like drive the radio with
 * Hamlib's "QRPLabs QMX" rig (model 2057) exactly as they drive a QMX.
 *
 * Commands are two letters, parameters, and a semicolon; there is never a CR
 * or LF in one. A CR or LF on a CAT port is a person at a terminal (or a
 * console script): the port becomes the console (CAT_PUSH_TERMINAL), as QMX
 * enters its terminal applications on a CR.
 *
 * What Hamlib's QMX backend sends (ts480.c qrplabs_qmx_caps, kenwood.c):
 * ID and IF and VN on open; FA/FB, MD, TX/RX, FR/FT for VFO and split; it
 * maps PKTUSB to MD9 and PKTLSB to MD6 (the FSK modes QMX uses for DIGI)
 * and reads them back the same way. Everything else here is QMX's set for
 * other software: TQ, KS, AG, FW, SM, PC, SW, SP, RT/RC, PS, AI, OM, Q1, Q3,
 * Q9, QB, QC, SS, and the menu manager MM/ML.
 *
 * No hardware here: the radio is reached through cat_ops_t, so the host
 * suite cat_logic_test runs this file as is. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The longest command kept, without its ';'. MM paths are the long ones;
 * "MMAUDIO|USB AUDIO POLICY=DIGI CLEAN" is 35. */
#define CAT_CMD_MAX   48U
/* Room every reply but MM's and ML's needs (IF is 38 bytes). */
#define CAT_REPLY_MIN 40U

/* Kenwood MD digits. 6 and 9 are QMX's DIGI (Hamlib PKTLSB/PKTUSB): here
 * LSB/USB with USB audio as the clean data source. */
enum {
    CAT_MD_LSB = 1, CAT_MD_USB = 2, CAT_MD_CW = 3, CAT_MD_FM = 4, CAT_MD_AM = 5,
    CAT_MD_LSB_DATA = 6, CAT_MD_CWR = 7, CAT_MD_USB_DATA = 9
};

/* Numeric state the commands read and write; get returns CAT_PARAM_NONE when
 * the radio has no value (the command then answers ?;). */
typedef enum {
    CAT_P_WPM = 0,     /* KS: keyer speed, WPM */
    CAT_P_VOLUME,      /* AG: AF gain 0..255 */
    CAT_P_IQ,          /* Q9: 1 = raw I/Q to the sound card */
    CAT_P_VOX,         /* Q3: 1 = USB audio keys TX without CAT */
    CAT_P_SOURCE,      /* SS: SSB TX source, 0 USB, 1 two-tone, 2 microphone */
    CAT_P_SIDEBAND,    /* Q1: 1 = LSB, 0 = USB */
    CAT_P_TIMEOUT_ON,  /* QB: CAT transmit timeout enabled */
    CAT_P_TIMEOUT_S,   /* QC: CAT transmit timeout, seconds */
    CAT_P_POWER_DW,    /* PC: output power, tenths of a watt (read only) */
    CAT_P_SWR_X100,    /* SW: SWR x 100 while transmitting (read only) */
    CAT_P_SMETER_DB,   /* SM: signal level, dB (read only) */
    CAT_P_FILTER_HZ,   /* FW: receive filter width, Hz (read only) */
    CAT_P_COUNT
} cat_param_t;
#define CAT_PARAM_NONE INT32_MIN

typedef struct {
    /* Dial frequency, Hz: a tune just asked for, else the carrier. */
    uint32_t (*get_freq)(void);
    /* 0 when refused (out of range, or a page that owns the carrier). */
    uint8_t  (*set_freq)(uint32_t hz);
    uint8_t  (*get_mode)(void);           /* CAT_MD_* */
    uint8_t  (*set_mode)(uint8_t md);     /* 0 for a mode the radio has not */
    /* 1 while transmitting or CAT transmit is on. */
    uint8_t  (*get_tx)(void);
    void     (*set_tx)(uint8_t on);
    int32_t  (*get)(cat_param_t id);
    uint8_t  (*set)(cat_param_t id, int32_t value);
    /* MM: `arg` is everything after "MM" (path, path=value or path?), NUL
     * terminated. Writes the reply body (without "MM" and ';', NUL
     * terminated, at most cap - 1 characters) and returns 1 with *has_reply
     * set when there is one (a get or a query), 1 with it clear for a set, 0
     * for an error. NULL: no menu manager. */
    uint8_t  (*menu)(const char *arg, char *body, uint16_t cap, uint8_t *has_reply);
    /* ML: the values of list type `list`, '|' separated, NUL terminated. 0 if
     * none. */
    uint8_t  (*menu_list)(uint32_t list, char *body, uint16_t cap);
    /* VN reply body, QMX style ("1_00_021QMX"). */
    const char *version;
} cat_ops_t;

typedef struct {
    char     cmd[CAT_CMD_MAX + 1U]; /* NUL terminated */
    uint8_t  len;
    uint8_t  overflow;  /* the command outgrew cmd; answered ?; at its ';' */
    uint8_t  ai;        /* AI mode as set (no unsolicited replies are sent) */
    uint32_t vfo_b;     /* FB as set; 0 reads back as FA */
} cat_parser_t;

typedef enum {
    CAT_PUSH_NONE = 0,  /* byte taken, nothing to send */
    CAT_PUSH_DONE,      /* a command completed; *reply_len bytes to send (may be 0) */
    CAT_PUSH_TERMINAL   /* CR or LF: hand cmd[0..len) and this byte to the console */
} cat_push_t;

void CAT_ParserReset(cat_parser_t *p);
/* One byte from the host; a reply is written to `reply` (cap bytes, at
 * least CAT_REPLY_MIN; longer MM/ML replies are cut to fit). */
cat_push_t CAT_ParserPush(cat_parser_t *p, const cat_ops_t *ops, char c,
                          char *reply, uint16_t cap, uint16_t *reply_len);
/* Run one command (without ';', NUL terminated at len), e.g.
 * "FA14074000". Returns the reply length. */
uint16_t CAT_Execute(cat_parser_t *p, const cat_ops_t *ops, const char *cmd, uint8_t len,
                     char *reply, uint16_t cap);

#ifdef __cplusplus
}
#endif

#endif
