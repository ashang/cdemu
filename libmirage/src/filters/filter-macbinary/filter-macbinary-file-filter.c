/*
 *  libMirage: MacBinary file filter: File filter object
 *  Copyright (C) 2013 Henrik Stokseth
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

#include "filter-macbinary.h"

#define __debug__ "MACBINARY-FileFilter"


/**********************************************************************\
 *                          Private structure                         *
\**********************************************************************/
#define MIRAGE_FILE_FILTER_MACBINARY_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE((obj), MIRAGE_TYPE_FILE_FILTER_MACBINARY, MirageFileFilterMacBinaryPrivate))

struct _MirageFileFilterMacBinaryPrivate
{
    macbinary_header_t header;
    rsrc_fork_t        *rsrc_fork;

    guint16 *crctab;
};


/**********************************************************************\
 *                         CRC16-CCITT XModem                         *
\**********************************************************************/

#define CRC16POLY 0x1021 /* Generator polynomial */

/* Call this to init the fast CRC-16 calculation table */
static guint16 *crcinit(guint genpoly)
{
    guint16 *crctab = g_new(guint16, 256);

    for (gint val = 0; val <= 255; val++) {
        guint crc = val << 8;

        for (gint i = 0; i < 8; ++i) {
            crc <<= 1;

            if (crc & 0x10000) {
                crc ^= genpoly;
            }
        }

        crctab[val] = crc;
    }

    return crctab;
}

/* Calculate a CRC-16 for length bytes pointed at by buffer */
static guint16 docrc(guchar *buffer, guint length, guint16 *crctab)
{
    guint16 crc = 0;

    while (length-- > 0) {
        crc = (crc << 8) ^ crctab[(crc >> 8) ^ *buffer++];
    }

    return crc;
}


/**********************************************************************\
 *             Endianness conversion and debug functions              *
\**********************************************************************/
static void mirage_file_filter_macbinary_fixup_header(macbinary_header_t *header)
{
    g_assert(header);

    header->vert_pos      = GUINT16_FROM_BE(header->vert_pos);
    header->horiz_pos     = GUINT16_FROM_BE(header->horiz_pos);
    header->window_id     = GUINT16_FROM_BE(header->window_id);
    header->getinfo_len   = GUINT16_FROM_BE(header->getinfo_len);
    header->secondary_len = GUINT16_FROM_BE(header->secondary_len);
    header->crc16         = GUINT16_FROM_BE(header->crc16);

    header->datafork_len = GUINT32_FROM_BE(header->datafork_len);
    header->resfork_len  = GUINT32_FROM_BE(header->resfork_len);
    header->created      = GUINT32_FROM_BE(header->created);
    header->modified     = GUINT32_FROM_BE(header->modified);
    header->unpacked_len = GUINT32_FROM_BE(header->unpacked_len);
}

static void mirage_file_filter_macbinary_fixup_bcem_block(bcem_block_t *bcem_block)
{
    g_assert(bcem_block);

    bcem_block->imagetype    = GUINT16_FROM_BE(bcem_block->imagetype);
    bcem_block->unknown1     = GUINT16_FROM_BE(bcem_block->unknown1);
    bcem_block->num_sectors  = GUINT32_FROM_BE(bcem_block->num_sectors);
    bcem_block->unknown2     = GUINT32_FROM_BE(bcem_block->unknown2);
    bcem_block->unknown3     = GUINT32_FROM_BE(bcem_block->unknown3);
    bcem_block->crc32        = GUINT32_FROM_BE(bcem_block->crc32);
    bcem_block->is_segmented = GUINT32_FROM_BE(bcem_block->is_segmented);

    for (guint u = 0; u < 9; u++) {
        bcem_block->unknown4[u] = GUINT32_FROM_BE(bcem_block->unknown4[u]);
    }

    bcem_block->num_blocks   = GUINT32_FROM_BE(bcem_block->num_blocks);
}

static void mirage_file_filter_macbinary_fixup_bcem_data(bcem_data_t *bcem_data)
{
    guint8 temp;

    g_assert(bcem_data);

    temp = bcem_data->sector[0];
    bcem_data->sector[0] = bcem_data->sector[2];
    bcem_data->sector[2] = temp;

    bcem_data->offset = GUINT32_FROM_BE(bcem_data->offset);
    bcem_data->length = GUINT32_FROM_BE(bcem_data->length);
}

static void mirage_file_filter_macbinary_fixup_bcm_block(bcm_block_t *bcm_block)
{
    g_assert(bcm_block);

    bcm_block->part     = GUINT16_FROM_BE(bcm_block->part);
    bcm_block->parts    = GUINT16_FROM_BE(bcm_block->parts);
    bcm_block->unknown2 = GUINT32_FROM_BE(bcm_block->unknown2);
}

static void mirage_file_filter_macbinary_print_header(MirageFileFilterMacBinary *self, macbinary_header_t *header, guint16 calculated_crc)
{
    GString *filename = NULL;

    g_assert(self && header);

    filename = g_string_new_len(header->filename, header->fn_length);
    g_assert(filename);

    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "\n%s: MacBinary header:\n", __debug__);
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  Original filename: %s\n", __debug__, filename->str);
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  File type: %.4s creator: %.4s\n", __debug__, header->filetype, header->creator);
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  Data fork length: %d\n", __debug__, header->datafork_len);
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  Resource fork length: %d\n", __debug__, header->resfork_len);
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  Get info comment length: %d\n", __debug__, header->getinfo_len);

    if (calculated_crc == header->crc16) {
        MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  Length of total files: %d\n", __debug__, header->unpacked_len);
        MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  Length of secondary header: %d\n", __debug__, header->secondary_len);
        MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  CRC16: 0x%04x (calculated: 0x%04x)\n", __debug__, header->crc16, calculated_crc);
        MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  Version used to pack: %d\n", __debug__, header->pack_ver-129);
        MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  Version needed to unpack: %d\n", __debug__, header->unpack_ver-129);
        MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  Finder flags: 0x%04x\n", __debug__, (header->finder_flags << 8) + header->finder_flags_2);
    } else {
        MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  Finder flags: 0x%04x\n", __debug__, header->finder_flags << 8);
    }
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "\n");

    g_string_free(filename, TRUE);
}

static void mirage_file_filter_macbinary_print_bcem_block(MirageFileFilterMacBinary *self, bcem_block_t *bcem_block)
{
    GString *imagename = NULL;
    
    g_assert(self && bcem_block);

    imagename = g_string_new_len(bcem_block->imagename, bcem_block->imagename_len);
    g_assert(imagename);

    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "\n%s: bcem block:\n", __debug__);
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  Image type: 0x%x\n", __debug__, bcem_block->imagetype);
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  Unknown1: 0x%04x\n", __debug__, bcem_block->unknown1);
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  Image name: %s\n", __debug__, imagename->str);
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  Number of sectors: %u\n", __debug__, bcem_block->num_sectors);
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  Unknown2: 0x%08x\n", __debug__, bcem_block->unknown2);
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  Unknown3: 0x%08x\n", __debug__, bcem_block->unknown3);
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  CRC32: 0x%08x\n", __debug__, bcem_block->crc32);
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  Is segmented: %u\n", __debug__, bcem_block->is_segmented);

    for (guint u = 0; u < 9; u++) {
        MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  Unknown4[%u]: 0x%08x\n", __debug__, u, bcem_block->unknown4[u]);
    }

    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  Number of blocks: %u\n\n", __debug__, bcem_block->num_blocks);

    g_string_free(imagename, TRUE);
}


/**********************************************************************\
 *              MirageFileFilter methods implementations              *
\**********************************************************************/
static gboolean mirage_file_filter_macbinary_can_handle_data_format (MirageFileFilter *_self, GError **error)
{
    MirageFileFilterMacBinary *self = MIRAGE_FILE_FILTER_MACBINARY(_self);
    GInputStream *stream = g_filter_input_stream_get_base_stream(G_FILTER_INPUT_STREAM(self));

    macbinary_header_t *header = &self->priv->header;
    rsrc_fork_t        *rsrc_fork = NULL;

    guint16 calculated_crc = 0;

    /* Read MacBinary header */
    g_seekable_seek(G_SEEKABLE(stream), 0, G_SEEK_SET, NULL, NULL);
    if (g_input_stream_read(stream, header, sizeof(macbinary_header_t), NULL, NULL) != sizeof(macbinary_header_t)) {
        g_set_error(error, MIRAGE_ERROR, MIRAGE_ERROR_CANNOT_HANDLE, "Filter cannot handle given data: failed to read MacBinary header!");
        return FALSE;
    }

    /* We need to calculate CRC16 before we fixup the header */
    self->priv->crctab = crcinit(CRC16POLY);
    calculated_crc = docrc((guchar *) header, sizeof(macbinary_header_t) - 4, self->priv->crctab);

    /* Fixup header endianness */
    mirage_file_filter_macbinary_fixup_header(header);

    /* Validate MacBinary header */
    if (header->version != 0 || header->reserved_1 != 0 || header->reserved_2 != 0 ||
        header->fn_length < 1 || header->fn_length > 63) {
        g_set_error(error, MIRAGE_ERROR, MIRAGE_ERROR_CANNOT_HANDLE, "Filter cannot handle given data: invalid header!");
        return FALSE;
    }

    /* Valid CRC indicates v2.0 */
    if (calculated_crc != header->crc16) {
        /* Do we have v1.0 then? Hard to say for sure... */
        #ifndef TRUST_UNRELIABLE_V1_CHECK
        MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s: File validates as MacBinary v1.0, however the check is unreliable and therefore disabled!\n", __debug__);
        g_set_error(error, MIRAGE_ERROR, MIRAGE_ERROR_CANNOT_HANDLE, "File validates as MacBinary v1.0, however the check is unreliable and therefore disabled!");
        return FALSE;
        #endif
    }

    /* Print some info */
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s: parsing the underlying stream data...\n", __debug__);

    mirage_file_filter_macbinary_print_header(self, header, calculated_crc);

    /* Read the resource fork if any exists */
    if (header->resfork_len) {
        goffset     rsrc_fork_pos = 0;
        gchar       *rsrc_fork_data = NULL;

        rsrc_fork_pos = sizeof(macbinary_header_t) + header->datafork_len;
        if (header->datafork_len % 128) {
            rsrc_fork_pos += 128 - (header->datafork_len % 128);
        }

        rsrc_fork_data = g_malloc(header->resfork_len);
        if (!rsrc_fork_data) {
            g_set_error(error, MIRAGE_ERROR, MIRAGE_ERROR_CANNOT_HANDLE, "Failed to allocate memory!");
            return FALSE;
        }

        g_seekable_seek(G_SEEKABLE(stream), rsrc_fork_pos, G_SEEK_SET, NULL, NULL);
        if (g_input_stream_read(stream, rsrc_fork_data, header->resfork_len, NULL, NULL) != header->resfork_len) {
            g_set_error(error, MIRAGE_ERROR, MIRAGE_ERROR_CANNOT_HANDLE, "Failed to read resource-fork!");
            return FALSE;
        }

        rsrc_fork = self->priv->rsrc_fork = rsrc_fork_read_binary(rsrc_fork_data);
        if (!rsrc_fork) {
            g_set_error(error, MIRAGE_ERROR, MIRAGE_ERROR_CANNOT_HANDLE, "Failed to parse resource-fork!");
            return FALSE;
        }

        g_free(rsrc_fork_data);
    }

    /* Print some interesting info from the resource-fork */
    if (rsrc_fork) {
        rsrc_ref_t *rsrc_ref = rsrc_find_ref_by_type_and_id(rsrc_fork, "bcm#", 128);
        
        if (rsrc_ref) {
            bcm_block_t *bcm_block = (bcm_block_t *) rsrc_ref->data;

            mirage_file_filter_macbinary_fixup_bcm_block(bcm_block);

            MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s: This file is part %u of a set of %u files! Interesting value: 0x%x\n", 
                         __debug__, bcm_block->part, bcm_block->parts, bcm_block->unknown2);
        }

        rsrc_ref = rsrc_find_ref_by_type_and_id(rsrc_fork, "bcem", 128);

        if (rsrc_ref) {
            bcem_block_t *bcem_block = (bcem_block_t *) rsrc_ref->data;
            bcem_data_t  *bcem_data = (bcem_data_t *) (rsrc_ref->data + sizeof(bcem_block_t));

            mirage_file_filter_macbinary_fixup_bcem_block(bcem_block);

            mirage_file_filter_macbinary_print_bcem_block(self, bcem_block);

            for (guint b = 0; b < bcem_block->num_blocks; b++) {
                guint32 sector;
                mirage_file_filter_macbinary_fixup_bcem_data(&bcem_data[b]);

                sector = (bcem_data[b].sector[2] << 16) + (bcem_data[b].sector[1] << 8) + bcem_data[b].sector[0];

                MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s: Block %3u: Sector: %8u Type: %4d Offset: 0x%08x Length: 0x%08x (%u)\n", 
                             __debug__, b, sector, bcem_data[b].type, bcem_data[b].offset, bcem_data[b].length, bcem_data[b].length);
            }
            MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "\n");
        }
    }

    /* The data fork contains the image data */
    mirage_file_filter_set_file_size(MIRAGE_FILE_FILTER(self), header->datafork_len);

    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s: parsing completed successfully\n\n", __debug__);

    return TRUE;
}


static gssize mirage_file_filter_macbinary_partial_read (MirageFileFilter *_self, void *buffer, gsize count)
{
    MirageFileFilterMacBinary *self = MIRAGE_FILE_FILTER_MACBINARY(_self);
    GInputStream *stream = g_filter_input_stream_get_base_stream(G_FILTER_INPUT_STREAM(self));
    goffset position = mirage_file_filter_get_position(MIRAGE_FILE_FILTER(self));

    goffset read_pos = position + sizeof(macbinary_header_t);

    gint ret;

    /* Are we behind end of stream? */
    if (position >= self->priv->header.datafork_len) {
        MIRAGE_DEBUG(self, MIRAGE_DEBUG_FILE_IO, "%s: stream position %ld (0x%lX) beyond end of stream, doing nothing!\n", __debug__, position, position);
        return 0;
    }

    /* Seek to the position */
    if (!g_seekable_seek(G_SEEKABLE(stream), read_pos, G_SEEK_SET, NULL, NULL)) {
        MIRAGE_DEBUG(self, MIRAGE_DEBUG_WARNING, "%s: failed to seek to %ld in underlying stream!\n", __debug__, read_pos);
        return -1;
    }

    /* Read data into buffer */
    ret = g_input_stream_read(stream, buffer, count, NULL, NULL);
    if (ret == -1) {
        MIRAGE_DEBUG(self, MIRAGE_DEBUG_WARNING, "%s: failed to read %d bytes from underlying stream!\n", __debug__, count);
        return -1;
    } else if (ret == 0) {
        MIRAGE_DEBUG(self, MIRAGE_DEBUG_WARNING, "%s: unexpectedly reached EOF!\n", __debug__);
        return -1;
    }

    return count;
}


/**********************************************************************\
 *                             Object init                            *
\**********************************************************************/
G_DEFINE_DYNAMIC_TYPE(MirageFileFilterMacBinary, mirage_file_filter_macbinary, MIRAGE_TYPE_FILE_FILTER);

void mirage_file_filter_macbinary_type_register (GTypeModule *type_module)
{
    return mirage_file_filter_macbinary_register_type(type_module);
}


static void mirage_file_filter_macbinary_init (MirageFileFilterMacBinary *self)
{
    self->priv = MIRAGE_FILE_FILTER_MACBINARY_GET_PRIVATE(self);

    mirage_file_filter_generate_info(MIRAGE_FILE_FILTER(self),
        "FILTER-MACBINARY",
        "MACBINARY File Filter",
        1,
        "MacBinary images (*.bin, *.macbin)", "application/x-macbinary"
    );

    self->priv->crctab = NULL;
    self->priv->rsrc_fork = NULL;
}

static void mirage_file_filter_macbinary_finalize (GObject *gobject)
{
    MirageFileFilterMacBinary *self = MIRAGE_FILE_FILTER_MACBINARY(gobject);

    if (self->priv->rsrc_fork) {
        rsrc_fork_free(self->priv->rsrc_fork);
    }

    g_free(self->priv->crctab);

    /* Chain up to the parent class */
    return G_OBJECT_CLASS(mirage_file_filter_macbinary_parent_class)->finalize(gobject);
}

static void mirage_file_filter_macbinary_class_init (MirageFileFilterMacBinaryClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    MirageFileFilterClass *file_filter_class = MIRAGE_FILE_FILTER_CLASS(klass);

    gobject_class->finalize = mirage_file_filter_macbinary_finalize;

    file_filter_class->can_handle_data_format = mirage_file_filter_macbinary_can_handle_data_format;

    file_filter_class->partial_read = mirage_file_filter_macbinary_partial_read;

    /* Register private structure */
    g_type_class_add_private(klass, sizeof(MirageFileFilterMacBinaryPrivate));
}

static void mirage_file_filter_macbinary_class_finalize (MirageFileFilterMacBinaryClass *klass G_GNUC_UNUSED)
{
}

