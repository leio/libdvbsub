/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 4 -*- */
/*
 * libdvbsub - DVB subtitle decoding
 * Copyright (C) Mart Raudsepp 2009 <mart.raudsepp@artecdesign.ee>
 * 
 * Heavily uses code algorithms ported from ffmpeg's libavcodec/dvbsubdec.c,
 * especially the segment parsers. The original license applies to this
 * ported code and the whole code in this file as well.
 *
 * Original copyright information follows:
 */
/*
 * DVB subtitle decoding for ffmpeg
 * Copyright (c) 2005 Ian Caulfield
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "dvb-sub.h"
#include <string.h> /* memset */
#include <gst/gstutils.h> /* GST_READ_UINT16_BE */

/* FIXME: Are we waiting for an acquisition point before trying to do things? */
/* FIXME: In the end convert some of the guint8/16 (especially stack variables) back to gint for access efficiency */

typedef struct DVBSubObjectDisplay {
	/* FIXME: Use more correct sizes */
	int object_id;
	int region_id;

	int x_pos;
	int y_pos;

	int fgcolor;
	int bgcolor;

	/* FIXME: Should we use GSList? The relating interaction and pointer assigment is quite complex and perhaps unsuited for a plain GSList anyway */
	struct DVBSubObjectDisplay *region_list_next;
	struct DVBSubObjectDisplay *object_list_next;
} DVBSubObjectDisplay;

typedef struct DVBSubObject {
	/* FIXME: Use more correct sizes */
	int id;

	int type;

	/* FIXME: Should we use GSList? */
	DVBSubObjectDisplay *display_list;
	struct DVBSubObject *next;
} DVBSubObject;

typedef struct DVBSubRegionDisplay { /* FIXME: Figure out if this structure is only used temporarily in page_segment parser, or also more */
	int region_id;

	int x_pos;
	int y_pos;

	struct DVBSubRegionDisplay *next;
} DVBSubRegionDisplay;

typedef struct DVBSubRegion
{
	guint8 id;
	guint16 width;
	guint16 height;
	guint8 depth;/* If we want to make this a guint8, then need to ensure it isn't wrap around with reserved values in region handling code */

	guint8 clut;
	guint8 bgcolor;

	/* FIXME: Validate these fields existence and exact types */
	guint8 *pbuf;
	int buf_size;

	DVBSubObjectDisplay *display_list;
} DVBSubRegion;

typedef struct _DvbSubPrivate DvbSubPrivate;
struct _DvbSubPrivate
{
	guint8 page_time_out;
	GSList *region_list;
	GSList *object_list;
	/* FIXME... */
	int display_list_size;
	DVBSubRegionDisplay *display_list;
};

#define DVB_SUB_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE ((obj), DVB_TYPE_SUB, DvbSubPrivate))

G_DEFINE_TYPE (DvbSub, dvb_sub, G_TYPE_OBJECT);

typedef enum
{
	TOP_FIELD = 0,
	BOTTOM_FIELD = 1
} DvbSubPixelDataSubBlockFieldType;

static DVBSubObject *
get_object (DvbSub *dvb_sub, guint16 object_id)
{
	const DvbSubPrivate *priv = (DvbSubPrivate *)dvb_sub->private_data;
	GSList *list = priv->object_list;

	while (list && ((DVBSubObject *)list->data)->id != object_id) {
		list = g_slist_next (list);
	}

	return list ? (DVBSubObject *)list->data : NULL;
}

static DVBSubRegion*
get_region (DvbSub *dvb_sub, guint8 region_id)
{
	const DvbSubPrivate *priv = (DvbSubPrivate *)dvb_sub->private_data;
	GSList *list = priv->region_list;

	while (list && ((DVBSubRegion *)list->data)->id != region_id) {
		list = g_slist_next (list);
	}

	return list ? (DVBSubRegion *)list->data : NULL;
}

static void
delete_region_display_list (DvbSub *dvb_sub, DVBSubRegion *region)
{
	/* FIXME: Fill in with proper object data deletion after the creation is done... */
}

static void
delete_state(DvbSub *dvb_sub)
{
	/* FIXME: Implement */
}

static void
dvb_sub_init (DvbSub *self)
{
	DvbSubPrivate *priv;

	self->private_data = priv = DVB_SUB_GET_PRIVATE (self);

	/* TODO: Add initialization code here */
	/* FIXME: Do we have a reason to initiate the members to zero, or are we guaranteed that anyway? */
	priv->region_list = NULL;
	priv->object_list = NULL;
	priv->page_time_out = 0; /* FIXME: Maybe 255 instead? */
}

static void
dvb_sub_finalize (GObject *object)
{
	DvbSub *self = DVB_SUB (object);
	DvbSubPrivate *priv = (DvbSubPrivate *)self->private_data;
	/* TODO: Add deinitalization code here */
	/* FIXME: Clear up region_list contents */

	G_OBJECT_CLASS (dvb_sub_parent_class)->finalize (object);
}

static void
dvb_sub_class_init (DvbSubClass *klass)
{
	GObjectClass* object_class = (GObjectClass *)klass;

	object_class->finalize = dvb_sub_finalize;

	g_type_class_add_private (klass, sizeof (DvbSubPrivate));
}

static void
_dvb_sub_parse_page_segment (DvbSub *dvb_sub, guint16 page_id, guint8 *buf, gint buf_size) /* FIXME: Use guint for buf_size here and in many other places? */
{
	DvbSubPrivate *priv = (DvbSubPrivate *)dvb_sub->private_data;
	DVBSubRegionDisplay *display;
	DVBSubRegionDisplay *tmp_display_list, **tmp_ptr;

	const guint8 *buf_end = buf + buf_size;
	guint8 region_id;
	guint8 page_state;

	static int counter = 0;
	static gchar *page_state_str[] = {
		"Normal case",
		"ACQUISITION POINT",
		"Mode Change",
		"RESERVED"
	};

	if (buf_size < 1)
		return;

	priv->page_time_out = *buf++;
	page_state = ((*buf++) >> 2) & 3;

	++counter;
	g_print ("PAGE COMPOSITION %d: page_id = %u, length = %d, page_time_out = %u seconds, page_state = %s\nm",
	         counter, page_id, buf_size, priv->page_time_out, page_state_str[page_state]);

	if (page_state == 2) { /* Mode change */
		delete_state (dvb_sub);
	}

	tmp_display_list = priv->display_list;
	priv->display_list = NULL;
	priv->display_list_size = 0;

	while (buf + 5 < buf_end) {
		region_id = *buf++;
		buf += 1;

		display = tmp_display_list;
		tmp_ptr = &tmp_display_list;

		while (display && display->region_id != region_id) {
			tmp_ptr = &display->next;
			display = display->next;
		}

		if (!display)
			display = g_slice_new0(DVBSubRegionDisplay);

		display->region_id = region_id;

		display->x_pos = GST_READ_UINT16_BE (buf);
		buf += 2;
		display->y_pos = GST_READ_UINT16_BE (buf);
		buf += 2;

		*tmp_ptr = display->next;

		display->next = priv->display_list;
		priv->display_list = display;
		priv->display_list_size++;

		g_print ("PAGE COMPOSITION %d: REGION information: ID = %u, address = %ux%u\n", counter,
		          region_id, display->x_pos, display->y_pos);
	}

	while (tmp_display_list) {
		display = tmp_display_list;

		tmp_display_list = display->next;

		g_slice_free (DVBSubRegionDisplay, display);
	}
}

static void
_dvb_sub_parse_region_segment (DvbSub *dvb_sub, guint16 page_id, guint8 *buf, gint buf_size)
{
	DvbSubPrivate *priv = (DvbSubPrivate *)dvb_sub->private_data;

	const guint8 *buf_end = buf + buf_size;
	guint8 region_id;
	guint16 object_id;
	DVBSubRegion *region;
	DVBSubObject *object;
	DVBSubObjectDisplay *object_display;
	gboolean fill;

	if (buf_size < 10)
		return;

	region_id = *buf++;

	region = get_region (dvb_sub, region_id);

	if (!region) { /* Create a new region */
		region = g_slice_new0 (DVBSubRegion);
		region->id = region_id;
		priv->region_list = g_slist_prepend (priv->region_list, region);
	}

	fill = ((*buf++) >> 3) & 1;

	region->width = GST_READ_UINT16_BE (buf);
	buf += 2;
	region->height = GST_READ_UINT16_BE (buf);
	buf += 2;

	if (region->width * region->height != region->buf_size) { /* FIXME: Read closer from spec what happens when dimensions change */
		if (region->pbuf)
			g_free (region->pbuf);

		region->buf_size = region->width * region->height;

		region->pbuf = g_malloc (region->buf_size); /* TODO: We can probably use GSlice here if careful about freeing while buf_size still records the correct size */

		fill = 1; /* FIXME: Validate from spec that fill is forced on (in the following codes context) when dimensions change */
	}

	region->depth = 1 << (((*buf++) >> 2) & 7);
	if (region->depth < 2 || region->depth > 8) {
		g_warning ("region depth %d is invalid\n", region->depth);
		region->depth = 4; /* FIXME: Check from spec this is the default? */
	}

	region->clut = *buf++;

	if (region->depth == 8)
		region->bgcolor = *buf++;
	else {
		buf += 1;

		if (region->depth == 4)
			region->bgcolor = (((*buf++) >> 4) & 15);
		else
			region->bgcolor = (((*buf++) >> 2) & 3);
	}

	g_print ("REGION DATA: id = %u, (%ux%u)@%u-bit\n", region_id, region->width, region->height, region->depth);

	if (fill) {
		memset (region->pbuf, region->bgcolor, region->buf_size);
		g_print ("REGION DATA: Filling region (%u) with bgcolor = %u\n", region->id, region->bgcolor);
	}

	delete_region_display_list (dvb_sub, region); /* Delete the region display list for current region - FIXME: why? */

	while (buf + 6 <= buf_end) {
		object_id = GST_READ_UINT16_BE (buf);
		buf += 2;

		object = get_object(dvb_sub, object_id);

		if (!object) {
			object = g_slice_new0 (DVBSubObject);

			object->id = object_id;
			priv->object_list = g_slist_prepend (priv->object_list, object);
		}

		object->type = (*buf) >> 6;

		object_display = g_slice_new0 (DVBSubObjectDisplay);

		object_display->object_id = object_id;
		object_display->region_id = region_id;

		object_display->x_pos = GST_READ_UINT16_BE (buf) & 0xfff;
		buf += 2;
		object_display->y_pos = GST_READ_UINT16_BE (buf) & 0xfff;
		buf += 2;

		if ((object->type == 1 || object->type == 2) && buf + 2 <= buf_end) {
			object_display->fgcolor = *buf++;
			object_display->bgcolor = *buf++;
		}

		object_display->region_list_next = region->display_list;
		region->display_list = object_display;

		object_display->object_list_next = object->display_list;
		object->display_list = object_display;

		g_print ("REGION DATA: object_id = %u, region_id = %u, pos = %ux%u, obj_type = %u",
		         object->id, region->id, object_display->x_pos, object_display->y_pos,
		         object->type);
		if (object->type == 1 || object->type == 2)
			g_print (", fgcolor = %u, bgcolor = %u\n", object_display->fgcolor, object_display->bgcolor);
		else
			g_print ("\n");
	}
}

static void
_dvb_sub_parse_clut_definition (DvbSub *dvb_sub, guint16 page_id, guint8 *buf, gint buf_size)
{
	/* TODO */
}

static int
_dvb_sub_read_2bit_string(guint8 *destbuf, gint dbuf_len,
                          const guint8 **srcbuf, gint buf_size,
                          guint8 non_mod, guint8 *map_table)
{
	g_warning ("Inside %s", __PRETTY_FUNCTION__);
	/* TODO */
	return dbuf_len;
}

static int
_dvb_sub_read_4bit_string(guint8 *destbuf, gint dbuf_len,
                          const guint8 **srcbuf, gint buf_size,
                          guint8 non_mod, guint8 *map_table)
{
	g_warning ("Inside %s", __PRETTY_FUNCTION__);
	/* TODO */
	return dbuf_len;
}

static int
_dvb_sub_read_8bit_string(guint8 *destbuf, gint dbuf_len,
                          const guint8 **srcbuf, gint buf_size,
                          guint8 non_mod, guint8 *map_table)
{
	g_warning ("Inside %s", __PRETTY_FUNCTION__);
	/* TODO */
	return dbuf_len;
}

static void
_dvb_sub_parse_pixel_data_block(DvbSub *dvb_sub, DVBSubObjectDisplay *display,
                                const guint8 *buf, gint buf_size,
                                DvbSubPixelDataSubBlockFieldType top_bottom,
                                guint8 non_mod)
{
	DvbSubPrivate *priv = (DvbSubPrivate *)dvb_sub->private_data;

	DVBSubRegion *region = get_region(dvb_sub, display->region_id);
	const guint8 *buf_end = buf + buf_size;
	guint8 *pbuf;
	int x_pos, y_pos;
	int i;

	guint8 map2to4[] = { 0x0,  0x7,  0x8,  0xf};
	guint8 map2to8[] = {0x00, 0x77, 0x88, 0xff};
	guint8 map4to8[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
	                    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
	guint8 *map_table;

	g_print ("DVB pixel block size %d, %s field:\n", buf_size,
	         top_bottom ? "bottom" : "top");

#if 1 //def DEBUG_PACKET_CONTENTS
	gst_util_dump_mem (buf, buf_size);
#endif

	if (region == NULL) {
		g_print ("Region is NULL, returning\n");
		return;
	}

	pbuf = region->pbuf;

	x_pos = display->x_pos;
	y_pos = display->y_pos;

	if ((y_pos & 1) != top_bottom)
		y_pos++;

	while (buf < buf_end) {
		if (x_pos > region->width || y_pos > region->height) {
			g_warning ("Invalid object location!\n"); /* FIXME: Be more verbose */
			return;
		}

		switch (*buf++) {
			case 0x10:
				if (region->depth == 8)
					map_table = map2to8;
				else if (region->depth == 4)
					map_table = map2to4;
				else
					map_table = NULL;

				/* FIXME: I don't see any guards about buffer size here - buf++ happens with the switch, but size
				 * FIXME: passed is the global size apparently? */
				x_pos += _dvb_sub_read_2bit_string(pbuf + (y_pos * region->width) + x_pos,
				                                   region->width - x_pos, &buf, buf_size,
				                                   non_mod, map_table);
				break;
			case 0x11:
				if (region->depth < 4) {
					g_warning ("4-bit pixel string in %d-bit region!\n", region->depth);
					return;
				}

				if (region->depth == 8)
					map_table = map4to8;
				else
					map_table = NULL;

				/* FIXME: I don't see any guards about buffer size here - buf++ happens with the switch, but size
				 * FIXME: passed is the global size apparently? */
				x_pos += _dvb_sub_read_4bit_string(pbuf + (y_pos * region->width) + x_pos,
				                                   region->width - x_pos, &buf, buf_size,
				                                   non_mod, map_table);
				break;
			case 0x12:
				if (region->depth < 8) {
					g_warning ("8-bit pixel string in %d-bit region!\n", region->depth);
					return;
				}

				/* FIXME: I don't see any guards about buffer size here - buf++ happens with the switch, but size
				 * FIXME: passed is the global size apparently? */
				x_pos += _dvb_sub_read_8bit_string(pbuf + (y_pos * region->width) + x_pos,
				                                   region->width - x_pos, &buf, buf_size,
				                                   non_mod, NULL);
				break;

			case 0x20:
				/* FIXME: I don't see any guards about buffer size here - buf++ happens with the switch, but
				 * FIXME: buffer is walked without length checks? */
				map2to4[0] = (*buf) >> 4;
				map2to4[1] = (*buf++) & 0xf;
				map2to4[2] = (*buf) >> 4;
				map2to4[3] = (*buf++) & 0xf;
				break;
			case 0x21:
				for (i = 0; i < 4; i++)
					map2to8[i] = *buf++;
				break;
			case 0x22:
				for (i = 0; i < 16; i++)
					map4to8[i] = *buf++;
				break;

			case 0xf0:
				x_pos = display->x_pos;
				y_pos += 2;
				break;
			default:
				g_warning ("Unknown/unsupported pixel block 0x%x", *(buf-1));
		}
	}
}

static void
_dvb_sub_parse_object_segment (DvbSub *dvb_sub, guint16 page_id, guint8 *buf, gint buf_size)
{
	DvbSubPrivate *priv = (DvbSubPrivate *)dvb_sub->private_data;

	const guint8 *buf_end = buf + buf_size;
	guint object_id;
	DVBSubObject *object;

	guint8 coding_method, non_modifying_color;

	object_id = GST_READ_UINT16_BE (buf);
	buf += 2;

	object = get_object (dvb_sub, object_id);

	if (!object) {
		g_warning ("Nothing known about object with ID %u yet inside parse_object_segment, bailing out", object_id);
		return;
	}

	coding_method = ((*buf) >> 2) & 3;
	non_modifying_color = ((*buf++) >> 1) & 1;

	if (coding_method == 0) {
		const guint8 *block;
		DVBSubObjectDisplay *display;
		guint16 top_field_len, bottom_field_len;

		top_field_len = GST_READ_UINT16_BE (buf);
		buf += 2;
		bottom_field_len = GST_READ_UINT16_BE (buf);
		buf += 2;

		if (buf + top_field_len + bottom_field_len > buf_end) {
			g_warning ("%s: Field data size too large\n", __PRETTY_FUNCTION__);
			return;
		}

		for (display = object->display_list; display; display = display->object_list_next) {
			block = buf;

			_dvb_sub_parse_pixel_data_block(dvb_sub, display, block, top_field_len, TOP_FIELD,
			                                non_modifying_color);

			if (bottom_field_len > 0)
				block = buf + top_field_len;
			else
				bottom_field_len = top_field_len;

			_dvb_sub_parse_pixel_data_block(dvb_sub, display, block, bottom_field_len, BOTTOM_FIELD,
			                                non_modifying_color);
		}

	} else if (coding_method == 1) {
		g_warning ("'a string of characters' coding method not supported (yet?)!");
	} else {
		g_warning ("%s: Unknown object coding 0x%x\n", __PRETTY_FUNCTION__, coding_method);
	}
}

static void
_dvb_sub_parse_end_of_display_set (DvbSub *dvb_sub, guint16 page_id, guint8 *buf, gint buf_size)
{
	static int counter = 0;
	++counter;
	g_print ("END OF DISPLAY SET %d: page_id = %u, length = %d\n", counter, page_id, buf_size);
	/* TODO */
}

/**
 * dvb_sub_new:
 *
 * Creates a new #DvbSub.
 *
 * Returns: a newly created #DvbSub
 */
GObject *
dvb_sub_new (void)
{
  DvbSub *dvbsub = g_object_new (DVB_TYPE_SUB, NULL);

  return G_OBJECT (dvbsub);
}

/**
 * dvb_sub_feed:
 * @dvb_sub: a #DvbSub
 * @data: The data to feed to the parser
 * @len: Length of the data
 *
 * Feeds the DvbSub parser with new binary data to parse.
 * The data given must be a full PES packet, which must
 * include a PTS field in the headers.
 *
 * Returns: a negative value on errors; Amount of data consumed on success. TODO
 */
gint
dvb_sub_feed (DvbSub *dvb_sub, guint8 *data, gint len)
{
	guint64 pts = 0;
	unsigned int pos = 0;
	unsigned int total_pos = 0; /* FIXME: This is just for debugging that all was processed */
	guint16 PES_packet_len;
	guint8 PES_packet_header_len;
	gint counter = 0;

	g_print ("Inside dvb_sub_feed with length %d\n", len);

	while (TRUE) {
		++counter;
		g_print ("=============== PES packet number %04u ===============\n", counter);
		g_print ("At position %u\n", pos);
		data = data + pos;
		len = len - pos;
		total_pos += pos;
		pos = 0;
		if (len <= 3) {
			g_warning ("Length %d too small for further processing. Finishing after %u bytes have been processed", len, total_pos);
			return -1;
		}

		if (data[0] != 0x00 || data[1] != 0x00 || data[2] != 0x01) {
			g_warning ("Data fed to dvb_sub_feed is not a PES packet - does not start with a code_prefix of 0x000001");
			return -1;
		}

		if (data[3] != 0xBD) {
			g_warning ("Data fed to dvb_sub_feed is not a PES packet of type private_stream_1, but rather '0x%x', so not a subtitle stream", data[3]);
			return -1;
		}

		PES_packet_len = (data[4] << 8) | data[5];
		g_print("PES packet length is %u\n", PES_packet_len);
		pos = 6;

		/* FIXME: Validate sizes inbetween here */

		pos = 8; /* Later we should get that value with walking with pos++ instead */
		PES_packet_header_len = data[pos++];
		pos += PES_packet_header_len; /* FIXME: Currently including all header values, including PTS */

		dvb_sub_feed_with_pts (dvb_sub, pts, data + pos, PES_packet_len - PES_packet_header_len - 3); /* 2 bytes between PES_packet_len and PES_packet_header_len fields, minus header_len itself */
		pos += PES_packet_len - PES_packet_header_len - 3;
		g_print("Finished PES packet number %u\n", counter);
	}
	return 0; /* FIXME */
}

#define DVB_SUB_SEGMENT_PAGE_COMPOSITION 0x10
#define DVB_SUB_SEGMENT_REGION_COMPOSITION 0x11
#define DVB_SUB_SEGMENT_CLUT_DEFINITION 0x12
#define DVB_SUB_SEGMENT_OBJECT_DATA 0x13
#define DVB_SUB_SEGMENT_END_OF_DISPLAY_SET 0x80
#define DVB_SUB_SEGMENT_STUFFING 0xFF

#define DVB_SUB_SYNC_BYTE 0x0f
/**
 * dvb_sub_feed_with_pts:
 * @dvb_sub: a #DvbSub
 * @pts: The PTS of the data
 * @data: The data to feed to the parser
 * @len: Length of the data
 *
 * Feeds the DvbSub parser with new binary data to parse,
 * with an associated PTS value. E.g, data left after PES
 * packet header has been already parsed, which contains
 * the PTS information).
 *
 * Returns: -1 if data was unhandled (e.g, not a subtitle packet),
 *			-2 if data parsing was unsuccesful (e.g, length was invalid),
 *			0 or positive if data was handled. FIXME: List the positive return values.
 */
gint
dvb_sub_feed_with_pts (DvbSub *dvb_sub, guint64 pts, guint8* data, gint len)
{
	unsigned int pos = 0;
	guint8 segment_type;
	guint16 segment_len;
	guint16 page_id;

	g_print ("Inside dvb_sub_feed_with_pts with pts=%lu and length %d\n", pts, len);

	g_return_val_if_fail (data != NULL, -1);

	if (len <= 3) { /* len(0x20 0x00 end_of_PES_data_field_marker) */
		g_warning ("Data length too short");
		return -1;
	}

	if (data[pos++] != 0x20) {
		g_warning ("Tried to handle a PES packet private data that isn't a subtitle packet (does not start with 0x20)");
		return -1;
	}

	if (data[pos++] != 0x00) {
		g_warning ("'Subtitle stream in this PES packet' was not 0x00, so this is in theory not a DVB subtitle stream (but some other subtitle standard?); bailing out");
		return -1;
	}

	while (data[pos++] == DVB_SUB_SYNC_BYTE) {
		if ((len - pos) < (2*2+1)) {
			g_warning ("Data after SYNC BYTE too short, less than needed to even get to segment_length");
			return -2;
		}
		segment_type = data[pos++];
		g_print ("=== Segment type is 0x%x\n", segment_type);
		page_id = (data[pos] << 8) | data[pos+1];
		g_print ("page_id is 0x%x\n", page_id);
		pos += 2;
		segment_len = (data[pos] << 8) | data[pos+1];
		g_print ("segment_length is %d (0x%x 0x%x)\n", segment_len, data[pos], data[pos+1]);
		pos += 2;
		if ((len - pos) < segment_len) {
			g_warning ("segment_length was told to be %u, but we only have %d bytes left", segment_len, len - pos);
			return -2;
		}

		// TODO: Parse the segment per type
		switch (segment_type) {
			case DVB_SUB_SEGMENT_PAGE_COMPOSITION:
				g_print ("Page composition segment at buffer pos %u\n", pos);
				_dvb_sub_parse_page_segment (dvb_sub, page_id, data + pos, segment_len); /* FIXME: Not sure about args */
				break;
			case DVB_SUB_SEGMENT_REGION_COMPOSITION:
				g_print ("Region composition segment at buffer pos %u\n", pos);
				_dvb_sub_parse_region_segment (dvb_sub, page_id, data + pos, segment_len); /* FIXME: Not sure about args */
				break;
			case DVB_SUB_SEGMENT_CLUT_DEFINITION:
				g_print ("CLUT definition segment at buffer pos %u\n", pos);
				_dvb_sub_parse_clut_definition (dvb_sub, page_id, data + pos, segment_len); /* FIXME: Not sure about args */
				break;
			case DVB_SUB_SEGMENT_OBJECT_DATA:
				g_print ("Object data segment at buffer pos %u\n", pos);
				_dvb_sub_parse_object_segment (dvb_sub, page_id, data + pos, segment_len); /* FIXME: Not sure about args */
				break;
			case DVB_SUB_SEGMENT_END_OF_DISPLAY_SET:
				g_print ("End of display set at buffer pos %u\n", pos);
				_dvb_sub_parse_end_of_display_set (dvb_sub, page_id, data + pos, segment_len); /* FIXME: Not sure about args */
				break;
			default:
				g_warning ("Unhandled segment type 0x%x", segment_type);
				break;
		}

		pos += segment_len;

		if (pos == len) {
			g_warning ("Data ended without a PES data end marker");
			return 1;
		}
	}

	return -2;
}
