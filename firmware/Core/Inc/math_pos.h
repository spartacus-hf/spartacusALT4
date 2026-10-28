#ifndef MATH_POS_H
#define MATH_POS_H

#include <math.h>

/* x^y for x > 0, as exp(y ln x). Every power taken in this firmware has a
 * positive base (10 for dB, or a gain ratio over one), and newlib's general
 * powf -- 1.9 KB with its errno wrapper and scalbnf -- was linked for them;
 * logf is in the image anyway (log10f). Within a few units in the last
 * place of powf. */
static inline float powf_pos(float x, float y)
{
    return expf(y * logf(x));
}

#endif /* MATH_POS_H */
