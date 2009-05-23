/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * Libbrasero-burn
 * Copyright (C) Philippe Rouquier 2005-2009 <bonfire-app@wanadoo.fr>
 *
 * Libbrasero-burn is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The Libbrasero-burn authors hereby grant permission for non-GPL compatible
 * GStreamer plugins to be used and distributed together with GStreamer
 * and Libbrasero-burn. This permission is above and beyond the permissions granted
 * by the GPL license by which Libbrasero-burn is covered. If you modify this code
 * you may extend this exception to your version of the code, but you are not
 * obligated to do so. If you do not wish to do so, delete this exception
 * statement from your version.
 * 
 * Libbrasero-burn is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to:
 * 	The Free Software Foundation, Inc.,
 * 	51 Franklin Street, Fifth Floor
 * 	Boston, MA  02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <glib-object.h>

#include "brasero-misc.h"

#include "brasero-track-stream-cfg.h"
#include "brasero-io.h"
#include "brasero-tags.h"

typedef struct _BraseroTrackStreamCfgPrivate BraseroTrackStreamCfgPrivate;
struct _BraseroTrackStreamCfgPrivate
{
	BraseroIOJobBase *load_uri;

	GError *error;

	guint loading:1;
};

#define BRASERO_TRACK_STREAM_CFG_PRIVATE(o)  (G_TYPE_INSTANCE_GET_PRIVATE ((o), BRASERO_TYPE_TRACK_STREAM_CFG, BraseroTrackStreamCfgPrivate))

G_DEFINE_TYPE (BraseroTrackStreamCfg, brasero_track_stream_cfg, BRASERO_TYPE_TRACK_STREAM);


static void
brasero_video_project_result_cb (GObject *obj,
				 GError *error,
				 const gchar *uri,
				 GFileInfo *info,
				 gpointer user_data)
{
	guint64 len;
	BraseroTrackStreamCfgPrivate *priv;

	priv = BRASERO_TRACK_STREAM_CFG_PRIVATE (obj);
	priv->loading = FALSE;

	/* Check the return status for this file */
	if (error) {
		priv->error = g_error_copy (error);
		brasero_track_changed (BRASERO_TRACK (obj));
		return;
	}

	/* FIXME: we don't know whether it's audio or video that is required */
	if (g_file_info_get_file_type (info) != G_FILE_TYPE_REGULAR
	|| (!g_file_info_get_attribute_boolean (info, BRASERO_IO_HAS_VIDEO)
	&&  !g_file_info_get_attribute_boolean (info, BRASERO_IO_HAS_AUDIO))) {
		gchar *name;

		BRASERO_GET_BASENAME_FOR_DISPLAY (uri, name);
		priv->error = g_error_new (BRASERO_BURN_ERROR,
					   BRASERO_BURN_ERR,
					   /* Translators: %s is the name of the file */
					   _("\"%s\" is not suitable for audio or video media"),
					   name);
		g_free (name);

		brasero_track_changed (BRASERO_TRACK (obj));
		return;
	}

	if (g_file_info_get_is_symlink (info)) {
		gchar *sym_uri;

		sym_uri = g_strconcat ("file://", g_file_info_get_symlink_target (info), NULL);
		if (BRASERO_TRACK_STREAM_CLASS (brasero_track_stream_cfg_parent_class)->set_source)
			BRASERO_TRACK_STREAM_CLASS (brasero_track_stream_cfg_parent_class)->set_source (BRASERO_TRACK_STREAM (obj), sym_uri);

		g_free (sym_uri);
	}

	if (BRASERO_TRACK_STREAM_CLASS (brasero_track_stream_cfg_parent_class)->set_format)
		BRASERO_TRACK_STREAM_CLASS (brasero_track_stream_cfg_parent_class)->set_format (BRASERO_TRACK_STREAM (obj),
												(g_file_info_get_attribute_boolean (info, BRASERO_IO_HAS_VIDEO)?
												 BRASERO_VIDEO_FORMAT_UNDEFINED:BRASERO_AUDIO_FORMAT_NONE)|
												(g_file_info_get_attribute_boolean (info, BRASERO_IO_HAS_AUDIO)?
												 BRASERO_AUDIO_FORMAT_UNDEFINED:BRASERO_AUDIO_FORMAT_NONE)|
												BRASERO_METADATA_INFO);

	/* size */
	len = g_file_info_get_attribute_uint64 (info, BRASERO_IO_LEN);
	if (BRASERO_TRACK_STREAM_CLASS (brasero_track_stream_cfg_parent_class)->set_boundaries)
		BRASERO_TRACK_STREAM_CLASS (brasero_track_stream_cfg_parent_class)->set_boundaries (BRASERO_TRACK_STREAM (obj),
												    0,
												    len,
												    0);
					     
	/* Get the song info */
	if (g_file_info_get_attribute_string (info, BRASERO_IO_TITLE))
		brasero_track_tag_add_string (BRASERO_TRACK (obj),
					      BRASERO_TRACK_STREAM_TITLE_TAG,
					      g_file_info_get_attribute_string (info, BRASERO_IO_TITLE));
	if (g_file_info_get_attribute_string (info, BRASERO_IO_ARTIST))
		brasero_track_tag_add_string (BRASERO_TRACK (obj),
					      BRASERO_TRACK_STREAM_ARTIST_TAG,
					      g_file_info_get_attribute_string (info, BRASERO_IO_ARTIST));
	if (g_file_info_get_attribute_string (info, BRASERO_IO_COMPOSER))
		brasero_track_tag_add_string (BRASERO_TRACK (obj),
					      BRASERO_TRACK_STREAM_COMPOSER_TAG,
					      g_file_info_get_attribute_string (info, BRASERO_IO_COMPOSER));
	if (g_file_info_get_attribute_int32 (info, BRASERO_IO_ISRC))
		brasero_track_tag_add_int (BRASERO_TRACK (obj),
					   BRASERO_TRACK_STREAM_ISRC_TAG,
					   g_file_info_get_attribute_int32 (info, BRASERO_IO_ISRC));

	brasero_track_changed (BRASERO_TRACK (obj));
}

static void
brasero_track_stream_cfg_get_info (BraseroTrackStreamCfg *track)
{
	BraseroTrackStreamCfgPrivate *priv;
	gchar *uri;

	priv = BRASERO_TRACK_STREAM_CFG_PRIVATE (track);

	if (priv->error) {
		g_error_free (priv->error);
		priv->error = NULL;
	}

	/* get info async for the file */
	if (!priv->load_uri)
		priv->load_uri = brasero_io_register (G_OBJECT (track),
						      brasero_video_project_result_cb,
						      NULL,
						      NULL);

	priv->loading = TRUE;
	uri = brasero_track_stream_get_source (BRASERO_TRACK_STREAM (track), TRUE);
	brasero_io_get_file_info (uri,
				  priv->load_uri,
				  BRASERO_IO_INFO_PERM|
				  BRASERO_IO_INFO_MIME|
				  BRASERO_IO_INFO_URGENT|
				  BRASERO_IO_INFO_METADATA|
				  BRASERO_IO_INFO_METADATA_MISSING_CODEC|
				  BRASERO_IO_INFO_METADATA_THUMBNAIL,
				  track);
}

static BraseroBurnResult
brasero_track_stream_cfg_set_source (BraseroTrackStream *track,
				     const gchar *uri)
{
	if (BRASERO_TRACK_STREAM_CLASS (brasero_track_stream_cfg_parent_class)->set_source)
		BRASERO_TRACK_STREAM_CLASS (brasero_track_stream_cfg_parent_class)->set_source (track, uri);

	brasero_track_stream_cfg_get_info (BRASERO_TRACK_STREAM_CFG (track));
	brasero_track_changed (BRASERO_TRACK (track));
	return BRASERO_BURN_OK;
}

static BraseroBurnResult
brasero_track_stream_cfg_get_status (BraseroTrack *track,
				     BraseroStatus *status)
{
	BraseroTrackStreamCfgPrivate *priv;

	priv = BRASERO_TRACK_STREAM_CFG_PRIVATE (track);

	if (priv->error) {
		brasero_status_set_error (status, g_error_copy (priv->error));
		return BRASERO_BURN_ERR;
	}

	if (priv->loading) {
		if (status)
			brasero_status_set_not_ready (status,
						      -1.0,
						      _("Analysing video files"));

		return BRASERO_BURN_NOT_READY;
	}

	if (status)
		brasero_status_set_completed (status);

	return BRASERO_BURN_OK;
}

static void
brasero_track_stream_cfg_init (BraseroTrackStreamCfg *object)
{ }

static void
brasero_track_stream_cfg_finalize (GObject *object)
{
	BraseroTrackStreamCfgPrivate *priv;

	priv = BRASERO_TRACK_STREAM_CFG_PRIVATE (object);
	if (priv->error) {
		g_error_free (priv->error);
		priv->error = NULL;
	}

	G_OBJECT_CLASS (brasero_track_stream_cfg_parent_class)->finalize (object);
}

static void
brasero_track_stream_cfg_class_init (BraseroTrackStreamCfgClass *klass)
{
	GObjectClass* object_class = G_OBJECT_CLASS (klass);
	BraseroTrackClass* track_class = BRASERO_TRACK_CLASS (klass);
	BraseroTrackStreamClass *parent_class = BRASERO_TRACK_STREAM_CLASS (klass);

	g_type_class_add_private (klass, sizeof (BraseroTrackStreamCfgPrivate));

	object_class->finalize = brasero_track_stream_cfg_finalize;

	track_class->get_status = brasero_track_stream_cfg_get_status;

	parent_class->set_source = brasero_track_stream_cfg_set_source;
}

