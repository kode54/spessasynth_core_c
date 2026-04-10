/**
 * midi_writer.c
 * SMF serializer.  Port of midi_writer.ts.
 */

#include <stdlib.h>
#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/midi.h>
#else
#include "spessasynth/midi/midi.h"
#endif

/* ── Dynamic byte buffer ─────────────────────────────────────────────────── */

typedef struct {
	uint8_t *data;
	size_t len;
	size_t cap;
} ByteBuf;

static bool bb_grow(ByteBuf *b, size_t need) {
	if(b->len + need <= b->cap) return true;
	size_t nc = b->cap ? b->cap * 2 : 256;
	while(nc < b->len + need) nc *= 2;
	uint8_t *tmp = (uint8_t *)realloc(b->data, nc);
	if(!tmp) return false;
	b->data = tmp;
	b->cap = nc;
	return true;
}

static bool bb_push(ByteBuf *b, uint8_t byte) {
	if(!bb_grow(b, 1)) return false;
	b->data[b->len++] = byte;
	return true;
}

static bool bb_push_bytes(ByteBuf *b, const uint8_t *src, size_t n) {
	if(!bb_grow(b, n)) return false;
	memcpy(b->data + b->len, src, n);
	b->len += n;
	return true;
}

static bool bb_push_str(ByteBuf *b, const char *s) {
	return bb_push_bytes(b, (const uint8_t *)s, strlen(s));
}

static bool bb_push_be32(ByteBuf *b, uint32_t v) {
	uint8_t tmp[4] = {
		(uint8_t)(v >> 24), (uint8_t)(v >> 16),
		(uint8_t)(v >> 8), (uint8_t)(v)
	};
	return bb_push_bytes(b, tmp, 4);
}

static bool bb_push_be16(ByteBuf *b, uint16_t v) {
	uint8_t tmp[2] = { (uint8_t)(v >> 8), (uint8_t)(v) };
	return bb_push_bytes(b, tmp, 2);
}

/* ── VLQ encoding ─────────────────────────────────────────────────────────── */

static bool bb_push_vlq(ByteBuf *b, uint32_t v) {
	uint8_t buf[4];
	int n = 0;
	buf[n++] = (uint8_t)(v & 0x7F);
	v >>= 7;
	while(v) {
		buf[n++] = (uint8_t)((v & 0x7F) | 0x80);
		v >>= 7;
	}
	/* bytes are in reverse; push in correct order */
	for(int i = n - 1; i >= 0; i--)
		if(!bb_push(b, buf[i])) return false;
	return true;
}

/* ── Write a single track ─────────────────────────────────────────────────── */

static bool write_track(ByteBuf *out, const SS_MIDITrack *track) {
	ByteBuf tb = { NULL, 0, 0 };
	uint32_t current_tick = 0;
	uint8_t running_byte = 0; /* 0 = none */

	for(size_t i = 0; i < track->event_count; i++) {
		const SS_MIDIMessage *e = &track->events[i];
		uint8_t sb = e->status_byte;

		/* Skip EndOfTrack here; we append it manually at the end */
		if(sb == SS_META_END_OF_TRACK) {
			uint32_t delta = e->ticks > current_tick ? e->ticks - current_tick : 0;
			current_tick += delta;
			continue;
		}

		uint32_t delta = e->ticks > current_tick ? e->ticks - current_tick : 0;
		current_tick += delta;

		if(!bb_push_vlq(&tb, delta)) goto fail;

		/* Meta event: sb <= 0x7F (stored directly as the meta type) */
		if(sb < 0x80) {
			/* FF <type> <vlq-length> <data> */
			if(!bb_push(&tb, 0xFF)) goto fail;
			if(!bb_push(&tb, sb)) goto fail;
			if(!bb_push_vlq(&tb, e->data_length)) goto fail;
			if(e->data_length > 0)
				if(!bb_push_bytes(&tb, e->data, e->data_length)) goto fail;
			running_byte = 0; /* meta cancels running status */

		} else if(sb == 0xF0 || sb == 0xF7) {
			/* SysEx: F0 <vlq-length> <data> */
			if(!bb_push(&tb, sb)) goto fail;
			if(!bb_push_vlq(&tb, e->data_length)) goto fail;
			if(e->data_length > 0)
				if(!bb_push_bytes(&tb, e->data, e->data_length)) goto fail;
			running_byte = 0;

		} else {
			/* Voice message — use running status */
			if(running_byte != sb) {
				running_byte = sb;
				if(!bb_push(&tb, sb)) goto fail;
			}
			if(e->data_length > 0)
				if(!bb_push_bytes(&tb, e->data, e->data_length)) goto fail;
		}
	}

	/* Write EndOfTrack: delta=0, FF 2F 00 */
	if(!bb_push(&tb, 0x00)) goto fail;
	if(!bb_push(&tb, 0xFF)) goto fail;
	if(!bb_push(&tb, SS_META_END_OF_TRACK)) goto fail;
	if(!bb_push(&tb, 0x00)) goto fail;

	/* Write MTrk header + track body into out */
	if(!bb_push_str(out, "MTrk")) goto fail;
	if(!bb_push_be32(out, (uint32_t)tb.len)) goto fail;
	if(!bb_push_bytes(out, tb.data, tb.len)) goto fail;

	free(tb.data);
	return true;
fail:
	free(tb.data);
	return false;
}

/* ── Public serializer ────────────────────────────────────────────────────── */

bool ss_midi_write(const SS_MIDIFile *midi, uint8_t **out_data, size_t *out_size) {
	if(!midi || !out_data || !out_size) return false;
	if(midi->track_count == 0) return false;

	ByteBuf out = { NULL, 0, 0 };

	/* MThd header */
	if(!bb_push_str(&out, "MThd")) goto fail;
	if(!bb_push_be32(&out, 6)) goto fail; /* chunk size */
	if(!bb_push_be16(&out, (uint16_t)(0x00 | midi->format))) goto fail;
	if(!bb_push_be16(&out, (uint16_t)midi->track_count)) goto fail;
	if(!bb_push_be16(&out, midi->time_division)) goto fail;

	/* Track chunks */
	for(size_t ti = 0; ti < midi->track_count; ti++) {
		if(!write_track(&out, &midi->tracks[ti])) goto fail;
	}

	*out_data = out.data;
	*out_size = out.len;
	return true;

fail:
	free(out.data);
	*out_data = NULL;
	*out_size = 0;
	return false;
}
