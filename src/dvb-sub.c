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

/* FIXME: Are we waiting for an acquisition point before trying to do things? */

typedef struct _DvbSubPrivate DvbSubPrivate;
struct _DvbSubPrivate
{
	GSList *region_list;
	GSList *object_list;
	//int pid;
	/* FIXME... */
};

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

#define DVB_SUB_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE ((obj), DVB_TYPE_SUB, DvbSubPrivate))

G_DEFINE_TYPE (DvbSub, dvb_sub, G_TYPE_OBJECT);

#define READ_UINT16_BE(data) \
	( (((guint16) (((guint8 *) (data))[0])) << (8)) | \
	  (((guint16) (((guint8 *) (data))[1]))) )

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
dvb_sub_init (DvbSub *self)
{
	DvbSubPrivate *priv;

	self->private_data = priv = DVB_SUB_GET_PRIVATE (self);

	/* TODO: Add initialization code here */
	/* FIXME: Do we have a reason to initiate the members to zero, or are we guaranteed that anyway? */
	priv->region_list = NULL;
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
_dvb_sub_parse_page_composition (DvbSub *dvb_sub, guint16 page_id, guint8 *buf, gint buf_size) /* FIXME: Use guint for buf_size here and in many other places? */
{
	int i;
	unsigned int processed_len;
	static int counter = 0;
	static gchar *page_state[] = {
		"Normal case",
		"ACQUISITION POINT",
		"Mode Change",
		"RESERVED"
	};

	++counter;
	g_print ("PAGE COMPOSITION %d: page_id = %u, length = %d; content is:\nPAGE COMPOSITION %d: ", counter, page_id, buf_size, counter);
	for (i = 0; i < buf_size; ++i)
		g_print ("0x%x ", buf[i]);
	g_print("\n");

	g_print ("PAGE COMPOSITION %d: Page timeout = %u seconds\n", counter, buf[0]);
	g_print ("PAGE COMPOSITION %d: page version number = 0x%x (rolling counter from 0x0 to 0xf and then wraparound)\n", counter, (buf[1] >> 4) & 0xf);
	g_print ("PAGE COMPOSITION %d: page_state = %s\n", counter, page_state[(buf[1] >> 2) & 0x3]);
	g_print ("PAGE COMPOSITION %d: Reserved = 0x%x\n", counter, buf[1] & 0x3);

	processed_len = 2;
	while (processed_len < buf_size) {
		if (buf_size - processed_len < 6) {
			g_warning ("PAGE COMPOSITION %d: Not enough bytes for a region block! 6 needed, but only have %d left in this page composition segment", counter, buf_size - processed_len);
			return;
		}

		g_print ("PAGE COMPOSITION %d: REGION information: ID = %u, address = %ux%u  (reserved value that we don't care was 0x%x)\n", counter,
		          buf[processed_len],
		         (buf[processed_len + 2] << 8) | buf[processed_len + 3],
		         (buf[processed_len + 4] << 8) | buf[processed_len + 5],
		          buf[processed_len + 1]
		        );

		processed_len += 6;
	}
	/* TODO */
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

	region->width = READ_UINT16_BE (buf);
	buf += 2;
	region->height = READ_UINT16_BE (buf);
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
		object_id = READ_UINT16_BE (buf);
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

		object_display->x_pos = READ_UINT16_BE (buf) & 0xfff;
		buf += 2;
		object_display->y_pos = READ_UINT16_BE (buf) & 0xfff;
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

static void
_dvb_sub_parse_object_segment (DvbSub *dvb_sub, guint16 page_id, guint8 *buf, gint buf_size)
{
	static int counter = 0;
	static gchar *coding_method[] = {
		"PIXELS",
		"STRING",
		"Reserved (0x02)",
		"Reserved (0x03)"
	};

	guint16 object_id;

	++counter;
#if DEBUG_CONTENTS
	{
		int i;
		g_print ("OBJECT DATA %d: page_id = %u, length = %d; content is:\nOBJECT DATA %d (content): ", counter, page_id, buf_size, counter);
		for (i = 0; i < buf_size; ++i)
			g_print ("0x%x ", buf[i]);
		g_print("\n");
	}
#endif

	object_id = READ_UINT16_BE (buf);
	g_print ("OBJECT DATA %d: object_id = %u (0x%x 0x%x)\n", counter, object_id, buf[0], buf[1]);
	g_print ("OBJECT DATA %d: object version number = 0x%x (rolling counter from 0x0 to 0xf and then wraparound)\n", counter, (buf[2] >> 4) & 0xf);
	g_print ("OBJECT DATA %d: coding_method = %s\n", counter, coding_method[(buf[1] >> 2) & 0x3]);
	g_print ("OBJECT DATA %d: Reserved = 0x%x\n", counter, buf[1] & 0x3);

	/* TODO */
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
				_dvb_sub_parse_page_composition (dvb_sub, page_id, data + pos, segment_len); /* FIXME: Not sure about args */
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
