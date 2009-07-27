/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 4 -*- */
/*
 * libdvbsub
 * Copyright (C) Mart Raudsepp 2009 <mart.raudsepp@artecdesign.ee>
 * 
 */

#include "dvb-sub.h"

typedef struct _DvbSubPrivate DvbSubPrivate;
struct _DvbSubPrivate
{
	int pid;
	/* FIXME... */
};

#define DVB_SUB_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE ((obj), DVB_TYPE_SUB, DvbSubPrivate))

G_DEFINE_TYPE (DvbSub, dvb_sub, G_TYPE_OBJECT);

static void
dvb_sub_init (DvbSub *self)
{
	DvbSubPrivate *priv;

	self->private_data = priv = DVB_SUB_GET_PRIVATE (self);

	/* TODO: Add initialization code here */
}

static void
dvb_sub_finalize (GObject *object)
{
	DvbSub *self = DVB_SUB (object);
	DvbSubPrivate *priv = (DvbSubPrivate *)self->private_data;
	/* TODO: Add deinitalization code here */

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
_dvb_sub_handle_page_composition (DvbSub *dvb_sub, guint16 page_id, guint8 *data, gint len) /* FIXME: Use guint for len here and in many other places? */
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
	g_print ("PAGE COMPOSITION %d: page_id = %u, length = %d; content is:\nPAGE COMPOSITION %d: ", counter, page_id, len, counter);
	for (i = 0; i < len; ++i)
		g_print ("0x%x ", data[i]);
	g_print("\n");

	g_print ("PAGE COMPOSITION %d: Page timeout = %u seconds\n", counter, data[0]);
	g_print ("PAGE COMPOSITION %d: page version number = 0x%x (rolling counter from 0x0 to 0xf and then wraparound)\n", counter, (data[1] >> 4) & 0xf);
	g_print ("PAGE COMPOSITION %d: page_state = %s\n", counter, page_state[(data[1] >> 2) & 0x3]);
	g_print ("PAGE COMPOSITION %d: Reserved = 0x%x\n", counter, data[1] & 0x3);

	processed_len = 2;
	while (processed_len < len) {
		if (len - processed_len < 6) {
			g_warning ("PAGE COMPOSITION %d: Not enough bytes for a region block! 6 needed, but only have %d left in this page composition segment", counter, len - processed_len);
			return;
		}

		g_print ("PAGE COMPOSITION %d: REGION information: ID = %u, address = %ux%u  (reserved value that we don't care was 0x%x)\n", counter,
		          data[processed_len],
		         (data[processed_len + 2] << 8) | data[processed_len + 3],
		         (data[processed_len + 4] << 8) | data[processed_len + 5],
		          data[processed_len + 1]
		        );

		processed_len += 6;
	}
	/* TODO */
}

static void
_dvb_sub_handle_region_composition (DvbSub *dvb_sub, guint16 page_id, guint8 *data, gint len)
{
	/* TODO */
}

static void
_dvb_sub_handle_clut_definition (DvbSub *dvb_sub, guint16 page_id, guint8 *data, gint len)
{
	/* TODO */
}

static void
_dvb_sub_handle_object_data (DvbSub *dvb_sub, guint16 page_id, guint8 *data, gint len)
{
	static int counter = 0;
	int i;
	static gchar *coding_method[] = {
		"PIXELS",
		"STRING",
		"Reserved (0x02)",
		"Reserved (0x03)"
	};

	++counter;
	g_print ("OBJECT DATA %d: page_id = %u, length = %d; content is:\nOBJECT DATA %d (content): ", counter, page_id, len, counter);
	for (i = 0; i < len; ++i)
		g_print ("0x%x ", data[i]);
	g_print("\n");

	g_print ("OBJECT DATA %d: object_id = %u (0x%x 0x%x)\n", counter, (data[0] << 8) | data[1], data[0], data[1]);
	g_print ("OBJECT DATA %d: object version number = 0x%x (rolling counter from 0x0 to 0xf and then wraparound)\n", counter, (data[2] >> 4) & 0xf);
	g_print ("OBJECT DATA %d: coding_method = %s\n", counter, coding_method[(data[1] >> 2) & 0x3]);
	g_print ("OBJECT DATA %d: Reserved = 0x%x\n", counter, data[1] & 0x3);

	/* TODO */
}

static void
_dvb_sub_handle_end_of_display_set (DvbSub *dvb_sub, guint16 page_id, guint8 *data, gint len)
{
	static int counter = 0;
	++counter;
	g_print ("END OF DISPLAY SET %d: page_id = %u, length = %d\n", counter, page_id, len);
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
				_dvb_sub_handle_page_composition (dvb_sub, page_id, data + pos, segment_len); /* FIXME: Not sure about args */
				break;
			case DVB_SUB_SEGMENT_REGION_COMPOSITION:
				g_print ("Region composition segment at buffer pos %u\n", pos);
				_dvb_sub_handle_region_composition (dvb_sub, page_id, data + pos, segment_len); /* FIXME: Not sure about args */
				break;
			case DVB_SUB_SEGMENT_CLUT_DEFINITION:
				g_print ("CLUT definition segment at buffer pos %u\n", pos);
				_dvb_sub_handle_clut_definition (dvb_sub, page_id, data + pos, segment_len); /* FIXME: Not sure about args */
				break;
			case DVB_SUB_SEGMENT_OBJECT_DATA:
				g_print ("Object data segment at buffer pos %u\n", pos);
				_dvb_sub_handle_object_data (dvb_sub, page_id, data + pos, segment_len); /* FIXME: Not sure about args */
				break;
			case DVB_SUB_SEGMENT_END_OF_DISPLAY_SET:
				g_print ("End of display set at buffer pos %u\n", pos);
				_dvb_sub_handle_end_of_display_set (dvb_sub, page_id, data + pos, segment_len); /* FIXME: Not sure about args */
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
