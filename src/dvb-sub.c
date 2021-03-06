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
#include <string.h>             /* memset */
#include <gst/gstutils.h>       /* GST_READ_UINT16_BE */
#include <gst/base/gstbitreader.h>      /* GstBitReader */
#include "ffmpeg-colorspace.h"  /* YUV_TO_RGB1_CCIR */
#include "dvb-log.h"

//#define DEBUG_SAVE_IMAGES /* NOTE: This requires netpbm on the system - pnmtopng is called with system() */

/* FIXME: Are we waiting for an acquisition point before trying to do things? */
/* FIXME: In the end convert some of the guint8/16 (especially stack variables) back to gint for access efficiency */

/**
 * SECTION:dvb-sub
 * @short_description: a DVB subtitle parsing class
 * @stability: Unstable
 *
 * The #DvbSub represents an object used for parsing a DVB subpicture,
 * and signalling the API user for new bitmaps to show on screen.
 */

#define MAX_NEG_CROP 1024
static guint8 ff_cropTbl[256 + 2 * MAX_NEG_CROP] = { 0, };

#define cm (ff_cropTbl + MAX_NEG_CROP)

/* FIXME: This is really ARGB... We might need this configurable for performant
 * FIXME: use in GStreamer as well if that likes RGBA more (Qt prefers ARGB) */
#define RGBA(r,g,b,a) (((a) << 24) | ((r) << 16) | ((g) << 8) | (b))

typedef struct DVBSubCLUT
{
  int id;                       /* default_clut uses -1 for this, so guint8 isn't fine without adaptations first */

  guint32 clut4[4];
  guint32 clut16[16];
  guint32 clut256[256];

  struct DVBSubCLUT *next;
} DVBSubCLUT;

static DVBSubCLUT default_clut;

typedef struct DVBSubObjectDisplay
{
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

typedef struct DVBSubObject
{
  /* FIXME: Use more correct sizes */
  int id;                       /* FIXME: Use guint8 after checking it's fine in all code using it */

  int type;

  /* FIXME: Should we use GSList? */
  DVBSubObjectDisplay *display_list;
  struct DVBSubObject *next;
} DVBSubObject;

typedef struct DVBSubRegionDisplay
{                               /* FIXME: Figure out if this structure is only used temporarily in page_segment parser, or also more */
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
  guint8 depth;                 /* If we want to make this a guint8, then need to ensure it isn't wrap around with reserved values in region handling code */

  guint8 clut;
  guint8 bgcolor;

  /* FIXME: Validate these fields existence and exact types */
  guint8 *pbuf;
  int buf_size;

  DVBSubObjectDisplay *display_list;

  struct DVBSubRegion *next;
} DVBSubRegion;

typedef struct _DvbSubPrivate DvbSubPrivate;
struct _DvbSubPrivate
{
  int fd;
  DvbSubCallbacks callbacks;
  gpointer user_data;

  guint8 page_time_out;
  DVBSubRegion *region_list;
  DVBSubCLUT *clut_list;
  DVBSubObject *object_list;
  /* FIXME... */
  int display_list_size;
  DVBSubRegionDisplay *display_list;
  GString *pes_buffer;
  DVBSubtitleWindow display_def;
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
get_object (DvbSub * dvb_sub, guint16 object_id)
{
  const DvbSubPrivate *priv = (DvbSubPrivate *) dvb_sub->private_data;
  DVBSubObject *ptr = priv->object_list;

  while (ptr && ptr->id != object_id) {
    ptr = ptr->next;
  }

  return ptr;
}

static DVBSubCLUT *
get_clut (DvbSub * dvb_sub, gint clut_id)
{
  const DvbSubPrivate *priv = (DvbSubPrivate *) dvb_sub->private_data;
  DVBSubCLUT *ptr = priv->clut_list;

  while (ptr && ptr->id != clut_id) {
    ptr = ptr->next;
  }

  return ptr;
}

// FIXME: Just pass private_data pointer directly here and in other get_* helper functions?
static DVBSubRegion *
get_region (DvbSub * dvb_sub, guint8 region_id)
{
  const DvbSubPrivate *priv = (DvbSubPrivate *) dvb_sub->private_data;
  DVBSubRegion *ptr = priv->region_list;

  while (ptr && ptr->id != region_id) {
    ptr = ptr->next;
  }

  return ptr;
}

static void
delete_region_display_list (DvbSub * dvb_sub, DVBSubRegion * region)
{
  const DvbSubPrivate *priv = (DvbSubPrivate *) dvb_sub->private_data;
  DVBSubObject *object, *obj2;
  DVBSubObject **obj2_ptr;
  DVBSubObjectDisplay *display, *obj_disp, **obj_disp_ptr;

  while (region->display_list) {
    display = region->display_list;

    object = get_object (dvb_sub, display->object_id);

    if (object) {
      obj_disp_ptr = &object->display_list;
      obj_disp = *obj_disp_ptr;

      while (obj_disp && obj_disp != display) {
        obj_disp_ptr = &obj_disp->object_list_next;
        obj_disp = *obj_disp_ptr;
      }

      if (obj_disp) {
        *obj_disp_ptr = obj_disp->object_list_next;

        if (!object->display_list) {
          obj2_ptr = &priv->object_list;
          obj2 = *obj2_ptr;

          while (obj2 != object) {
            g_assert (obj2);
            obj2_ptr = &obj2->next;
            obj2 = *obj2_ptr;
          }

          *obj2_ptr = obj2->next;

          g_slice_free (DVBSubObject, obj2);
        }
      }
    }

    region->display_list = display->region_list_next;

    g_slice_free (DVBSubObjectDisplay, display);
  }
}

static void
delete_state (DvbSub * dvb_sub)
{
  DvbSubPrivate *priv = (DvbSubPrivate *) dvb_sub->private_data;
  DVBSubRegion *region;

  while (priv->region_list) {
    region = priv->region_list;

    priv->region_list = region->next;

    delete_region_display_list (dvb_sub, region);
    if (region->pbuf)
      g_free (region->pbuf);

    g_slice_free (DVBSubRegion, region);
  }

  g_slice_free_chain (DVBSubCLUT, priv->clut_list, next);
  priv->clut_list = NULL;

  /* Should already be null */
  if (priv->object_list)
    g_warning ("Memory deallocation error!");
}

static void
dvb_sub_init (DvbSub * self)
{
  DvbSubPrivate *priv;

  self->private_data = priv = DVB_SUB_GET_PRIVATE (self);

  /* TODO: Add initialization code here */
  /* FIXME: Do we have a reason to initiate the members to zero, or are we guaranteed that anyway? */
  priv->fd = -1;
  priv->region_list = NULL;
  priv->object_list = NULL;
  priv->page_time_out = 0;      /* FIXME: Maybe 255 instead? */
  priv->pes_buffer = g_string_new (NULL);

  /* display/window information */
  priv->display_def.version = -1;
  priv->display_def.window_flag = 0;
  priv->display_def.display_width = 720;
  priv->display_def.display_height = 576;
}

static void
dvb_sub_finalize (GObject * object)
{
  DvbSub *self = DVB_SUB (object);
  DvbSubPrivate *priv = (DvbSubPrivate *) self->private_data;
  /* TODO: Add deinitalization code here */
  /* FIXME: Clear up region_list contents */
  if (priv->fd >= 0)
    dvb_sub_close_pid (self);
  delete_state (self);          /* close_pid should have called this, but lets be sure */
  g_string_free (priv->pes_buffer, TRUE);

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
dvb_sub_class_init (DvbSubClass * klass)
{
  int i, r, g, b, a = 0;
  GObjectClass *object_class = (GObjectClass *) klass;

  object_class->finalize = dvb_sub_finalize;

  g_type_class_add_private (klass, sizeof (DvbSubPrivate));

  dsputil_static_init ();       /* Initializes ff_cropTbl table, used in YUV_TO_RGB conversion */

  /* Initialize the static default_clut structure, from which other clut
   * structures are initialized from (to start off with default CLUTs
   * as defined in the specification). */
  default_clut.id = -1;

  default_clut.clut4[0] = RGBA (0, 0, 0, 0);
  default_clut.clut4[1] = RGBA (255, 255, 255, 255);
  default_clut.clut4[2] = RGBA (0, 0, 0, 255);
  default_clut.clut4[3] = RGBA (127, 127, 127, 255);

  default_clut.clut16[0] = RGBA (0, 0, 0, 0);
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
    default_clut.clut16[i] = RGBA (r, g, b, 255);
  }

  default_clut.clut256[0] = RGBA (0, 0, 0, 0);
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
    default_clut.clut256[i] = RGBA (r, g, b, a);
  }
}

static void
_dvb_sub_parse_page_segment (DvbSub * dvb_sub, guint16 page_id, guint8 * buf,
    gint buf_size)
{                               /* FIXME: Use guint for buf_size here and in many other places? */
  DvbSubPrivate *priv = (DvbSubPrivate *) dvb_sub->private_data;
  DVBSubRegionDisplay *display;
  DVBSubRegionDisplay *tmp_display_list, **tmp_ptr;

  const guint8 *buf_end = buf + buf_size;
  guint8 region_id;
  guint8 page_state;

#ifdef DEBUG
  static int counter = 0;
  static gchar *page_state_str[] = {
    "Normal case",
    "ACQUISITION POINT",
    "Mode Change",
    "RESERVED"
  };
#endif

  if (buf_size < 1)
    return;

  priv->page_time_out = *buf++;
  page_state = ((*buf++) >> 2) & 3;

#ifdef DEBUG
  ++counter;
  dvb_log (DVB_LOG_PAGE, G_LOG_LEVEL_DEBUG,
      "%d: page_id = %u, length = %d, page_time_out = %u seconds, page_state = %s",
      counter, page_id, buf_size, priv->page_time_out,
      page_state_str[page_state]);
#endif

  if (page_state == 2) {        /* Mode change */
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
      display = g_slice_new0 (DVBSubRegionDisplay);

    display->region_id = region_id;

    display->x_pos = GST_READ_UINT16_BE (buf);
    buf += 2;
    display->y_pos = GST_READ_UINT16_BE (buf);
    buf += 2;

    *tmp_ptr = display->next;

    display->next = priv->display_list;
    priv->display_list = display;
    priv->display_list_size++;

    dvb_log (DVB_LOG_PAGE, G_LOG_LEVEL_DEBUG,
        "%d: REGION information: ID = %u, address = %ux%u",
        counter, region_id, display->x_pos, display->y_pos);
  }

  while (tmp_display_list) {
    display = tmp_display_list;

    tmp_display_list = display->next;

    g_slice_free (DVBSubRegionDisplay, display);
  }
}

static void
_dvb_sub_parse_region_segment (DvbSub * dvb_sub, guint16 page_id, guint8 * buf,
    gint buf_size)
{
  DvbSubPrivate *priv = (DvbSubPrivate *) dvb_sub->private_data;

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

  if (!region) {                /* Create a new region */
    region = g_slice_new0 (DVBSubRegion);
    region->id = region_id;
    region->next = priv->region_list;
    priv->region_list = region;
  }

  fill = ((*buf++) >> 3) & 1;

  region->width = GST_READ_UINT16_BE (buf);
  buf += 2;
  region->height = GST_READ_UINT16_BE (buf);
  buf += 2;

  if (region->width * region->height != region->buf_size) {     /* FIXME: Read closer from spec what happens when dimensions change */
    if (region->pbuf)
      g_free (region->pbuf);

    region->buf_size = region->width * region->height;

    region->pbuf = g_malloc (region->buf_size); /* TODO: We can probably use GSlice here if careful about freeing while buf_size still records the correct size */

    fill = 1;                   /* FIXME: Validate from spec that fill is forced on (in the following codes context) when dimensions change */
  }

  region->depth = 1 << (((*buf++) >> 2) & 7);
  if (region->depth < 2 || region->depth > 8) {
    g_warning ("region depth %d is invalid\n", region->depth);
    region->depth = 4;          /* FIXME: Check from spec this is the default? */
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

  dvb_log (DVB_LOG_REGION, G_LOG_LEVEL_DEBUG,
      "id = %u, (%ux%u)@%u-bit",
      region_id, region->width, region->height, region->depth);

  if (fill) {
    memset (region->pbuf, region->bgcolor, region->buf_size);
    dvb_log (DVB_LOG_REGION, G_LOG_LEVEL_DEBUG,
        "Filling region (%u) with bgcolor = %u", region->id, region->bgcolor);
  }

  delete_region_display_list (dvb_sub, region); /* Delete the region display list for current region - FIXME: why? */

  while (buf + 6 <= buf_end) {
    object_id = GST_READ_UINT16_BE (buf);
    buf += 2;

    object = get_object (dvb_sub, object_id);

    if (!object) {
      object = g_slice_new0 (DVBSubObject);

      object->id = object_id;

      object->next = priv->object_list;
      priv->object_list = object;
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

    dvb_log (DVB_LOG_REGION, G_LOG_LEVEL_DEBUG,
        "REGION DATA: object_id = %u, region_id = %u, pos = %ux%u, obj_type = %u",
        object->id, region->id, object_display->x_pos, object_display->y_pos,
        object->type);
    if (object->type == 1 || object->type == 2)
      dvb_log (DVB_LOG_REGION, G_LOG_LEVEL_DEBUG,
          "REGION DATA: fgcolor = %u, bgcolor = %u\n", object_display->fgcolor,
          object_display->bgcolor);
  }
}

static void
_dvb_sub_parse_clut_segment (DvbSub * dvb_sub, guint16 page_id, guint8 * buf,
    gint buf_size)
{
  DvbSubPrivate *priv = (DvbSubPrivate *) dvb_sub->private_data;

  const guint8 *buf_end = buf + buf_size;
  guint8 clut_id;
  DVBSubCLUT *clut;
  int entry_id, depth, full_range;
  int y, cr, cb, alpha;
  int r, g, b, r_add, g_add, b_add;

#ifdef DEBUG_PACKET_CONTENTS
  g_print ("DVB clut packet:\n");
  gst_util_dump_mem (buf, buf_size);
#endif

  clut_id = *buf++;
  buf += 1;

  clut = get_clut (dvb_sub, clut_id);

  if (!clut) {
    clut = g_slice_new (DVBSubCLUT);    /* FIXME-MEMORY-LEAK: This seems to leak per valgrind */

    memcpy (clut, &default_clut, sizeof (DVBSubCLUT));

    clut->id = clut_id;

    clut->next = priv->clut_list;
    priv->clut_list = clut;
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

    YUV_TO_RGB1_CCIR (cb, cr);
    YUV_TO_RGB2_CCIR (r, g, b, y);

    dvb_log (DVB_LOG_CLUT, G_LOG_LEVEL_DEBUG,
        "CLUT DEFINITION: clut %d := (%d,%d,%d,%d)", entry_id, r, g, b, alpha);

    if (depth & 0x80)
      clut->clut4[entry_id] = RGBA (r, g, b, 255 - alpha);
    if (depth & 0x40)
      clut->clut16[entry_id] = RGBA (r, g, b, 255 - alpha);
    if (depth & 0x20)
      clut->clut256[entry_id] = RGBA (r, g, b, 255 - alpha);
  }
}

// FFMPEG-FIXME: The same code in ffmpeg is much more complex, it could use the same
// FFMPEG-FIXME: refactoring as done here
static int
_dvb_sub_read_2bit_string (guint8 * destbuf, gint dbuf_len,
    const guint8 ** srcbuf, gint buf_size, guint8 non_mod, guint8 * map_table)
{
  GstBitReader gb = GST_BIT_READER_INIT (*srcbuf, buf_size);
  /* FIXME: Handle FALSE returns from gst_bit_reader_get_* calls? */

  gboolean stop_parsing = FALSE;
  guint32 bits;
  guint32 pixels_read = 0;

  static gboolean warning_shown = FALSE;
  if (!warning_shown) {
    g_warning
        ("Parsing 2bit color DVB sub-picture. This is not tested at all. If you see this message, "
        "please provide the developers with sample media with these subtitles, if possible.");
    warning_shown = TRUE;
  }
  dvb_log (DVB_LOG_PIXEL, G_LOG_LEVEL_DEBUG,
      "(n=2): Inside %s with dbuf_len = %d", __PRETTY_FUNCTION__, dbuf_len);

  while (!stop_parsing && (gst_bit_reader_get_remaining (&gb) > 0)) {
    guint run_length = 0, clut_index = 0;
    gst_bit_reader_get_bits_uint32 (&gb, &bits, 2);

    if (bits) {                 /* 2-bit_pixel-code */
      run_length = 1;
      clut_index = bits;
    } else {                    /* 2-bit_zero */
      gst_bit_reader_get_bits_uint32 (&gb, &bits, 1);
      if (bits == 1) {          /* switch_1 == '1' */
        gst_bit_reader_get_bits_uint32 (&gb, &run_length, 3);
        run_length += 3;
        gst_bit_reader_get_bits_uint32 (&gb, &clut_index, 2);
      } else {                  /* switch_1 == '0' */
        gst_bit_reader_get_bits_uint32 (&gb, &bits, 1);
        if (bits == 1) {        /* switch_2 == '1' */
          run_length = 1;       /* 1x pseudo-colour '00' */
        } else {                /* switch_2 == '0' */
          gst_bit_reader_get_bits_uint32 (&gb, &bits, 2);
          switch (bits) {       /* switch_3 */
            case 0x0:          /* end of 2-bit/pixel_code_string */
              stop_parsing = TRUE;
              break;
            case 0x1:          /* two pixels shall be set to pseudo colour (entry) '00' */
              run_length = 2;
              break;
            case 0x2:          /* the following 6 bits contain run length coded pixel data */
              gst_bit_reader_get_bits_uint32 (&gb, &run_length, 4);
              run_length += 12;
              gst_bit_reader_get_bits_uint32 (&gb, &clut_index, 2);
              break;
            case 0x3:          /* the following 10 bits contain run length coded pixel data */
              gst_bit_reader_get_bits_uint32 (&gb, &run_length, 8);
              run_length += 29;
              gst_bit_reader_get_bits_uint32 (&gb, &clut_index, 2);
              break;
          }
        }
      }
    }

    /* If run_length is zero, continue. Only case happening is when
     * stop_parsing is TRUE too, so next cycle shouldn't run */
    if (run_length == 0)
      continue;

    /* Trim the run_length to not go beyond the line end and consume
     * it from remaining length of dest line */
    run_length = MIN (run_length, dbuf_len);
    dbuf_len -= run_length;

    /* Make clut_index refer to the index into the desired bit depths
     * CLUT definition table */
    if (map_table)
      clut_index = map_table[clut_index];       /* now clut_index signifies the index into map_table dest */

    /* Now we can simply memset run_length count of destination bytes
     * to clut_index, but only if not non_modifying */
    dvb_log (DVB_LOG_RUNLEN, G_LOG_LEVEL_DEBUG,
        "Setting %u pixels to color 0x%x in destination buffer; dbuf_len left is %d pixels",
        run_length, clut_index, dbuf_len);
    if (!(non_mod == 1 && bits == 1))
      memset (destbuf, clut_index, run_length);

    destbuf += run_length;
    pixels_read += run_length;
  }

  // FIXME: Test skip_to_byte instead of adding 7 bits, once everything else is working good
  //gst_bit_reader_skip_to_byte (&gb);
  *srcbuf += (gst_bit_reader_get_pos (&gb) + 7) >> 3;

  dvb_log (DVB_LOG_PIXEL, G_LOG_LEVEL_DEBUG,
      "Returning from 2bit_string parser with %u pixels read", pixels_read);
  // FIXME: Shouldn't need this variable if tracking things in the loop better
  return pixels_read;
}

// FFMPEG-FIXME: The same code in ffmpeg is much more complex, it could use the same
// FFMPEG-FIXME: refactoring as done here, explained in commit 895296c3
static int
_dvb_sub_read_4bit_string (guint8 * destbuf, gint dbuf_len,
    const guint8 ** srcbuf, gint buf_size, guint8 non_mod, guint8 * map_table)
{
  GstBitReader gb = GST_BIT_READER_INIT (*srcbuf, buf_size);
  /* FIXME: Handle FALSE returns from gst_bit_reader_get_* calls? */
  gboolean stop_parsing = FALSE;
  guint32 bits;
  guint32 pixels_read = 0;

  dvb_log (DVB_LOG_RUNLEN, G_LOG_LEVEL_DEBUG,
      "Entering 4bit_string parser at srcbuf position %p with buf_size = %d; destination buffer size is %d @ %p",
      *srcbuf, buf_size, dbuf_len, destbuf);

  while (!stop_parsing && (gst_bit_reader_get_remaining (&gb) > 0)) {
    guint run_length = 0, clut_index = 0;
    gst_bit_reader_get_bits_uint32 (&gb, &bits, 4);

    if (bits) {
      run_length = 1;
      clut_index = bits;
    } else {
      gst_bit_reader_get_bits_uint32 (&gb, &bits, 1);
      if (bits == 0) {          /* switch_1 == '0' */
        gst_bit_reader_get_bits_uint32 (&gb, &run_length, 3);
        if (!run_length) {
          stop_parsing = TRUE;
        } else {
          run_length += 2;
        }
      } else {                  /* switch_1 == '1' */
        gst_bit_reader_get_bits_uint32 (&gb, &bits, 1);
        if (bits == 0) {        /* switch_2 == '0' */
          gst_bit_reader_get_bits_uint32 (&gb, &run_length, 2);
          run_length += 4;
          gst_bit_reader_get_bits_uint32 (&gb, &clut_index, 4);
        } else {                /* switch_2 == '1' */
          gst_bit_reader_get_bits_uint32 (&gb, &bits, 2);
          switch (bits) {
            case 0x0:          /* switch_3 == '00' */
              run_length = 1;   /* 1 pixel of pseudo-color 0 */
              break;
            case 0x1:          /* switch_3 == '01' */
              run_length = 2;   /* 2 pixels of pseudo-color 0 */
              break;
            case 0x2:          /* switch_3 == '10' */
              gst_bit_reader_get_bits_uint32 (&gb, &run_length, 4);
              run_length += 9;
              gst_bit_reader_get_bits_uint32 (&gb, &clut_index, 4);
              break;
            case 0x3:          /* switch_3 == '11' */
              gst_bit_reader_get_bits_uint32 (&gb, &run_length, 8);
              run_length += 25;
              gst_bit_reader_get_bits_uint32 (&gb, &clut_index, 4);
              break;
          }
        }
      }
    }

    /* If run_length is zero, continue. Only case happening is when
     * stop_parsing is TRUE too, so next cycle shouldn't run */
    if (run_length == 0)
      continue;

    /* Trim the run_length to not go beyond the line end and consume
     * it from remaining length of dest line */
    run_length = MIN (run_length, dbuf_len);
    dbuf_len -= run_length;

    /* Make clut_index refer to the index into the desired bit depths
     * CLUT definition table */
    if (map_table)
      clut_index = map_table[clut_index];       /* now clut_index signifies the index into map_table dest */

    /* Now we can simply memset run_length count of destination bytes
     * to clut_index, but only if not non_modifying */
    dvb_log (DVB_LOG_RUNLEN, G_LOG_LEVEL_DEBUG,
        "Setting %u pixels to color 0x%x in destination buffer; dbuf_len left is %d pixels",
        run_length, clut_index, dbuf_len);
    if (!(non_mod == 1 && bits == 1))
      memset (destbuf, clut_index, run_length);

    destbuf += run_length;
    pixels_read += run_length;
  }

  // FIXME: Test skip_to_byte instead of adding 7 bits, once everything else is working good
  //gst_bit_reader_skip_to_byte (&gb);
  *srcbuf += (gst_bit_reader_get_pos (&gb) + 7) >> 3;

  dvb_log (DVB_LOG_PIXEL, G_LOG_LEVEL_DEBUG,
      "Returning from 4bit_string parser with %u pixels read", pixels_read);
  // FIXME: Shouldn't need this variable if tracking things in the loop better
  return pixels_read;
}

static int
_dvb_sub_read_8bit_string (guint8 * destbuf, gint dbuf_len,
    const guint8 ** srcbuf, gint buf_size, guint8 non_mod, guint8 * map_table)
{
  GstBitReader gb = GST_BIT_READER_INIT (*srcbuf, buf_size);
  /* FIXME: Handle FALSE returns from gst_bit_reader_get_* calls? */

  gboolean stop_parsing = FALSE;
  guint32 bits;
  guint32 pixels_read = 0;

  static gboolean warning_shown = FALSE;
  if (!warning_shown) {
    g_warning
        ("Parsing 8bit color DVB sub-picture. This is not tested at all. If you see this message, "
        "please provide the developers with sample media with these subtitles, if possible.");
    warning_shown = TRUE;
  }
  dvb_log (DVB_LOG_PIXEL, G_LOG_LEVEL_DEBUG,
      "(n=8): Inside %s with dbuf_len = %d", __PRETTY_FUNCTION__, dbuf_len);

  /* FFMPEG-FIXME: ffmpeg uses a manual byte walking algorithm, which might be more performant,
   * FFMPEG-FIXME: but it does almost absolutely no buffer length checking, so could walk over
   * FFMPEG-FIXME: memory boundaries. While we don't check gst_bit_reader_get_bits_uint32
   * FFMPEG-FIXME: return values either and therefore might get some pixels corrupted, we at
   * FFMPEG-FIXME: lest have no chance of reading memory we don't own and visual corruption
   * FFMPEG-FIXME: is guaranteed anyway when not all bytes are present */
  /* Rephrased - it's better to work with bytes with default value '0' instead of reading from memory we don't own. */
  while (!stop_parsing && (gst_bit_reader_get_remaining (&gb) > 0)) {
    guint run_length = 0, clut_index = 0;
    gst_bit_reader_get_bits_uint32 (&gb, &bits, 8);

    if (bits) {                 /* 8-bit_pixel-code */
      run_length = 1;
      clut_index = bits;
    } else {                    /* 8-bit_zero */
      gst_bit_reader_get_bits_uint32 (&gb, &bits, 1);
      if (bits == 0) {          /* switch_1 == '0' */
        /* run_length_1-127 for pseudo-colour _entry) '0x00' */
        gst_bit_reader_get_bits_uint32 (&gb, &run_length, 7);
        if (run_length == 0) {  /* end_of_string_signal */
          stop_parsing = TRUE;
        }
      } else {                  /* switch_1 == '1' */
        /* run_length_3-127 */
        gst_bit_reader_get_bits_uint32 (&gb, &run_length, 7);
        gst_bit_reader_get_bits_uint32 (&gb, &clut_index, 8);
#ifdef DEBUG
        /* Emit a debugging message about stream not following specification */
        if (run_length < 3) {
          dvb_log (DVB_LOG_STREAM, G_LOG_LEVEL_WARNING,
              "8-bit/pixel_code_string::run_length_3-127 value was %u, but the spec requires it must be >=3",
              run_length);
        }
#endif
      }
    }

    /* If run_length is zero, continue. Only case happening is when
     * stop_parsing is TRUE too, so next cycle shouldn't run */
    if (run_length == 0)
      continue;

    /* Trim the run_length to not go beyond the line end and consume
     * it from remaining length of dest line */
    run_length = MIN (run_length, dbuf_len);
    dbuf_len -= run_length;

    /* Make clut_index refer to the index into the desired bit depths
     * CLUT definition table */
    if (map_table)
      clut_index = map_table[clut_index];       /* now clut_index signifies the index into map_table dest */

    /* Now we can simply memset run_length count of destination bytes
     * to clut_index, but only if not non_modifying */
    dvb_log (DVB_LOG_RUNLEN, G_LOG_LEVEL_DEBUG,
        "Setting %u pixels to color 0x%x in destination buffer; dbuf_len left is %d pixels",
        run_length, clut_index, dbuf_len);
    if (!(non_mod == 1 && bits == 1))
      memset (destbuf, clut_index, run_length);

    destbuf += run_length;
    pixels_read += run_length;
  }

  dvb_log (DVB_LOG_PIXEL, G_LOG_LEVEL_DEBUG,
      "Returning from 8bit_string parser with %u pixels read", pixels_read);
  // FIXME: Shouldn't need this variable if tracking things in the loop better
  return pixels_read;
}

static void
_dvb_sub_parse_pixel_data_block (DvbSub * dvb_sub,
    DVBSubObjectDisplay * display, const guint8 * buf, gint buf_size,
    DvbSubPixelDataSubBlockFieldType top_bottom, guint8 non_mod)
{
  DvbSubPrivate *priv = (DvbSubPrivate *) dvb_sub->private_data;

  DVBSubRegion *region = get_region (dvb_sub, display->region_id);
  const guint8 *buf_end = buf + buf_size;
  guint8 *pbuf;
  int x_pos, y_pos;
  int i;
  gboolean dest_buf_filled = FALSE;

  guint8 map2to4[] = { 0x0, 0x7, 0x8, 0xf };
  guint8 map2to8[] = { 0x00, 0x77, 0x88, 0xff };
  guint8 map4to8[] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
  };
  guint8 *map_table;

  dvb_log (DVB_LOG_PIXEL, G_LOG_LEVEL_DEBUG,
      "(parse_block): DVB pixel block size %d, %s field:",
      buf_size, top_bottom ? "bottom" : "top");

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
    dvb_log (DVB_LOG_PIXEL, G_LOG_LEVEL_DEBUG,
        "Iteration start, %u bytes missing from end; buf = %p, buf_end = %p;  "
        "Region is number %u, with a dimension of %dx%d; We are at position %dx%d",
        buf_end - buf, buf, buf_end,
        region->id, region->width, region->height, x_pos, y_pos);
    // FFMPEG-FIXME: ffmpeg doesn't check for equality and so can overflow destination buffer later on with bad input data
    // FFMPEG-FIXME: However that makes it warn on end_of_object_line and map tables as well, so we add the dest_buf_filled tracking
    // FIXME: Removed x_pos checking here, because we don't want to turn dest_buf_filled to TRUE permanently in that case
    // FIXME: We assume that region->width - x_pos as dbuf_len to read_nbit_string will take care of that case nicely;
    // FIXME: That is, that read_nbit_string never scribbles anything if dbuf_len passed to it is zero due to this.
    if (y_pos >= region->height) {
      dest_buf_filled = TRUE;
    }

    switch (*buf++) {
      case 0x10:
        if (dest_buf_filled) {
          g_warning ("Invalid object location for data_type 0x%x!\n", *(buf - 1));      /* FIXME: Be more verbose */
          g_print ("Remaining data after invalid object location:\n");
          gst_util_dump_mem (buf, buf_end - buf);
          return;
        }

        if (region->depth == 8)
          map_table = map2to8;
        else if (region->depth == 4)
          map_table = map2to4;
        else
          map_table = NULL;

        // FFMPEG-FIXME: ffmpeg code passes buf_size instead of buf_end - buf, and could
        // FFMPEG-FIXME: therefore potentially walk over the memory area we own
        x_pos +=
            _dvb_sub_read_2bit_string (pbuf + (y_pos * region->width) + x_pos,
            region->width - x_pos, &buf, buf_end - buf, non_mod, map_table);
        break;
      case 0x11:
        if (dest_buf_filled) {
          g_warning ("Invalid object location for data_type 0x%x!\n", *(buf - 1));      /* FIXME: Be more verbose */
          g_print ("Remaining data after invalid object location:\n");
          gst_util_dump_mem (buf, buf_end - buf);
          return;               // FIXME: Perhaps tell read_nbit_string that dbuf_len is zero and let it walk the bytes regardless? (Same FIXME for 2bit and 8bit)
        }

        if (region->depth < 4) {
          g_warning ("4-bit pixel string in %d-bit region!\n", region->depth);
          return;
        }

        if (region->depth == 8)
          map_table = map4to8;
        else
          map_table = NULL;

        dvb_log (DVB_LOG_PIXEL, G_LOG_LEVEL_DEBUG,
            "READ_nBIT_STRING (4): String data into position %dx%d; buf before is %p\n",
            x_pos, y_pos, buf);
        // FFMPEG-FIXME: ffmpeg code passes buf_size instead of buf_end - buf, and could
        // FFMPEG-FIXME: therefore potentially walk over the memory area we own
        x_pos +=
            _dvb_sub_read_4bit_string (pbuf + (y_pos * region->width) + x_pos,
            region->width - x_pos, &buf, buf_end - buf, non_mod, map_table);
        dvb_log (DVB_LOG_PIXEL, G_LOG_LEVEL_DEBUG,
            "READ_nBIT_STRING (4) finished: buf pointer now %p", buf);
        break;
      case 0x12:
        if (dest_buf_filled) {
          g_warning ("Invalid object location for data_type 0x%x!\n", *(buf - 1));      /* FIXME: Be more verbose */
          g_print ("Remaining data after invalid object location:\n");
          gst_util_dump_mem (buf, buf_end - buf);
          return;
        }

        if (region->depth < 8) {
          g_warning ("8-bit pixel string in %d-bit region!\n", region->depth);
          return;
        }
        // FFMPEG-FIXME: ffmpeg code passes buf_size instead of buf_end - buf, and could
        // FFMPEG-FIXME: therefore potentially walk over the memory area we own
        x_pos +=
            _dvb_sub_read_8bit_string (pbuf + (y_pos * region->width) + x_pos,
            region->width - x_pos, &buf, buf_end - buf, non_mod, NULL);
        break;

      case 0x20:
        dvb_log (DVB_LOG_PIXEL, G_LOG_LEVEL_DEBUG,
            "(parse_block): handling map2to4 table data");
        /* FIXME: I don't see any guards about buffer size here - buf++ happens with the switch, but
         * FIXME: buffer is walked without length checks? Same deal in other map table cases */
        map2to4[0] = (*buf) >> 4;
        map2to4[1] = (*buf++) & 0xf;
        map2to4[2] = (*buf) >> 4;
        map2to4[3] = (*buf++) & 0xf;
        break;
      case 0x21:
        dvb_log (DVB_LOG_PIXEL, G_LOG_LEVEL_DEBUG,
            "(parse_block): handling map2to8 table data");
        for (i = 0; i < 4; i++)
          map2to8[i] = *buf++;
        break;
      case 0x22:
        dvb_log (DVB_LOG_PIXEL, G_LOG_LEVEL_DEBUG,
            "(parse_block): handling map4to8 table data");
        for (i = 0; i < 16; i++)
          map4to8[i] = *buf++;
        break;

      case 0xf0:
        dvb_log (DVB_LOG_PIXEL, G_LOG_LEVEL_DEBUG,
            "(parse_block): end of object line code encountered");
        x_pos = display->x_pos;
        y_pos += 2;
        break;
      default:
        /* FIXME: Do we consume word align stuffing byte that could follow top/bottom data? */
        g_warning ("Unknown/unsupported pixel block 0x%x", *(buf - 1));
    }
  }
}

static void
_dvb_sub_parse_object_segment (DvbSub * dvb_sub, guint16 page_id, guint8 * buf,
    gint buf_size)
{
  DvbSubPrivate *priv = (DvbSubPrivate *) dvb_sub->private_data;

  const guint8 *buf_end = buf + buf_size;
  guint object_id;
  DVBSubObject *object;

  guint8 coding_method, non_modifying_color;

  object_id = GST_READ_UINT16_BE (buf);
  buf += 2;

  object = get_object (dvb_sub, object_id);

  dvb_log (DVB_LOG_OBJECT, G_LOG_LEVEL_DEBUG,
      "parse_object_segment: A new object segment has occurred for object_id = %u",
      object_id);

  if (!object) {
    g_warning
        ("Nothing known about object with ID %u yet inside parse_object_segment, bailing out",
        object_id);
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

    /* FIXME: Potential optimization opportunity here - parse the object pixmap only once, and copy it to all the
     * FIXME: regions that need it. One object being in multiple regions is a rare occurrence in real life, however */
    for (display = object->display_list; display;
        display = display->object_list_next) {
      block = buf;

      dvb_log (DVB_LOG_OBJECT, G_LOG_LEVEL_DEBUG,
          "Parsing top and bottom part of object id %d; top_field_len = %u, bottom_field_len = %u",
          display->object_id, top_field_len, bottom_field_len);
      _dvb_sub_parse_pixel_data_block (dvb_sub, display, block, top_field_len,
          TOP_FIELD, non_modifying_color);

      if (bottom_field_len > 0)
        block = buf + top_field_len;
      else
        bottom_field_len = top_field_len;

      _dvb_sub_parse_pixel_data_block (dvb_sub, display, block,
          bottom_field_len, BOTTOM_FIELD, non_modifying_color);
    }

  } else if (coding_method == 1) {
    g_warning ("'a string of characters' coding method not supported (yet?)!");
  } else {
    g_warning ("%s: Unknown object coding 0x%x\n", __PRETTY_FUNCTION__,
        coding_method);
  }
}

#ifdef DEBUG_SAVE_IMAGES
static void
png_save2 (const char *filename, guint32 * bitmap, int w, int h)
{
  int x, y, v;
  FILE *f;
  char fname[40], fname2[40];
  char command[1024];

  snprintf (fname, sizeof (fname), "%s.ppm", filename);

  f = fopen (fname, "w");
  if (!f) {
    perror (fname);
    exit (1);
  }
  fprintf (f, "P6\n" "%d %d\n" "%d\n", w, h, 255);
  for (y = 0; y < h; y++) {
    for (x = 0; x < w; x++) {
      v = bitmap[y * w + x];
      putc ((v >> 16) & 0xff, f);
      putc ((v >> 8) & 0xff, f);
      putc ((v >> 0) & 0xff, f);
    }
  }
  fclose (f);


  snprintf (fname2, sizeof (fname2), "%s-a.pgm", filename);

  f = fopen (fname2, "w");
  if (!f) {
    perror (fname2);
    exit (1);
  }
  fprintf (f, "P5\n" "%d %d\n" "%d\n", w, h, 255);
  for (y = 0; y < h; y++) {
    for (x = 0; x < w; x++) {
      v = bitmap[y * w + x];
      putc ((v >> 24) & 0xff, f);
    }
  }
  fclose (f);

  snprintf (command, sizeof (command),
      "pnmtopng -alpha %s %s > %s.png 2> /dev/null", fname2, fname, filename);
  system (command);

  //snprintf(command, sizeof(command), "rm %s %s", fname, fname2);
  //system(command);
}

static void
save_display_set (DvbSub * dvb_sub)
{
  DvbSubPrivate *priv = (DvbSubPrivate *) dvb_sub->private_data;

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
    region = get_region (dvb_sub, display->region_id);

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
      region = get_region (dvb_sub, display->region_id);

      x_off = display->x_pos - x_pos;
      y_off = display->y_pos - y_pos;

      clut = get_clut (dvb_sub, region->clut);

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

    snprintf (filename, sizeof (filename), "dvbs.%d", fileno_index);

    png_save2 (filename, pbuf, width, height);

    g_free (pbuf);
  }

  fileno_index++;
}
#endif

static gint
_dvb_sub_parse_display_definition_segment (DvbSub * dvb_sub, guint8 * buf,
    gint buf_size)
{
  int dds_version, info_byte;
  DvbSubPrivate *ctx = dvb_sub->private_data;

  if (buf_size < 5)
    return -1;

  info_byte = *buf++;
  dds_version = info_byte >> 4;

  if (ctx->display_def.version == dds_version)
    return 0;                   /* already have this display definition version */

  ctx->display_def.version = dds_version;
  ctx->display_def.display_width = GST_READ_UINT16_BE (buf) + 1;
  buf += 2;
  ctx->display_def.display_height = GST_READ_UINT16_BE (buf) + 1;
  buf += 2;

  ctx->display_def.window_flag = info_byte & 1 << 3;

  if (buf_size >= 13 && ctx->display_def.window_flag) {
    ctx->display_def.window_x = GST_READ_UINT16_BE (buf);
    buf += 2;
    ctx->display_def.window_y = GST_READ_UINT16_BE (buf);
    buf += 2;
    ctx->display_def.window_width =
        GST_READ_UINT16_BE (buf) - ctx->display_def.window_x + 1;
    buf += 2;
    ctx->display_def.window_height =
        GST_READ_UINT16_BE (buf) - ctx->display_def.window_y + 1;
    buf += 2;
  }

  return 0;
}

static gint
_dvb_sub_parse_end_of_display_set (DvbSub * dvb_sub, guint16 page_id,
    guint8 * buf, gint buf_size, guint64 pts)
{
  DvbSubPrivate *priv = (DvbSubPrivate *) dvb_sub->private_data;

  DVBSubtitles *sub = g_slice_new0 (DVBSubtitles);

  DVBSubRegion *region;
  DVBSubRegionDisplay *display;
  DVBSubtitleRect *rect;
  DVBSubCLUT *clut;
  guint32 *clut_table;
  int i;

  dvb_log (DVB_LOG_DISPLAY, G_LOG_LEVEL_DEBUG,
      "END OF DISPLAY SET: page_id = %u, length = %d\n", page_id, buf_size);

  sub->rects = NULL;
#if 0                           /* FIXME: PTS stuff not figured out yet */
  sub->start_display_time = 0;
  sub->end_display_time = priv->page_time_out * 1000;
  sub->format = 0;              /* 0 = graphics */
#endif

  sub->num_rects = priv->display_list_size;

  if (sub->num_rects > 0) {
    // FIXME-MEMORY-LEAK: This structure is not freed up yet
    sub->rects = g_malloc0 (sizeof (*sub->rects) * sub->num_rects);     /* GSlice? */
    for (i = 0; i < sub->num_rects; i++)
      sub->rects[i] = g_malloc0 (sizeof (*sub->rects[i]));      /* GSlice? */
  }

  i = 0;

  /* copy subtitle display and window information */
  sub->display_def = priv->display_def;

  for (display = priv->display_list; display; display = display->next) {
    region = get_region (dvb_sub, display->region_id);
    rect = sub->rects[i];

    if (!region)
      continue;

    rect->x = display->x_pos;
    rect->y = display->y_pos;
    rect->w = region->width;
    rect->h = region->height;
#if 0                           /* FIXME: Don't think we need to save the number of colors in the palette when we are saving as RGBA? */
    rect->nb_colors = 16;
#endif
#if 0                           /* FIXME: Needed to be specified once we support strings of characters based subtitles */
    rect->type = SUBTITLE_BITMAP;
#endif
    rect->pict.rowstride = region->width;
    rect->pict.palette_bits_count = region->depth;

    clut = get_clut (dvb_sub, region->clut);

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
    rect->pict.palette = g_malloc ((1 << region->depth) * sizeof (guint32));    /* FIXME: Can we use GSlice here? */
    memcpy (rect->pict.palette, clut_table,
        (1 << region->depth) * sizeof (guint32));
#if 0
    g_print ("rect->pict.data.palette content:\n");
    gst_util_dump_mem (rect->pict.palette,
        (1 << region->depth) * sizeof (guint32));
#endif

    rect->pict.data = g_malloc (region->buf_size);      /* FIXME: Can we use GSlice here? */
    memcpy (rect->pict.data, region->pbuf, region->buf_size);

    static unsigned counter = 0;
    ++counter;
    dvb_log (DVB_LOG_DISPLAY, G_LOG_LEVEL_DEBUG,
        "An object rect created: number %u, iteration %u, pos: %d:%d, size: %dx%d",
        counter, i, rect->x, rect->y, rect->w, rect->h);
#if 0
    g_print ("rect->pict.data content:\n");
    gst_util_dump_mem (rect->pict.data, region->buf_size);
#endif

    ++i;
  }

  sub->num_rects = i;

#ifdef DEBUG_SAVE_IMAGES
  save_display_set (dvb_sub);
#endif

  if (priv->callbacks.new_data)
    priv->callbacks.new_data (dvb_sub, pts, sub, priv->page_time_out,
        priv->user_data);

  /* Now free up all the temporary memory we allocated */
  for (i = 0; i < sub->num_rects; ++i) {
    rect = sub->rects[i];

    g_free (rect->pict.palette);
    g_free (rect->pict.data);
    g_free (rect);
  }
  g_free (sub->rects);
  g_slice_free (DVBSubtitles, sub);

  return 1;                     /* FIXME: The caller of this function is probably supposed to do something with the return value */
}

/**
 * dvb_sub_new:
 *
 * Creates a new #DvbSub.
 *
 * Return value: a newly created #DvbSub
 */
DvbSub *
dvb_sub_new (void)
{
  DvbSub *dvbsub = g_object_new (DVB_TYPE_SUB, NULL);

  return dvbsub;
}

/**
 * dvb_sub_feed:
 * @dvb_sub: a #DvbSub
 * @data: The data to feed to the parser
 * @len: Length of the data
 *
 * Feeds the DvbSub parser with new PES packet data to parse.
 * The data given must be a full PES packet, which must
 * include a PTS field in the headers.
 * Only one packet is handled in one call, so the available data
 * should be fed continously until all is consumed.
 *
 * Return value: a negative value on errors, -4 if simply not enough data for the PES packet;
 *               Amount of data consumed (length of handled PES packet on success)
 */
gint
dvb_sub_feed (DvbSub * dvb_sub, guint8 * data, gint len)
{
  guint64 pts = 0;
  unsigned int pos = 0;
  guint16 PES_packet_len;
  guint8 PES_packet_header_len;
  gboolean is_subtitle_packet = TRUE;
  gboolean pts_field_present = FALSE;

  if (len == 0)
    return 0;

  if (len <= 8) {
    dvb_log (DVB_LOG_PACKET, G_LOG_LEVEL_WARNING,
        "Length %d too small for further processing", len);
    return -1;
  }

  if (data[0] != 0x00 || data[1] != 0x00 || data[2] != 0x01) {
    dvb_log (DVB_LOG_PACKET, G_LOG_LEVEL_WARNING,
        "Data fed to dvb_sub_feed is not a PES packet - does not start with a code_prefix of 0x000001");
    return 1;                   // FIXME: Probably handle it? - we need to skip PES_packet_len from this elementary stream then and move on
  }

  if (data[3] != 0xBD) {
    dvb_log (DVB_LOG_PACKET, G_LOG_LEVEL_INFO,
        "Data fed to dvb_sub_feed is not a PES packet of type private_stream_1, but rather '0x%X', so not a subtitle stream",
        data[3]);
    is_subtitle_packet = FALSE;
  }

  PES_packet_len = (data[4] << 8) | data[5];
  dvb_log (DVB_LOG_PACKET, G_LOG_LEVEL_DEBUG,
      "PES packet length is %u", PES_packet_len);
  pos = 6;

  /* FIXME: If the packet is cut, we could be feeding data more than we actually have here, which breaks everything. Probably need to buffer up and handle it,
   * FIXME: Or push back in front to the file descriptor buffer (but we are using read, not libc buffered fread, so that idea might not be possible )*/
  if ((len - 5) < PES_packet_len) {
    dvb_log (DVB_LOG_PACKET, G_LOG_LEVEL_WARNING,
        "!!!!!!!!!!! claimed PES packet length was %d, but we only had %d bytes available, falling back and waiting more !!!!!!!!!",
        PES_packet_len, len - 5);
    return -4;
  }
  /* FIXME: Validate sizes inbetween here */

  /* If this is a non-subtitle packet, still skip the data, pretending we consumed it (FIXME: Signal up) */
  if (!is_subtitle_packet) {
    return pos + PES_packet_len;
  }

  pos++;

  if (data[pos++] & 0x80) {     /* PTS fields present (possibly also DTS). Technically this must be present per the spec */
    pts_field_present = TRUE;
  }

  /* pos should be 8 now anyway */
  pos = 8;

  PES_packet_header_len = data[pos++];

  if (pts_field_present) {
    /* '001x', PTS[32..30], marker, PTS[29..15], marker, PTS[14..0], marker */
    pts = ((guint64) (data[pos] & 0x0E)) << 29; /* PTS[32..30], ignore marker at rightmost bit */
    pts |= ((guint64) (data[pos + 1])) << 22;   /* PTS[29..22], full byte */
    pts |= ((guint64) (data[pos + 2] & 0xFE)) << 14;    /* PTS[21..15], ignore marker at rightmost bit */
    pts |= ((guint64) (data[pos + 3])) << 7;    /* PTS[14.. 7], full byte */
    pts |= ((guint64) (data[pos + 4] & 0xFE)) >> 1;     /* PTS[ 6.. 0], ignore marker at rightmost bit */
  }

  pos += PES_packet_header_len; /* FIXME: Currently including all header values with all but PTS ignored */

  dvb_sub_feed_with_pts (dvb_sub, pts, data + pos, PES_packet_len - PES_packet_header_len - 3); /* 2 bytes between PES_packet_len and PES_packet_header_len fields, minus header_len itself */
  pos += PES_packet_len - PES_packet_header_len - 3;
  dvb_log (DVB_LOG_PACKET, G_LOG_LEVEL_DEBUG,
      "Finished PES packet - consumed %u bytes of %d", pos, len);

  return pos;
}

#define DVB_SUB_SEGMENT_PAGE_COMPOSITION 0x10
#define DVB_SUB_SEGMENT_REGION_COMPOSITION 0x11
#define DVB_SUB_SEGMENT_CLUT_DEFINITION 0x12
#define DVB_SUB_SEGMENT_OBJECT_DATA 0x13
#define DVB_SUB_SEGMENT_DISPLAY_DEFINITION 0x14
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
 * Return value: -1 if data was unhandled (e.g, not a subtitle packet),
 *				 -2 if data parsing was unsuccesful (e.g, length was invalid),
 *				  0 or positive if data was handled. If positive, then amount of data consumed on success. FIXME: List the positive return values.
 */
gint
dvb_sub_feed_with_pts (DvbSub * dvb_sub, guint64 pts, guint8 * data, gint len)
{
  unsigned int pos = 0;
  guint8 segment_type;
  guint16 segment_len;
  guint16 page_id;

  dvb_log (DVB_LOG_PACKET, G_LOG_LEVEL_DEBUG,
      "Inside dvb_sub_feed_with_pts with pts=%" G_GUINT64_FORMAT
      " and length %d", pts, len);

  g_return_val_if_fail (data != NULL, -1);

  if (len <= 3) {               /* len(0x20 0x00 end_of_PES_data_field_marker) */
    g_warning ("Data length too short");
    return -1;
  }

  if (data[pos++] != 0x20) {
    g_warning
        ("Tried to handle a PES packet private data that isn't a subtitle packet (does not start with 0x20)");
    return -1;
  }

  if (data[pos++] != 0x00) {
    g_warning
        ("'Subtitle stream in this PES packet' was not 0x00, so this is in theory not a DVB subtitle stream (but some other subtitle standard?); bailing out");
    return -1;
  }

  while (data[pos++] == DVB_SUB_SYNC_BYTE) {
    if ((len - pos) < (2 * 2 + 1)) {
      g_warning
          ("Data after SYNC BYTE too short, less than needed to even get to segment_length");
      return -2;
    }
    segment_type = data[pos++];
    dvb_log (DVB_LOG_PACKET, G_LOG_LEVEL_DEBUG,
        "=== Segment type is 0x%x", segment_type);
    page_id = (data[pos] << 8) | data[pos + 1];
    dvb_log (DVB_LOG_PACKET, G_LOG_LEVEL_DEBUG, "page_id is 0x%x", page_id);
    pos += 2;
    segment_len = (data[pos] << 8) | data[pos + 1];
    dvb_log (DVB_LOG_PACKET, G_LOG_LEVEL_DEBUG,
        "segment_length is %d (0x%x 0x%x)", segment_len, data[pos],
        data[pos + 1]);
    pos += 2;
    if ((len - pos) < segment_len) {
      g_warning
          ("segment_length was told to be %u, but we only have %d bytes left",
          segment_len, len - pos);
      return -2;
    }
    // TODO: Parse the segment per type
    switch (segment_type) {
      case DVB_SUB_SEGMENT_PAGE_COMPOSITION:
        dvb_log (DVB_LOG_PACKET, G_LOG_LEVEL_DEBUG,
            "Page composition segment at buffer pos %u\n", pos);
        _dvb_sub_parse_page_segment (dvb_sub, page_id, data + pos, segment_len);        /* FIXME: Not sure about args */
        break;
      case DVB_SUB_SEGMENT_REGION_COMPOSITION:
        dvb_log (DVB_LOG_PACKET, G_LOG_LEVEL_DEBUG,
            "Region composition segment at buffer pos %u\n", pos);
        _dvb_sub_parse_region_segment (dvb_sub, page_id, data + pos, segment_len);      /* FIXME: Not sure about args */
        break;
      case DVB_SUB_SEGMENT_CLUT_DEFINITION:
        dvb_log (DVB_LOG_PACKET, G_LOG_LEVEL_DEBUG,
            "CLUT definition segment at buffer pos %u\n", pos);
        _dvb_sub_parse_clut_segment (dvb_sub, page_id, data + pos, segment_len);        /* FIXME: Not sure about args */
        break;
      case DVB_SUB_SEGMENT_OBJECT_DATA:
        dvb_log (DVB_LOG_PACKET, G_LOG_LEVEL_DEBUG,
            "Object data segment at buffer pos %u\n", pos);
        _dvb_sub_parse_object_segment (dvb_sub, page_id, data + pos, segment_len);      /* FIXME: Not sure about args */
        break;
      case DVB_SUB_SEGMENT_DISPLAY_DEFINITION:
        dvb_log (DVB_LOG_PACKET, G_LOG_LEVEL_DEBUG,
            "display definition segment at buffer pos %u\n", pos);
        _dvb_sub_parse_display_definition_segment (dvb_sub, data + pos,
            segment_len);
        break;
      case DVB_SUB_SEGMENT_END_OF_DISPLAY_SET:
        dvb_log (DVB_LOG_PACKET, G_LOG_LEVEL_DEBUG,
            "End of display set at buffer pos %u\n", pos);
        _dvb_sub_parse_end_of_display_set (dvb_sub, page_id, data + pos, segment_len, pts);     /* FIXME: Not sure about args */
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

  return pos;
}


/* FIXME: Move these functions and includes elsewhere later for easier integration
 * FIXME: for gstreamer needs */
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>              /* perror, printf */
#include <sys/ioctl.h>
#include <linux/dvb/dmx.h>

/**
 * dvb_sub_open_pid:
 * @dvb_sub: a #DvbSub
 * @pid: the MPEG-TS PID to open
 * @adapter: the device onto which to install the demux filter
 *
 * Opens a PID directly at the LinuxDVB layer. This is an alternative to feeding
 * data to the parser manually through dvb_sub_feed() or dvb_sub_feed_with_pts().
 * The caller is expected to call dvb_sub_read_data when the returned file
 * descriptor has data available for reading. This is typically achieved by
 * installing a file watch for reading, with e.g g_io_add_watch(), and calling
 * dvb_sub_read_data() in its callback.
 * The file descriptor returned must be closed with dvb_sub_close_pid(),
 * not through standard C file close functions.
 *
 * Return value: the file descriptor opened, -1 on error.
 */
int
dvb_sub_open_pid (DvbSub * dvb_sub, guint16 pid, const gchar * adapter)
{
  DvbSubPrivate *priv;
  int ret;
  struct dmx_pes_filter_params pesfilter = {
    .pid = pid,
    .input = DMX_IN_DVR,
    .output = DMX_OUT_TAP,
    .pes_type = DMX_PES_SUBTITLE,
    .flags = 0
  };

  g_return_val_if_fail (dvb_sub != NULL, -1);
  g_return_val_if_fail (DVB_IS_SUB (dvb_sub), -1);
  g_return_val_if_fail (pid > 0, -1);

  priv = (DvbSubPrivate *) dvb_sub->private_data;

  if (priv->fd >= 0)            /* a file is already open, close it first */
    dvb_sub_close_pid (dvb_sub);

  priv->fd = TEMP_FAILURE_RETRY (open (adapter, O_RDONLY | O_NONBLOCK));        /* FIXME: allow other hardware demuxers and adapters */
  if (priv->fd < 0) {
    perror (adapter);
    return -1;
  }

  ret = TEMP_FAILURE_RETRY (ioctl (priv->fd, DMX_SET_PES_FILTER, &pesfilter));
  if (ret != 0) {
    perror ("ioctl DMX_SET_PES_FILTER");
    goto error;
  }

  ret = TEMP_FAILURE_RETRY (ioctl (priv->fd, DMX_START));
  if (ret != 0) {
    perror ("ioctl DMX_START");
    goto error;
  } else {
    printf ("DVB-SUB demuxing started on pid %u\n", pid);
  }

  return priv->fd;
  /* FIXME: Compare with zvbi dvb integration code perhaps */

error:
  close (priv->fd);
  priv->fd = -1;
  return -1;
}

/**
 * dvb_sub_close_pid:
 * @dvb_sub: a #DvbSub
 *
 * Closes the PID related file descriptor previously opened by dvb_sub_open_pid().
 */
void
dvb_sub_close_pid (DvbSub * dvb_sub)
{
  DvbSubPrivate *priv;

  g_return_if_fail (dvb_sub != NULL);
  g_return_if_fail (DVB_IS_SUB (dvb_sub));

  priv = (DvbSubPrivate *) dvb_sub->private_data;

  g_return_if_fail (priv->fd >= 0);

  close (priv->fd);
  priv->fd = -1;
  delete_state (dvb_sub);
}

/**
 * dvb_sub_read_data:
 * @dvb_sub: a #DvbSub
 *
 * Reads any available data from the objects open file descriptor to the MPEG-TS
 * elementary stream with PID as passed to dvb_sub_open_pid().
 */
void
dvb_sub_read_data (DvbSub * dvb_sub)
{
  DvbSubPrivate *priv;
  GString *data;                /* FIXME: Probably don't use GString in the long run? */
  gchar buf[4096];
  ssize_t len_read;
  int ret;

  g_return_if_fail (dvb_sub != NULL);
  g_return_if_fail (DVB_IS_SUB (dvb_sub));

  priv = (DvbSubPrivate *) dvb_sub->private_data;

  g_return_if_fail (priv->fd >= 0);

  while ((len_read = read (priv->fd, buf, 4096))) {
    if (len_read < 0) {
      if (errno != EAGAIN)
        g_warning
            ("Error during demux file descriptor read. Code: %d, message: %s",
            errno, strerror (errno));
      /* FIXME: What should we actually do here? */
      break;
    }

    g_string_append_len (priv->pes_buffer, buf, len_read);
  }

  dvb_log (DVB_LOG_PACKET, G_LOG_LEVEL_DEBUG,
      "read_data called by API user, feeding %" G_GSIZE_FORMAT
      " bytes into DVB subtitle parser", priv->pes_buffer->len);

  do {
    ret =
        dvb_sub_feed (dvb_sub, (guint8 *) (priv->pes_buffer->str),
        priv->pes_buffer->len);
    if (ret > 0)
      /* FIXME: MIN() is a temporary workaround because dvb_sub_feed(_with_pts) returned in a rare circumstance
       * a number bigger than the length passed to it. That needs to be fixed instead, but it was a rare
       * occurrence with not enough debug enabled at the time to find the problem quickly enough for next release,
       * so just at least don't end up in an infinite loop meanwhile. */
      g_string_erase (priv->pes_buffer, 0, MIN (ret, priv->pes_buffer->len));
  } while (ret > 0);
}

/**
 * dvb_sub_set_callbacks:
 * @dvb_sub: a #DvbSub
 * @callbacks: the callbacks to install
 * @user_data: a user_data argument for the callback
 *
 * Set callback which will be executed when new subpictures are available.
 */
void
dvb_sub_set_callbacks (DvbSub * dvb_sub, DvbSubCallbacks * callbacks,
    gpointer user_data)
{
  DvbSubPrivate *priv;

  g_return_if_fail (dvb_sub != NULL);
  g_return_if_fail (DVB_IS_SUB (dvb_sub));
  g_return_if_fail (callbacks != NULL);

  priv = (DvbSubPrivate *) dvb_sub->private_data;

  priv->callbacks = *callbacks;
  priv->user_data = user_data;
}
