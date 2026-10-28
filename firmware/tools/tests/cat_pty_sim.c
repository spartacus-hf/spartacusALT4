/* The radio's CAT parser (Core/Src/cat_logic.c) on a pseudo-terminal, with a
 * fake radio behind it, so Hamlib and WSJT-X can be pointed at it on the PC:
 *
 *   cc -std=c11 -I Core/Inc tools/tests/cat_pty_sim.c Core/Src/cat_logic.c -o cat_pty_sim
 *   ./cat_pty_sim /tmp/cat_pty.name /tmp/cat_pty.log &
 *   rigctl -m 2057 -r $(cat /tmp/cat_pty.name) f F 7074000 M PKTUSB 0 T 1 T 0
 *
 * Every command and reply goes to the log, one line each ("-> FA;", "<- ...").
 * Not part of the host suites: it serves until killed. */
#define _DEFAULT_SOURCE /* cfmakeraw, usleep */
#define _XOPEN_SOURCE 600
#include "cat_logic.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static uint32_t freq = 14074000U;
static uint8_t mode = CAT_MD_USB;
static uint8_t tx;
static int32_t params[CAT_P_COUNT];

static uint32_t get_freq(void) { return freq; }
static uint8_t set_freq(uint32_t hz) { if (hz < 1000000U || hz > 28333333U) return 0U; freq = hz; return 1U; }
static uint8_t get_mode(void) { return mode; }
static uint8_t set_mode(uint8_t md)
{
    if (md == 0U || md == 8U || md > 9U) return 0U;
    mode = (md == CAT_MD_CWR) ? CAT_MD_CW : md;
    return 1U;
}
static uint8_t get_tx(void) { return tx; }
static void set_tx(uint8_t on) { tx = on; }
static int32_t get(cat_param_t id) { return params[id]; }
static uint8_t set(cat_param_t id, int32_t v) { if (id >= CAT_P_POWER_DW) return 0U; params[id] = v; return 1U; }

static const cat_ops_t ops = { get_freq, set_freq, get_mode, set_mode, get_tx, set_tx, get, set,
                               NULL, NULL, "8_10_000SPARTACUS" };

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s NAMEFILE LOGFILE\n", argv[0]); return 2; }
    params[CAT_P_WPM] = 20; params[CAT_P_VOLUME] = 128; params[CAT_P_TIMEOUT_ON] = 1;
    params[CAT_P_TIMEOUT_S] = 120; params[CAT_P_FILTER_HZ] = 3200; params[CAT_P_SWR_X100] = -1;
    const int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) || unlockpt(m)) { perror("pty"); return 1; }
    const char *name = ptsname(m);
    struct termios t;
    const int s = open(name, O_RDWR | O_NOCTTY); /* held open: keeps the master alive between clients */
    tcgetattr(s, &t); cfmakeraw(&t); tcsetattr(s, TCSANOW, &t);
    FILE *nf = fopen(argv[1], "w"); fprintf(nf, "%s\n", name); fclose(nf);
    FILE *log = fopen(argv[2], "a"); setvbuf(log, NULL, _IOLBF, 0);
    cat_parser_t p;
    CAT_ParserReset(&p);
    char seen[CAT_CMD_MAX + 2U];
    unsigned seen_len = 0U;
    for (;;) {
        char c;
        if (read(m, &c, 1) != 1) { usleep(1000); continue; }
        if (seen_len < sizeof(seen) - 1U) seen[seen_len++] = c;
        char r[96];
        uint16_t rl = 0U;
        const cat_push_t k = CAT_ParserPush(&p, &ops, c, r, sizeof(r), &rl);
        if (k == CAT_PUSH_TERMINAL) { fprintf(log, "-- line ending: terminal\n"); CAT_ParserReset(&p); seen_len = 0U; continue; }
        if (k != CAT_PUSH_DONE) continue;
        fprintf(log, "-> %.*s (freq=%u md=%u tx=%u)\n", (int)seen_len, seen, (unsigned)freq, mode, tx);
        seen_len = 0U;
        if (rl) { fprintf(log, "   <- %.*s\n", (int)rl, r); (void)!write(m, r, rl); }
    }
}
