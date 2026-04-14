#ifndef SS_MIDI_H
#define SS_MIDI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if __has_include(<spessasynth_core/generator_types.h>)
#include <spessasynth_core/midi_enums.h>
#include <spessasynth_core/file.h>
#else
#include "spessasynth/midi/midi_enums.h"
#include "spessasynth/utils/file.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── A single MIDI event ─────────────────────────────────────────────────── */

typedef struct {
	size_t ticks; /* absolute ticks from track start */
	uint8_t status_byte; /* for voice: 0x80-0xEF; for meta/sysex: type byte */
	uint8_t *data; /* owned; NULL for events with no data bytes */
	size_t data_length;
} SS_MIDIMessage;

/** Free the data buffer inside a message (does not free the struct itself). */
void ss_midi_message_free_data(SS_MIDIMessage *msg);

/* ── A single MIDI track ─────────────────────────────────────────────────── */

typedef struct {
	char name[256];
	int port; /* -1 if not set */
	uint8_t channels_used; /* bitmask for channels 0-7; extended as needed */
	uint32_t channels_used_hi; /* channels 8-15 */
	SS_MIDIMessage *events;
	size_t event_count;
	size_t event_capacity;
} SS_MIDITrack;

SS_MIDITrack *ss_midi_track_new(void);
void ss_midi_track_free(SS_MIDITrack *track);
bool ss_midi_track_push_event(SS_MIDITrack *track, SS_MIDIMessage msg);
void ss_midi_track_delete_event(SS_MIDITrack *track, size_t index);

/* ── Tempo change entry ───────────────────────────────────────────────────── */

typedef struct {
	size_t ticks;
	double tempo; /* BPM */
} SS_TempoChange;

/* ── Loop points ─────────────────────────────────────────────────────────── */

typedef struct {
	size_t start;
	size_t end;
} SS_MIDILoop;

/* ── RMID info fields ─────────────────────────────────────────────────────── */

typedef struct {
	uint8_t *name;
	size_t name_len;
	uint8_t *artist;
	size_t artist_len;
	uint8_t *album;
	size_t album_len;
	uint8_t *genre;
	size_t genre_len;
	uint8_t *picture;
	size_t picture_len; /* raw image bytes */
	uint8_t *comment;
	size_t comment_len;
	uint8_t *copyright;
	size_t copyright_len;
	uint8_t *creation_date;
	size_t creation_date_len;
	uint8_t *info_encoding;
	size_t info_encoding_len;
	/* Additional fields matching TypeScript rmidiInfo */
	uint8_t *engineer;
	size_t engineer_len;
	uint8_t *software;
	size_t software_len;
	uint8_t *subject;
	size_t subject_len;
	uint8_t *midi_encoding;
	size_t midi_encoding_len;
} SS_RMIDIInfo;

void ss_rmidi_info_free(SS_RMIDIInfo *info);

/* ── The MIDI file ────────────────────────────────────────────────────────── */

typedef struct {
	SS_MIDITrack *tracks;
	size_t track_count;
	size_t track_capacity;
	uint16_t time_division;
	uint8_t format; /* 0, 1, or 2 */
	double duration; /* seconds */
	size_t first_note_on; /* ticks */
	size_t last_voice_event_tick;
	SS_MIDILoop loop;
	struct {
		uint32_t min;
		uint32_t max;
	} key_range;

	SS_TempoChange *tempo_changes; /* reversed order: last to first */
	size_t tempo_change_count;
	size_t tempo_change_capacity;

	int *port_channel_offset_map;
	size_t port_channel_offset_map_count;

	/* RMID fields */
	int bank_offset;
	bool is_dls_rmidi;
	bool is_karaoke;
	bool is_multi_port;
	SS_RMIDIInfo rmidi_info;

	/* Embedded sound bank (owned) */
	uint8_t *embedded_soundbank;
	size_t embedded_soundbank_size;

	/* Binary name (owned, raw bytes) */
	uint8_t *binary_name;
	size_t binary_name_length;

	char file_name[512];
} SS_MIDIFile;

SS_MIDIFile *ss_midi_new(void);
void ss_midi_free(SS_MIDIFile *midi);

/** Parse a MIDI/RMIDI/XMF file from a raw buffer. Returns NULL on error. */
SS_MIDIFile *ss_midi_load(SS_File *file, const char *file_name);

/** Serialize to Standard MIDI File format.  Caller must free() the buffer. */
bool ss_midi_write(const SS_MIDIFile *midi, SS_File *file);

/** Convert MIDI ticks to seconds using the embedded tempo map. */
double ss_midi_ticks_to_seconds(const SS_MIDIFile *midi, size_t ticks);

/** Rebuild internal caches (tempo map, loop, ports, name). Call after editing. */
void ss_midi_flush(SS_MIDIFile *midi);

#ifdef __cplusplus
}
#endif

#endif /* SS_MIDI_H */
