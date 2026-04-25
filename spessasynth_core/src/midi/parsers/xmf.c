/**
 * xmf.c
 * eXtensible Music Format (XMF / Mobile XMF) parser.  Walks the
 * header tree per RP-030 / RP-039 / RP-043, extracts the embedded
 * SMF FileNode (and any DLS FileNode as the embedded soundbank),
 * and delegates SMF parsing to ss_midi_parse_smf.  zlib is used to
 * inflate packed (compressed) FileNodes.
 *
 * References:
 *   RP-030 "XMF Specification"
 *   RP-039 "XMF Patch Prefix Meta Event"
 *   RP-042 "Mobile XMF Content Format Specification"
 *   RP-043 "XMF Meta File Format Updates 1.01"
 */

#include <stdlib.h>
#include <string.h>

#include "parsers.h"

#ifdef SS_HAVE_ZLIB
#include <zlib.h>
#endif

/* ── Field specifier IDs (5.2.1, RP-030) ────────────────────────────────── */
enum {
	XMF_FIELD_XMF_FILE_TYPE = 0,
	XMF_FIELD_NODE_NAME = 1,
	XMF_FIELD_NODE_ID_NUMBER = 2,
	XMF_FIELD_RESOURCE_FORMAT = 3,
	XMF_FIELD_FILENAME_ON_DISK = 4,
	XMF_FIELD_FILENAME_EXT_ON_DISK = 5,
	XMF_FIELD_MAC_OS_FILE_TYPE = 6,
	XMF_FIELD_MIME_TYPE = 7,
	XMF_FIELD_TITLE = 8,
	XMF_FIELD_COPYRIGHT = 9,
	XMF_FIELD_COMMENT = 10,
	XMF_FIELD_AUTOSTART = 11,
	XMF_FIELD_PRELOAD = 12,
	XMF_FIELD_CONTENT_DESCRIPTION = 13,
	XMF_FIELD_ID3_METADATA = 14
};

/* ── String format IDs (Universal Contents Format, RP-030) ─────────────── */
enum {
	XMF_STRFMT_ASCII_VISIBLE = 0,
	XMF_STRFMT_ASCII_HIDDEN = 1,
	XMF_STRFMT_UNICODE_VISIBLE = 2,
	XMF_STRFMT_UNICODE_HIDDEN = 3,
	XMF_STRFMT_TEXT_THRESHOLD = 4 /* >= is binary content */
};

/* ── Resource format type IDs (Format Type ID, RP-030) ──────────────────── */
enum {
	XMF_FMT_TYPE_STANDARD = 0,
	XMF_FMT_TYPE_MMA = 1,
	XMF_FMT_TYPE_REGISTERED = 2,
	XMF_FMT_TYPE_NON_REGISTERED = 3
};

/* ── Standard resource format IDs ───────────────────────────────────────── */
enum {
	XMF_FMT_SMF_TYPE_0 = 0,
	XMF_FMT_SMF_TYPE_1 = 1,
	XMF_FMT_DLS1 = 2,
	XMF_FMT_DLS2 = 3,
	XMF_FMT_DLS22 = 4,
	XMF_FMT_MOBILE_DLS = 5
};

/* ── Reference type IDs (3.4, RP-030) ───────────────────────────────────── */
enum {
	XMF_REF_INLINE = 1,
	XMF_REF_IN_FILE_RESOURCE = 2,
	XMF_REF_IN_FILE_NODE = 3,
	XMF_REF_EXTERNAL_FILE = 4,
	XMF_REF_EXTERNAL_XMF = 5,
	XMF_REF_XMF_URI_AND_NODE_ID = 6
};

/* ── Unpacker IDs (3.5, RP-030) ─────────────────────────────────────────── */
enum {
	XMF_UNPACKER_NONE = 0,
	XMF_UNPACKER_MMA = 1,
	XMF_UNPACKER_REGISTERED = 2,
	XMF_UNPACKER_NON_REGISTERED = 3
};

/* ── Helpers ────────────────────────────────────────────────────────────── */

static void set_rmidi_field(uint8_t **dst, size_t *dst_len,
                            SS_File *file, size_t offset, size_t len) {
	if(len == 0) return;
	uint8_t *buf = (uint8_t *)malloc(len);
	if(!buf) return;
	ss_file_read_bytes(file, offset, buf, len);
	free(*dst);
	*dst = buf;
	*dst_len = len;
}

static bool process_node(SS_MIDIFile *m, SS_File *node, SS_File **midi_out);

/* ── Inflate a packed payload ───────────────────────────────────────────── */

static SS_File *xmf_inflate(SS_File *node, size_t payload_start, size_t payload_size, size_t decoded_size) {
#ifdef SS_HAVE_ZLIB
	if(payload_size == 0 || decoded_size == 0) return NULL;
	uint8_t *raw = (uint8_t *)malloc(payload_size);
	if(!raw) return NULL;
	ss_file_read_bytes(node, payload_start, raw, payload_size);

	uint8_t *out = (uint8_t *)malloc(decoded_size);
	if(!out) {
		free(raw);
		return NULL;
	}

	uLongf dest_len = (uLongf)decoded_size;
	int rc = uncompress(out, &dest_len, raw, (uLong)payload_size);
	free(raw);
	if(rc != Z_OK) {
		free(out);
		return NULL;
	}

	SS_File *content = ss_file_open_from_memory(out, (size_t)dest_len, true);
	if(!content) free(out);
	return content;
#else
	(void)node;
	(void)payload_start;
	(void)payload_size;
	(void)decoded_size;
	return NULL;
#endif
}

/* ── Process a single XMF node ──────────────────────────────────────────── */
/* `node` is a slice whose offset 0 is the start of the node's length VLQ
 * and whose size is the node's full byte length. */

static bool process_node(SS_MIDIFile *m, SS_File *node, SS_File **midi_out) {
	size_t node_size = ss_file_size(node);
	size_t pos = 0;

	/* Node header */
	size_t node_length = ss_file_read_vlq(node, pos); pos = ss_file_tell(node);
	size_t item_count = ss_file_read_vlq(node, pos); pos = ss_file_tell(node);
	size_t header_size = ss_file_read_vlq(node, pos); pos = ss_file_tell(node);

	if(node_length > node_size || header_size > node_length) return false;

	/* Metadata table */
	size_t metadata_size = ss_file_read_vlq(node, pos); pos = ss_file_tell(node);
	size_t metadata_end = pos + metadata_size;
	if(metadata_end > header_size) return false;

	bool has_resource_format = false;
	int format_type_id = -1;
	int resource_format_id = -1;

	while(pos < metadata_end) {
		/* FieldSpecifier */
		uint8_t first = ss_file_read_u8(node, pos);
		size_t field_id = (size_t)-1;
		bool is_named = false;

		if(first == 0) {
			pos++;
			field_id = ss_file_read_vlq(node, pos); pos = ss_file_tell(node);
		} else {
			size_t name_len = ss_file_read_vlq(node, pos); pos = ss_file_tell(node);
			pos += name_len;
			is_named = true;
		}
		if(pos > metadata_end) return false;

		/* NumberOfInternationalContents */
		size_t n_versions = ss_file_read_vlq(node, pos); pos = ss_file_tell(node);

		if(n_versions == 0) {
			/* UniversalContents only */
			size_t data_len = ss_file_read_vlq(node, pos); pos = ss_file_tell(node);
			size_t data_start = pos;
			if(data_start + data_len > metadata_end) return false;

			size_t fmt_id = ss_file_read_vlq(node, pos); pos = ss_file_tell(node);
			size_t consumed = pos - data_start;
			size_t payload_off = pos;
			size_t payload_len = (data_len > consumed) ? data_len - consumed : 0;

			if(!is_named && payload_len > 0) {
				if(field_id == XMF_FIELD_RESOURCE_FORMAT) {
					/* Binary form: [ResourceFormatTypeID, StandardResourceFormatID|...] */
					uint8_t buf[4] = { 0 };
					size_t take = payload_len < sizeof(buf) ? payload_len : sizeof(buf);
					ss_file_read_bytes(node, payload_off, buf, take);
					format_type_id = buf[0];
					if(take >= 2) resource_format_id = buf[1];
					has_resource_format = true;
				} else if(fmt_id < XMF_STRFMT_TEXT_THRESHOLD) {
					/* Textual metadata — record into rmidi_info (last write wins) */
					switch(field_id) {
						case XMF_FIELD_TITLE:
						case XMF_FIELD_NODE_NAME:
							set_rmidi_field(&m->rmidi_info.name,
							                &m->rmidi_info.name_len,
							                node, payload_off, payload_len);
							break;
						case XMF_FIELD_COPYRIGHT:
							set_rmidi_field(&m->rmidi_info.copyright,
							                &m->rmidi_info.copyright_len,
							                node, payload_off, payload_len);
							break;
						case XMF_FIELD_COMMENT:
							set_rmidi_field(&m->rmidi_info.comment,
							                &m->rmidi_info.comment_len,
							                node, payload_off, payload_len);
							break;
						default:
							break;
					}
				}
			}
			pos = data_start + data_len;
		} else {
			/* International contents — skip bytewise */
			size_t intl_len = ss_file_read_vlq(node, pos); pos = ss_file_tell(node);
			pos += intl_len;
		}
	}
	pos = metadata_end;

	/* Unpackers */
	bool packed = false;
	size_t decoded_size = 0;

	size_t unpackers_block_start = pos;
	size_t unpackers_total_len = ss_file_read_vlq(node, pos); pos = ss_file_tell(node);
	size_t unpackers_end = unpackers_block_start + unpackers_total_len;
	if(unpackers_end > header_size) return false;

	if(unpackers_total_len > 0) {
		packed = true;
		while(pos < unpackers_end) {
			size_t unpacker_id = ss_file_read_vlq(node, pos); pos = ss_file_tell(node);
			switch(unpacker_id) {
				case XMF_UNPACKER_NONE: {
					/* StandardUnpackerID */
					(void)ss_file_read_vlq(node, pos); pos = ss_file_tell(node);
					break;
				}
				case XMF_UNPACKER_MMA: {
					uint8_t b0 = ss_file_read_u8(node, pos++);
					if(b0 == 0) {
						/* Three-byte manufacturer ID */
						pos += 2;
					}
					(void)ss_file_read_vlq(node, pos); pos = ss_file_tell(node);
					break;
				}
				default:
					/* Registered/NonRegistered or unknown — unsupported */
					return false;
			}
			decoded_size = ss_file_read_vlq(node, pos); pos = ss_file_tell(node);
		}
	}

	/* Body — start of reference type id is at header_size */
	pos = header_size;
	int ref_type = (int)ss_file_read_vlq(node, pos); pos = ss_file_tell(node);

	if(ref_type != XMF_REF_INLINE) {
		/* External / cross-node references aren't supported */
		return false;
	}

	size_t payload_start = pos;
	if(payload_start > node_length) return false;
	size_t payload_size = node_length - payload_start;

	bool is_file = (item_count == 0);

	if(is_file) {
		/* FileNode — interpret content based on resource format */
		SS_File *content = NULL;
		size_t content_size = 0;

		if(packed) {
			content = xmf_inflate(node, payload_start, payload_size, decoded_size);
			if(!content) return false;
			content_size = ss_file_size(content);
		} else {
			content = ss_file_slice(node, payload_start, payload_size);
			if(!content) return false;
			content_size = payload_size;
		}

		if(has_resource_format && format_type_id == XMF_FMT_TYPE_STANDARD) {
			switch(resource_format_id) {
				case XMF_FMT_SMF_TYPE_0:
				case XMF_FMT_SMF_TYPE_1: {
					if(*midi_out) ss_file_close(*midi_out);
					*midi_out = content;
					content = NULL;
					break;
				}
				case XMF_FMT_DLS1:
				case XMF_FMT_DLS2:
				case XMF_FMT_DLS22:
				case XMF_FMT_MOBILE_DLS: {
					uint8_t *bank = (uint8_t *)malloc(content_size);
					if(bank) {
						ss_file_read_bytes(content, 0, bank, content_size);
						free(m->embedded_soundbank);
						m->embedded_soundbank = bank;
						m->embedded_soundbank_size = content_size;
					}
					break;
				}
				default:
					break;
			}
		}
		if(content) ss_file_close(content);
	} else {
		/* FolderNode — iterate sub-nodes */
		SS_File *folder = ss_file_slice(node, payload_start, payload_size);
		if(!folder) return false;

		size_t sub_pos = 0;
		bool ok = true;
		size_t children_seen = 0;
		while(sub_pos < payload_size && children_seen < item_count) {
			size_t sub_start = sub_pos;
			size_t sub_len = ss_file_read_vlq(folder, sub_pos);
			if(sub_len == 0 || sub_start + sub_len > payload_size) {
				ok = false;
				break;
			}
			SS_File *child = ss_file_slice(folder, sub_start, sub_len);
			if(!child) {
				ok = false;
				break;
			}
			ok = process_node(m, child, midi_out);
			ss_file_close(child);
			if(!ok) break;
			sub_pos = sub_start + sub_len;
			children_seen++;
		}
		ss_file_close(folder);
		if(!ok) return false;
	}

	return true;
}

/* ── Detect XMF magic ────────────────────────────────────────────────────── */

bool ss_midi_is_xmf(SS_File *file, size_t size) {
	if(size < 8) return false;
	return ss_file_read_u8(file, 0) == 'X' &&
	       ss_file_read_u8(file, 1) == 'M' &&
	       ss_file_read_u8(file, 2) == 'F' &&
	       ss_file_read_u8(file, 3) == '_';
}

/* ── Public XMF parser ───────────────────────────────────────────────────── */

bool ss_midi_parse_xmf(SS_MIDIFile *m, SS_File *file, size_t size) {
	if(!ss_midi_is_xmf(file, size)) return false;

	m->bank_offset = 0;

	char version[5];
	ss_file_read_string(file, 4, version, 4);

	size_t pos = 8;
	/* Version 2.00 (RP-039) carries an additional FileTypeID + FileTypeRevisionID pair */
	if(memcmp(version, "2.00", 4) == 0) {
		pos += 8;
	}

	/* FileLength (VLQ) — informational, ignore */
	(void)ss_file_read_vlq(file, pos); pos = ss_file_tell(file);

	/* MetadataTableLength (VLQ) — file-level metadata table; skip */
	size_t md_len = ss_file_read_vlq(file, pos); pos = ss_file_tell(file);
	if(pos + md_len > size) return false;
	pos += md_len;

	/* TreeStart — absolute file offset of the root TreeNode */
	size_t tree_start = ss_file_read_vlq(file, pos); pos = ss_file_tell(file);
	if(tree_start >= size) return false;

	/* Determine root node length and slice it */
	size_t root_pos = tree_start;
	size_t root_length = ss_file_read_vlq(file, root_pos);
	if(root_length == 0 || tree_start + root_length > size) {
		root_length = size - tree_start;
	}

	SS_File *root = ss_file_slice(file, tree_start, root_length);
	if(!root) return false;

	SS_File *midi_data = NULL;
	bool ok = process_node(m, root, &midi_data);
	ss_file_close(root);

	if(!ok) {
		if(midi_data) ss_file_close(midi_data);
		free(m->embedded_soundbank);
		m->embedded_soundbank = NULL;
		m->embedded_soundbank_size = 0;
		return false;
	}

	if(!midi_data) {
		/* No SMF FileNode found */
		return false;
	}

	bool parsed = ss_midi_parse_smf(m, midi_data, ss_file_size(midi_data));
	ss_file_close(midi_data);
	return parsed;
}
