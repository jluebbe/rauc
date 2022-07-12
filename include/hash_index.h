#pragma once

#include <glib.h>

#include "config_file.h"
#include "slot.h"
#include "stats.h"

#define R_HASH_INDEX_ERROR r_hash_index_error_quark()
GQuark r_hash_index_error_quark(void);

typedef enum {
	R_HASH_INDEX_ERROR_SIZE,
	R_HASH_INDEX_ERROR_NOT_FOUND,
	R_HASH_INDEX_ERROR_MODIFIED,
} RHashIndexErrorError;

typedef struct {
	guint8 data[4096];
	guint8 hash[32];
} RaucHashIndexChunk;

typedef struct {
	gchar *label; /* label for debugging */
	int data_fd; /* file descriptor of the indexed data */
	guint32 count; /* number of chunks */
	GBytes *hashes; /* either GBytes in memory or GMappedFile */
	guint32 *lookup; /* chunk numbers sorted by chunk hash */
	guint32 invalid_below; /* for old index of target */
	guint32 invalid_from; /* for new index of target */
	RaucStats *match_stats; /* how many searches were successful */
	gboolean skip_hash_check; /* whether to skip the hash check (for bundle payload protected by verity) */
} RaucHashIndex;

RaucHashIndex *r_hash_index_open(const gchar *label, int data_fd, const gchar *hashes_filename, GError **error)
G_GNUC_WARN_UNUSED_RESULT;

RaucHashIndex *r_hash_index_reuse(const gchar *label, const RaucHashIndex *idx, int new_data_fd, GError **error)
G_GNUC_WARN_UNUSED_RESULT;

RaucHashIndex *r_hash_index_open_slot(const gchar *label, const RaucSlot *slot, int flags, GError **error)
G_GNUC_WARN_UNUSED_RESULT;

RaucHashIndex *r_hash_index_open_image(const gchar *label, const RaucImage *image, GError **error)
G_GNUC_WARN_UNUSED_RESULT;

gboolean r_hash_index_export(const RaucHashIndex *idx, const gchar *hashes_filename, GError **error)
G_GNUC_WARN_UNUSED_RESULT;

gboolean r_hash_index_export_slot(const RaucHashIndex *idx, const RaucSlot *slot, const RaucChecksum *checksum, GError **error)
G_GNUC_WARN_UNUSED_RESULT;

gboolean r_hash_index_get_chunk(const RaucHashIndex *idx, const guint8 *hash, RaucHashIndexChunk *chunk, GError **error)
G_GNUC_WARN_UNUSED_RESULT;

void r_hash_index_free(RaucHashIndex *idx);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(RaucHashIndex, r_hash_index_free);

#define R_HASH_INDEX_ZERO_CHUNK "\xad\x7f\xac\xb2\x58\x6f\xc6\xe9\x66\xc0\x4\xd7\xd1\xd1\x6b\x2\x4f\x58\x5\xff\x7c\xb4\x7c\x7a\x85\xda\xbd\x8b\x48\x89\x2c\xa7"
