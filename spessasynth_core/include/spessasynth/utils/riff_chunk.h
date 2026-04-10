#ifndef SS_RIFF_CHUNK_H
#define SS_RIFF_CHUNK_H

#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/indexed_byte_array.h>
#else
#include "indexed_byte_array.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A parsed RIFF chunk with a 4-character header and data slice.
 * data is a non-owning view into the parent buffer.
 */
typedef struct {
	char header[5]; /* 4-char FourCC + NUL */
	size_t size;
	SS_IBA data; /* non-owning slice */
} SS_RIFFChunk;

/**
 * Read a RIFF chunk from iba at its current_index.
 * If skip_size is true the size field is still consumed but the data slice
 * is set to the remaining buffer (used for top-level LIST chunks whose data
 * immediately follows the header).
 *
 * Returns true on success, false on underflow.
 */
bool ss_riff_read_chunk(SS_IBA *iba, SS_RIFFChunk *out, bool skip_size, bool riff64);

/** Convenience: read a chunk and verify its header equals expected_header. */
bool ss_riff_read_chunk_expect(SS_IBA *iba, SS_RIFFChunk *out,
                               const char *expected_header,
                               bool riff64);

#ifdef __cplusplus
}
#endif

#endif /* SS_RIFF_CHUNK_H */
