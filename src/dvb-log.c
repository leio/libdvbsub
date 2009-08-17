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

#include "dvb-log.h"

/**
 * SECTION:dvb-log
 * @short_description: a simple environment variable controlled logging facility
 * @stability: Private
 *
 * The dvb_log() function here provides an internal debugging logging facility
 * for libdvbsub. It is enabled only when the library is compiled with debugging
 * support enabled, otherwise dvb_log() is a no-op. Documentation exists for
 * this private facility for the purpose of easier debugging of the library and
 * tracking its data and parsing flow.
 */

#ifdef DEBUG
const char *dvb_log_type_list[] = {
	"GENERAL", /* DVB_LOG_GENERAL */
	"PAGE",    /* DVB_LOG_PAGE    */
	"REGION",  /* DVB_LOG_REGION  */
	"CLUT",    /* DVB_LOG_CLUT    */
	"OBJECT",  /* DVB_LOG_OBJECT  */
	"PIXEL",   /* DVB_LOG_PIXEL   */
	"RUNLEN",  /* DVB_LOG_RUNLEN  */
	"DISPLAY", /* DVB_LOG_DISPLAY */
	"STREAM"   /* DVB_LOG_STREAM  */
};

/**
 * dvb_log:
 * @log_type: the log type, one of DVB_LOG_TYPE
 * @log_level: the log level, either from #GLogLevelFlags or a user-defined level.
 * @format: the message format. See the printf() documentation.
 * @args: the parameters to insert into the format string.
 *
 * Logs a debugging message if the library is debugging enabled.
 */
void dvb_log (const gint      log_type,
              GLogLevelFlags  log_level,
              const gchar    *format,
              ...)
{
	static gboolean enabled_log_types[DVB_LOG_LAST] = {0, };
	static gboolean enabled_log_types_initialized = FALSE;

	if (!enabled_log_types_initialized) {
		const gchar *env_log_types = g_getenv ("DVB_LOG");
		int i;

		enabled_log_types_initialized = TRUE;

		/* Figure out what log types are enabled */
		if (!env_log_types) {
			/* Enable all log types if none given */
			for (i = 0; i < DVB_LOG_LAST; ++i)
				enabled_log_types[i] = TRUE;
		} else {
			int j;
			gchar **split = g_strsplit (env_log_types, ";", 0);
			i = 0;
			while (split[i] != NULL) {
				for (j = 0; j < DVB_LOG_LAST; ++j) {
					if (g_str_equal (dvb_log_type_list[j], split[i])) {
						enabled_log_types[j] = TRUE;
						break; /* Match found */
					}
				}
				++i;
			}
		}
	}

	if (enabled_log_types[log_type]) {
		gchar *format2 = g_strdup_printf ("%s: %s", dvb_log_type_list[log_type], format);
		va_list args;
		va_start (args, format);
		g_logv ("libdvbsub", log_level, format2, args);
		va_end (args);
	}
}
#endif /* DEBUG */
