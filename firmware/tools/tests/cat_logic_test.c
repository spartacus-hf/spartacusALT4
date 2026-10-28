/* Host suite for Core/Src/cat_logic.c: the Kenwood TS-480 / QMX CAT set the
 * radio answers on its USB serial ports, against a fake radio. Checked here:
 * the replies Hamlib's QRPLabs QMX backend parses by position (IF is 38
 * bytes, FA 11 digits), the DIGI modes 6/9, PTT, the refusals, and the CR/LF
 * switch to the terminal. tools/cat_pty_sim.py runs the same file under the
 * real Hamlib. */
#include "cat_logic.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t freq = 14074000U;
static uint8_t mode = CAT_MD_USB_DATA;
static uint8_t tx;
static int32_t params[CAT_P_COUNT];
static char last_menu[CAT_CMD_MAX + 1U];

static uint32_t get_freq(void) { return freq; }
static uint8_t set_freq(uint32_t hz)
{
    if (hz < 1000000U || hz > 28333333U) return 0U;
    freq = hz;
    return 1U;
}
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
static uint8_t set(cat_param_t id, int32_t v)
{
    if (id >= CAT_P_POWER_DW) return 0U; /* meters are read only */
    if (id == CAT_P_SOURCE && v == 1) return 0U;
    params[id] = v;
    return 1U;
}
static uint8_t menu(const char *arg, char *body, uint16_t cap, uint8_t *has_reply)
{
    snprintf(last_menu, sizeof(last_menu), "%s", arg);
    if (strchr(arg, '=')) { *has_reply = 0U; return strstr(arg, "BAD") ? 0U : 1U; }
    snprintf(body, cap, "%s", strchr(arg, '?') ? "5|7|USB RAW IQ" : "ON");
    *has_reply = 1U;
    return 1U;
}
static uint8_t menu_list(uint32_t list, char *body, uint16_t cap)
{
    if (list != 7U) return 0U;
    snprintf(body, cap, "OFF|ON");
    return 1U;
}

static const cat_ops_t ops = {
    get_freq, set_freq, get_mode, set_mode, get_tx, set_tx, get, set, menu, menu_list,
    "8_10_000SPARTACUS",
};

/* Feed `in` and return everything replied, concatenated. */
static const char *feed(cat_parser_t *p, const char *in)
{
    static char out[1024];
    char r[160];
    uint16_t n = 0U, rl = 0U;
    out[0] = '\0';
    for (const char *c = in; *c; ++c) {
        const cat_push_t k = CAT_ParserPush(p, &ops, *c, r, (uint16_t)sizeof(r), &rl);
        assert(k != CAT_PUSH_TERMINAL);
        if (k == CAT_PUSH_DONE) { memcpy(&out[n], r, rl); n = (uint16_t)(n + rl); out[n] = '\0'; }
    }
    return out;
}

static void test_identity(cat_parser_t *p)
{
    assert(!strcmp(feed(p, "ID;"), "ID020;"));
    assert(!strcmp(feed(p, "VN;"), "VN8_10_000SPARTACUS;"));
    assert(!strcmp(feed(p, "OM;"), "OMSP;"));
    assert(!strcmp(feed(p, "PS;"), "PS1;"));
    assert(!strcmp(feed(p, "PS0;"), ""));          /* never switches off */
    assert(!strcmp(feed(p, "id;"), "ID020;"));     /* lower case accepted */
    assert(!strcmp(feed(p, "ZZ;"), "?;"));
    assert(!strcmp(feed(p, "X;"), "?;"));
    assert(!strcmp(feed(p, ";;"), ""));
}

static void test_if(cat_parser_t *p)
{
    freq = 7074000U; mode = CAT_MD_USB_DATA; tx = 0U;
    const char *r = feed(p, "IF;");
    assert(strlen(r) == 38U);
    assert(!strcmp(r, "IF00007074000     +000000000090000000;"));
    tx = 1U;
    r = feed(p, "IF;");
    assert(r[28] == '1' && r[29] == '9');
    tx = 0U;
}

static void test_freq(cat_parser_t *p)
{
    assert(!strcmp(feed(p, "FA14074000;"), ""));
    assert(freq == 14074000U);
    assert(!strcmp(feed(p, "FA;"), "FA00014074000;"));
    assert(!strcmp(feed(p, "FA00007074000;"), ""));
    assert(freq == 7074000U);
    assert(!strcmp(feed(p, "FA50313000;"), "?;"));   /* out of range: refused */
    assert(freq == 7074000U);
    assert(!strcmp(feed(p, "FA7O74000;"), "?;"));    /* not a number */
    assert(!strcmp(feed(p, "FA123456789012;"), "?;"));
    /* FB is held, never tuned; until set it reads as FA. */
    assert(!strcmp(feed(p, "FB;"), "FB00007074000;"));
    assert(!strcmp(feed(p, "FB7016000;"), ""));
    assert(!strcmp(feed(p, "FB;"), "FB00007016000;"));
    assert(freq == 7074000U);
}

static void test_mode(cat_parser_t *p)
{
    assert(!strcmp(feed(p, "MD9;"), ""));
    assert(!strcmp(feed(p, "MD;"), "MD9;"));
    assert(!strcmp(feed(p, "MD7;"), ""));
    assert(!strcmp(feed(p, "MD;"), "MD3;"));
    assert(!strcmp(feed(p, "MD8;"), "?;"));
    assert(!strcmp(feed(p, "MD0;"), "?;"));
    assert(!strcmp(feed(p, "MD22;"), "?;"));
    assert(!strcmp(feed(p, "MD6;MD;"), "MD6;"));   /* strung together */
}

static void test_ptt(cat_parser_t *p)
{
    assert(!strcmp(feed(p, "TX;"), "") && tx == 1U);
    assert(!strcmp(feed(p, "TQ;"), "TQ1;"));
    assert(!strcmp(feed(p, "RX;"), "") && tx == 0U);
    assert(!strcmp(feed(p, "TX1;"), "") && tx == 1U);
    assert(!strcmp(feed(p, "TQ0;"), "") && tx == 0U);
    assert(!strcmp(feed(p, "TQ1;"), "") && tx == 1U);
    assert(!strcmp(feed(p, "TX5;"), "?;") && tx == 1U);
    assert(!strcmp(feed(p, "RX1;"), "?;"));
    assert(!strcmp(feed(p, "RX;"), "") && tx == 0U);
}

static void test_vfo_split(cat_parser_t *p)
{
    assert(!strcmp(feed(p, "FR;FT;SP;RT;XT;"), "FR0;FT0;SP0;RT0;XT0;"));
    assert(!strcmp(feed(p, "FR0;FT0;SP0;RC;"), ""));
    assert(!strcmp(feed(p, "FT1;"), "?;"));
    assert(!strcmp(feed(p, "SP1;"), "?;"));
    assert(!strcmp(feed(p, "AI;"), "AI0;"));
    assert(!strcmp(feed(p, "AI2;AI;"), "AI2;"));
    assert(!strcmp(feed(p, "AI0;"), ""));
}

static void test_params(cat_parser_t *p)
{
    params[CAT_P_WPM] = 18;
    assert(!strcmp(feed(p, "KS;"), "KS018;"));
    assert(!strcmp(feed(p, "KS025;KS;"), "KS025;"));
    params[CAT_P_VOLUME] = 91;
    assert(!strcmp(feed(p, "AG;"), "AG0091;"));
    assert(!strcmp(feed(p, "AG0;"), "AG0091;"));
    assert(!strcmp(feed(p, "AG0128;"), "") && params[CAT_P_VOLUME] == 128);
    assert(!strcmp(feed(p, "AG64;"), "") && params[CAT_P_VOLUME] == 64);
    assert(!strcmp(feed(p, "AG0300;"), "?;"));
    assert(!strcmp(feed(p, "Q91;Q9;"), "Q91;"));
    assert(!strcmp(feed(p, "Q90;Q9;"), "Q90;"));
    assert(!strcmp(feed(p, "Q30;Q3;"), "Q30;"));
    assert(!strcmp(feed(p, "SS0;SS;"), "SS0;"));
    assert(!strcmp(feed(p, "SS1;"), "?;"));
    assert(!strcmp(feed(p, "QC120;QC;"), "QC120;"));
    params[CAT_P_FILTER_HZ] = 3200;
    assert(!strcmp(feed(p, "FW;"), "FW3200;"));
    params[CAT_P_FILTER_HZ] = 300;
    assert(!strcmp(feed(p, "FW;"), "FW0300;"));
    params[CAT_P_SMETER_DB] = 42;
    assert(!strcmp(feed(p, "SM;"), "SM00042;"));
    assert(!strcmp(feed(p, "SM0;"), "SM00042;"));
    params[CAT_P_POWER_DW] = 45;
    assert(!strcmp(feed(p, "PC;"), "PC45;"));
    params[CAT_P_SWR_X100] = -1;
    assert(!strcmp(feed(p, "SW;"), "SW;"));      /* in receive, as QMX */
    params[CAT_P_SWR_X100] = 121;
    assert(!strcmp(feed(p, "SW;"), "SW121;"));
    params[CAT_P_POWER_DW] = CAT_PARAM_NONE;
    assert(!strcmp(feed(p, "PC;"), "?;"));
    assert(!strcmp(feed(p, "PC50;"), "?;"));     /* no power setting by CAT */
}

static void test_menu(cat_parser_t *p)
{
    assert(!strcmp(feed(p, "MMAUDIO|USB RAW IQ;"), "MMON;"));
    assert(!strcmp(last_menu, "AUDIO|USB RAW IQ"));
    assert(!strcmp(feed(p, "MMAUDIO|USB RAW IQ?;"), "MM5|7|USB RAW IQ;"));
    assert(!strcmp(feed(p, "MMAUDIO|USB RAW IQ=OFF;"), ""));
    assert(!strcmp(last_menu, "AUDIO|USB RAW IQ=OFF"));
    assert(!strcmp(feed(p, "MMAUDIO|BAD=1;"), "?;"));
    assert(!strcmp(feed(p, "ML7;"), "MLOFF|ON;"));
    assert(!strcmp(feed(p, "ML3;"), "?;"));
    assert(!strcmp(feed(p, "MM;"), "?;"));
}

static void test_overflow(cat_parser_t *p)
{
    char longcmd[CAT_CMD_MAX + 20U];
    memset(longcmd, 'A', sizeof(longcmd));
    longcmd[0] = 'M'; longcmd[1] = 'M';
    longcmd[sizeof(longcmd) - 2U] = ';';
    longcmd[sizeof(longcmd) - 1U] = '\0';
    assert(!strcmp(feed(p, longcmd), "?;"));
    assert(!strcmp(feed(p, "ID;"), "ID020;")); /* and the parser recovered */
    /* Control bytes and a leading space are dropped. */
    assert(!strcmp(feed(p, " \x01ID;"), "ID020;"));
}

static void test_terminal(cat_parser_t *p)
{
    char r[160];
    uint16_t rl = 0U;
    const char *line = "status";
    for (const char *c = line; *c; ++c) assert(CAT_ParserPush(p, &ops, *c, r, sizeof(r), &rl) == CAT_PUSH_NONE);
    /* A line ending: what was typed so far is the console's. */
    assert(CAT_ParserPush(p, &ops, '\n', r, sizeof(r), &rl) == CAT_PUSH_TERMINAL);
    assert(p->len == 6U && !memcmp(p->cmd, "status", 6U));
    CAT_ParserReset(p);
    assert(CAT_ParserPush(p, &ops, '\r', r, sizeof(r), &rl) == CAT_PUSH_TERMINAL && p->len == 0U);
    CAT_ParserReset(p);
}

int main(void)
{
    cat_parser_t p;
    CAT_ParserReset(&p);
    test_identity(&p);
    test_if(&p);
    test_freq(&p);
    test_mode(&p);
    test_ptt(&p);
    test_vfo_split(&p);
    test_params(&p);
    test_menu(&p);
    test_overflow(&p);
    test_terminal(&p);
    printf("cat_logic: PASS\n");
    return 0;
}
