#ifndef SS_DELAY_H
#define SS_DELAY_H

#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/delay_line.h>
#else
#include "spessasynth/synthesizer/dsp/delay_line.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SS_DelayParams {
	unsigned char send_level_to_reverb;
	unsigned char pre_lowpass;
	unsigned char level_right;
	unsigned char level;
	unsigned char level_center;
	unsigned char level_left;
	unsigned char feedback;
	unsigned char time_ratio_right;
	unsigned char time_ratio_left;
	unsigned char time_center;
} SS_DelayParams;

typedef struct SS_Delay {
	SS_DelayParams parameters;

	float preLPFfc;
	float preLPFa;
	float preLPFz;

	SS_DelayLine *delayLeft;
	SS_DelayLine *delayRight;
	SS_DelayLine *delayCenter;

	float sample_rate;
	int max_buffer_size;

	float *delay_center_output;
	float *delay_pre_lpf;

	float delay_center_time;
	float delay_left_multiplier;
	float delay_right_multiplier;

	float gain;
	float reverb_gain;
} SS_Delay;

SS_Delay *ss_delay_create(float sample_rate, int max_buffer_size);
void ss_delay_clear(SS_Delay *delay);
void ss_delay_free(SS_Delay *delay);

void ss_delay_process(SS_Delay *delay,
					  const float *inputL, const float *inputR,
					  float *outputL, float *outputR,
					  float *reverbL, float *reverbR,
					  int sample_count
					  );

void ss_delay_set_send_level_to_reverb(SS_Delay *delay, unsigned char value);
void ss_delay_set_pre_lowpass(SS_Delay *delay, unsigned char value);
void ss_delay_set_level_right(SS_Delay *delay, unsigned char value);
void ss_delay_set_level(SS_Delay *delay, unsigned char value);
void ss_delay_set_level_center(SS_Delay *delay, unsigned char value);
void ss_delay_set_level_left(SS_Delay *delay, unsigned char value);
void ss_delay_set_feedback(SS_Delay *delay, unsigned char value);
void ss_delay_set_time_ratio_right(SS_Delay *delay, unsigned char value);
void ss_delay_set_time_ratio_left(SS_Delay *delay, unsigned char value);
void ss_delay_set_time_center(SS_Delay *delay, unsigned char value);

void ss_delay_set_macro(SS_Delay *delay, unsigned char value);

#ifdef __cplusplus
}
#endif

#endif /* SS_DELAY_H */
