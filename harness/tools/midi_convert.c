/* Re-emits any MIDI the port can parse as a standard MIDI file.
 *
 * Upstream reads SMF and RMIDI only, so a format the port supports on its own
 * -- HMI, XMI, MUS -- has no reference render to compare against.  Converting
 * first gives both engines the same input and puts the file back inside the
 * harness. */
#include <stdio.h>
#include <stdlib.h>

#include "spessasynth/midi/midi.h"
#include "spessasynth/utils/file.h"

int main(int argc, char **argv) {
	if(argc < 3) {
		fprintf(stderr, "usage: midi_convert <in> <out.mid>\n");
		return 2;
	}

	SS_File *in = ss_file_open_from_file(argv[1]);
	if(!in) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
	SS_MIDIFile *midi = ss_midi_load(in, argv[1]);
	ss_file_close(in);
	if(!midi) { fprintf(stderr, "cannot parse %s\n", argv[1]); return 1; }

	SS_File *out = ss_file_open_blank_file(argv[2]);
	if(!out) { fprintf(stderr, "cannot create %s\n", argv[2]); return 1; }
	const bool ok = ss_midi_write(midi, out);
	ss_file_close(out);
	if(!ok) { fprintf(stderr, "write failed\n"); return 1; }

	printf("%s -> %s: %zu tracks, division %d, first note %zu, loop %zu..%zu\n",
	       argv[1], argv[2], midi->track_count, (int)midi->time_division,
	       midi->first_note_on, midi->loop.start, midi->loop.end);
	return 0;
}
