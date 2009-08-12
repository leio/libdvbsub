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
 * DvbSubCallbacks:
 * @new_data: Called when new subpicture data is available for display. @dvb_sub
 *    is the #DvbSub instance this callback originates from; @user_data is the
 * same user_data as was passed through dvb_sub_set_callbacks().
 *
 * A set of callbacks that can be installed on the #DvbSub with
 * dvb_sub_set_callbacks().
 */
typedef struct {
	void     (*new_data) (DvbSub *dvb_sub, /* TODO ,*/ gpointer user_data);
	/*< private >*/
	gpointer _dvb_sub_reserved[3];
} DvbSubCallbacks;

GType    dvb_sub_get_type      (void) G_GNUC_CONST;
GObject *dvb_sub_new           (void);
gint     dvb_sub_feed          (DvbSub *dvb_sub, guint8 *data, gint len);
gint     dvb_sub_feed_with_pts (DvbSub *dvb_sub, guint64 pts, guint8 *data, gint len);
void     dvb_sub_set_callbacks (DvbSub *dvb_sub, DvbSubCallbacks *callbacks, gpointer user_data);

G_END_DECLS

#endif /* _DVB_SUB_H_ */
