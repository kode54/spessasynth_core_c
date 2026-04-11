/**
 * midi_loader.c
 * SMF / RMIDI parser.  Port of midi_loader.ts + basic_midi.ts (parseInternal).
 *
 * Supports:
 *   - Standard MIDI File (SMF) format 0, 1, 2
 *   - RIFF-MIDI (RMIDI) with embedded SF2/DLS soundbank
 *   - RMIDI LIST/INFO metadata (INAM, IART, IALB/IPRD, IGNR, IPIC, ICOP, ICMT, ICRD/ICRT, IENC, DBNK)
 *   - Loop-point detection (CC2/4/111/116/117, marker "loopstart"/"loopend")
 *   - Karaoke detection (@KMIDI KARAOKE FILE)
 *   - MIDI Tuning Standard meta events (ignored at load time; applied by processor)
 *
 * XMF support is not implemented (would require a separate xmf_loader.c).
 */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strncasecmp */

#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/indexed_byte_array.h>
#include <spessasynth_core/midi.h>
#else
#include "spessasynth/midi/midi.h"
#include "spessasynth/utils/indexed_byte_array.h"
#endif

/* ── Data-bytes-per-message-type table ───────────────────────────────────── */
/* Indexed by (status >> 4), values 8..E */
static const int DATA_BYTES[16] = {
	0, 0, 0, 0, 0, 0, 0, 0, /* 0-7: unused */
	2, /* 8x note off        */
	2, /* 9x note on         */
	2, /* Ax poly pressure   */
	2, /* Bx controller      */
	1, /* Cx program change  */
	1, /* Dx channel pressure*/
	2, /* Ex pitch wheel     */
	0 /* Fx sys / meta      */
};

/* ── Track helpers ───────────────────────────────────────────────────────── */

SS_MIDITrack *ss_midi_track_new(void) {
	return (SS_MIDITrack *)calloc(1, sizeof(SS_MIDITrack));
}

void ss_midi_track_free(SS_MIDITrack *t) {
	if(!t) return;
	for(size_t i = 0; i < t->event_count; i++)
		free(t->events[i].data);
	free(t->events);
	free(t);
}

bool ss_midi_track_push_event(SS_MIDITrack *t, SS_MIDIMessage msg) {
	if(t->event_count >= t->event_capacity) {
		size_t nc = t->event_capacity ? t->event_capacity * 2 : 256;
		SS_MIDIMessage *tmp = (SS_MIDIMessage *)realloc(t->events,
		                                                nc * sizeof(*tmp));
		if(!tmp) return false;
		t->events = tmp;
		t->event_capacity = nc;
	}
	t->events[t->event_count++] = msg;
	return true;
}

void ss_midi_track_delete_event(SS_MIDITrack *t, size_t idx) {
	if(idx >= t->event_count) return;
	free(t->events[idx].data);
	memmove(&t->events[idx], &t->events[idx + 1],
	        (t->event_count - idx - 1) * sizeof(t->events[0]));
	t->event_count--;
}

void ss_midi_message_free_data(SS_MIDIMessage *msg) {
	if(!msg) return;
	free(msg->data);
	msg->data = NULL;
}

/* ── SS_MIDIFile lifecycle ───────────────────────────────────────────────── */

SS_MIDIFile *ss_midi_new(void) {
	SS_MIDIFile *m = (SS_MIDIFile *)calloc(1, sizeof(SS_MIDIFile));
	if(!m) return NULL;
	m->bank_offset = 0;
	return m;
}

void ss_rmidi_info_free(SS_RMIDIInfo *info) {
	if(!info) return;
	free(info->name);
	free(info->artist);
	free(info->album);
	free(info->genre);
	free(info->picture);
	free(info->comment);
	free(info->copyright);
	free(info->creation_date);
	free(info->info_encoding);
	free(info->engineer);
	free(info->software);
	free(info->subject);
	free(info->midi_encoding);
	memset(info, 0, sizeof(*info));
}

void ss_midi_free(SS_MIDIFile *m) {
	if(!m) return;
	for(size_t i = 0; i < m->track_count; i++)
		ss_midi_track_free(&m->tracks[i]);
	free(m->tracks);
	free(m->tempo_changes);
	free(m->port_channel_offset_map);
	free(m->embedded_soundbank);
	free(m->binary_name);
	ss_rmidi_info_free(&m->rmidi_info);
	free(m);
}

/* ── Tempo map ────────────────────────────────────────────────────────────── */

static bool midi_push_tempo(SS_MIDIFile *m, uint32_t ticks, double bpm) {
	if(m->tempo_change_count >= m->tempo_change_capacity) {
		size_t nc = m->tempo_change_capacity ? m->tempo_change_capacity * 2 : 16;
		SS_TempoChange *tmp = (SS_TempoChange *)realloc(m->tempo_changes,
		                                                nc * sizeof(*tmp));
		if(!tmp) return false;
		m->tempo_changes = tmp;
		m->tempo_change_capacity = nc;
	}
	m->tempo_changes[m->tempo_change_count].ticks = ticks;
	m->tempo_changes[m->tempo_change_count].tempo = bpm;
	m->tempo_change_count++;
	return true;
}

/* Sort tempo changes by tick descending (like TS: last → first) */
static int tempo_cmp_desc(const void *a, const void *b) {
	const SS_TempoChange *ta = (const SS_TempoChange *)a;
	const SS_TempoChange *tb = (const SS_TempoChange *)b;
	if(tb->ticks > ta->ticks) return 1;
	if(tb->ticks < ta->ticks) return -1;
	return 0;
}

double ss_midi_ticks_to_seconds(const SS_MIDIFile *m, uint32_t ticks_in) {
	uint32_t ticks = ticks_in;
	if(m->tempo_change_count == 0 || m->time_division == 0) return 0.0;
	double total = 0.0;
	for(size_t i = 0; i < m->tempo_change_count; i++) {
		const SS_TempoChange *tc = &m->tempo_changes[i];
		if(tc->ticks > ticks) continue;
		uint32_t delta = ticks - tc->ticks;
		total += (double)delta * 60.0 / (tc->tempo * (double)m->time_division);
		ticks = tc->ticks;
		if(ticks == 0) break;
	}
	return total;
}

/* ── RMIDI info field copy ───────────────────────────────────────────────── */

static void rmidi_set_field(uint8_t **dst, size_t *dst_len,
                            const uint8_t *src, size_t len) {
	free(*dst);
	*dst = (uint8_t *)malloc(len);
	if(*dst) {
		memcpy(*dst, src, len);
		*dst_len = len;
	} else
		*dst_len = 0;
}

/* ── Read a RIFF chunk (8-byte header + data) ─────────────────────────────── */

#if 0
typedef struct {
    char      header[5];
    uint32_t  size;   /* data size, not including 8-byte chunk header */
    size_t    data_offset; /* offset into parent IBA where data starts */
} RiffChunk;

static bool read_riff_chunk(SS_IBA *iba, RiffChunk *out)
{
    if (iba->current_index + 8 > iba->length) return false;
    memcpy(out->header, iba->data + iba->current_index, 4);
    out->header[4] = '\0';
    iba->current_index += 4;
    /* RIFF size is little-endian */
    out->size = (uint32_t)iba->data[iba->current_index]
              | ((uint32_t)iba->data[iba->current_index + 1] <<  8)
              | ((uint32_t)iba->data[iba->current_index + 2] << 16)
              | ((uint32_t)iba->data[iba->current_index + 3] << 24);
    iba->current_index += 4;
    out->data_offset = iba->current_index;
    return true;
}

/* Read a MIDI chunk (4-byte type + 4-byte big-endian size) */
typedef struct {
    char      type[5];
    uint32_t  size;
    size_t    data_start; /* index into containing buffer after header */
} MIDIChunkHdr;

static bool read_midi_chunk(SS_IBA *iba, MIDIChunkHdr *out)
{
    if (iba->current_index + 8 > iba->length) return false;
    memcpy(out->type, iba->data + iba->current_index, 4);
    out->type[4] = '\0';
    iba->current_index += 4;
    uint8_t *p = iba->data + iba->current_index;
    out->size = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
              | ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
    iba->current_index += 4;
    out->data_start = iba->current_index;
    return true;
}
#endif

/* ── VLQ read from a local cursor ─────────────────────────────────────────── */

static uint32_t read_vlq(const uint8_t *buf, size_t buf_len, size_t *pos) {
	uint32_t v = 0;
	for(int i = 0; i < 4; i++) {
		if(*pos >= buf_len) break;
		uint8_t b = buf[(*pos)++];
		v = (v << 7) | (b & 0x7F);
		if(!(b & 0x80)) break;
	}
	return v;
}

/* ── Read 3-byte big-endian tempo (µs/beat) → BPM ──────────────────────── */

static double read_tempo_bpm(const uint8_t *d) {
	uint32_t us = ((uint32_t)d[0] << 16) | ((uint32_t)d[1] << 8) | d[2];
	if(us == 0) us = 500000;
	return 60000000.0 / (double)us;
}

/* ── Lowercase trim for marker comparison ────────────────────────────────── */

static void str_lower_trim(const char *src, size_t len,
                           char *dst, size_t dst_size) {
	size_t i = 0, j = 0;
	/* Skip leading whitespace */
	while(i < len && isspace((unsigned char)src[i])) i++;
	/* Copy and lower */
	while(i < len && j + 1 < dst_size) {
		char c = (char)tolower((unsigned char)src[i++]);
		if(!isspace((unsigned char)c))
			dst[j++] = c;
		else { /* collapse trailing space */
			dst[j++] = c;
		}
	}
	/* Trim trailing whitespace */
	while(j > 0 && isspace((unsigned char)dst[j - 1])) j--;
	dst[j] = '\0';
}

/* ── parseInternal — builds tempo map, loop, duration, key range ─────────── */

static void midi_parse_internal(SS_MIDIFile *m) {
	/* Reset all derived values */
	m->tempo_change_count = 0;
	m->first_note_on = 0;
	m->last_voice_event_tick = 0;
	m->loop.start = 0;
	m->loop.end = 0;
	m->key_range.min = 127;
	m->key_range.max = 0;
	m->is_karaoke = false;
	m->is_multi_port = false;

	/* Seed tempo map with 120 BPM at tick 0 */
	midi_push_tempo(m, 0, 120.0);

	bool loop_start_found = false;
	int loop_end_count = 0;
	bool first_note_set = false;
	bool karaoke_has_title = false;

	/* Port tracking: map port_num → channel_offset */
	int max_port = 0;
	int port_offsets[64];
	memset(port_offsets, -1, sizeof(port_offsets));
	port_offsets[0] = 0;

	for(size_t ti = 0; ti < m->track_count; ti++) {
		SS_MIDITrack *track = &m->tracks[ti];
		int track_port = (track->port >= 0) ? track->port : 0;
		if(track_port > max_port) max_port = track_port;

		for(size_t ei = 0; ei < track->event_count; ei++) {
			SS_MIDIMessage *e = &track->events[ei];
			uint8_t sb = e->status_byte;

			/* ── Voice message ──────────────────────────────────────────── */
			if(sb >= 0x80 && sb < 0xF0) {
				if(e->ticks > m->last_voice_event_tick)
					m->last_voice_event_tick = e->ticks;

				uint8_t type = sb & 0xF0;
				uint8_t channel = sb & 0x0F;
				(void)channel;

				if(type == 0x90 && e->data_length >= 2) {
					/* Note-on */
					uint8_t note = e->data[0];
					uint8_t vel = e->data[1];
					if(vel > 0) {
						if(!first_note_set) {
							m->first_note_on = e->ticks;
							first_note_set = true;
						}
						if(note < m->key_range.min) m->key_range.min = note;
						if(note > m->key_range.max) m->key_range.max = note;
					}
				} else if(type == 0xB0 && e->data_length >= 2) {
					/* Controller change — loop detection */
					uint8_t cc = e->data[0];
					uint8_t val = e->data[1];
					(void)val;
					switch(cc) {
						case 2: /* Touhou loop start */
						case 111: /* RPG Maker */
						case 116: /* EMIDI/XMI loop start */
							m->loop.start = e->ticks;
							loop_start_found = true;
							break;
						case 4: /* Touhou loop end */
						case 117: /* EMIDI/XMI loop end */
							if(loop_end_count == 0) {
								m->loop.end = e->ticks;
							} else {
								m->loop.end = 0; /* duplicate → not a real loop end */
							}
							loop_end_count++;
							break;
						case 0: /* Bank select MSB — detect DLS RMIDI bank offset */
							if(m->is_dls_rmidi && e->data_length >= 2 &&
							   e->data[1] != 0 && e->data[1] != 127) {
								m->bank_offset = 1;
							}
							break;
					}
				}
			}

			/* ── Meta event ─────────────────────────────────────────────── */
			switch(sb) {
				case SS_META_END_OF_TRACK:
					/* Remove mid-track End of Track events */
					if(ei != track->event_count - 1) {
						ss_midi_track_delete_event(track, ei);
						ei--;
					}
					break;

				case SS_META_SET_TEMPO:
					if(e->data_length >= 3)
						midi_push_tempo(m, e->ticks, read_tempo_bpm(e->data));
					break;

				case SS_META_TRACK_NAME:
					if(e->data_length > 0 && track->name[0] == '\0') {
						size_t copy = e->data_length < 255 ? e->data_length : 255;
						memcpy(track->name, e->data, copy);
						track->name[copy] = '\0';
						/* Use track name as MIDI name if none found yet */
						if(m->binary_name == NULL) {
							m->binary_name = (uint8_t *)malloc(copy);
							if(m->binary_name) {
								memcpy(m->binary_name, e->data, copy);
								m->binary_name_length = copy;
							}
						}
					}
					break;

				case SS_META_MIDI_PORT:
					if(e->data_length >= 1) {
						int port = (int)e->data[0];
						track->port = port;
						if(port > 0) m->is_multi_port = true;
						if(port > max_port) max_port = port;
					}
					break;

				case SS_META_MARKER: {
					/* Loop-point markers: "loopstart" / "loopend" / "start" */
					char lower[64];
					str_lower_trim((const char *)e->data,
					               e->data_length < 63 ? e->data_length : 63,
					               lower, sizeof(lower));
					if(strcmp(lower, "loopstart") == 0 ||
					   strcmp(lower, "start") == 0) {
						m->loop.start = e->ticks;
						loop_start_found = true;
					} else if(strcmp(lower, "loopend") == 0) {
						m->loop.end = e->ticks;
						loop_end_count++;
					}
					break;
				}

				case SS_META_TEXT: {
					if(e->data_length == 0) break;
					char buf[32];
					size_t clen = e->data_length < 31 ? e->data_length : 31;
					memcpy(buf, e->data, clen);
					buf[clen] = '\0';
					/* Karaoke detection */
					if(strstr(buf, "@KMIDI") || strstr(buf, "KARAOKE")) {
						m->is_karaoke = true;
					}
					break;
				}

				case SS_META_LYRIC: {
					if(e->data_length == 0) break;
					char buf[32];
					size_t clen = e->data_length < 31 ? e->data_length : 31;
					memcpy(buf, e->data, clen);
					buf[clen] = '\0';
					if(strstr(buf, "@KMIDI") || strstr(buf, "KARAOKE")) {
						m->is_karaoke = true;
					}
					break;
				}
			}
		}
	}

	/* If only loop start found, loop end = last voice event */
	if(loop_start_found && m->loop.end == 0)
		m->loop.end = m->last_voice_event_tick;

	/* Sort tempo changes descending by tick (last → first) */
	qsort(m->tempo_changes, m->tempo_change_count,
	      sizeof(m->tempo_changes[0]), tempo_cmp_desc);

	/* Compute duration */
	m->duration = ss_midi_ticks_to_seconds(m, m->last_voice_event_tick);

	/* Build port_channel_offset_map */
	if(m->is_multi_port) {
		size_t map_size = (size_t)(max_port + 1);
		free(m->port_channel_offset_map);
		m->port_channel_offset_map = (int *)calloc(map_size, sizeof(int));
		if(m->port_channel_offset_map) {
			for(int p = 0; p <= max_port; p++)
				m->port_channel_offset_map[p] = p * 16;
			m->port_channel_offset_map_count = map_size;
		}
	}

	(void)karaoke_has_title;
}

/* ── Parse a single MTrk chunk ───────────────────────────────────────────── */

static bool parse_track(SS_MIDIFile *m, const uint8_t *buf, size_t buf_len,
                        uint32_t start_ticks) {
	SS_MIDITrack *track = &m->tracks[m->track_count++];
	memset(track, 0, sizeof(*track));
	track->port = -1;

	size_t pos = 0;
	uint32_t abs_tick = start_ticks;
	uint8_t running = 0; /* running status byte */

	while(pos < buf_len) {
		/* Delta time */
		abs_tick += read_vlq(buf, buf_len, &pos);

		if(pos >= buf_len) break;

		uint8_t status_check = buf[pos];
		uint8_t status_byte;

		if(status_check >= 0x80) {
			status_byte = status_check;
			pos++;

			if(status_byte == 0xFF) {
				/* Meta event: next byte is the meta type */
				if(pos >= buf_len) break;
				uint8_t meta_type = buf[pos++];
				uint32_t meta_len = read_vlq(buf, buf_len, &pos);

				SS_MIDIMessage msg;
				msg.ticks = abs_tick;
				msg.status_byte = meta_type;
				msg.data_length = meta_len;
				msg.data = NULL;

				if(meta_len > 0 && pos + meta_len <= buf_len) {
					msg.data = (uint8_t *)malloc(meta_len);
					if(msg.data) memcpy(msg.data, buf + pos, meta_len);
				}
				pos += meta_len;
				ss_midi_track_push_event(track, msg);
				/* Don't update running status for meta events */
				continue;

			} else if(status_byte == 0xF0 || status_byte == 0xF7) {
				/* SysEx */
				uint32_t slen = read_vlq(buf, buf_len, &pos);
				SS_MIDIMessage msg;
				msg.ticks = abs_tick;
				msg.status_byte = status_byte;
				msg.data_length = slen;
				msg.data = NULL;
				if(slen > 0 && pos + slen <= buf_len) {
					msg.data = (uint8_t *)malloc(slen);
					if(msg.data) memcpy(msg.data, buf + pos, slen);
				}
				pos += slen;
				ss_midi_track_push_event(track, msg);
				running = 0; /* clear running status */
				continue;

			} else {
				/* Voice message — update running status */
				running = status_byte;
			}
		} else {
			/* Use running status */
			if(running == 0) {
				/* No running status — corrupt file, skip */
				continue;
			}
			status_byte = running;
			/* Do NOT advance pos — the byte at pos is data */
		}

		/* Voice message data */
		int data_bytes = DATA_BYTES[(status_byte >> 4) & 0xF];
		if(pos + (size_t)data_bytes > buf_len) break;

		SS_MIDIMessage msg;
		msg.ticks = abs_tick;
		msg.status_byte = status_byte;
		msg.data_length = (uint32_t)data_bytes;
		msg.data = NULL;

		if(data_bytes > 0) {
			msg.data = (uint8_t *)malloc((size_t)data_bytes);
			if(msg.data) memcpy(msg.data, buf + pos, (size_t)data_bytes);
		}
		pos += (size_t)data_bytes;
		ss_midi_track_push_event(track, msg);
	}

	return true;
}

/* ── Public loader ───────────────────────────────────────────────────────── */

SS_MIDIFile *ss_midi_load(const uint8_t *data, size_t size, const char *file_name) {
	if(!data || size < 14) return NULL;

	SS_MIDIFile *m = ss_midi_new();
	if(!m) return NULL;

	if(file_name && *file_name)
		strncpy(m->file_name, file_name, sizeof(m->file_name) - 1);

	/* ── Detect file type ──────────────────────────────────────────────── */
	const uint8_t *smf_data = data;
	size_t smf_size = size;

	/* RMIDI: starts with "RIFF" */
	if(data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F') {
		/* Validate: offset 8 must be "RMID" */
		if(size < 12) goto bad;
		if(memcmp(data + 8, "RMID", 4) != 0) goto bad;

		bool is_sf2_rmidi = false;
		bool found_dbnk = false;

		/* First RIFF chunk after "RIFF????RMID" is the "data" chunk
		 * containing the raw SMF bytes */
		size_t pos = 12;
		/* Read "data" sub-chunk */
		if(pos + 8 > size) goto bad;
		if(memcmp(data + pos, "data", 4) != 0) goto bad;
		uint32_t smf_chunk_size = (uint32_t)data[pos + 4] | ((uint32_t)data[pos + 5] << 8) | ((uint32_t)data[pos + 6] << 16) | ((uint32_t)data[pos + 7] << 24);
		pos += 8;
		smf_data = data + pos;
		smf_size = smf_chunk_size;
		pos += smf_chunk_size;
		if(pos & 1) pos++; /* RIFF pads to even */

		/* Scan remaining chunks for RIFF(sfbk/dls) and LIST(INFO) */
		while(pos + 8 <= size) {
			char chunk_id[5];
			memcpy(chunk_id, data + pos, 4);
			chunk_id[4] = '\0';
			uint32_t csz = (uint32_t)data[pos + 4] | ((uint32_t)data[pos + 5] << 8) | ((uint32_t)data[pos + 6] << 16) | ((uint32_t)data[pos + 7] << 24);
			size_t cdata_start = pos + 8;
			size_t cdata_end = cdata_start + csz;
			if(cdata_end > size) cdata_end = size;
			pos += 8 + csz;
			if(pos & 1) pos++;

			if(strcmp(chunk_id, "RIFF") == 0) {
				/* Sub-RIFF: check 4-byte type */
				if(cdata_start + 4 > size) continue;
				char sub_type[5];
				memcpy(sub_type, data + cdata_start, 4);
				sub_type[4] = '\0';
				/* lowercase compare */
				for(int k = 0; k < 4; k++)
					sub_type[k] = (char)tolower((unsigned char)sub_type[k]);

				if(strcmp(sub_type, "sfbk") == 0 ||
				   strcmp(sub_type, "sfpk") == 0 ||
				   strcmp(sub_type, "dls ") == 0) {
					/* Embedded soundbank */
					size_t bank_size = cdata_end - (cdata_start - 8); /* include 8-byte hdr */
					size_t bank_start = cdata_start - 8;
					m->embedded_soundbank = (uint8_t *)malloc(bank_size);
					if(m->embedded_soundbank) {
						memcpy(m->embedded_soundbank, data + bank_start, bank_size);
						m->embedded_soundbank_size = bank_size;
					}
					if(strcmp(sub_type, "dls ") == 0) {
						m->is_dls_rmidi = true;
					} else {
						is_sf2_rmidi = true;
					}
				}
			} else if(strcmp(chunk_id, "LIST") == 0) {
				/* LIST/INFO */
				if(cdata_start + 4 > size) continue;
				if(memcmp(data + cdata_start, "INFO", 4) != 0) continue;

				size_t ip = cdata_start + 4;
				while(ip + 8 <= cdata_end) {
					char ifid[5];
					memcpy(ifid, data + ip, 4);
					ifid[4] = '\0';
					uint32_t ifsz = (uint32_t)data[ip + 4] | ((uint32_t)data[ip + 5] << 8) | ((uint32_t)data[ip + 6] << 16) | ((uint32_t)data[ip + 7] << 24);
					ip += 8;
					size_t ifend = ip + ifsz;
					if(ifend > cdata_end) ifend = cdata_end;

					const uint8_t *ifd = data + ip;
					size_t ifdlen = ifend - ip;

#define RMIDI_SET(field) rmidi_set_field(&m->rmidi_info.field, &m->rmidi_info.field##_len, ifd, ifdlen)
					if(strcmp(ifid, "INAM") == 0) {
						RMIDI_SET(name);
					} else if(strcmp(ifid, "IART") == 0) {
						RMIDI_SET(artist);
					} else if(strcmp(ifid, "IALB") == 0) {
						RMIDI_SET(album);
					} else if(strcmp(ifid, "IPRD") == 0) {
						RMIDI_SET(album);
					} else if(strcmp(ifid, "IGNR") == 0) {
						RMIDI_SET(genre);
					} else if(strcmp(ifid, "IPIC") == 0) {
						RMIDI_SET(picture);
					} else if(strcmp(ifid, "ICOP") == 0) {
						RMIDI_SET(copyright);
					} else if(strcmp(ifid, "ICMT") == 0) {
						RMIDI_SET(comment);
					} else if(strcmp(ifid, "ICRD") == 0) {
						RMIDI_SET(creation_date);
					} else if(strcmp(ifid, "ICRT") == 0) {
						RMIDI_SET(creation_date);
					} else if(strcmp(ifid, "IENC") == 0) {
						RMIDI_SET(info_encoding);
					} else if(strcmp(ifid, "IENG") == 0) {
						RMIDI_SET(engineer);
					} else if(strcmp(ifid, "ISFT") == 0) {
						RMIDI_SET(software);
					} else if(strcmp(ifid, "ISBJ") == 0) {
						RMIDI_SET(subject);
					} else if(strcmp(ifid, "MENC") == 0) {
						RMIDI_SET(midi_encoding);
					} else if(strcmp(ifid, "DBNK") == 0) {
						/* 2-byte little-endian bank offset */
						if(ifdlen >= 2)
							m->bank_offset = (int)((uint16_t)ifd[0] | ((uint16_t)ifd[1] << 8));
						found_dbnk = true;
					}
#undef RMIDI_SET
					ip = ifend;
					if(ip & 1) ip++;
				}
			}
		}

		/* Bank offset defaults */
		if(is_sf2_rmidi && !found_dbnk)
			m->bank_offset = 1;
		if(m->is_dls_rmidi)
			m->bank_offset = 0;
		if(!m->embedded_soundbank)
			m->bank_offset = 0;

	} else if(data[0] == 'X' && data[1] == 'M' && data[2] == 'F' && data[3] == '_') {
		/* XMF: not implemented — treat as plain SMF and hope for the best */
		smf_data = data;
		smf_size = size;
	}
	/* else: plain SMF */

	/* ── Parse SMF ─────────────────────────────────────────────────────── */
	{
		size_t pos = 0;

		/* MThd */
		if(pos + 8 > smf_size) goto bad;
		if(memcmp(smf_data + pos, "MThd", 4) != 0) goto bad;
		uint32_t hdr_size = ((uint32_t)smf_data[pos + 4] << 24) | ((uint32_t)smf_data[pos + 5] << 16) | ((uint32_t)smf_data[pos + 6] << 8) | (uint32_t)smf_data[pos + 7];
		pos += 8;
		if(hdr_size < 6 || pos + 6 > smf_size) goto bad;

		m->format = smf_data[pos + 1]; /* low byte of big-endian uint16 */
		uint16_t n_tracks = (uint16_t)(((uint16_t)smf_data[pos + 2] << 8) | smf_data[pos + 3]);
		m->time_division = (uint16_t)(((uint16_t)smf_data[pos + 4] << 8) | smf_data[pos + 5]);
		pos += hdr_size;

		/* Allocate track array */
		m->tracks = (SS_MIDITrack *)calloc(n_tracks, sizeof(SS_MIDITrack));
		if(!m->tracks) goto bad;
		m->track_capacity = n_tracks;

		uint32_t prev_track_end_ticks = 0;

		for(uint16_t ti = 0; ti < n_tracks; ti++) {
			if(pos + 8 > smf_size) break;
			if(memcmp(smf_data + pos, "MTrk", 4) != 0) {
				/* Skip unrecognized chunk */
				uint32_t skip_sz = ((uint32_t)smf_data[pos + 4] << 24) | ((uint32_t)smf_data[pos + 5] << 16) | ((uint32_t)smf_data[pos + 6] << 8) | (uint32_t)smf_data[pos + 7];
				pos += 8 + skip_sz;
				continue;
			}
			uint32_t trk_sz = ((uint32_t)smf_data[pos + 4] << 24) | ((uint32_t)smf_data[pos + 5] << 16) | ((uint32_t)smf_data[pos + 6] << 8) | (uint32_t)smf_data[pos + 7];
			pos += 8;

			uint32_t start_ticks = 0;
			/* Format 2: tracks play sequentially; each track starts after the previous */
			if(m->format == 2 && ti > 0) {
				SS_MIDITrack *prev = &m->tracks[ti - 1];
				if(prev->event_count > 0)
					start_ticks = prev->events[prev->event_count - 1].ticks;
			}
			(void)prev_track_end_ticks;

			size_t trk_end = pos + trk_sz;
			if(trk_end > smf_size) trk_end = smf_size;
			parse_track(m, smf_data + pos, trk_end - pos, start_ticks);
			pos = trk_end;
		}
	}

	midi_parse_internal(m);
	return m;

bad:
	ss_midi_free(m);
	return NULL;
}

/* ── flush ────────────────────────────────────────────────────────────────── */

void ss_midi_flush(SS_MIDIFile *m) {
	if(!m) return;
	midi_parse_internal(m);
}
