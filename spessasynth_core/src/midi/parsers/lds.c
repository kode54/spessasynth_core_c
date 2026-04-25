/**
 * lds.c
 * Loudness Sound System (LDS) tracker-to-MIDI converter.
 * Port of midi_processor_lds.cpp from midi_processing.
 *
 * LDS is an AdLib FM tracker format.  Instead of emitting MIDI events
 * directly from the file, we simulate the tracker's 70 Hz playback loop
 * and translate each tick's state changes into MIDI events.
 *
 * Effect toggles (compile-time):
 *   SS_LDS_ENABLE_WHEEL  - pitch-wheel-based glide/portamento.  Default ON.
 *   SS_LDS_ENABLE_VIB    - vibrato LFO (pitch-wheel-driven).  Default OFF.
 *   SS_LDS_ENABLE_TREM   - amplitude tremolo (CC7-driven).  Default OFF.
 *
 * Arpeggio is not implemented.
 *
 * Produces SMF format 1 at TPQN 35 so one tracker tick maps to one MIDI
 * tick at ~70 Hz with the default 500000 µs/beat tempo (AdLib refresh).
 *
 * Detection: requires a ".lds" filename extension and file[0] <= 2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parsers.h"

/* ── Feature toggles ─────────────────────────────────────────────────────── */

#define SS_LDS_ENABLE_WHEEL
/* #define SS_LDS_ENABLE_VIB */
/* #define SS_LDS_ENABLE_TREM */

#ifdef SS_LDS_ENABLE_WHEEL
#define LDS_WHEEL_RANGE_HIGH 12
#define LDS_WHEEL_RANGE_LOW 0
#define LDS_WHEEL_SCALE(x) (((int)(x) * 512) / LDS_WHEEL_RANGE_HIGH)
#define LDS_WHEEL_LOW(x) (LDS_WHEEL_SCALE(x) & 127)
#define LDS_WHEEL_HIGH(x) (((LDS_WHEEL_SCALE(x) >> 7) + 64) & 127)
#endif

#ifdef SS_LDS_ENABLE_VIB
static const uint8_t LDS_VIBTAB[64] = {
	0, 13, 25, 37, 50, 62, 74, 86, 98, 109, 120, 131, 142, 152, 162,
	171, 180, 189, 197, 205, 212, 219, 225, 231, 236, 240, 244, 247,
	250, 252, 254, 255, 255, 255, 254, 252, 250, 247, 244, 240, 236,
	231, 225, 219, 212, 205, 197, 189, 180, 171, 162, 152, 142, 131,
	120, 109, 98, 86, 74, 62, 50, 37, 25, 13
};
#endif

#ifdef SS_LDS_ENABLE_TREM
static const uint8_t LDS_TREMTAB[128] = {
	0, 0, 1, 1, 2, 4, 5, 7, 10, 12, 15, 18, 21, 25, 29, 33, 37, 42, 47,
	52, 57, 62, 67, 73, 79, 85, 90, 97, 103, 109, 115, 121, 128, 134,
	140, 146, 152, 158, 165, 170, 176, 182, 188, 193, 198, 203, 208,
	213, 218, 222, 226, 230, 234, 237, 240, 243, 245, 248, 250, 251,
	253, 254, 254, 255, 255, 255, 254, 254, 253, 251, 250, 248, 245,
	243, 240, 237, 234, 230, 226, 222, 218, 213, 208, 203, 198, 193,
	188, 182, 176, 170, 165, 158, 152, 146, 140, 134, 127, 121, 115,
	109, 103, 97, 90, 85, 79, 73, 67, 62, 57, 52, 47, 42, 37, 33, 29,
	25, 21, 18, 15, 12, 10, 7, 5, 4, 2, 1, 1, 0
};
#endif

static const uint8_t LDS_DEFAULT_TEMPO[3] = { 0x07, 0xA1, 0x20 }; /* 500000 µs/beat */
static const uint8_t LDS_LOOP_START[] = { 'l', 'o', 'o', 'p', 'S', 't', 'a', 'r', 't' };
static const uint8_t LDS_LOOP_END[] = { 'l', 'o', 'o', 'p', 'E', 'n', 'd' };

/* ── Data types ──────────────────────────────────────────────────────────── */

typedef struct {
	uint8_t keyoff;
#ifdef SS_LDS_ENABLE_WHEEL
	uint8_t portamento;
	int8_t glide;
#endif
#ifdef SS_LDS_ENABLE_VIB
	uint8_t vibrato;
	uint8_t vibrato_delay;
#endif
#ifdef SS_LDS_ENABLE_TREM
	uint8_t modulator_tremolo;
	uint8_t carrier_tremolo;
	uint8_t tremolo_delay;
#endif
	uint8_t midi_instrument;
	uint8_t midi_velocity;
	uint8_t midi_key;
	int8_t midi_transpose;
} LDSPatch;

typedef struct {
#ifdef SS_LDS_ENABLE_WHEEL
	int16_t gototune, lasttune;
#endif
	uint16_t packpos;
	int8_t finetune;
#ifdef SS_LDS_ENABLE_WHEEL
	uint8_t glideto, portspeed;
#endif
	uint8_t nextvol;
	uint8_t keycount, packwait;
#ifdef SS_LDS_ENABLE_VIB
	uint8_t vibwait, vibspeed, vibrate, vibcount;
#endif
#ifdef SS_LDS_ENABLE_TREM
	uint8_t trmstay, trmwait, trmspeed, trmrate, trmcount;
	uint8_t trcwait, trcspeed, trcrate, trccount;
#endif
	struct {
		uint8_t chandelay, sound;
		uint16_t high;
	} chancheat;
} LDSChanState;

typedef struct {
	uint16_t pattern_number;
	uint8_t transpose;
} LDSPosition;

/* ── Detection ───────────────────────────────────────────────────────────── */

static bool ends_with_lds(const char *name) {
	if(!name || !*name) return false;
	size_t n = strlen(name);
	if(n < 4) return false;
	const char *ext = name + n - 4;
	return (ext[0] == '.') &&
	       (ext[1] == 'l' || ext[1] == 'L') &&
	       (ext[2] == 'd' || ext[2] == 'D') &&
	       (ext[3] == 's' || ext[3] == 'S');
}

bool ss_midi_is_lds(SS_File *file, size_t size, const char *file_name) {
	if(!ends_with_lds(file_name)) return false;
	if(size < 1) return false;
	return ss_file_read_u8(file, 0) <= 2;
}

/* ── Event emission helpers ──────────────────────────────────────────────── */

static bool emit(SS_MIDITrack *track, size_t ticks, uint8_t status_byte,
                 const uint8_t *data, size_t data_len) {
	SS_MIDIMessage msg;
	memset(&msg, 0, sizeof(msg));
	msg.ticks = ticks;
	msg.status_byte = status_byte;
	msg.data_length = data_len;
	if(data_len > 0) {
		msg.data = (uint8_t *)malloc(data_len);
		if(!msg.data) return false;
		memcpy(msg.data, data, data_len);
	}
	if(!ss_midi_track_push_event(track, msg)) {
		free(msg.data);
		return false;
	}
	return true;
}

static bool emit_cc(SS_MIDITrack *track, size_t ticks, uint8_t channel,
                    uint8_t cc, uint8_t value) {
	uint8_t buf[2] = { cc, value };
	return emit(track, ticks, (uint8_t)(0xB0 | (channel & 0x0F)), buf, 2);
}

static bool emit_note_on(SS_MIDITrack *track, size_t ticks, uint8_t channel,
                         uint8_t note, uint8_t vel) {
	uint8_t buf[2] = { note, vel };
	return emit(track, ticks, (uint8_t)(0x90 | (channel & 0x0F)), buf, 2);
}

static bool emit_note_off(SS_MIDITrack *track, size_t ticks, uint8_t channel,
                          uint8_t note, uint8_t vel) {
	uint8_t buf[2] = { note, vel };
	return emit(track, ticks, (uint8_t)(0x80 | (channel & 0x0F)), buf, 2);
}

static bool emit_program(SS_MIDITrack *track, size_t ticks, uint8_t channel,
                         uint8_t program) {
	return emit(track, ticks, (uint8_t)(0xC0 | (channel & 0x0F)), &program, 1);
}

#ifdef SS_LDS_ENABLE_WHEEL
static bool emit_pitch_wheel(SS_MIDITrack *track, size_t ticks, uint8_t channel,
                             uint8_t low, uint8_t high) {
	uint8_t buf[2] = { low, high };
	return emit(track, ticks, (uint8_t)(0xE0 | (channel & 0x0F)), buf, 2);
}
#endif

/* ── Track playback state passed to playsound ────────────────────────────── */

typedef struct {
	LDSPatch *patches;
	size_t patch_count;
	uint8_t *current_instrument; /* [9] */
	uint8_t *last_channel; /* [9] */
	uint8_t *last_instrument; /* [9] */
	uint8_t *last_note; /* [9] */
	uint8_t *last_volume; /* [9] */
	uint8_t *last_sent_volume; /* [11] */
#ifdef SS_LDS_ENABLE_WHEEL
	int16_t *last_pitch_wheel; /* [11] */
#endif
} LDSPlaybackState;

/* ── playsound: emit MIDI for a triggered LDS note ───────────────────────── */

static bool playsound(LDSPlaybackState *s, LDSChanState *c, uint8_t allvolume,
                      size_t current_timestamp, unsigned sound, unsigned chan,
                      unsigned high, SS_MIDITrack *track) {
	s->current_instrument[chan] = (uint8_t)sound;
	if(sound >= s->patch_count) return true;
	LDSPatch *patch = &s->patches[s->current_instrument[chan]];
	unsigned channel = (patch->midi_instrument >= 0x80) ? 9 : ((chan == 9) ? 10 : chan);
	unsigned saved_last_note = s->last_note[chan];
	unsigned note;

	if(channel != 9) {
		high = (unsigned)((int)high + c->finetune);
		high = (unsigned)((int)high + ((int)patch->midi_transpose << 4));
		note = high
#ifdef SS_LDS_ENABLE_WHEEL
		       - (unsigned)c->lasttune
#endif
		;

#ifdef SS_LDS_ENABLE_WHEEL
		if(c->glideto != 0) {
			c->gototune = (int16_t)((int)note - ((int)s->last_note[chan] << 4) + c->lasttune);
			c->portspeed = c->glideto;
			c->glideto = 0;
			c->finetune = 0;
			return true;
		}
#endif

		if(patch->midi_instrument != s->last_instrument[chan]) {
			if(!emit_program(track, current_timestamp, (uint8_t)channel,
			                 patch->midi_instrument)) return false;
			s->last_instrument[chan] = patch->midi_instrument;
		}
	} else {
		note = (unsigned)((patch->midi_instrument & 0x7F) << 4);
	}

	unsigned volume = 127;
	if(c->nextvol) {
		volume = (c->nextvol & 0x3F) * 127 / 63;
		s->last_volume[chan] = (uint8_t)volume;
	}
	if(allvolume) volume = volume * allvolume / 255;

	if(volume != s->last_sent_volume[channel]) {
		if(!emit_cc(track, current_timestamp, s->last_channel[chan],
		            7, (uint8_t)volume)) return false;
		s->last_sent_volume[channel] = (uint8_t)volume;
	}

	if(saved_last_note != 0xFF) {
		if(!emit_note_off(track, current_timestamp, s->last_channel[chan],
		                  (uint8_t)saved_last_note, 127)) return false;
		s->last_note[chan] = 0xFF;
#ifdef SS_LDS_ENABLE_WHEEL
		if(channel != 9) {
			note = (unsigned)((int)note + c->lasttune);
			c->lasttune = 0;
			if(s->last_pitch_wheel[channel] != 0) {
				if(!emit_pitch_wheel(track, current_timestamp,
				                     s->last_channel[chan], 0, 64)) return false;
				s->last_pitch_wheel[channel] = 0;
			}
		}
#endif
	}
#ifdef SS_LDS_ENABLE_WHEEL
	if(c->lasttune != s->last_pitch_wheel[channel]) {
		if(!emit_pitch_wheel(track, current_timestamp, (uint8_t)channel,
		                     (uint8_t)LDS_WHEEL_LOW(c->lasttune),
		                     (uint8_t)LDS_WHEEL_HIGH(c->lasttune))) return false;
		s->last_pitch_wheel[channel] = c->lasttune;
	}
	if(!patch->glide || s->last_note[chan] == 0xFF)
#endif
	{
#ifdef SS_LDS_ENABLE_WHEEL
		if(!patch->portamento || s->last_note[chan] == 0xFF)
#endif
		{
			if(!emit_note_on(track, current_timestamp, (uint8_t)channel,
			                 (uint8_t)(note >> 4), patch->midi_velocity))
				return false;
			s->last_note[chan] = (uint8_t)(note >> 4);
			s->last_channel[chan] = (uint8_t)channel;
#ifdef SS_LDS_ENABLE_WHEEL
			c->gototune = c->lasttune;
#endif
		}
#ifdef SS_LDS_ENABLE_WHEEL
		else {
			c->gototune = (int16_t)((int)note - ((int)s->last_note[chan] << 4) + c->lasttune);
			c->portspeed = patch->portamento;
			s->last_note[chan] = (uint8_t)saved_last_note;
			if(!emit_note_on(track, current_timestamp, (uint8_t)channel,
			                 (uint8_t)saved_last_note, patch->midi_velocity))
				return false;
		}
#endif
	}
#ifdef SS_LDS_ENABLE_WHEEL
	else {
		if(!emit_note_on(track, current_timestamp, (uint8_t)channel,
		                 (uint8_t)(note >> 4), patch->midi_velocity))
			return false;
		s->last_note[chan] = (uint8_t)(note >> 4);
		s->last_channel[chan] = (uint8_t)channel;
		c->gototune = patch->glide;
		c->portspeed = patch->portamento;
	}
#endif

#ifdef SS_LDS_ENABLE_VIB
	if(!patch->vibrato) {
		c->vibwait = c->vibspeed = c->vibrate = 0;
	} else {
		c->vibwait = patch->vibrato_delay;
		c->vibspeed = (uint8_t)((patch->vibrato >> 4) + 2);
		c->vibrate = (uint8_t)((patch->vibrato & 15) + 1);
	}
#endif

#ifdef SS_LDS_ENABLE_TREM
	if(!(c->trmstay & 0xF0)) {
		c->trmwait = (uint8_t)((patch->tremolo_delay & 0xF0) >> 3);
		c->trmspeed = (uint8_t)(patch->modulator_tremolo >> 4);
		c->trmrate = (uint8_t)(patch->modulator_tremolo & 15);
		c->trmcount = 0;
	}
	if(!(c->trmstay & 0x0F)) {
		c->trcwait = (uint8_t)((patch->tremolo_delay & 15) << 1);
		c->trcspeed = (uint8_t)(patch->carrier_tremolo >> 4);
		c->trcrate = (uint8_t)(patch->carrier_tremolo & 15);
		c->trccount = 0;
	}
#endif

#ifdef SS_LDS_ENABLE_VIB
	c->vibcount = 0;
#endif
#ifdef SS_LDS_ENABLE_WHEEL
	c->glideto = 0;
#endif
	c->keycount = patch->keyoff;
	c->nextvol = 0;
	c->finetune = 0;
	return true;
}

/* ── Compact scratch tracks into m->tracks after simulation ──────────────── */

static void move_track(SS_MIDITrack *dst, SS_MIDITrack *src) {
	*dst = *src;
	memset(src, 0, sizeof(*src));
	dst->port = -1;
}

/* ── Public LDS parser ───────────────────────────────────────────────────── */

bool ss_midi_parse_lds(SS_MIDIFile *m, SS_File *file, size_t size) {
	/* Header + patches + positions + patterns. */
	if(size < 1) return false;
	size_t pos = 0;
	uint8_t mode = ss_file_read_u8(file, pos++);
	if(mode > 2) return false;
	if(size - pos < 4) return false;
	/* speed = LE16 at pos 1; ignored */
	uint8_t tempo = ss_file_read_u8(file, pos + 2);
	uint8_t pattern_length = ss_file_read_u8(file, pos + 3);
	pos += 4;

	if(size - pos < 10) return false;
	uint8_t channel_delay[9];
	for(int i = 0; i < 9; i++) channel_delay[i] = ss_file_read_u8(file, pos++);
	pos++; /* register_bd; ignored */

	if(size - pos < 2) return false;
	uint16_t patch_count = ss_file_read_le16(file, pos);
	pos += 2;
	if(patch_count == 0) return false;
	if(size - pos < (size_t)patch_count * 46) return false;

	LDSPatch *patches = (LDSPatch *)calloc(patch_count, sizeof(LDSPatch));
	if(!patches) return false;

	for(uint16_t i = 0; i < patch_count; i++) {
		LDSPatch *p = &patches[i];
		pos += 11;
		p->keyoff = ss_file_read_u8(file, pos++);
#ifdef SS_LDS_ENABLE_WHEEL
		p->portamento = ss_file_read_u8(file, pos++);
		p->glide = (int8_t)ss_file_read_u8(file, pos++);
		pos++;
#else
		pos += 3;
#endif
#ifdef SS_LDS_ENABLE_VIB
		p->vibrato = ss_file_read_u8(file, pos++);
		p->vibrato_delay = ss_file_read_u8(file, pos++);
#else
		pos += 2;
#endif
#ifdef SS_LDS_ENABLE_TREM
		p->modulator_tremolo = ss_file_read_u8(file, pos++);
		p->carrier_tremolo = ss_file_read_u8(file, pos++);
		p->tremolo_delay = ss_file_read_u8(file, pos++);
#else
		pos += 3;
#endif
		pos += 20; /* arpeggio/digital-instrument fields — not used */
		p->midi_instrument = ss_file_read_u8(file, pos++);
		p->midi_velocity = ss_file_read_u8(file, pos++);
		p->midi_key = ss_file_read_u8(file, pos++);
		p->midi_transpose = (int8_t)ss_file_read_u8(file, pos++);
		pos += 2;

#ifdef SS_LDS_ENABLE_WHEEL
		/* Drum patches do not glide (reference's "hax"). */
		if(p->midi_instrument >= 0x80) p->glide = 0;
#endif
	}

	if(size - pos < 2) {
		free(patches);
		return false;
	}
	uint16_t position_count = ss_file_read_le16(file, pos);
	pos += 2;
	if(position_count == 0) {
		free(patches);
		return false;
	}
	if(size - pos < (size_t)position_count * 27) {
		free(patches);
		return false;
	}

	LDSPosition *positions = (LDSPosition *)calloc((size_t)position_count * 9,
	                                               sizeof(LDSPosition));
	if(!positions) {
		free(patches);
		return false;
	}
	for(uint16_t i = 0; i < position_count; i++) {
		for(int j = 0; j < 9; j++) {
			LDSPosition *p = &positions[i * 9 + j];
			uint16_t patnum = ss_file_read_le16(file, pos);
			if(patnum & 1) {
				free(patches);
				free(positions);
				return false;
			}
			p->pattern_number = patnum >> 1;
			p->transpose = ss_file_read_u8(file, pos + 2);
			pos += 3;
		}
	}

	if(size - pos < 2) {
		free(patches);
		free(positions);
		return false;
	}
	pos += 2;

	size_t pattern_count = (size - pos) / 2;
	uint16_t *patterns = (uint16_t *)calloc(pattern_count > 0 ? pattern_count : 1,
	                                        sizeof(uint16_t));
	if(!patterns) {
		free(patches);
		free(positions);
		return false;
	}
	for(size_t i = 0; i < pattern_count; i++) {
		patterns[i] = ss_file_read_le16(file, pos);
		pos += 2;
	}

	/* ── Simulation state ────────────────────────────────────────────────── */
	LDSChanState channel[9];
	memset(channel, 0, sizeof(channel));

	size_t *position_timestamps =
	(size_t *)malloc(position_count * sizeof(size_t));
	if(!position_timestamps) {
		free(patches);
		free(positions);
		free(patterns);
		return false;
	}
	for(uint16_t i = 0; i < position_count; i++) position_timestamps[i] = SIZE_MAX;

	uint8_t current_instrument[9] = { 0 };
	uint8_t last_channel[9] = { 0 };
	uint8_t last_instrument[9];
	uint8_t last_note[9];
	uint8_t last_volume[9];
	uint8_t last_sent_volume[11];
#ifdef SS_LDS_ENABLE_WHEEL
	int16_t last_pitch_wheel[11];
#endif
	memset(last_instrument, 0xFF, sizeof(last_instrument));
	memset(last_note, 0xFF, sizeof(last_note));
	memset(last_volume, 127, sizeof(last_volume));
	memset(last_sent_volume, 127, sizeof(last_sent_volume));
#ifdef SS_LDS_ENABLE_WHEEL
	memset(last_pitch_wheel, 0, sizeof(last_pitch_wheel));
#endif

	LDSPlaybackState pstate;
	pstate.patches = patches;
	pstate.patch_count = patch_count;
	pstate.current_instrument = current_instrument;
	pstate.last_channel = last_channel;
	pstate.last_instrument = last_instrument;
	pstate.last_note = last_note;
	pstate.last_volume = last_volume;
	pstate.last_sent_volume = last_sent_volume;
#ifdef SS_LDS_ENABLE_WHEEL
	pstate.last_pitch_wheel = last_pitch_wheel;
#endif

	/* Output: 10 tracks max (conductor + 9 channels). */
	m->format = 1;
	m->time_division = 35;
	m->tracks = (SS_MIDITrack *)calloc(10, sizeof(SS_MIDITrack));
	if(!m->tracks) {
		free(patches);
		free(positions);
		free(patterns);
		free(position_timestamps);
		return false;
	}
	m->track_capacity = 10;
	m->track_count = 10;
	for(int i = 0; i < 10; i++) m->tracks[i].port = -1;

	SS_MIDITrack *conductor = &m->tracks[0];
	SS_MIDITrack *ch_tracks = &m->tracks[1]; /* 9 tracks */

	/* Conductor: default tempo + CC init across channels 0..10 + EOT. */
#define BAIL(x)                    \
	do {                           \
		free(patches);             \
		free(positions);           \
		free(patterns);            \
		free(position_timestamps); \
		return (x);                \
	} while(0)

	if(!emit(conductor, 0, SS_META_SET_TEMPO,
	         LDS_DEFAULT_TEMPO, sizeof(LDS_DEFAULT_TEMPO))) BAIL(false);
	for(uint8_t ch = 0; ch < 11; ch++) {
		if(!emit_cc(conductor, 0, ch, 120, 0)) BAIL(false); /* all sound off */
		if(!emit_cc(conductor, 0, ch, 121, 0)) BAIL(false); /* reset controllers */
#ifdef SS_LDS_ENABLE_WHEEL
		if(!emit_cc(conductor, 0, ch, 0x65, 0)) BAIL(false); /* RPN MSB */
		if(!emit_cc(conductor, 0, ch, 0x64, 0)) BAIL(false); /* RPN LSB = pitch bend range */
		if(!emit_cc(conductor, 0, ch, 0x06, LDS_WHEEL_RANGE_HIGH)) BAIL(false);
		if(!emit_cc(conductor, 0, ch, 0x26, LDS_WHEEL_RANGE_LOW)) BAIL(false);
		if(!emit_pitch_wheel(conductor, 0, ch, 0, 64)) BAIL(false);
#endif
	}
	if(!emit(conductor, 0, SS_META_END_OF_TRACK, NULL, 0)) BAIL(false);

	/* ── Tracker playback loop ───────────────────────────────────────────── */
	uint8_t tempo_now = 3;
	uint8_t fadeonoff = 0;
	uint8_t allvolume = 0;
	uint8_t hardfade = 0;
	uint8_t pattplay = 0;
	uint16_t posplay = 0;
	uint16_t jumppos = 0;
	uint32_t mainvolume = 0;

	const uint16_t maxsound = 0x3F;
	const uint16_t maxpos = 0xFF;

	size_t current_timestamp = 0;
	bool playing = true;

	while(playing) {
		if(fadeonoff) {
			if(fadeonoff <= 128) {
				if(allvolume > fadeonoff || allvolume == 0) {
					allvolume -= fadeonoff;
				} else {
					allvolume = 1;
					fadeonoff = 0;
					if(hardfade != 0) {
						playing = false;
						hardfade = 0;
						for(int i = 0; i < 9; i++) channel[i].keycount = 1;
					}
				}
			} else if((unsigned)((allvolume + (0x100 - fadeonoff)) & 0xFF) <= mainvolume) {
				allvolume = (uint8_t)(allvolume + 0x100 - fadeonoff);
			} else {
				allvolume = (uint8_t)mainvolume;
				fadeonoff = 0;
			}
		}

		/* Channel-delay triggers. */
		for(int chan = 0; chan < 9; chan++) {
			LDSChanState *c = &channel[chan];
			if(c->chancheat.chandelay) {
				if(!--c->chancheat.chandelay) {
					if(!playsound(&pstate, c, allvolume, current_timestamp,
					              c->chancheat.sound, chan, c->chancheat.high,
					              &ch_tracks[chan])) BAIL(false);
				}
			}
		}

		/* New tick: advance pattern if tempo counter drained. */
		if(!tempo_now) {
			if(pattplay == 0 && position_timestamps[posplay] == SIZE_MAX)
				position_timestamps[posplay] = current_timestamp;

			bool vbreak = false;
			for(unsigned _chan = 0; _chan < 9; _chan++) {
				LDSChanState *c = &channel[_chan];
				if(!c->packwait) {
					uint16_t patnum = positions[posplay * 9 + _chan].pattern_number;
					uint8_t transpose = positions[posplay * 9 + _chan].transpose;

					if((size_t)patnum + c->packpos >= pattern_count) BAIL(false);

					uint16_t comword = patterns[patnum + c->packpos];
					uint8_t comhi = (uint8_t)(comword >> 8);
					uint8_t comlo = (uint8_t)(comword & 0xFF);

					if(comword) {
						if(comhi == 0x80) {
							c->packwait = comlo;
						} else if(comhi >= 0x80) {
							switch(comhi) {
								case 0xFF: {
									unsigned volume = (comlo & 0x3F) * 127 / 63;
									last_volume[_chan] = (uint8_t)volume;
									if(volume != last_sent_volume[last_channel[_chan]]) {
										if(!emit_cc(&ch_tracks[_chan],
										            current_timestamp,
										            last_channel[_chan],
										            7, (uint8_t)volume)) BAIL(false);
										last_sent_volume[last_channel[_chan]] = (uint8_t)volume;
									}
									break;
								}
								case 0xFE:
									tempo = (uint8_t)(comword & 0x3F);
									break;
								case 0xFD:
									c->nextvol = comlo;
									break;
								case 0xFC:
									playing = false;
									break;
								case 0xFB:
									c->keycount = 1;
									break;
								case 0xFA:
									vbreak = true;
									jumppos = (uint16_t)((posplay + 1) & maxpos);
									break;
								case 0xF9:
									vbreak = true;
									jumppos = (uint16_t)(comlo & maxpos);
									if(jumppos <= posplay) {
										if(!emit(conductor,
										         position_timestamps[jumppos],
										         SS_META_MARKER,
										         LDS_LOOP_START,
										         sizeof(LDS_LOOP_START))) BAIL(false);
										if(!emit(conductor,
										         current_timestamp + tempo - 1,
										         SS_META_MARKER,
										         LDS_LOOP_END,
										         sizeof(LDS_LOOP_END))) BAIL(false);
										playing = false;
									}
									break;
								case 0xF8:
#ifdef SS_LDS_ENABLE_WHEEL
									c->lasttune = 0;
#endif
									break;
								case 0xF7:
#ifdef SS_LDS_ENABLE_VIB
									c->vibwait = 0;
									c->vibspeed = (uint8_t)((comlo >> 4) + 2);
									c->vibrate = (uint8_t)((comlo & 15) + 1);
#endif
									break;
								case 0xF6:
#ifdef SS_LDS_ENABLE_WHEEL
									c->glideto = comlo;
#endif
									break;
								case 0xF5:
									c->finetune = (int8_t)comlo;
									break;
								case 0xF4:
									if(!hardfade) {
										allvolume = mainvolume = comlo;
										fadeonoff = 0;
									}
									break;
								case 0xF3:
									if(!hardfade) fadeonoff = comlo;
									break;
								case 0xF2:
#ifdef SS_LDS_ENABLE_TREM
									c->trmstay = comlo;
#endif
									break;
								case 0xF1: {
									uint8_t pan_val = (uint8_t)((comlo & 0x3F) * 127 / 63);
									if(!emit_cc(&ch_tracks[_chan],
									            current_timestamp,
									            last_channel[_chan],
									            10, pan_val)) BAIL(false);
									break;
								}
								case 0xF0: {
									uint8_t prog = (uint8_t)(comlo & 0x7F);
									if(!emit_program(&ch_tracks[_chan],
									                 current_timestamp,
									                 last_channel[_chan],
									                 prog)) BAIL(false);
									break;
								}
								default:
#ifdef SS_LDS_ENABLE_WHEEL
									if(comhi < 0xA0)
										c->glideto = (uint8_t)(comhi & 0x1F);
#endif
									break;
							}
						} else {
							uint8_t sound;
							uint16_t high;
							int8_t transp = (int8_t)(transpose << 1);
							transp = (int8_t)(transp >> 1);

							if(transpose & 128) {
								sound = (uint8_t)((comlo + transp) & maxsound);
								high = (uint16_t)(comhi << 4);
							} else {
								sound = (uint8_t)(comlo & maxsound);
								high = (uint16_t)((comhi + transp) << 4);
							}

							if(!channel_delay[_chan]) {
								if(!playsound(&pstate, c, allvolume,
								              current_timestamp, sound, _chan,
								              high, &ch_tracks[_chan])) BAIL(false);
							} else {
								c->chancheat.chandelay = channel_delay[_chan];
								c->chancheat.sound = sound;
								c->chancheat.high = high;
							}
						}
					}
					c->packpos++;
				} else {
					c->packwait--;
				}
			}

			tempo_now = tempo;
			pattplay++;
			if(vbreak) {
				pattplay = 0;
				for(int i = 0; i < 9; i++) {
					channel[i].packpos = 0;
					channel[i].packwait = 0;
				}
				posplay = jumppos;
				if(posplay >= position_count) BAIL(false);
			} else if(pattplay >= pattern_length) {
				pattplay = 0;
				for(int i = 0; i < 9; i++) {
					channel[i].packpos = 0;
					channel[i].packwait = 0;
				}
				posplay = (uint16_t)((posplay + 1) & maxpos);
				if(posplay >= position_count) playing = false;
			}
		} else {
			tempo_now--;
		}

		/* Per-channel effects (key-off, glide/wheel, vibrato, tremolo). */
		for(int chan = 0; chan < 9; chan++) {
			LDSChanState *c = &channel[chan];

			if(c->keycount > 0) {
				if(c->keycount == 1 && last_note[chan] != 0xFF) {
					if(!emit_note_off(&ch_tracks[chan], current_timestamp,
					                  last_channel[chan], last_note[chan], 127))
						BAIL(false);
					last_note[chan] = 0xFF;
#ifdef SS_LDS_ENABLE_WHEEL
					if(last_pitch_wheel[last_channel[chan]] != 0) {
						if(!emit_pitch_wheel(&ch_tracks[chan], current_timestamp,
						                     last_channel[chan], 0, 64)) BAIL(false);
						last_pitch_wheel[last_channel[chan]] = 0;
						c->lasttune = 0;
						c->gototune = 0;
					}
#endif
				}
				c->keycount--;
			}

#ifdef SS_LDS_ENABLE_WHEEL
			if(c->lasttune != c->gototune) {
				if(c->lasttune > c->gototune) {
					if(c->lasttune - c->gototune < c->portspeed)
						c->lasttune = c->gototune;
					else
						c->lasttune = (int16_t)(c->lasttune - c->portspeed);
				} else {
					if(c->gototune - c->lasttune < c->portspeed)
						c->lasttune = c->gototune;
					else
						c->lasttune = (int16_t)(c->lasttune + c->portspeed);
				}
				int16_t value = c->lasttune;
				if(value != last_pitch_wheel[last_channel[chan]]) {
					if(!emit_pitch_wheel(&ch_tracks[chan], current_timestamp,
					                     last_channel[chan],
					                     (uint8_t)LDS_WHEEL_LOW(value),
					                     (uint8_t)LDS_WHEEL_HIGH(value))) BAIL(false);
					last_pitch_wheel[last_channel[chan]] = value;
				}
			} else {
#ifdef SS_LDS_ENABLE_VIB
				if(!c->vibwait) {
					if(c->vibrate) {
						uint16_t wibc = (uint16_t)(LDS_VIBTAB[c->vibcount & 0x3F] *
						                           c->vibrate);
						int16_t tune;
						if((c->vibcount & 0x40) == 0)
							tune = (int16_t)(c->lasttune + (wibc >> 8));
						else
							tune = (int16_t)(c->lasttune - (wibc >> 8));
						if(tune != last_pitch_wheel[last_channel[chan]]) {
							if(!emit_pitch_wheel(&ch_tracks[chan],
							                     current_timestamp,
							                     last_channel[chan],
							                     (uint8_t)LDS_WHEEL_LOW(tune),
							                     (uint8_t)LDS_WHEEL_HIGH(tune)))
								BAIL(false);
							last_pitch_wheel[last_channel[chan]] = tune;
						}
						c->vibcount = (uint8_t)(c->vibcount + c->vibspeed);
					}
				} else {
					c->vibwait--;
				}
#endif
			}
#endif

#ifdef SS_LDS_ENABLE_TREM
			{
				unsigned volume = last_volume[chan];

				if(!c->trmwait) {
					if(c->trmrate) {
						uint16_t tremc = (uint16_t)(LDS_TREMTAB[c->trmcount & 0x7F] *
						                            c->trmrate);
						if((tremc >> 7) <= volume)
							volume -= (tremc >> 7);
						else
							volume = 0;
						c->trmcount = (uint8_t)(c->trmcount + c->trmspeed);
					}
				} else {
					c->trmwait--;
				}

				if(!c->trcwait) {
					if(c->trcrate) {
						uint16_t tremc = (uint16_t)(LDS_TREMTAB[c->trccount & 0x7F] *
						                            c->trcrate);
						if((tremc >> 7) <= volume)
							volume -= (tremc >> 8);
						else
							volume = 0;
					}
				} else {
					c->trcwait--;
				}

				if(allvolume) volume = volume * allvolume / 255;
				if(volume != last_sent_volume[last_channel[chan]]) {
					if(!emit_cc(&ch_tracks[chan], current_timestamp,
					            last_channel[chan], 7, (uint8_t)volume)) BAIL(false);
					last_sent_volume[last_channel[chan]] = (uint8_t)volume;
				}
			}
#endif
		}

		current_timestamp++;
	}
	current_timestamp--;

	/* Final note-offs and pitch-wheel centering. */
	for(int i = 0; i < 9; i++) {
		if(ch_tracks[i].event_count > 0 && last_note[i] != 0xFF) {
			if(!emit_note_off(&ch_tracks[i],
			                  current_timestamp + channel[i].keycount,
			                  last_channel[i], last_note[i], 127)) BAIL(false);
#ifdef SS_LDS_ENABLE_WHEEL
			if(last_pitch_wheel[last_channel[i]] != 0) {
				if(!emit_pitch_wheel(&ch_tracks[i],
				                     current_timestamp + channel[i].keycount,
				                     last_channel[i], 0, 0x40)) BAIL(false);
			}
#endif
		}
	}

	/* Compact: drop empty channel tracks so m->tracks contains
	 * conductor + non-empty channel tracks contiguously. */
	size_t out = 1;
	for(int i = 0; i < 9; i++) {
		if(ch_tracks[i].event_count > 0) {
			if(out != (size_t)(i + 1))
				move_track(&m->tracks[out], &ch_tracks[i]);
			out++;
		} else {
			/* Free any stray resources even though count is 0. */
			free(ch_tracks[i].events);
			memset(&ch_tracks[i], 0, sizeof(ch_tracks[i]));
			ch_tracks[i].port = -1;
		}
	}
	/* Zero out unused trailing slots (they already have no events). */
	for(size_t i = out; i < 10; i++) {
		free(m->tracks[i].events);
		memset(&m->tracks[i], 0, sizeof(m->tracks[i]));
		m->tracks[i].port = -1;
	}
	m->track_count = out;

	free(patches);
	free(positions);
	free(patterns);
	free(position_timestamps);
	return true;

#undef BAIL
}
