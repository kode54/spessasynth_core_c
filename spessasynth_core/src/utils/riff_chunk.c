#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/riff_chunk.h>
#else
#include "spessasynth/utils/riff_chunk.h"
#endif

bool ss_riff_read_chunk(SS_File *file, SS_RIFFChunk *out, bool skip_size, bool riff64) {
	const size_t size_size = riff64 ? 8 : 4;
	if(ss_file_remaining(file) < (8 + size_size)) return false;

	size_t pos = ss_file_tell(file);
	ss_file_read_string(file, pos, out->header, 4);
	uint64_t sz = ss_file_read_le(file, pos + 4, size_size);
	out->size = sz;

	if(skip_size) {
		/* The data slice covers everything from current_index to end */
		out->file = ss_file_slice(file, pos + 8, ss_file_remaining(file) - (pos + 8));
	} else {
		/* Non-owning slice of exactly sz bytes */
		size_t start = ss_file_tell(file);
		if(sz > ss_file_remaining(file)) sz = ss_file_remaining(file);

		out->file = ss_file_slice(file, start, sz);

		ss_file_skip(file, sz);
		/* SF2 pads chunks to even size */
		if(sz & 1) ss_file_skip(file, 1);
	}
	return true;
}

bool ss_riff_read_chunk_expect(SS_File *file, SS_RIFFChunk *out,
                               const char *expected_header,
                               bool riff64) {
	if(!ss_riff_read_chunk(file, out, false, riff64)) return false;
	return strcmp(out->header, expected_header) == 0;
}

void ss_riff_close_chunk(SS_RIFFChunk *chunk) {
	ss_file_close(chunk->file);
	chunk->file = NULL;
}
