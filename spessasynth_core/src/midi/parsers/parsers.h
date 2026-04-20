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

#endif /* SS_MIDI_PARSERS_H */
