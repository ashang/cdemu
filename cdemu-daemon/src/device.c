/*
 *  CDEmu daemon: device
 *  Copyright (C) 2006-2014 Rok Mandeljc
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "cdemu.h"
#include "device-private.h"

#define __debug__ "Device"


/**********************************************************************\
 *                              Device ID                             *
\**********************************************************************/
static void cdemu_device_set_device_id (CdemuDevice *self, const gchar *vendor_id, const gchar *product_id, const gchar *revision, const gchar *vendor_specific)
{
    g_free(self->priv->id_vendor_id);
    self->priv->id_vendor_id = g_strndup(vendor_id, 8);

    g_free(self->priv->id_product_id);
    self->priv->id_product_id = g_strndup(product_id, 16);

    g_free(self->priv->id_revision);
    self->priv->id_revision = g_strndup(revision, 4);

    g_free(self->priv->id_vendor_specific);
    self->priv->id_vendor_specific = g_strndup(vendor_specific, 20);
}


/**********************************************************************\
 *                            Device init                             *
\**********************************************************************/
gboolean cdemu_device_initialize (CdemuDevice *self, gint number, const gchar *audio_driver)
{
    MirageContext *context;
    GSource *source;
    gint buffer_size;

    self->priv->mapping_complete = FALSE;

    /* Set device number and device name */
    self->priv->number = number;
    self->priv->device_name = g_strdup_printf("cdemu%i", number);

    /* Init device mutex */
#if !GLIB_CHECK_VERSION(2, 32, 0)
    self->priv->device_mutex = g_mutex_new();
#else
    self->priv->device_mutex = g_new(GMutex, 1);
    g_mutex_init(self->priv->device_mutex);
#endif

    /* Create GLib main context and main loop for events */
    self->priv->main_context = g_main_context_new();
    self->priv->main_loop = g_main_loop_new(self->priv->main_context, FALSE);

    /* Create mapping setup timer with 1-second granularity */
    source = g_timeout_source_new_seconds(1);
    g_source_set_callback(source, (GSourceFunc)cdemu_device_setup_mapping, self, NULL);
    g_source_attach(source, self->priv->main_context);
    g_source_unref(source);

    /* Create a MirageContext to use as a debug context for device */
    context = g_object_new(MIRAGE_TYPE_CONTEXT, NULL);
    mirage_context_set_debug_name(context, self->priv->device_name);
    mirage_context_set_debug_domain(context, "CDEMU");
    mirage_contextual_set_context(MIRAGE_CONTEXTUAL(self), context);
    g_object_unref(context);

    /* Allocate kernel I/O buffer */
    buffer_size = cdemu_device_get_kernel_io_buffer_size(self);
    self->priv->kernel_io_buffer = g_try_malloc0(buffer_size);
    if (!self->priv->kernel_io_buffer) {
        CDEMU_DEBUG(self, DAEMON_DEBUG_WARNING, "%s: failed to allocate kernel I/O buffer (%d bytes)!\n", __debug__, buffer_size);
        return FALSE;
    }

    /* Allocate buffer/"cache"; 4kB should be enough for everything, I think */
    buffer_size = 4096;
    self->priv->buffer_capacity = buffer_size;
    self->priv->buffer = g_try_malloc0(buffer_size);
    if (!self->priv->buffer) {
        CDEMU_DEBUG(self, DAEMON_DEBUG_WARNING, "%s: failed to allocate cache buffer (%d bytes)!\n", __debug__, buffer_size);
        return FALSE;
    }

    /* Create audio play object */
    self->priv->audio_play = g_object_new(CDEMU_TYPE_AUDIO, NULL);
    /* Set parent */
    mirage_object_set_parent(MIRAGE_OBJECT(self->priv->audio_play), self);
    /* Initialize */
    cdemu_audio_initialize(self->priv->audio_play, audio_driver, &self->priv->current_address, self->priv->device_mutex);

    /* Create debug context for disc */
    self->priv->mirage_context = g_object_new(MIRAGE_TYPE_CONTEXT, NULL);
    mirage_context_set_debug_name(self->priv->mirage_context, self->priv->device_name);
    mirage_context_set_debug_domain(self->priv->mirage_context, "libMirage");

    /* Set up default device ID */
    cdemu_device_set_device_id(self, "CDEmu   ", "Virt. CD/DVD-ROM", "1.10", "    cdemu.sf.net    ");

    /* Initialize mode pages and features and set profile */
    cdemu_device_mode_pages_init(self);
    cdemu_device_features_init(self);
    cdemu_device_set_profile(self, ProfileIndex_NONE);

    /* Enable DPM and disable transfer rate emulation by default */
    self->priv->dpm_emulation = FALSE;
    self->priv->tr_emulation = FALSE;
    self->priv->bad_sector_emulation = FALSE;

    self->priv->dvd_report_css = FALSE;

    return TRUE;
}


/**********************************************************************\
 *                            Device number                           *
\**********************************************************************/
gint cdemu_device_get_device_number (CdemuDevice *self)
{
    return self->priv->number;
}


/**********************************************************************\
 *                            Device status                           *
\**********************************************************************/
gboolean cdemu_device_get_status (CdemuDevice *self, gchar ***file_names)
{
    gboolean loaded;

    g_mutex_lock(self->priv->device_mutex);

    loaded = self->priv->loaded;
    if (loaded) {
        MirageDisc *disc = self->priv->disc;
        if (file_names) {
            gchar **tmp_filenames = mirage_disc_get_filenames(disc);
            *file_names = g_strdupv(tmp_filenames);
        }
    } else {
        if (file_names) {
            *file_names = g_new0(gchar *, 1); /* NULL-terminated, hence 1 */
        }
    }

    g_mutex_unlock(self->priv->device_mutex);

    return loaded;
}


/**********************************************************************\
 *                            Device options                          *
\**********************************************************************/
GVariant *cdemu_device_get_option (CdemuDevice *self, gchar *option_name, GError **error)
{
    GVariant *option_value = NULL;

    /* Lock */
    g_mutex_lock(self->priv->device_mutex);

    /* Get option */
    if (!g_strcmp0(option_name, "dpm-emulation")) {
        /* *** dpm-emulation *** */
        option_value = g_variant_new("b", self->priv->dpm_emulation);
    } else if (!g_strcmp0(option_name, "tr-emulation")) {
        /* *** tr-emulation *** */
        option_value = g_variant_new("b", self->priv->tr_emulation);
    } else if (!g_strcmp0(option_name, "bad-sector-emulation")) {
        /* *** bad-sector-emulation *** */
        option_value = g_variant_new("b", self->priv->bad_sector_emulation);
    } else if (!g_strcmp0(option_name, "dvd-report-css")) {
        /* *** dvd-report-css *** */
        option_value = g_variant_new("b", self->priv->dvd_report_css);
    } else if (!g_strcmp0(option_name, "device-id")) {
        /* *** device-id *** */
        option_value = g_variant_new("(ssss)", self->priv->id_vendor_id, self->priv->id_product_id, self->priv->id_revision, self->priv->id_vendor_specific);
    } else if (!g_strcmp0(option_name, "daemon-debug-mask")) {
        /* *** daemon-debug-mask *** */
        MirageContext *context = mirage_contextual_get_context(MIRAGE_CONTEXTUAL(self));
        if (context) {
            gint mask = mirage_context_get_debug_mask(context);
            option_value = g_variant_new("i", mask);
            g_object_unref(context);
        }
    } else if (!g_strcmp0(option_name, "library-debug-mask")) {
        /* *** library-debug-mask *** */
        gint mask = mirage_context_get_debug_mask(self->priv->mirage_context);
        option_value = g_variant_new("i", mask);
    } else {
        /* Option not found */
        CDEMU_DEBUG(self, DAEMON_DEBUG_WARNING, "%s: option '%s' not found; client bug?\n", __debug__, option_name);
        g_set_error(error, CDEMU_ERROR, CDEMU_ERROR_INVALID_ARGUMENT, Q_("Invalid option name '%s'!"), option_name);
    }

    /* Unlock */
    g_mutex_unlock(self->priv->device_mutex);

    return option_value;
}

gboolean cdemu_device_set_option (CdemuDevice *self, gchar *option_name, GVariant *option_value, GError **error)
{
    gboolean succeeded = TRUE;

    /* Lock */
    g_mutex_lock(self->priv->device_mutex);

    /* Get option */
    if (!g_strcmp0(option_name, "dpm-emulation")) {
        /* *** dpm-emulation *** */
        if (!g_variant_is_of_type(option_value, G_VARIANT_TYPE("b"))) {
            g_set_error(error, CDEMU_ERROR, CDEMU_ERROR_INVALID_ARGUMENT, Q_("Invalid argument type for option '%s'!"), option_name);
            succeeded = FALSE;
        } else {
            g_variant_get(option_value, "b", &self->priv->dpm_emulation);
        }
    } else if (!g_strcmp0(option_name, "tr-emulation")) {
        /* *** tr-emulation *** */
        if (!g_variant_is_of_type(option_value, G_VARIANT_TYPE("b"))) {
            g_set_error(error, CDEMU_ERROR, CDEMU_ERROR_INVALID_ARGUMENT, Q_("Invalid argument type for option '%s'!"), option_name);
            succeeded = FALSE;
        } else {
            g_variant_get(option_value, "b", &self->priv->tr_emulation);
        }
    } else if (!g_strcmp0(option_name, "bad-sector-emulation")) {
        /* *** bad-sector-emulation *** */
        if (!g_variant_is_of_type(option_value, G_VARIANT_TYPE("b"))) {
            g_set_error(error, CDEMU_ERROR, CDEMU_ERROR_INVALID_ARGUMENT, Q_("Invalid argument type for option '%s'!"), option_name);
            succeeded = FALSE;
        } else {
            g_variant_get(option_value, "b", &self->priv->bad_sector_emulation);
        }
    } else if (!g_strcmp0(option_name, "dvd-report-css")) {
        /* *** dvd-report-css *** */
        if (!g_variant_is_of_type(option_value, G_VARIANT_TYPE("b"))) {
            g_set_error(error, CDEMU_ERROR, CDEMU_ERROR_INVALID_ARGUMENT, Q_("Invalid argument type for option '%s'!"), option_name);
            succeeded = FALSE;
        } else {
            g_variant_get(option_value, "b", &self->priv->dvd_report_css);
        }
    } else if (!g_strcmp0(option_name, "device-id")) {
        /* *** device-id *** */
        if (!g_variant_is_of_type(option_value, G_VARIANT_TYPE("(ssss)"))) {
            g_set_error(error, CDEMU_ERROR, CDEMU_ERROR_INVALID_ARGUMENT, Q_("Invalid argument type for option '%s'!"), option_name);
            succeeded = FALSE;
        } else {
            gchar *vendor_id, *product_id, *revision, *vendor_specific;
            g_variant_get(option_value, "(ssss)", &vendor_id, &product_id, &revision, &vendor_specific);

            cdemu_device_set_device_id(self, vendor_id, product_id, revision, vendor_specific);

            g_free(vendor_id);
            g_free(product_id);
            g_free(revision);
            g_free(vendor_specific);
        }
    } else if (!g_strcmp0(option_name, "daemon-debug-mask")) {
        /* *** daemon-debug-mask *** */
        if (!g_variant_is_of_type(option_value, G_VARIANT_TYPE("i"))) {
            g_set_error(error, CDEMU_ERROR, CDEMU_ERROR_INVALID_ARGUMENT, Q_("Invalid argument type for option '%s'!"), option_name);
            succeeded = FALSE;
        } else {
            MirageContext *context = mirage_contextual_get_context(MIRAGE_CONTEXTUAL(self));
            if (context) {
                gint mask;
                g_variant_get(option_value, "i", &mask);
                mirage_context_set_debug_mask(context, mask);
                g_object_unref(context);
            }
        }
    } else if (!g_strcmp0(option_name, "library-debug-mask")) {
        /* *** library-debug-mask *** */
        if (!g_variant_is_of_type(option_value, G_VARIANT_TYPE("i"))) {
            g_set_error(error, CDEMU_ERROR, CDEMU_ERROR_INVALID_ARGUMENT, Q_("Invalid argument type for option '%s'!"), option_name);
            succeeded = FALSE;
        } else {
            gint mask;
            g_variant_get(option_value, "i", &mask);
            mirage_context_set_debug_mask(self->priv->mirage_context, mask);
        }
    } else {
        /* Option not found */
        CDEMU_DEBUG(self, DAEMON_DEBUG_WARNING, "%s: option '%s' not found; client bug?\n", __debug__, option_name);
        g_set_error(error, CDEMU_ERROR, CDEMU_ERROR_INVALID_ARGUMENT, Q_("Invalid option name '%s'!"), option_name);
        succeeded = FALSE;
    }

    /* Unlock */
    g_mutex_unlock(self->priv->device_mutex);

    /* Signal that option has been changed */
    if (succeeded) {
        g_signal_emit_by_name(self, "option-changed", option_name, NULL);
    }

    return succeeded;
}


/**********************************************************************\
 *                             Object init                            *
\**********************************************************************/
G_DEFINE_TYPE_WITH_PRIVATE(CdemuDevice, cdemu_device, MIRAGE_TYPE_OBJECT)

static void cdemu_device_init (CdemuDevice *self)
{
    self->priv = cdemu_device_get_instance_private(self);

    self->priv->io_channel = NULL;
    self->priv->io_thread = NULL;
    self->priv->main_context = NULL;
    self->priv->main_loop = NULL;
    self->priv->io_watch = NULL;

    self->priv->device_name = NULL;
    self->priv->device_mutex = NULL;

    self->priv->kernel_io_buffer = NULL;
    self->priv->buffer = NULL;

    self->priv->audio_play = NULL;

    self->priv->disc = NULL;
    self->priv->mirage_context = NULL;

    self->priv->mode_pages_list = NULL;

    self->priv->features_list = NULL;

    self->priv->id_vendor_id = NULL;
    self->priv->id_product_id = NULL;
    self->priv->id_revision = NULL;
    self->priv->id_vendor_specific = NULL;

    self->priv->device_sg = NULL;
    self->priv->device_sr = NULL;

    self->priv->write_descriptors = NULL;

    self->priv->image_writer = NULL;
    self->priv->recording = NULL;

    self->priv->leadin_cdtext_packs = NULL;
    self->priv->num_leadin_cdtext_packs = 0;
}

static void cdemu_device_dispose (GObject *gobject)
{
    CdemuDevice *self = CDEMU_DEVICE(gobject);

    /* Stop the device */
    cdemu_device_stop(self);

    /* Unload disc */
    self->priv->locked = FALSE; /* Make sure we can unload the disc */
    cdemu_device_unload_disc(self, NULL);

    /* Unref audio play object */
    if (self->priv->audio_play) {
        g_object_unref(self->priv->audio_play);
        self->priv->audio_play = NULL;
    }

    /* Unref debug context */
    if (self->priv->mirage_context) {
        g_object_unref(self->priv->mirage_context);
        self->priv->mirage_context = NULL;
    }

    /* Unref main context and main loop */
    if (self->priv->main_loop) {
        g_main_loop_unref(self->priv->main_loop);
        self->priv->main_loop = NULL;
    }

    if (self->priv->main_context) {
        g_main_context_unref(self->priv->main_context);
        self->priv->main_context = NULL;
    }

    /* Chain up to the parent class */
    return G_OBJECT_CLASS(cdemu_device_parent_class)->dispose(gobject);
}

static void cdemu_device_finalize (GObject *gobject)
{
    CdemuDevice *self = CDEMU_DEVICE(gobject);

    /* Free mode pages */
    cdemu_device_mode_pages_cleanup(self);

    /* Free features */
    cdemu_device_features_cleanup(self);

    /* Free write speed descriptors */
    if (self->priv->write_descriptors) {
        g_list_free_full(self->priv->write_descriptors, g_free);
    }

    /* Free device map */
    g_free(self->priv->device_sg);
    g_free(self->priv->device_sr);

    /* Free kernel I/O buffer */
    g_free(self->priv->kernel_io_buffer);

    /* Free buffer/"cache" */
    g_free(self->priv->buffer);

    /* Free device name */
    g_free(self->priv->device_name);

    /* Free device ID */
    g_free(self->priv->id_vendor_id);
    g_free(self->priv->id_product_id);
    g_free(self->priv->id_revision);
    g_free(self->priv->id_vendor_specific);

    /* Free mutex */
#if !GLIB_CHECK_VERSION(2, 32, 0)
    g_mutex_free(self->priv->device_mutex);
#else
    g_mutex_clear(self->priv->device_mutex);
    g_free(self->priv->device_mutex);
#endif

    /* Chain up to the parent class */
    return G_OBJECT_CLASS(cdemu_device_parent_class)->finalize(gobject);
}

static void cdemu_device_class_init (CdemuDeviceClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);

    gobject_class->dispose = cdemu_device_dispose;
    gobject_class->finalize = cdemu_device_finalize;

    /* Signals */
    g_signal_new("status-changed", G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0, NULL);
    g_signal_new("option-changed", G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, g_cclosure_marshal_VOID__STRING, G_TYPE_NONE, 1, G_TYPE_STRING, NULL);
    g_signal_new("kernel-io-error", G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0, NULL);
    g_signal_new("mapping-ready", G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0, NULL);
}
