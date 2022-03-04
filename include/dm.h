#pragma once

#include <glib.h>

typedef struct _RaucDMVerity {
	gchar *uuid;
	gchar *lower_dev;
	gchar *upper_dev;
	guint64 data_size;
	gchar *root_digest;
	gchar *salt;
} RaucDMVerity;

/**
 * Allocates a new RaucDMVerity with uuid set.
 *
 * @return a pointer to the new RaucDMVerity
 */
RaucDMVerity *r_dm_new_verity(void);

/**
 * Frees the memory allocated by a RaucDMVerity.
 *
 * @param dm_verity struct to free
 */
void r_dm_free_verity(RaucDMVerity *dm_verity);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(RaucDMVerity, r_dm_free_verity);

/**
 * Configure a dm-verity target in the kernel using the provided parameters and
 * return the resulting device name in the struct.
 *
 * @param dm_verity struct with configuration
 * @param error Return location for a GError
 *
 * @return TRUE on success, FALSE if an error occurred
 */
gboolean r_dm_setup_verity(RaucDMVerity *dm_verity, GError **error);

/**
 * Remove a previously configured dm-verity target from the kernel.
 *
 * @param dm_verity struct with configuration
 * @param deferred TRUE if the kernel should remove the target when unused
 * @param error Return location for a GError
 *
 * @return TRUE on success, FALSE if an error occurred
 */
gboolean r_dm_remove_verity(RaucDMVerity *dm_verity, gboolean deferred, GError **error);
