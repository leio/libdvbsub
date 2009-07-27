/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 4 -*- */
/*
 * libdvbsub
 * Copyright (C) Mart Raudsepp 2009 <mart.raudsepp@artecdesign.ee>
 *
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

struct _DvbSub
{
	GObject parent_instance;

	/*< private >*/
	gpointer private_data;
};

GType    dvb_sub_get_type      (void) G_GNUC_CONST;
GObject *dvb_sub_new           (void);
gint     dvb_sub_feed          (DvbSub *dvb_sub, guchar *data, gint len);
gint     dvb_sub_feed_with_pts (DvbSub *dvb_sub, guint64 pts, guchar* data, gint len);

G_END_DECLS

#endif /* _DVB_SUB_H_ */
