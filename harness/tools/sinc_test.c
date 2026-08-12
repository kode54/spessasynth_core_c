/* Measures the sinc resampler directly, since there is nothing upstream to
 * compare it against: spessasynth_core has no sinc mode.
 *
 * Feeds a synthesised sine through the wavetable oscillator at a series of
 * playback ratios and reports how much of the signal folds back below Nyquist
 * when the source tone is pushed above it. A resampler whose window does not
 * widen with the ratio loses its rejection exactly where it is needed, which
 * is what this measures.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spessasynth/synthesizer/synth.h"

#define RATE 48000
#define SRC_LEN 8192
#define OUT_LEN 4096

extern bool ss_wavetable_get_sample(SS_Voice *v, float *out, int count,
                                    SS_InterpolationType interp);
extern void ss_sinc_table_init(void);

/* Goertzel magnitude at a normalised frequency, in dB relative to full scale. */
static double magnitude_db(const float *x, int n, double cycles_per_sample) {
	const double w = 2.0 * M_PI * cycles_per_sample;
	const double coeff = 2.0 * cos(w);
	double s0 = 0, s1 = 0, s2 = 0;
	for(int i = 0; i < n; i++) {
		s0 = x[i] + coeff * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	const double power = (s1 * s1) + (s2 * s2) - (coeff * s1 * s2);
	const double mag = sqrt(power > 0 ? power : 0) * 2.0 / n;
	return 20.0 * log10(mag > 1e-12 ? mag : 1e-12);
}

int main(void) {
	ss_sinc_table_init();

	static float src[SRC_LEN];
	static float out[OUT_LEN];

	printf("Source tone at 0.30 cycles/sample, read at increasing ratios.\n");
	printf("Above 2x the tone passes Nyquist and folds back; the alias is what\n");
	printf("the kernel is there to reject.\n\n");
	printf("%6s %10s %12s %12s\n", "ratio", "taps", "alias dB", "rejection");

	int failures = 0;
	for(double ratio = 1.0; ratio <= 8.01; ratio *= 2.0) {
		/* A tone whose resampled frequency lands above Nyquist for ratio > 1.6,
		 * chosen so the fold-back never lands on DC — where the measurement
		 * would read the absence of an offset rather than the alias. */
		const double src_freq = 0.3;
		for(int i = 0; i < SRC_LEN; i++)
			src[i] = (float)sin(2.0 * M_PI * src_freq * i);

		SS_Voice v;
		memset(&v, 0, sizeof(v));
		v.sample.sample_data = src;
		v.sample.sample_data_len = SRC_LEN;
		v.sample.end = SRC_LEN;
		v.sample.cursor = 0;
		v.sample.is_looping = false;
		v.sample.playback_step = 1.0f;
		v.current_tuning_calculated = ratio;

		memset(out, 0, sizeof(out));
		ss_wavetable_get_sample(&v, out, OUT_LEN, SS_INTERP_SINC);

		/* Where the tone lands, and where its first fold-back lands. */
		const double played = src_freq * ratio;
		/* Fold the played frequency into [0, 0.5]: wrap modulo the sample
		 * rate first, then mirror about Nyquist. */
		double alias = fmod(played, 1.0);
		if(alias > 0.5) alias = 1.0 - alias;
		const double alias_db = magnitude_db(out, OUT_LEN, alias);

		/* Reference level: the same measurement at unity, where nothing folds. */
		const int taps = (int)(2 * (ratio > 1.0 ? ceil(4.0 * ratio) : 4.0));
		printf("%6.1f %10d %12.1f", ratio, taps > 64 ? 64 : taps, alias_db);

		if(ratio > 2.0) {
			/* Past 2x the source tone is above the new Nyquist, so anything
			 * measured at the fold-back frequency is alias. */
			printf(" %12.1f", -alias_db);
			if(alias_db > -20.0) {
				printf("   FAIL (weak rejection)");
				failures++;
			}
		} else {
			printf(" %12s", "-");
		}
		printf("\n");
	}

	printf("\n%s\n", failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}
