/**
 * spessasynth.h — Public umbrella header for SpessaSynth Core (C port).
 *
 * Include this single header to access the complete public API.
 *
 * Quick-start example:
 *
 *   #include <spessasynth/spessasynth.h>
 *   #include <stdio.h>
 *
 *   int main(void) {
 *       // Load soundfont
 *       FILE *f = fopen("bank.sf2", "rb");
 *       fseek(f, 0, SEEK_END); size_t sz = ftell(f); rewind(f);
 *       uint8_t *buf = malloc(sz);
 *       fread(buf, 1, sz, f); fclose(f);
 *
 *       SS_SoundBank *bank = ss_soundbank_load(buf, sz);
 *       free(buf);
 *
 *       // Load MIDI
 *       f = fopen("song.mid", "rb");
 *       fseek(f, 0, SEEK_END); sz = ftell(f); rewind(f);
 *       buf = malloc(sz); fread(buf, 1, sz, f); fclose(f);
 *       SS_MIDIFile *midi = ss_midi_load(buf, sz, "song.mid");
 *       free(buf);
 *
 *       // Create processor + sequencer
 *       SS_Processor *proc = ss_processor_create(44100, NULL);
 *       ss_processor_load_soundbank(proc, bank, "primary", false);
 *       SS_Sequencer *seq = ss_sequencer_create(proc);
 *       ss_sequencer_load_midi(seq, midi);
 *       ss_sequencer_play(seq);
 *
 *       // Render loop
 *       float left[512], right[512];
 *       while (!ss_sequencer_is_finished(seq)) {
 *           memset(left, 0, sizeof(left));
 *           memset(right, 0, sizeof(right));
 *           ss_sequencer_tick(seq, 512);
 *           ss_processor_render(proc, left, right, 512);
 *           // ... write left/right to audio device ...
 *       }
 *
 *       ss_sequencer_free(seq);
 *       ss_processor_free(proc);
 *       ss_soundbank_free(bank);
 *       ss_midi_free(midi);
 *   }
 */

#ifndef SPESSASYNTH_H
#define SPESSASYNTH_H

#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/chorus.h>
#include <spessasynth_core/dattorro_reverb.h>
#include <spessasynth_core/delay_line.h>
#include <spessasynth_core/generator_types.h>
#include <spessasynth_core/indexed_byte_array.h>
#include <spessasynth_core/midi.h>
#include <spessasynth_core/reverb.h>
#include <spessasynth_core/riff_chunk.h>
#include <spessasynth_core/sequencer.h>
#include <spessasynth_core/soundbank.h>
#include <spessasynth_core/synth.h>
#else
#include "midi/midi.h"
#include "sequencer/sequencer.h"
#include "soundbank/generator_types.h"
#include "soundbank/soundbank.h"
#include "synthesizer/dsp/chorus.h"
#include "synthesizer/dsp/dattorro_reverb.h"
#include "synthesizer/dsp/delay_line.h"
#include "synthesizer/dsp/reverb.h"
#include "synthesizer/synth.h"
#include "utils/indexed_byte_array.h"
#include "utils/riff_chunk.h"
#endif

/* ── WAV writer ──────────────────────────────────────────────────────────── */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	bool normalize_audio;
	float loop_start_seconds; /* 0 to disable */
	float loop_end_seconds;
	char title[256];
	char artist[256];
	char album[256];
	char comment[256];
} SS_WavWriteOptions;

/**
 * Encode interleaved or separated float buffers as a 16-bit PCM WAV.
 * channels[] is an array of num_channels float buffers, each of num_samples.
 * The returned buffer is heap-allocated; caller must free() it.
 * Returns true on success.
 */
bool ss_wav_write(const float *const *channels,
                  uint32_t num_channels,
                  uint32_t num_samples,
                  uint32_t sample_rate,
                  const SS_WavWriteOptions *opts,
                  uint8_t **out_data,
                  size_t *out_size);

#ifdef __cplusplus
}
#endif

#endif /* SPESSASYNTH_H */
