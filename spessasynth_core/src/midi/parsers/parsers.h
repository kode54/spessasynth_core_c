#ifndef SS_MIDI_PARSERS_H
#define SS_MIDI_PARSERS_H

#include <stdbool.h>
#include <stddef.h>

#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/midi.h>
#else
#include "spessasynth/midi/midi.h"
#endif

/** Parse a raw Standard MIDI File (starts with "MThd") into m->tracks,
 *  populating m->format, m->time_division, and the per-track events.
 *  Returns false on malformed input. */
bool ss_midi_parse_smf(SS_MIDIFile *m, SS_File *smf_data, size_t smf_size);

/** Parse a RIFF/RMID wrapper: extracts embedded soundbank and INFO
 *  metadata, then invokes ss_midi_parse_smf on the SMF payload.
 *  Returns false on malformed input. */
bool ss_midi_parse_rmidi(SS_MIDIFile *m, SS_File *file, size_t size);

/** Detect a DOOM/Heretic MUS file by magic and header consistency. */
bool ss_midi_is_mus(SS_File *file, size_t size);

/** Parse a MUS file into a single-track format-0 MIDI with a seeded
 *  tempo meta event.  Returns false on malformed input. */
bool ss_midi_parse_mus(SS_MIDIFile *m, SS_File *file, size_t size);

/** Detect a Microsoft DirectMusic Segment (MIDS) file. */
bool ss_midi_is_mids(SS_File *file, size_t size);

/** Parse a MIDS file into a single-track format-0 MIDI, emitting the
 *  embedded tempo meta events and packed voice messages.
 *  Returns false on malformed input. */
bool ss_midi_parse_mids(SS_MIDIFile *m, SS_File *file, size_t size);

/** Detect a Miles Sound System XMI (Extended MIDI) file. */
bool ss_midi_is_xmi(SS_File *file, size_t size);

/** Parse an XMI file.  Single-track files become SMF format 0; multi-
 *  track collections become format 2 with concatenated tick timelines.
 *  Returns false on malformed input. */
bool ss_midi_parse_xmi(SS_MIDIFile *m, SS_File *file, size_t size);

/** Detect a General MIDI Format (GMF) file. */
bool ss_midi_is_gmf(SS_File *file, size_t size);

/** Parse a GMF file into a two-track format-0 MIDI: a conductor track
 *  holding the tempo meta and a Roland GS-style reset SysEx, and an
 *  event track parsed from the raw MIDI stream after the 7-byte header.
 *  Pitch-wheel events are dropped per the reference port's convention.
 *  Returns false on malformed input. */
bool ss_midi_parse_gmf(SS_MIDIFile *m, SS_File *file, size_t size);

/** Detect an HMI Sound Operating System (HMIMIDIP / HMIMIDIR) file. */
bool ss_midi_is_hmp(SS_File *file, size_t size);

/** Parse an HMP file into an SMF format-1 MIDI.  Track 0 is a conductor
 *  seeded with the HMP default tempo; subsequent tracks are parsed from
 *  the HMP event stream (LE 7-bit-group deltas, no running status, no
 *  SysEx).  Returns false on malformed input. */
bool ss_midi_parse_hmp(SS_MIDIFile *m, SS_File *file, size_t size);

/** Detect an HMI-MIDISONG file. */
bool ss_midi_is_hmi(SS_File *file, size_t size);

/** Parse an HMI-MIDISONG file into an SMF format-1 MIDI.  Track 0 is a
 *  conductor holding the default tempo and any loopStart/loopEnd markers
 *  extracted from HMI's 0xFE 0x14/0x15 sub-events.  Subsequent tracks
 *  carry the decoded event streams (MIDI VLQ delta, running status,
 *  note-on duration synthesized into matching note-offs).  Returns false
 *  on malformed input. */
bool ss_midi_parse_hmi(SS_MIDIFile *m, SS_File *file, size_t size);

/** Detect a Loudness Sound System (LDS) tracker file.  Needs a ".lds"
 *  filename extension since the format has no unambiguous magic. */
bool ss_midi_is_lds(SS_File *file, size_t size, const char *file_name);

/** Simulate an LDS tracker playback, translating per-tick state into
 *  MIDI events.  Emits SMF format 1 at TPQN 35 (one MIDI tick per
 *  70 Hz AdLib refresh) with a conductor track plus up to nine channel
 *  tracks.  Returns false on malformed input. */
bool ss_midi_parse_lds(SS_MIDIFile *m, SS_File *file, size_t size);

#endif /* SS_MIDI_PARSERS_H */
