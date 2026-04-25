#define _USE_MATH_DEFINES
#include <math.h>
#include <stdlib.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/synth.h>
#else
#include "spessasynth/synthesizer/synth.h"
#endif

/* ── LFO ──────────────────────────────────────────────────────────────────── */

float ss_lfo_value(double start_time, float freq_hz, double current_time) {
	if(current_time < start_time) return 0.0f;
	if(freq_hz <= 0.0f) return 0.0f;
	double elapsed = current_time - start_time;
	return (float)sin(2.0 * M_PI * freq_hz * elapsed);
}
