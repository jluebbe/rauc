#pragma once

#include <glib.h>

#include <slot.h>

gboolean r_custom_set_state(RaucSlot *slot, gboolean good, GError **error)
G_GNUC_WARN_UNUSED_RESULT;

gboolean r_custom_set_primary(RaucSlot *slot, GError **error)
G_GNUC_WARN_UNUSED_RESULT;

RaucSlot *r_custom_get_primary(GError **error)
G_GNUC_WARN_UNUSED_RESULT;

gboolean r_custom_get_state(RaucSlot* slot, gboolean *good, GError **error)
G_GNUC_WARN_UNUSED_RESULT;
