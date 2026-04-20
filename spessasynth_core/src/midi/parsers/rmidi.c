/**
 * rmidi.c
 * RIFF-MIDI (RMID) parser.  Unwraps the RIFF container, extracts the
 * embedded SF2/DLS soundbank and LIST/INFO metadata, then delegates
 * SMF payload parsing to ss_midi_parse_smf.
 *
 * Supported INFO fields: INAM, IART, IALB/IPRD, IGNR, IPIC, ICOP, ICMT,
 * ICRD/ICRT, IENC, IENG, ISFT, ISBJ, MENC, DBNK.
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "parsers.h"

/* ── RMIDI info field copy ───────────────────────────────────────────────── */

static void rmidi_set_field(uint8_t **dst, size_t *dst_len,
                            SS_File *file, size_t offset, size_t len) {
	free(*dst);
	*dst = (uint8_t *)malloc(len);
	if(*dst) {
		ss_file_read_bytes(file, offset, *dst, len);
		*dst_len = len;
	} else
		*dst_len = 0;
}

/* ── Public RMIDI parser ─────────────────────────────────────────────────── */

bool ss_midi_parse_rmidi(SS_MIDIFile *m, SS_File *file, size_t size) {
	if(size < 12) return false;

	size_t riff_size = ss_file_read_le(file, 4, 4);
	char rmid_id[5];
	ss_file_read_string(file, 8, rmid_id, 4);

	if(strcmp(rmid_id, "RMID") != 0) return false;

	bool is_sf2_rmidi = false;
	bool found_dbnk = false;
	SS_File *smf_data = NULL;
	size_t smf_size = 0;

	/* First RIFF chunk after "RIFF????RMID" is the "data" chunk
	 * containing the raw SMF bytes */
	size_t pos = 12;
	if(pos + 8 > size) return false;
	if(riff_size + 8 > size) return false;

	char data_id[5];
	ss_file_read_string(file, pos, data_id, 4);

	if(strcmp(data_id, "data") != 0) return false;

	size_t smf_chunk_size = ss_file_read_le(file, pos + 4, 4);

	pos += 8;

	smf_data = ss_file_slice(file, pos, smf_chunk_size);
	if(!smf_data) return false;
	smf_size = smf_chunk_size;
	pos += smf_chunk_size;
	if(pos & 1) pos++; /* RIFF pads to even */

	/* Scan remaining chunks for RIFF(sfbk/dls) and LIST(INFO) */
	while(pos + 8 <= size) {
		char chunk_id[5];
		ss_file_read_string(file, pos, chunk_id, 4);
		size_t csz = ss_file_read_le(file, pos + 4, 4);
		size_t cdata_start = pos + 8;
		size_t cdata_end = cdata_start + csz;
		if(cdata_end > size) cdata_end = size;
		pos += 8 + csz;
		if(pos & 1) pos++;

		if(strcmp(chunk_id, "RIFF") == 0) {
			/* Sub-RIFF: check 4-byte type */
			if(cdata_start + 4 > size) continue;
			char sub_type[5];
			ss_file_read_string(file, cdata_start, sub_type, 4);
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
					ss_file_read_bytes(file, bank_start, m->embedded_soundbank, bank_size);
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
			char info_id[5];
			ss_file_read_string(file, cdata_start, info_id, 4);
			if(strcmp(info_id, "INFO") != 0) continue;

			size_t ip = cdata_start + 4;
			while(ip + 8 <= cdata_end) {
				char ifid[5];
				ss_file_read_string(file, ip, ifid, 4);
				size_t ifsz = ss_file_read_le(file, ip + 4, 4);
				ip += 8;
				size_t ifend = ip + ifsz;
				if(ifend > cdata_end) ifend = cdata_end;

				const size_t ifd = ip;
				size_t ifdlen = ifend - ip;

#define RMIDI_SET(field) rmidi_set_field(&m->rmidi_info.field, &m->rmidi_info.field##_len, file, ifd, ifdlen)
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
						m->bank_offset = (uint16_t)ss_file_read_le(file, ifd, 2);
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

	bool ok = ss_midi_parse_smf(m, smf_data, smf_size);
	ss_file_close(smf_data);
	return ok;
}
