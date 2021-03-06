/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 4 -*- */
/*
 * libdvbsub - DVB subtitle decoding
 * Copyright (C) Mart Raudsepp 2009 <mart.raudsepp@artecdesign.ee>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef _DVB_SUB_H_
#define _DVB_SUB_H_

#include <glib-object.h>

G_BEGIN_DECLS

#define DVB_TYPE_SUB             (dvb_sub_get_type ())
#define DVB_SUB(obj)             (G_TYPE_CHECK_INSTANCE_CAST ((obj), DVB_TYPE_SUB, DvbSub))
#define DVB_SUB_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST ((klass), DVB_TYPE_SUB, DvbSubClass))
#define DVB_IS_SUB(obj)          (G_TYPE_CHECK_INSTANCE_TYPE ((obj), DVB_TYPE_SUB))
#define DVB_IS_SUB_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE ((klass), DVB_TYPE_SUB))
#define DVB_SUB_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS ((obj), DVB_TYPE_SUB, DvbSubClass))

typedef struct _DvbSubClass DvbSubClass;
typedef struct _DvbSub DvbSub;

struct _DvbSubClass
{
	GObjectClass parent_class;
};

/**
 * DvbSub:
 *
 * The #DvbSub struct contains only private fields and should not be
 * directly accessed.
 */
struct _DvbSub
{
	GObject parent_instance;

	/*< private >*/
	gpointer private_data;
};

/**
 * DVBSubtitlePicture:
 * @data: the data in the form of palette indices, each byte represents one pixel
 *   as an index into the @palette.
 * @palette: the palette used for this subtitle rectangle, up to 256 items depending
 *   on the depth of the subpicture; each palette item is in ARGB form, 8-bits per channel.
 * @palette_bits_count: the amount of bits used in indeces into @palette in @data.
 * @rowstride: the number of bytes between the start of a row and the start of the next row.
 *
 * A structure representing the contents of a subtitle rectangle.
 *
 * FIXME: Expose the depth of the palette, and perhaps also the height in this struct.
 */
typedef struct DVBSubtitlePicture {
	guint8 *data;
	guint32 *palette;
	guint8 palette_bits_count;
	int rowstride;
} DVBSubtitlePicture;

/**
 * DVBSubtitleRect:
 * @x: x coordinate of top left corner
 * @y: y coordinate of top left corner
 * @w: the width of this subpicture rectangle
 * @h: the height of this subpicture rectangle
 * @pict: the content of this subpicture rectangle
 *
 * A structure representing one subtitle objects position, dimension and content.
 */
typedef struct DVBSubtitleRect {
	int x;
	int y;
	int w;
	int h;

	DVBSubtitlePicture pict;
} DVBSubtitleRect;

/**
 * DVBSubtitleWindow
 * @version: version 
 * @display_window_flag: window_* are valid
 * @display_width: assumed width of display
 * @display_height: assumed height of display
 * @window_x: x coordinate of top left corner of the subtitle window
 * @window_y: y coordinate of top left corner of the subtitle window
 * @window_width: width of the subtitle window
 * @window_height: height of the subtitle window
 *
 * A structure presenting display and window information
 * display definition segment from ETSI EN 300 743 V1.3.1
 */
typedef struct DVBSubtitleWindow {
    gint version;
    gint window_flag;

    gint display_width;
    gint display_height;

    gint window_x;
    gint window_y;
    gint window_width;
    gint window_height;
} DVBSubtitleWindow;

/**
 * DVBSubtitles:
 * @num_rects: the number of #DVBSubtitleRect in @rects
 * @rects: dynamic array of #DVBSubtitleRect
 *
 * A structure representing a set of subtitle objects.
 */
typedef struct DVBSubtitles {
	unsigned int num_rects;
	DVBSubtitleRect **rects;
	DVBSubtitleWindow display_def;
} DVBSubtitles;

/**
 * DvbSubCallbacks:
 * @new_data: called when new subpicture data is available for display. @dvb_sub
 *    is the #DvbSub instance this callback originates from; @subs is the set of
 *    subtitle objects that should be display for no more than @page_time_out
 *    seconds at @pts; @user_data is the same user_data as was passed through
 *    dvb_sub_set_callbacks();
 *
 * A set of callbacks that can be installed on the #DvbSub with
 * dvb_sub_set_callbacks().
 */
typedef struct {
	void     (*new_data) (DvbSub *dvb_sub, guint64 pts, DVBSubtitles * subs, guint8 page_time_out, gpointer user_data);
	/*< private >*/
	gpointer _dvb_sub_reserved[3];
} DvbSubCallbacks;

GType    dvb_sub_get_type      (void) G_GNUC_CONST;
DvbSub  *dvb_sub_new           (void);
gint     dvb_sub_feed          (DvbSub *dvb_sub, guint8 *data, gint len);
gint     dvb_sub_feed_with_pts (DvbSub *dvb_sub, guint64 pts, guint8 *data, gint len);
int      dvb_sub_open_pid      (DvbSub *dvb_sub, guint16 pid, const gchar *adapter);
void     dvb_sub_close_pid     (DvbSub *dvb_sub);
void     dvb_sub_read_data     (DvbSub *dvb_sub);
void     dvb_sub_set_callbacks (DvbSub *dvb_sub, DvbSubCallbacks *callbacks, gpointer user_data);

G_END_DECLS

#endif /* _DVB_SUB_H_ */
