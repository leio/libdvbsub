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
#include <gst/base/gstbitreader.h> /* GstBitReader */
#include "ffmpeg-colorspace.h" /* YUV_TO_RGB1_CCIR */

#define DEBUG_SAVE_IMAGES /* NOTE: This requires netpbm on the system - pnmtopng is called with system() */

/* FIXME: Are we waiting for an acquisition point before trying to do things? */
/* FIXME: In the end convert some of the guint8/16 (especially stack variables) back to gint for access efficiency */

#define MAX_NEG_CROP 1024
static guint8 ff_cropTbl[256 + 2 * MAX_NEG_CROP] = { 0, };

#define cm (ff_cropTbl + MAX_NEG_CROP)
#define RGBA(r,g,b,a) (((a) << 24) | ((r) << 16) | ((g) << 8) | (b))

typedef struct DVBSubCLUT {
    int id; /* default_clut uses -1 for this, so guint8 isn't fine without adaptations first */

    guint32 clut4[4];
    guint32 clut16[16];
    guint32 clut256[256];
} DVBSubCLUT;

static DVBSubCLUT default_clut;

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
	int id; /* FIXME: Use guint8 after checking it's fine in all code using it */

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

/* FIXME: AVCodec representation of graphics data. Can be removed once it is converted away from ffmpeg way */
/**
 * four components are given, that's all.
 * the last component is alpha
 */
typedef struct AVPicture {
	guint8 *data[4];
	int linesize[4]; /** number of bytes per line */ /* FIXME: Why does this have four elements, we use only first */
} AVPicture;

typedef struct DVBSubtitleRect {
	int x; /** x coordinate of top left corner */
	int y; /** y coordinate of top left corner */
	int w; /** width */
	int h; /** height */

	/* FIXME: AVCodec representation of graphics data.
	 * FIXME: Convert to be suitable for us in Qt and GStreamer. */
	/** data+linesize for the bitmap of this subtitle.*/
	AVPicture pict;
} DVBSubtitleRect;

/* FIXME: Temporary */
typedef struct AVSubtitle {
	unsigned num_rects;
	DVBSubtitleRect **rects;
} AVSubtitle;

typedef struct _DvbSubPrivate DvbSubPrivate;
struct _DvbSubPrivate
{
	guint8 page_time_out;
	GSList *region_list;
	GSList *clut_list;
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

/* FIXME: It might make sense to pass DvbSubPrivate for all the get_* functions, instead of public DvbSub */
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

static DVBSubCLUT *
get_clut (DvbSub *dvb_sub, gint clut_id)
{
	const DvbSubPrivate *priv = (DvbSubPrivate *)dvb_sub->private_data;
	GSList *list = priv->clut_list;

	while (list && ((DVBSubCLUT *)list->data)->id != clut_id) {
		list = g_slist_next (list);
	}

	return list ? (DVBSubCLUT *)list->data : NULL;
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

/* init static data necessary for ffmpeg-colorspace conversion */
static void
dsputil_static_init (void)
{
	int i;

	for (i = 0; i < 256; i++)
		ff_cropTbl[i + MAX_NEG_CROP] = i;
	for (i = 0; i < MAX_NEG_CROP; i++) {
		ff_cropTbl[i] = 0;
		ff_cropTbl[i + MAX_NEG_CROP + 256] = 255;
	}
}

static void
dvb_sub_class_init (DvbSubClass *klass)
{
	int i, r, g, b, a = 0;
	GObjectClass* object_class = (GObjectClass *)klass;

	object_class->finalize = dvb_sub_finalize;

	g_type_class_add_private (klass, sizeof (DvbSubPrivate));

	dsputil_static_init (); /* Initializes ff_cropTbl table, used in YUV_TO_RGB conversion */

	/* Initialize the static default_clut structure, from which other clut
	 * structures are initialized from (to start off with default CLUTs
	 * as defined in the specification). */
	default_clut.id = -1;

	default_clut.clut4[0] = RGBA(  0,   0,   0,   0);
	default_clut.clut4[1] = RGBA(255, 255, 255, 255);
	default_clut.clut4[2] = RGBA(  0,   0,   0, 255);
	default_clut.clut4[3] = RGBA(127, 127, 127, 255);

	default_clut.clut16[0] = RGBA(  0,   0,   0,   0);
	for (i = 1; i < 16; i++) {
		if (i < 8) {
			r = (i & 1) ? 255 : 0;
			g = (i & 2) ? 255 : 0;
			b = (i & 4) ? 255 : 0;
		} else {
			r = (i & 1) ? 127 : 0;
			g = (i & 2) ? 127 : 0;
			b = (i & 4) ? 127 : 0;
		}
		default_clut.clut16[i] = RGBA(r, g, b, 255);
	}

	default_clut.clut256[0] = RGBA(  0,   0,   0,   0);
	for (i = 1; i < 256; i++) {
		if (i < 8) {
			r = (i & 1) ? 255 : 0;
			g = (i & 2) ? 255 : 0;
			b = (i & 4) ? 255 : 0;
			a = 63;
		} else {
			switch (i & 0x88) {
				case 0x00:
					r = ((i & 1) ? 85 : 0) + ((i & 0x10) ? 170 : 0);
					g = ((i & 2) ? 85 : 0) + ((i & 0x20) ? 170 : 0);
					b = ((i & 4) ? 85 : 0) + ((i & 0x40) ? 170 : 0);
					a = 255;
					break;
				case 0x08:
					r = ((i & 1) ? 85 : 0) + ((i & 0x10) ? 170 : 0);
					g = ((i & 2) ? 85 : 0) + ((i & 0x20) ? 170 : 0);
					b = ((i & 4) ? 85 : 0) + ((i & 0x40) ? 170 : 0);
					a = 127;
					break;
				case 0x80:
					r = 127 + ((i & 1) ? 43 : 0) + ((i & 0x10) ? 85 : 0);
					g = 127 + ((i & 2) ? 43 : 0) + ((i & 0x20) ? 85 : 0);
					b = 127 + ((i & 4) ? 43 : 0) + ((i & 0x40) ? 85 : 0);
					a = 255;
					break;
				case 0x88:
					r = ((i & 1) ? 43 : 0) + ((i & 0x10) ? 85 : 0);
					g = ((i & 2) ? 43 : 0) + ((i & 0x20) ? 85 : 0);
					b = ((i & 4) ? 43 : 0) + ((i & 0x40) ? 85 : 0);
					a = 255;
					break;
			}
		}
		default_clut.clut256[i] = RGBA(r, g, b, a);
	}
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
	g_print ("PAGE COMPOSITION %d: page_id = %u, length = %d, page_time_out = %u seconds, page_state = %s\n",
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
_dvb_sub_parse_clut_segment (DvbSub *dvb_sub, guint16 page_id, guint8 *buf, gint buf_size)
{
	DvbSubPrivate *priv = (DvbSubPrivate *)dvb_sub->private_data;

	const guint8 *buf_end = buf + buf_size;
	guint8 clut_id;
	DVBSubCLUT *clut;
	int entry_id, depth , full_range;
	int y, cr, cb, alpha;
	int r, g, b, r_add, g_add, b_add;

#if 1 //def DEBUG_PACKET_CONTENTS
	g_print("DVB clut packet:\n");
	gst_util_dump_mem (buf, buf_size);
#endif

	clut_id = *buf++;
	buf += 1;

	clut = get_clut(dvb_sub, clut_id);

	if (!clut) {
		clut = g_slice_new (DVBSubCLUT);

		memcpy(clut, &default_clut, sizeof(DVBSubCLUT));

		clut->id = clut_id;

		priv->clut_list = g_slist_prepend (priv->clut_list, clut);
	}

	while (buf + 4 < buf_end) {
		entry_id = *buf++;

		depth = (*buf) & 0xe0;

		if (depth == 0) {
			g_warning ("Invalid clut depth 0x%x!", *buf);
			return;
		}

		full_range = (*buf++) & 1;

		if (full_range) {
			y = *buf++;
			cr = *buf++;
			cb = *buf++;
			alpha = *buf++;
		} else {
			y = buf[0] & 0xfc;
			cr = (((buf[0] & 3) << 2) | ((buf[1] >> 6) & 3)) << 4;
			cb = (buf[1] << 2) & 0xf0;
			alpha = (buf[1] << 6) & 0xc0;

			buf += 2;
		}

		if (y == 0)
			alpha = 0xff;

		YUV_TO_RGB1_CCIR(cb, cr);
		YUV_TO_RGB2_CCIR(r, g, b, y);

		g_print("CLUT DEFINITION: clut %d := (%d,%d,%d,%d)\n", entry_id, r, g, b, alpha);

		if (depth & 0x80)
			clut->clut4[entry_id] = RGBA(r,g,b,255 - alpha);
		if (depth & 0x40)
			clut->clut16[entry_id] = RGBA(r,g,b,255 - alpha);
		if (depth & 0x20)
			clut->clut256[entry_id] = RGBA(r,g,b,255 - alpha);
	}
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
	g_print ("READ_nBIT_STRING (4): Inside %s with dbuf_len = %d\n", __PRETTY_FUNCTION__, dbuf_len);
	/* TODO */
	GstBitReader gb = GST_BIT_READER_INIT (*srcbuf, buf_size);
	/* FIXME: Handle FALSE returns from gst_bit_reader_get_* calls? */

	guint32 bits;
	guint run_length;
	int pixels_read = 0;

	/* FIXME-FFMPEG: The code in libavcodec checks for bits remaining to be less than buf_size,
	 * FIXME-FFMPEG: but in my test sample they are always exactly equal, and the loop is never entered.
	 * FIXME-FFMPEG: This can't be right, so fixed to a less than or equal check; query ffmpeg folk */
	while (gst_bit_reader_get_remaining (&gb) <= buf_size << 3 && pixels_read < dbuf_len) {
		gst_bit_reader_get_bits_uint32 (&gb, &bits, 2);

		if (bits) {
			if (non_mod != 1 || bits != 1) {
				if (map_table) {
					*destbuf++ = map_table[bits];
					g_print ("READ_nBIT_STRING (4): Putting value in destbuf: 0x%x", map_table[bits]);
				}
				else {
					*destbuf++ = bits;
					g_print ("READ_nBIT_STRING (4): Putting value in destbuf: 0x%x\n", map_table[bits]);
				}
			}
			pixels_read++;
		} else {
			gst_bit_reader_get_bits_uint32 (&gb, &bits, 1);
			if (bits == 1) {
				gst_bit_reader_get_bits_uint32 (&gb, &run_length, 3);
				run_length += 3;
				gst_bit_reader_get_bits_uint32 (&gb, &bits, 2);;

				if (non_mod == 1 && bits == 1)
					pixels_read += run_length;
				else {
					if (map_table)
						bits = map_table[bits];
					g_print ("READ_nBIT_STRING (4): Putting value 0x%x in destbuf %d times\n", bits, run_length);
					while (run_length-- > 0 && pixels_read < dbuf_len) {
						*destbuf++ = bits;
						pixels_read++;
					}
				}
			} else {
				gst_bit_reader_get_bits_uint32 (&gb, &bits, 1);
				if (bits == 0) {
					gst_bit_reader_get_bits_uint32 (&gb, &bits, 2);
					if (bits == 2) {
						gst_bit_reader_get_bits_uint32 (&gb, &run_length, 4);
						run_length += 12;
						gst_bit_reader_get_bits_uint32 (&gb, &bits, 2);

						if (non_mod == 1 && bits == 1)
							pixels_read += run_length;
						else {
							if (map_table)
								bits = map_table[bits];
							while (run_length-- > 0 && pixels_read < dbuf_len) {
								*destbuf++ = bits;
								pixels_read++;
							}
						}
					} else if (bits == 3) {
						gst_bit_reader_get_bits_uint32 (&gb, &run_length, 8);
						run_length += 29;
						gst_bit_reader_get_bits_uint32 (&gb, &bits, 2);

						if (non_mod == 1 && bits == 1)
							pixels_read += run_length;
						else {
							if (map_table)
								bits = map_table[bits];
							while (run_length-- > 0 && pixels_read < dbuf_len) {
								*destbuf++ = bits;
								pixels_read++;
							}
						}
					} else if (bits == 1) {
						pixels_read += 2;
						if (map_table)
							bits = map_table[0];
						else
							bits = 0;
						if (pixels_read <= dbuf_len) {
							*destbuf++ = bits;
							*destbuf++ = bits;
						}
					} else {
						(*srcbuf) += (gst_bit_reader_get_remaining (&gb) + 7) >> 3;
						return pixels_read;
					}
				} else {
					if (map_table)
						bits = map_table[0];
					else
						bits = 0;
					*destbuf++ = bits;
					pixels_read++;
				}
			}
		}
	}

#if 1
	/* FIXME: What is this for? With current ported code it seems to always warn with current test case */
	gst_bit_reader_get_bits_uint32 (&gb, &bits, 6);
	if (bits)
		g_warning ("DVBSub error: line overflow");
#else /* Original code from libavcodec for reference until the always warning is fixed: */
	if (get_bits(&gb, 6))
		g_warning ("DVBSub error: line overflow");
#endif

	(*srcbuf) += (gst_bit_reader_get_remaining (&gb) + 7) >> 3;

	return pixels_read;
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

#ifdef DEBUG_PACKET_CONTENTS
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

#ifdef DEBUG_SAVE_IMAGES
static void png_save2(const char *filename, guint32 *bitmap, int w, int h)
{
	int x, y, v;
	FILE *f;
	char fname[40], fname2[40];
	char command[1024];

	snprintf(fname, sizeof(fname), "%s.ppm", filename);

	f = fopen(fname, "w");
	if (!f) {
		perror(fname);
		exit(1);
	}
	fprintf(f, "P6\n"
	        "%d %d\n"
	        "%d\n",
	        w, h, 255);
	for(y = 0; y < h; y++) {
		for(x = 0; x < w; x++) {
			v = bitmap[y * w + x];
			putc((v >> 16) & 0xff, f);
			putc((v >> 8) & 0xff, f);
			putc((v >> 0) & 0xff, f);
		}
	}
	fclose(f);


	snprintf(fname2, sizeof(fname2), "%s-a.pgm", filename);

	f = fopen(fname2, "w");
	if (!f) {
		perror(fname2);
		exit(1);
	}
	fprintf(f, "P5\n"
	        "%d %d\n"
	        "%d\n",
	        w, h, 255);
	for(y = 0; y < h; y++) {
		for(x = 0; x < w; x++) {
			v = bitmap[y * w + x];
			putc((v >> 24) & 0xff, f);
		}
	}
	fclose(f);

	snprintf(command, sizeof(command), "pnmtopng -alpha %s %s > %s.png 2> /dev/null", fname2, fname, filename);
	system(command);

	//snprintf(command, sizeof(command), "rm %s %s", fname, fname2);
	//system(command);
}

static void
save_display_set(DvbSub *dvb_sub)
{
	DvbSubPrivate *priv = (DvbSubPrivate *)dvb_sub->private_data;

	DVBSubRegion *region;
	DVBSubRegionDisplay *display;
	DVBSubCLUT *clut;
	guint32 *clut_table;
	int x_pos, y_pos, width, height;
	int x, y, y_off, x_off;
	guint32 *pbuf;
	char filename[32];
	static int fileno_index = 0;

	x_pos = -1;
	y_pos = -1;
	width = 0;
	height = 0;

	for (display = priv->display_list; display; display = display->next) {
		region = get_region(dvb_sub, display->region_id);

		if (x_pos == -1) {
			x_pos = display->x_pos;
			y_pos = display->y_pos;
			width = region->width;
			height = region->height;
		} else {
			if (display->x_pos < x_pos) {
				width += (x_pos - display->x_pos);
				x_pos = display->x_pos;
			}

			if (display->y_pos < y_pos) {
				height += (y_pos - display->y_pos);
				y_pos = display->y_pos;
			}

			if (display->x_pos + region->width > x_pos + width) {
				width = display->x_pos + region->width - x_pos;
			}

			if (display->y_pos + region->height > y_pos + height) {
				height = display->y_pos + region->height - y_pos;
			}
		}
	}

	if (x_pos >= 0) {

		pbuf = g_malloc (width * height * 4);

		for (display = priv->display_list; display; display = display->next) {
			region = get_region(dvb_sub, display->region_id);

			x_off = display->x_pos - x_pos;
			y_off = display->y_pos - y_pos;

			clut = get_clut(dvb_sub, region->clut);

			if (clut == 0)
				clut = &default_clut;

			switch (region->depth) {
				case 2:
					clut_table = clut->clut4;
					break;
				case 8:
					clut_table = clut->clut256;
					break;
				case 4:
				default:
					clut_table = clut->clut16;
					break;
			}

			for (y = 0; y < region->height; y++) {
				for (x = 0; x < region->width; x++) {
					pbuf[((y + y_off) * width) + x_off + x] =
						clut_table[region->pbuf[y * region->width + x]];
					//g_print ("pbuf@%dx%d = 0x%x\n", x_off + x, y_off + y, clut_table[region->pbuf[y * region->width + x]]);
				}
			}

		}

		snprintf (filename, sizeof(filename), "dvbs.%d", fileno_index);

		png_save2 (filename, pbuf, width, height);

		g_free (pbuf);
	}

	fileno_index++;
}
#endif

static gint
_dvb_sub_parse_end_of_display_set (DvbSub *dvb_sub, guint16 page_id, guint8 *buf, gint buf_size)
{
	DvbSubPrivate *priv = (DvbSubPrivate *)dvb_sub->private_data;

	/* FIXME: Temporarily declared in here for save_display_set testing */
	AVSubtitle *sub = g_slice_new0 (AVSubtitle);

	DVBSubRegion *region;
	DVBSubRegionDisplay *display;
	DVBSubtitleRect *rect;
	DVBSubCLUT *clut;
	guint32 *clut_table;
	int i;

	g_print ("END OF DISPLAY SET: page_id = %u, length = %d\n", page_id, buf_size);

	sub->rects = NULL;
#if 0 /* FIXME: PTS stuff not figured out yet */
	sub->start_display_time = 0;
	sub->end_display_time = priv->page_time_out * 1000;
	sub->format = 0; /* 0 = graphics */
#endif

	sub->num_rects = priv->display_list_size;

	if (sub->num_rects > 0){
		sub->rects = g_malloc0 (sizeof(*sub->rects) * sub->num_rects); /* GSlice? */
		for(i=0; i<sub->num_rects; i++)
			sub->rects[i] = g_malloc0 (sizeof(*sub->rects[i])); /* GSlice? */
	}

	i = 0;

	for (display = priv->display_list; display; display = display->next) {
		region = get_region(dvb_sub, display->region_id);
		rect = sub->rects[i];

		if (!region)
			continue;

		rect->x = display->x_pos;
		rect->y = display->y_pos;
		rect->w = region->width;
		rect->h = region->height;
#if 0 /* FIXME: Don't think we need to save the number of colors in the palette when we are saving as RGBA? */
		rect->nb_colors = 16;
#endif
#if 0 /* FIXME: Needed to be specified once we support strings of characters based subtitles */
		rect->type      = SUBTITLE_BITMAP;
#endif
		rect->pict.linesize[0] = region->width;

		clut = get_clut(dvb_sub, region->clut);

		if (!clut)
			clut = &default_clut;

		switch (region->depth) {
			case 2:
				clut_table = clut->clut4;
				break;
			case 8:
				clut_table = clut->clut256;
				break;
			case 4:
			default:
				clut_table = clut->clut16;
				break;
		}

		/* FIXME: Tweak this to be saved in a format most suitable for Qt and GStreamer instead.
		 * Currently kept in AVPicture for quick save_display_set testing */
		rect->pict.data[1] = g_malloc((1 << region->depth) * sizeof(guint32)); /* FIXME: Can we use GSlice here? */
		memcpy(rect->pict.data[1], clut_table, (1 << region->depth) * sizeof(guint32));
		g_print ("rect->pict.data[1] content:\n");
		gst_util_dump_mem (rect->pict.data[1], (1 << region->depth) * sizeof(guint32));

		rect->pict.data[0] = g_malloc(region->buf_size); /* FIXME: Can we use GSlice here? */
		memcpy(rect->pict.data[0], region->pbuf, region->buf_size);
		g_print ("rect->pict.data[0] content:\n");
		gst_util_dump_mem (rect->pict.data[0], region->buf_size);

		i++;
	}

	sub->num_rects = i;

#ifdef DEBUG_SAVE_IMAGES
	save_display_set(dvb_sub);
#endif

	return 1; /* FIXME: The caller of this function is probably supposed to do something with the return value */
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
				_dvb_sub_parse_clut_segment (dvb_sub, page_id, data + pos, segment_len); /* FIXME: Not sure about args */
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
