/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 4 -*- */
/*
 * main.c
 * Copyright (C) Mart Raudsepp 2009 <mart.raudsepp@artecdesign.ee>
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include <config.h>

#include <glib.h>

#include <dvb-sub.h>

static gchar **parse_filenames = NULL;

static GOptionEntry entries[] = {
	{ G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &parse_filenames, "File to source for parsing test", NULL },
	{ NULL, }
};

int
main (int argc, char *argv[])
{
	DvbSub *sub_parser = NULL;

	GError *error = NULL;
	GOptionContext *context;
	gchar *file_buf;
	gsize file_len;

	g_type_init ();

	context = g_option_context_new ("- Test program for libdvbsub parsing");
	g_option_context_add_main_entries (context, entries, NULL);

	if (!g_option_context_parse (context, &argc, &argv, &error))
	{
		g_print ("option parsing failed: %s\n", error->message);
		g_error_free (error);
	}

	if (!parse_filenames) {
		g_warning ("Filename required as program argument!");
		return -1;
	}

	sub_parser = DVB_SUB (dvb_sub_new ());

	if (!g_file_get_contents (parse_filenames[0], &file_buf, &file_len, NULL)) {
		g_error ("Read of file '%s' contents failed!", parse_filenames[0]);
		return -1;
	}

#ifdef DVBSUB_TEST_FROM_GST_DUMP
	dvb_sub_feed_with_pts (sub_parser, 0, (guchar*)file_buf, file_len);
#else
	dvb_sub_feed (sub_parser, (guchar*)file_buf, file_len);
#endif

	return 0;
}
