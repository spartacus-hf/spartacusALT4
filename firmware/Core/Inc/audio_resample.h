#ifndef AUDIO_RESAMPLE_H
#define AUDIO_RESAMPLE_H
#include <stdint.h>
extern const int16_t audio_sinc_q15[32][16];
typedef struct { float history[3][16]; float phase; uint32_t index; } audio_resample_t;
uint8_t AUDIO_Resample(audio_resample_t *s, const float input[3], float input_rate, float output[3]);
#endif
