#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
#include "cat_logic.h"

#include <stddef.h>

#define KEY(a, b) (((uint16_t)(uint8_t)(a) << 8) | (uint16_t)(uint8_t)(b))

static char upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }
static uint8_t is_digit(char c) { return (uint8_t)(c >= '0' && c <= '9'); }

static uint16_t put_s(char *b, uint16_t n, uint16_t cap, const char *s)
{
    while (s && *s && (uint16_t)(n + 1U) < cap) b[n++] = *s++;
    return n;
}

/* Decimal, zero-padded to `width` digits. */
static uint16_t put_u(char *b, uint16_t n, uint16_t cap, uint32_t v, uint8_t width)
{
    char t[12];
    uint8_t k = 0U;
    do { t[k++] = (char)('0' + (v % 10U)); v /= 10U; } while (v && (k < 10U));
    while ((k < width) && (k < sizeof(t))) t[k++] = '0';
    while (k && ((uint16_t)(n + 1U) < cap)) b[n++] = t[--k];
    return n;
}

/* `len` digits and nothing else, at most 11 (an FA frequency), into *out. */
static uint8_t parse_u(const char *s, uint8_t len, uint32_t *out)
{
    if ((len == 0U) || (len > 11U)) return 0U;
    uint64_t v = 0U;
    for (uint8_t i = 0U; i < len; ++i) {
        if (!is_digit(s[i])) return 0U;
        v = v * 10U + (uint64_t)(s[i] - '0');
    }
    if (v > 0xFFFFFFFFULL) return 0U;
    *out = (uint32_t)v;
    return 1U;
}

static uint16_t reply_err(char *r, uint16_t cap) { return put_s(r, 0U, cap, "?;"); }

/* "XX<value>;" with the value zero-padded to `width`. */
static uint16_t reply_u(char *r, uint16_t cap, const char *head, uint32_t v, uint8_t width)
{
    uint16_t n = put_s(r, 0U, cap, head);
    n = put_u(r, n, cap, v, width);
    return put_s(r, n, cap, ";");
}

/* TS-480 IF: frequency, 5 spaces, RIT offset, RIT, XIT, memory bank and
 * channel, TX/RX, mode, RX VFO, scan, split, tone, tone number, P15. 38
 * bytes with "IF" and ';' -- Hamlib reads it by position. */
static uint16_t reply_if(const cat_ops_t *ops, char *r, uint16_t cap)
{
    uint16_t n = put_s(r, 0U, cap, "IF");
    n = put_u(r, n, cap, ops->get_freq(), 11U);
    n = put_s(r, n, cap, "     +000000000"); /* RIT +0000, RIT 0, XIT 0, memory 0 00 */
    n = put_u(r, n, cap, ops->get_tx() ? 1U : 0U, 1U);
    n = put_u(r, n, cap, ops->get_mode(), 1U);
    return put_s(r, n, cap, "0000000;");
}

/* A numeric parameter: get with no argument, set with one. */
static uint16_t param_cmd(const cat_ops_t *ops, cat_param_t id, const char *head,
                          const char *arg, uint8_t alen, uint8_t width, char *r, uint16_t cap)
{
    if (alen == 0U) {
        const int32_t v = ops->get ? ops->get(id) : CAT_PARAM_NONE;
        if (v == CAT_PARAM_NONE || v < 0) return reply_err(r, cap);
        return reply_u(r, cap, head, (uint32_t)v, width);
    }
    uint32_t v = 0U;
    if (!parse_u(arg, alen, &v) || (v > 0x7FFFFFFFUL) || !ops->set || !ops->set(id, (int32_t)v)) return reply_err(r, cap);
    return 0U;
}

/* A read-only one; the argument a Kenwood get may carry (SM0) is ignored. */
static uint16_t meter_cmd(const cat_ops_t *ops, cat_param_t id, const char *head, uint8_t width,
                          char *r, uint16_t cap)
{
    const int32_t v = ops->get ? ops->get(id) : CAT_PARAM_NONE;
    if (v == CAT_PARAM_NONE) return reply_err(r, cap);
    if (v < 0) return put_s(r, put_s(r, 0U, cap, head), cap, ";"); /* SW; in receive, as QMX */
    return reply_u(r, cap, head, (uint32_t)v, width);
}

/* FR/FT/SP/RT/XT: this radio has VFO A, no split, no RIT/XIT. Reads 0,
 * accepts 0, refuses anything that would need the rest. */
static uint16_t zero_only(const char *head, const char *arg, uint8_t alen, char *r, uint16_t cap)
{
    if (alen == 0U) return reply_u(r, cap, head, 0U, 1U);
    return ((alen == 1U) && (arg[0] == '0')) ? 0U : reply_err(r, cap);
}

/* "MM"/"ML", the body the menu writes in place, ';'. The body may use all
 * but the 4 bytes of the head, the ';' and put_s's spare byte. */
static uint16_t wrap_body(char *r, uint16_t cap, const char *head)
{
    uint16_t n = put_s(r, 0U, cap, head);
    while ((n + 2U < cap) && r[n]) ++n;
    return put_s(r, n, cap, ";");
}

static uint16_t menu_cmd(const cat_ops_t *ops, const char *arg, uint8_t alen, char *r, uint16_t cap)
{
    uint8_t has_reply = 0U;
    if (!ops->menu || (alen == 0U) || (cap < 8U)) return reply_err(r, cap);
    r[2] = '\0';
    if (!ops->menu(arg, &r[2], (uint16_t)(cap - 4U), &has_reply)) return reply_err(r, cap);
    return has_reply ? wrap_body(r, cap, "MM") : 0U;
}

static uint16_t list_cmd(const cat_ops_t *ops, const char *arg, uint8_t alen, char *r, uint16_t cap)
{
    uint32_t list = 0U;
    if (!ops->menu_list || (cap < 8U) || !parse_u(arg, alen, &list)) return reply_err(r, cap);
    r[2] = '\0';
    if (!ops->menu_list(list, &r[2], (uint16_t)(cap - 4U))) return reply_err(r, cap);
    return wrap_body(r, cap, "ML");
}

/* The commands, by first letter: one small switch each keeps every function
 * readable (and inside the complexity budget). `b` is the second letter. */
typedef struct {
    cat_parser_t    *p;
    const cat_ops_t *ops;
    const char      *arg;
    uint8_t          alen;
    char            *r;
    uint16_t         cap;
} cat_cmd_t;

static __attribute__((noinline)) uint16_t exec_f(const cat_cmd_t *c, char b)
{
    uint32_t v = 0U;
    switch (b) {
        case 'A':
        case 'B': {
            const uint8_t vfo_b = (uint8_t)(b == 'B');
            if (c->alen == 0U) {
                const uint32_t hz = (vfo_b && c->p->vfo_b) ? c->p->vfo_b : c->ops->get_freq();
                return reply_u(c->r, c->cap, vfo_b ? "FB" : "FA", hz, 11U);
            }
            if (!parse_u(c->arg, c->alen, &v)) return reply_err(c->r, c->cap);
            if (vfo_b) { c->p->vfo_b = v; return 0U; }
            return c->ops->set_freq(v) ? 0U : reply_err(c->r, c->cap);
        }
        case 'R': return zero_only("FR", c->arg, c->alen, c->r, c->cap);
        case 'T': return zero_only("FT", c->arg, c->alen, c->r, c->cap);
        case 'W': return c->alen ? reply_err(c->r, c->cap) : meter_cmd(c->ops, CAT_P_FILTER_HZ, "FW", 4U, c->r, c->cap);
        default:  return reply_err(c->r, c->cap);
    }
}

static __attribute__((noinline)) uint16_t exec_m(const cat_cmd_t *c, char b)
{
    switch (b) {
        case 'D':
            if (c->alen == 0U) return reply_u(c->r, c->cap, "MD", c->ops->get_mode(), 1U);
            if ((c->alen != 1U) || !is_digit(c->arg[0]) || !c->ops->set_mode((uint8_t)(c->arg[0] - '0')))
                return reply_err(c->r, c->cap);
            return 0U;
        case 'M': return menu_cmd(c->ops, c->arg, c->alen, c->r, c->cap);
        case 'L': return list_cmd(c->ops, c->arg, c->alen, c->r, c->cap);
        default:  return reply_err(c->r, c->cap);
    }
}

/* TX, RX, TQ: transmit. TX; TX0; TX1; TX2; -- every Kenwood form keys. */
static __attribute__((noinline)) uint16_t exec_t(const cat_cmd_t *c, char a, char b)
{
    const char x = c->alen ? c->arg[0] : '\0';
    if ((a == 'T') && (b == 'X')) {
        if ((c->alen > 1U) || ((c->alen == 1U) && ((x < '0') || (x > '2')))) return reply_err(c->r, c->cap);
        c->ops->set_tx(1U);
        return 0U;
    }
    if ((a == 'R') && (b == 'X')) {
        if (c->alen) return reply_err(c->r, c->cap);
        c->ops->set_tx(0U);
        return 0U;
    }
    if ((a == 'T') && (b == 'Q')) {
        if (c->alen == 0U) return reply_u(c->r, c->cap, "TQ", c->ops->get_tx() ? 1U : 0U, 1U);
        if ((c->alen != 1U) || ((x != '0') && (x != '1'))) return reply_err(c->r, c->cap);
        c->ops->set_tx((uint8_t)(x == '1'));
        return 0U;
    }
    if ((a == 'R') && (b == 'T')) return zero_only("RT", c->arg, c->alen, c->r, c->cap);
    if ((a == 'R') && (b == 'C')) return c->alen ? reply_err(c->r, c->cap) : 0U;
    return reply_err(c->r, c->cap);
}

static __attribute__((noinline)) uint16_t exec_q(const cat_cmd_t *c, char b)
{
    static const struct { char b; uint8_t id; } q[] = {
        {'1', CAT_P_SIDEBAND}, {'3', CAT_P_VOX}, {'9', CAT_P_IQ}, {'B', CAT_P_TIMEOUT_ON}, {'C', CAT_P_TIMEOUT_S},
    };
    for (uint8_t i = 0U; i < (uint8_t)(sizeof(q) / sizeof(q[0])); ++i) {
        if (q[i].b != b) continue;
        const char head[3] = {'Q', b, '\0'};
        return param_cmd(c->ops, (cat_param_t)q[i].id, head, c->arg, c->alen, 1U, c->r, c->cap);
    }
    return reply_err(c->r, c->cap);
}

static __attribute__((noinline)) uint16_t exec_s(const cat_cmd_t *c, char b)
{
    switch (b) {
        case 'P': return zero_only("SP", c->arg, c->alen, c->r, c->cap);
        case 'M': return meter_cmd(c->ops, CAT_P_SMETER_DB, "SM0", 4U, c->r, c->cap);
        case 'S': return param_cmd(c->ops, CAT_P_SOURCE, "SS", c->arg, c->alen, 1U, c->r, c->cap);
        case 'W': return c->alen ? reply_err(c->r, c->cap) : meter_cmd(c->ops, CAT_P_SWR_X100, "SW", 1U, c->r, c->cap);
        default:  return reply_err(c->r, c->cap);
    }
}

/* AG; AG0; read; AG0nnn; or AGnnn; set (0..255). */
static __attribute__((noinline)) uint16_t exec_ag(const cat_cmd_t *c)
{
    const char *arg = c->arg;
    uint8_t alen = c->alen;
    uint32_t v = 0U;
    if ((alen == 0U) || ((alen == 1U) && (arg[0] == '0'))) {
        const int32_t g = c->ops->get ? c->ops->get(CAT_P_VOLUME) : CAT_PARAM_NONE;
        if ((g == CAT_PARAM_NONE) || (g < 0)) return reply_err(c->r, c->cap);
        return reply_u(c->r, c->cap, "AG0", (uint32_t)g, 3U);
    }
    if ((alen == 4U) && (arg[0] == '0')) { ++arg; --alen; }
    if (!parse_u(arg, alen, &v) || (v > 255U) || !c->ops->set || !c->ops->set(CAT_P_VOLUME, (int32_t)v))
        return reply_err(c->r, c->cap);
    return 0U;
}

static __attribute__((noinline)) uint16_t exec_other(const cat_cmd_t *c, char a, char b)
{
    char *r = c->r;
    const uint16_t cap = c->cap;
    switch (KEY(a, b)) {
        case KEY('I', 'D'): return c->alen ? reply_err(r, cap) : put_s(r, 0U, cap, "ID020;"); /* TS-480 */
        case KEY('V', 'N'): return put_s(r, put_s(r, put_s(r, 0U, cap, "VN"), cap, c->ops->version), cap, ";");
        case KEY('O', 'M'): return put_s(r, 0U, cap, "OMSP;");
        case KEY('I', 'F'): return reply_if(c->ops, r, cap);
        case KEY('X', 'T'): return zero_only("XT", c->arg, c->alen, r, cap);
        case KEY('A', 'G'): return exec_ag(c);
        case KEY('K', 'S'): return param_cmd(c->ops, CAT_P_WPM, "KS", c->arg, c->alen, 3U, r, cap);
        case KEY('P', 'C'): return c->alen ? reply_err(r, cap) : meter_cmd(c->ops, CAT_P_POWER_DW, "PC", 1U, r, cap);
        case KEY('A', 'I'):
            if (c->alen == 0U) return reply_u(r, cap, "AI", c->p->ai, 1U);
            if ((c->alen != 1U) || (c->arg[0] < '0') || (c->arg[0] > '4')) return reply_err(r, cap);
            c->p->ai = (uint8_t)(c->arg[0] - '0');
            return 0U;
        case KEY('P', 'S'):
            /* Power: always on, and PS0 never switches it off from a PC. */
            if (c->alen == 0U) return put_s(r, 0U, cap, "PS1;");
            return ((c->alen == 1U) && is_digit(c->arg[0])) ? 0U : reply_err(r, cap);
        default:
            return reply_err(r, cap);
    }
}

uint16_t CAT_Execute(cat_parser_t *p, const cat_ops_t *ops, const char *cmd, uint8_t len,
                     char *r, uint16_t cap)
{
    if (len == 0U) return 0U; /* ";;" */
    if (len < 2U) return reply_err(r, cap);
    const cat_cmd_t c = { p, ops, &cmd[2], (uint8_t)(len - 2U), r, cap };
    const char a = upper(cmd[0]), b = upper(cmd[1]);
    switch (a) {
        case 'F': return exec_f(&c, b);
        case 'M': return exec_m(&c, b);
        case 'Q': return exec_q(&c, b);
        case 'S': return exec_s(&c, b);
        case 'T':
        case 'R': return exec_t(&c, a, b);
        default:  return exec_other(&c, a, b);
    }
}

void CAT_ParserReset(cat_parser_t *p)
{
    p->len = 0U;
    p->overflow = 0U;
    p->ai = 0U;
    p->vfo_b = 0U;
}

cat_push_t CAT_ParserPush(cat_parser_t *p, const cat_ops_t *ops, char c,
                          char *reply, uint16_t cap, uint16_t *reply_len)
{
    *reply_len = 0U;
    if ((c == '\r') || (c == '\n')) return CAT_PUSH_TERMINAL;
    if (c == ';') {
        p->cmd[p->len] = '\0';
        if (p->overflow) *reply_len = reply_err(reply, cap);
        else *reply_len = CAT_Execute(p, ops, p->cmd, p->len, reply, cap);
        p->len = 0U;
        p->overflow = 0U;
        return CAT_PUSH_DONE;
    }
    /* Other control bytes never belong to a command; nor does a space
     * before one (KY and MM keep theirs, inside the command). */
    if (((uint8_t)c < 0x20U) || ((uint8_t)c == 0x7FU)) return CAT_PUSH_NONE;
    if ((c == ' ') && (p->len == 0U)) return CAT_PUSH_NONE;
    if (p->len >= CAT_CMD_MAX) { p->overflow = 1U; return CAT_PUSH_NONE; }
    p->cmd[p->len++] = c;
    return CAT_PUSH_NONE;
}
