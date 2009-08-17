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


#ifndef _DVB_SUB_LOG_H_
#define _DVB_SUB_LOG_H_

#include <glib.h>

G_BEGIN_DECLS

/**
 * DvbLogTypes:
 *
 * Flags specifying the type of debug logging messages.
 */
typedef enum
{
	/* dvb_log types // DVB_LOG environment variable string */
	DVB_LOG_GENERAL, /* GENERAL */
	DVB_LOG_PAGE,    /* PAGE */
	DVB_LOG_REGION,  /* REGION */
	DVB_LOG_CLUT,    /* CLUT */
	DVB_LOG_OBJECT,  /* OBJECT */
	DVB_LOG_PIXEL,   /* PIXEL */
	DVB_LOG_RUNLEN,  /* RUNLEN */
	DVB_LOG_DISPLAY, /* DISPLAY */
	DVB_LOG_STREAM,  /* STREAM - issues in the encoded stream (TV service provider encoder problem) */
	DVB_LOG_LAST  /* sentinel use only */
} DvbLogTypes;

#ifndef DEBUG
#define dvb_log(log_type, log_level, format...)
#else
void dvb_log (const gint      log_type,
              GLogLevelFlags  log_level,
              const gchar    *format,
              ...);
#endif

G_END_DECLS

#endif /* _DVB_SUB_LOG_H_ */
