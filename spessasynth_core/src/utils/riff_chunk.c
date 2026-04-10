#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/riff_chunk.h>
#else
#include "spessasynth/utils/riff_chunk.h"
#endif

bool ss_riff_read_chunk(SS_IBA *iba, SS_RIFFChunk *out, bool skip_size, bool riff64) {
	if(ss_iba_remaining(iba) < (8 + riff64 * 4)) return false;

	ss_iba_read_string(iba, out->header, 4);
	uint64_t sz = ss_iba_read_le(iba, riff64 ? 8 : 4);
	out->size = sz;

	if(skip_size) {
		/* The data slice covers everything from current_index to end */
		ss_iba_wrap(&out->data, iba->data + iba->current_index,
		            iba->length - iba->current_index);
	} else {
		/* Non-owning slice of exactly sz bytes */
		size_t start = iba->current_index;
		size_t end = start + sz;
		if(end > iba->length) end = iba->length;
		ss_iba_wrap(&out->data, iba->data + start, end - start);
		iba->current_index = end;
		/* SF2 pads chunks to even size */
		if(sz & 1) iba->current_index++;
	}
	return true;
}

bool ss_riff_read_chunk_expect(SS_IBA *iba, SS_RIFFChunk *out,
                               const char *expected_header,
                               bool riff64) {
	if(!ss_riff_read_chunk(iba, out, false, riff64)) return false;
	return strcmp(out->header, expected_header) == 0;
}
