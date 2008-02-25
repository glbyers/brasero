/***************************************************************************
 *            burn-local-image.c
 *
 *  dim jui  9 10:54:14 2006
 *  Copyright  2006  Rouquier Philippe
 *  brasero-app@wanadoo.fr
 ***************************************************************************/

/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <glib.h>
#include <glib-object.h>
#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>

#include <gio/gio.h>

#include <gmodule.h>

#include "burn-basics.h"
#include "burn-job.h"
#include "burn-plugin.h"
#include "burn-local-image.h"

BRASERO_PLUGIN_BOILERPLATE (BraseroLocalTrack, brasero_local_track, BRASERO_TYPE_JOB, BraseroJob);

struct _BraseroLocalTrackPrivate {
	GCancellable *cancel;
	guint64 data_size;
	guint64 read_bytes;

	gchar checksum [33];
	gchar *checksum_src;
	gchar *checksum_dest;

	GHashTable *nonlocals;

	guint thread_id;
	GThread *thread;

	GSList *src_list;
	GSList *dest_list;

	GError *error;
};
typedef struct _BraseroLocalTrackPrivate BraseroLocalTrackPrivate;

#define BRASERO_LOCAL_TRACK_PRIVATE(o)  (G_TYPE_INSTANCE_GET_PRIVATE ((o), BRASERO_TYPE_LOCAL_TRACK, BraseroLocalTrackPrivate))

static GObjectClass *parent_class = NULL;


/**
 * This part is for file transfer.
 * First we gather some information about the size of data to download then we
 * actually download it.
 */

static BraseroBurnResult
brasero_local_track_get_download_size (BraseroLocalTrack *self,
				       GFile *src,
				       GError **error)
{
	BraseroLocalTrackPrivate *priv;
	GFileEnumerator *enumerator;
	GFileInfo *info;

	priv = BRASERO_LOCAL_TRACK_PRIVATE (self);

	enumerator = g_file_enumerate_children (src,
						G_FILE_ATTRIBUTE_STANDARD_TYPE,
						G_FILE_QUERY_INFO_NONE,	/* follow symlinks */
						priv->cancel,
						error);
	if (!enumerator)
		return BRASERO_BURN_ERR;

	while ((info = g_file_enumerator_next_file (enumerator, priv->cancel, error))) {
		if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY) {
			GFile *child;
	
			child = g_file_get_child (src, g_file_info_get_name (info));
			brasero_local_track_get_download_size (self, child, error);
			g_object_unref (child);
		}
		else
			priv->data_size += g_file_info_get_size (info);

		g_object_unref (info);
	}

	g_file_enumerator_close (enumerator, priv->cancel, NULL);
	g_object_unref (enumerator);

	return BRASERO_BURN_OK;
}

static void
brasero_local_track_progress_cb (goffset current_num_bytes,
				 goffset total_num_bytes,
				 gpointer callback_data)
{
	BraseroLocalTrack *self = BRASERO_LOCAL_TRACK (callback_data);
	BraseroLocalTrackPrivate *priv;

	priv = BRASERO_LOCAL_TRACK_PRIVATE (self);

	brasero_job_start_progress (BRASERO_JOB (self), FALSE);
	brasero_job_set_progress (BRASERO_JOB (self),
				  (gdouble) (priv->read_bytes + current_num_bytes) /
				  (gdouble)  priv->data_size);
}

static BraseroBurnResult
brasero_local_track_file_transfer (BraseroLocalTrack *self,
				   GFile *src,
				   GFile *dest,
				   GError **error)
{
	gboolean result;
	gchar *name, *string;
	BraseroLocalTrackPrivate *priv;

	priv = BRASERO_LOCAL_TRACK_PRIVATE (self);

	name = g_file_get_basename (src);
	string = g_strdup_printf (_("Copying `%s` locally"), name);
	g_free (name);

	/* get the source name from the info */
	brasero_job_set_current_action (BRASERO_JOB (self),
					BRASERO_BURN_ACTION_FILE_COPY,
					string,
					TRUE);
	g_free (string);

	result = g_file_copy (src,
			      dest,
			      G_FILE_COPY_ALL_METADATA,
			      priv->cancel,
			      brasero_local_track_progress_cb,
			      self,
			      error);

	return result? BRASERO_BURN_OK:BRASERO_BURN_ERR;
}

static BraseroBurnResult
brasero_local_track_recursive_transfer (BraseroLocalTrack *self,
					GFile *src,
					GFile *dest,
					GError **error)
{
	GFileInfo *info;
	GFileEnumerator *enumerator;
	BraseroLocalTrackPrivate *priv;

	priv = BRASERO_LOCAL_TRACK_PRIVATE (self);

	enumerator = g_file_enumerate_children (src,
						G_FILE_ATTRIBUTE_STANDARD_TYPE,
						G_FILE_QUERY_INFO_NONE,	/* follow symlinks */
						priv->cancel,
						error);
	if (!enumerator)
		return BRASERO_BURN_ERR;

	while ((info = g_file_enumerator_next_file (enumerator, priv->cancel, error))) {
		BraseroBurnResult result;
		GFile *dest_child;
		GFile *src_child;

		if (error) {
			g_file_enumerator_close (enumerator, priv->cancel, NULL);
			g_object_unref (enumerator);
			g_object_unref (info);
			return BRASERO_BURN_ERR;
		}

		src_child = g_file_get_child (src, g_file_info_get_name (info));
		dest_child = g_file_get_child (dest, g_file_info_get_name (info));

		if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY) {
			gchar *path;

			path = g_file_get_path (dest_child);

			/* create a directory with the same name and explore it */
			if (g_mkdir (path, 700)) {
				g_set_error (error,
					     BRASERO_BURN_ERROR,
					     BRASERO_BURN_ERROR_GENERAL,
					     _("a directory couldn't be created (%s)"),
					     strerror (errno));
				result = BRASERO_BURN_ERR;
			}
			else {
				result = brasero_local_track_recursive_transfer (self,
										 src_child,
										 dest_child,
										 error);
			}

			g_free (path);
		}
		else {
			result = brasero_local_track_file_transfer (self,
								    src_child,
								    dest_child,
								    error);
			priv->read_bytes += g_file_info_get_size (info);
		}

		g_object_unref (info);
		g_object_unref (src_child);
		g_object_unref (dest_child);

		if (result != BRASERO_BURN_OK) {
			g_file_enumerator_close (enumerator, priv->cancel, NULL);
			g_object_unref (enumerator);
			return BRASERO_BURN_ERR;

		}
	}

	g_file_enumerator_close (enumerator, priv->cancel, NULL);
	g_object_unref (enumerator);

	return BRASERO_BURN_OK;
}

static BraseroBurnResult
brasero_local_track_transfer (BraseroLocalTrack *self,
			      GFile *src,
			      GFile *dest,
			      GError **error)
{
	GFileInfo *info;
	BraseroBurnResult result;
	BraseroLocalTrackPrivate *priv;

	priv = BRASERO_LOCAL_TRACK_PRIVATE (self);

	/* Retrieve some information about the file we have to copy */
	info = g_file_query_info (src,
				  G_FILE_ATTRIBUTE_STANDARD_TYPE,
				  G_FILE_QUERY_INFO_NONE, /* follow symlinks */
				  priv->cancel,
				  error);
	if (!info || error)
		return BRASERO_BURN_ERR;

	/* Retrieve the size of all the data. */
	if (g_file_info_get_file_type (info) != G_FILE_TYPE_DIRECTORY)
		priv->data_size = g_file_info_get_size (info);
	else
		brasero_local_track_get_download_size (self, src, error);

	priv->read_bytes = 0;

	/* start the downloading */
	if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY) {
		gchar *dest_path;

		dest_path = g_file_get_path (dest);
		if (g_mkdir_with_parents (dest_path, 700)) {
			g_free (dest_path);
			g_object_unref (info);

			g_set_error (error,
				     BRASERO_BURN_ERROR,
				     BRASERO_BURN_ERROR_GENERAL,
				     _("a directory couldn't be created (%s)"),
				     strerror (errno));
			return BRASERO_BURN_ERR;
		}

		g_free (dest_path);
		result = brasero_local_track_recursive_transfer (self, src, dest, error);
	}
	else {
		result = brasero_local_track_file_transfer (self, src, dest, error);
		priv->read_bytes += g_file_info_get_size (info);
	}

	g_object_unref (info);

	return result;
}

/**
 * That's for URI translation ...
 */

static gchar *
brasero_local_track_translate_uri (BraseroLocalTrack *self,
				   gchar *uri)
{
	gchar *newuri;
	gchar *parent;
	BraseroLocalTrackPrivate *priv;

	priv = BRASERO_LOCAL_TRACK_PRIVATE (self);

	if (uri == NULL)
		return NULL;

	/* see if it is a local file */
	if (g_str_has_prefix (uri, "file://"))
		return uri;

	/* see if it was downloaded itself */
	newuri = g_hash_table_lookup (priv->nonlocals, uri);
	if (newuri) {
		g_free (uri);

		/* we copy this string as it will be freed when freeing 
		 * downloaded GSList */
		return g_strdup (newuri);
	}

	/* see if one of its parent will be downloaded */
	parent = g_path_get_dirname (uri);
	while (parent [1] != '\0') {
		gchar *tmp;

		tmp = g_hash_table_lookup (priv->nonlocals, parent);
		if (tmp) {
			newuri = g_strconcat (tmp,
					      uri + strlen (parent),
					      NULL);
			g_free (parent);
			g_free (uri);
			return newuri;
		}

		tmp = parent;
		parent = g_path_get_dirname (tmp);
		g_free (tmp);
	}

	/* that should not happen */
	g_warning ("Can't find a downloaded parent for this non local uri.\n");

	g_free (parent);
	g_free (uri);
	return NULL;
}

static BraseroBurnResult
brasero_local_track_read_checksum (BraseroLocalTrack *self)
{
	gint bytes;
	FILE *file;
	gchar *path;
	BraseroLocalTrackPrivate *priv;

	priv = BRASERO_LOCAL_TRACK_PRIVATE (self);

	memset (priv->checksum, 0, sizeof (priv->checksum));

	/* get the file_checksum from the md5 file */
	path = g_filename_from_uri (priv->checksum_dest, NULL, NULL);
	file = fopen (path, "r");
	g_free (path);

	if (!file)
		return BRASERO_BURN_ERR;

	bytes = fread (priv->checksum, 1, sizeof (priv->checksum) - 1, file);
	fclose (file);

	if (bytes != sizeof (priv->checksum) - 1)
		return BRASERO_BURN_ERR;

	return BRASERO_BURN_OK;
}

static BraseroBurnResult
brasero_local_track_download_checksum (BraseroLocalTrack *self)
{
	BraseroLocalTrackPrivate *priv;
	BraseroBurnResult result;
	GFile *src, *dest;

	priv = BRASERO_LOCAL_TRACK_PRIVATE (self);

	/* generate a unique name for dest */
	result = brasero_job_get_tmp_file (BRASERO_JOB (self),
					   NULL,
					   &priv->checksum_dest,
					   NULL);
	if (result != BRASERO_BURN_OK)
		goto error;

	brasero_job_set_current_action (BRASERO_JOB (self),
					BRASERO_BURN_ACTION_FILE_COPY,
					_("Copying files md5sum file"),
					TRUE);

	src = g_file_new_for_uri (priv->checksum_src);
	dest = g_file_new_for_uri (priv->checksum_dest);

	result = brasero_local_track_transfer (self,
					       src,
					       dest,
					       NULL);

	g_object_unref (dest);
	g_object_unref (src);

	return result;

error:
	/* we give up */
	g_free (priv->checksum_src);
	priv->checksum_src = NULL;
	g_free (priv->checksum_dest);
	priv->checksum_dest = NULL;
	return result;
}

static gboolean
brasero_local_track_thread_finished (BraseroLocalTrack *self)
{
	BraseroTrack *track;
	BraseroTrackType input;
	BraseroLocalTrackPrivate *priv;

	priv = BRASERO_LOCAL_TRACK_PRIVATE (self);

	priv->thread_id = 0;

	if (priv->cancel) {
		g_object_unref (priv->cancel);
		priv->cancel = NULL;
		if (g_cancellable_is_cancelled (priv->cancel))
			return FALSE;
	}

	if (priv->error) {
		GError *error;

		error = priv->error;
		priv->error = NULL;
		brasero_job_error (BRASERO_JOB (self), error);
		return FALSE;
	}

	/* now we update all the track with the local uris in retval */
	brasero_job_get_current_track (BRASERO_JOB (self), &track);
	track = brasero_track_copy (track);
	brasero_track_get_type (track, &input);

	/* make a copy of the tracks instead of modifying them */
	switch (input.type) {
	case BRASERO_TRACK_TYPE_DATA: {
		GSList *grafts;
		GSList *unreadable;

		grafts = brasero_track_get_data_grafts_source (track);
		for (; grafts; grafts = grafts->next) {
			BraseroGraftPt *graft;

			graft = grafts->data;
			graft->uri = brasero_local_track_translate_uri (self, graft->uri);
		}

		/* translate the globally excluded */
		unreadable = brasero_track_get_data_excluded_source (track, FALSE);
		for (; unreadable; unreadable = unreadable->next)
			unreadable->data = brasero_local_track_translate_uri (self, unreadable->data);
	}
	break;

	case BRASERO_TRACK_TYPE_AUDIO: {
		gchar *uri;

		uri = brasero_track_get_audio_source (track, TRUE);
		uri = brasero_local_track_translate_uri (self, uri);
		brasero_track_set_audio_source (track, uri, input.subtype.audio_format); 
	}
	break;

	case BRASERO_TRACK_TYPE_IMAGE: {
		gchar *uri;

		uri = brasero_track_get_image_source (track, TRUE);
		uri = brasero_local_track_translate_uri (self, uri);
		brasero_track_set_image_source (track, uri, NULL, input.subtype.img_format);

		uri = brasero_track_get_toc_source (track, TRUE);
		uri = brasero_local_track_translate_uri (self, uri);
		brasero_track_set_image_source (track, NULL, uri, input.subtype.img_format);
	}
	break;

	default:
		BRASERO_JOB_NOT_SUPPORTED (self);
	}

	if (priv->checksum)
		brasero_track_set_checksum (track,
					    BRASERO_CHECKSUM_MD5,
					    priv->checksum);

	brasero_job_add_track (BRASERO_JOB (self), track);

	/* It's good practice to unref the track afterwards as we don't need it
	 * anymore. BraseroTaskCtx refs it. */
	brasero_track_unref (track);
	brasero_job_finished_track (BRASERO_JOB (self));
	return FALSE;
}

static gpointer
brasero_local_track_thread (gpointer data)
{
	BraseroLocalTrack *self = BRASERO_LOCAL_TRACK (data);
	BraseroLocalTrackPrivate *priv;
	GSList *src, *dest;

	priv = BRASERO_LOCAL_TRACK_PRIVATE (self);
	brasero_job_set_current_action (BRASERO_JOB (self),
					BRASERO_BURN_ACTION_FILE_COPY,
					_("Copying files locally"),
					TRUE);

	for (src = priv->src_list, dest = priv->dest_list;
	     src && dest;
	     src = src->next, dest = dest->next) {
		GFile *src_file;
		GFile *dest_file;
		BraseroBurnResult result;

		src_file = src->data;
		dest_file = dest->data;

		result = brasero_local_track_transfer (self,
						       src_file,
						       dest_file,
						       &priv->error);
		if (g_cancellable_is_cancelled (priv->cancel))
			goto end;

		if (result != BRASERO_BURN_OK)
			goto end;
	}

	/* successfully downloaded files, get a checksum if we can. */
	if (priv->checksum_src
	&& !priv->checksum_dest
	&&  brasero_local_track_download_checksum (self) == BRASERO_BURN_OK)
		brasero_local_track_read_checksum (self);

end:

	if (!g_cancellable_is_cancelled (priv->cancel))
		priv->thread_id = g_idle_add ((GSourceFunc) brasero_local_track_thread_finished, self);

	priv->thread = NULL;
	g_thread_exit (NULL);

	return NULL;
}

static BraseroBurnResult
brasero_local_track_start_thread (BraseroLocalTrack *self,
				  GError **error)
{
	BraseroLocalTrackPrivate *priv;

	priv = BRASERO_LOCAL_TRACK_PRIVATE (self);

	if (priv->thread)
		return BRASERO_BURN_RUNNING;

	priv->thread = g_thread_create (brasero_local_track_thread,
					self,
					TRUE,
					error);
	if (!priv->thread) 
		return BRASERO_BURN_ERR;

	priv->cancel = g_cancellable_new ();
	return BRASERO_BURN_OK;
}

static gboolean
_foreach_non_local_cb (const gchar *uri,
		       const gchar *localuri,
		       gpointer *data)
{
	BraseroLocalTrackPrivate *priv;
	GFile *file, *tmpfile;
	gchar *parent;
	gchar *tmp;

	priv = BRASERO_LOCAL_TRACK_PRIVATE (data);

	/* check that is hasn't any parent in the hash */
	parent = g_path_get_dirname (uri);
	while (parent [1] != '\0') {
		localuri = g_hash_table_lookup (priv->nonlocals, parent);
		if (localuri) {
			g_free (parent);
			return TRUE;
		}

		tmp = parent;
		parent = g_path_get_dirname (tmp);
		g_free (tmp);
	}
	g_free (parent);

	file = g_file_new_for_uri (uri);
	priv->src_list = g_slist_append (priv->src_list, file);

	tmpfile = g_file_new_for_uri (localuri);
	priv->dest_list = g_slist_append (priv->dest_list, tmpfile);

	return FALSE;
}

static BraseroBurnResult
brasero_local_track_add_if_non_local (BraseroLocalTrack *self,
				      const gchar *uri,
				      GError **error)
{
	BraseroLocalTrackPrivate *priv;
	BraseroBurnResult result;
	gchar *localuri = NULL;

	priv = BRASERO_LOCAL_TRACK_PRIVATE (self);

	if (!uri || g_str_has_prefix (uri, "file://"))
		return BRASERO_BURN_OK;

	/* add it to the list or uris to download */
	if (!priv->nonlocals)
		priv->nonlocals = g_hash_table_new_full (g_str_hash,
							 g_str_equal,
							 NULL,
							 g_free);

	if (g_str_has_prefix (uri, "burn://")) {
		GFile *file;
		GFileInfo *info;

		/* this is a special case for burn:// uris */
		file = g_file_new_for_uri (uri);
		info = g_file_query_info (file, "burn::backing-file", 0, NULL, error);
		g_object_unref (file);

		if (!info)
			return BRASERO_BURN_ERR;

		localuri = g_strdup (g_file_info_get_attribute_byte_string (info, "burn::backing-file"));
		g_object_unref (info);
		if (!localuri) {
			g_set_error (error,
				     BRASERO_BURN_ERROR,
				     BRASERO_BURN_ERROR_GENERAL,
				     _("impossible to retrieve local file path"));
			return BRASERO_BURN_ERR;
		}

		g_hash_table_insert (priv->nonlocals, (gpointer) uri, localuri);
		return BRASERO_BURN_OK;
	}

	/* generate a unique name */
	result = brasero_job_get_tmp_file (BRASERO_JOB (self),
					   NULL,
					   &localuri,
					   error);
	if (result != BRASERO_BURN_OK)
		return result;

	if (!g_str_has_prefix (localuri, "file://")) {
		gchar *tmp;

		tmp = localuri;
		localuri = g_strconcat ("file://", tmp, NULL);
		g_free (tmp);
	}

	/* we don't want to replace it if it has already been downloaded */
	if (!g_hash_table_lookup (priv->nonlocals, uri))
		g_hash_table_insert (priv->nonlocals, (gpointer) uri, localuri);

	return BRASERO_BURN_OK;
}

static BraseroBurnResult
brasero_local_track_start (BraseroJob *job,
			   GError **error)
{
	BraseroLocalTrackPrivate *priv;
	BraseroJobAction action;
	BraseroLocalTrack *self;
	BraseroTrackType input;
	BraseroTrack *track;
	GSList *grafts;
	gchar *uri;

	self = BRASERO_LOCAL_TRACK (job);
	priv = BRASERO_LOCAL_TRACK_PRIVATE (self);

	brasero_job_get_action (job, &action);

	/* skip that part */
	if (action == BRASERO_JOB_ACTION_SIZE)
		return BRASERO_BURN_NOT_RUNNING;

	if (action != BRASERO_JOB_ACTION_IMAGE)
		return BRASERO_BURN_NOT_SUPPORTED;

	/* can't be piped so brasero_job_get_current_track will work */
	brasero_job_get_current_track (job, &track);
	brasero_job_get_input_type (job, &input);

	/* make a list of all non local uris to be downloaded and put them in a
	 * list to avoid to download the same file twice. */
	switch (input.type) {
	case BRASERO_TRACK_TYPE_DATA:
		/* we put all the non local graft point uris in the hash */
		grafts = brasero_track_get_data_grafts_source (track);
		for (; grafts; grafts = grafts->next) {
			BraseroGraftPt *graft;

			graft = grafts->data;
			brasero_local_track_add_if_non_local (self, graft->uri, error);
		}
		break;

	case BRASERO_TRACK_TYPE_AUDIO:
		uri = brasero_track_get_audio_source (track, TRUE);
		brasero_local_track_add_if_non_local (self, uri, error);
		g_free (uri);
		break;

	case BRASERO_TRACK_TYPE_IMAGE:
		uri = brasero_track_get_image_source (track, TRUE);
		brasero_local_track_add_if_non_local (self, uri, error);

		/* This is an image. See if there is any md5 sum sitting in the
		 * same directory to check our download integrity */
		priv->checksum_src = g_strdup_printf ("%s.md5", uri);
		g_free (uri);

		uri = brasero_track_get_toc_source (track, TRUE);
		brasero_local_track_add_if_non_local (self, uri, error);
		g_free (uri);
		break;

	default:
		BRASERO_JOB_NOT_SUPPORTED (self);
	}

	/* see if there is anything to download */
	if (!priv->nonlocals) {
		BRASERO_JOB_LOG (self, "no foreign URIs");
		return BRASERO_BURN_NOT_RUNNING;
	}

	/* first we create a list of all the non local files that need to be
	 * downloaded. To be elligible a file must not have one of his parent
	 * in the hash. */
	g_hash_table_foreach_remove (priv->nonlocals,
				     (GHRFunc) _foreach_non_local_cb,
				     job);

	/* if there are files in list then download them otherwise stop */
	if (!priv->src_list) {
		/* that means there were only burn:// uris in nonlocals */
		BRASERO_JOB_LOG (self, "no foreign URIs");
		return BRASERO_BURN_NOT_RUNNING;
	}

	return brasero_local_track_start_thread (self, error);
}

static BraseroBurnResult
brasero_local_track_stop (BraseroJob *job,
			  GError **error)
{
	BraseroLocalTrackPrivate *priv = BRASERO_LOCAL_TRACK_PRIVATE (job);

	if (priv->cancel) {
		/* signal that we've been cancelled */
		g_cancellable_cancel (priv->cancel);
	}

	if (priv->thread)
		g_thread_join (priv->thread);

	if (priv->cancel) {
		/* unref it after the thread has stopped */
		g_object_unref (priv->cancel);
		priv->cancel = NULL;
	}

	if (priv->thread_id) {
		g_source_remove (priv->thread_id);
		priv->thread_id = 0;
	}

	if (priv->error) {
		g_error_free (priv->error);
		priv->error = NULL;
	}

	if (priv->src_list) {
		g_slist_foreach (priv->src_list, (GFunc) g_object_unref, NULL);
		g_slist_free (priv->src_list);
	}

	if (priv->dest_list) {
		g_slist_foreach (priv->dest_list, (GFunc) g_object_unref, NULL);
		g_slist_free (priv->dest_list);
	}

	if (priv->nonlocals) {
		g_hash_table_destroy (priv->nonlocals);
		priv->nonlocals = NULL;
	}

	if (priv->checksum_src) {
		g_free (priv->checksum_src);
		priv->checksum_src = NULL;
	}

	if (priv->checksum_dest) {
		g_free (priv->checksum_dest);
		priv->checksum_dest = NULL;
	}

	return BRASERO_BURN_OK;
}

static void
brasero_local_track_finalize (GObject *object)
{
	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
brasero_local_track_class_init (BraseroLocalTrackClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	BraseroJobClass *job_class = BRASERO_JOB_CLASS (klass);

	g_type_class_add_private (klass, sizeof (BraseroLocalTrackPrivate));

	parent_class = g_type_class_peek_parent (klass);
	object_class->finalize = brasero_local_track_finalize;

	job_class->start = brasero_local_track_start;
	job_class->stop = brasero_local_track_stop;
}

static void
brasero_local_track_init (BraseroLocalTrack *obj)
{ }

static BraseroBurnResult
brasero_local_track_export_caps (BraseroPlugin *plugin, gchar **error)
{
	GSList *caps;

	brasero_plugin_define (plugin,
			       "local-track",
			       _("local-track allows to burn files not stored locally"),
			       "Philippe Rouquier",
			       10);

	caps = brasero_caps_image_new (BRASERO_PLUGIN_IO_ACCEPT_FILE,
				       BRASERO_IMAGE_FORMAT_ANY);
	brasero_plugin_process_caps (plugin, caps);
	g_slist_free (caps);

	caps = brasero_caps_audio_new (BRASERO_PLUGIN_IO_ACCEPT_FILE,
				       BRASERO_AUDIO_FORMAT_UNDEFINED|
				       BRASERO_AUDIO_FORMAT_4_CHANNEL|
				       BRASERO_AUDIO_FORMAT_RAW);
	brasero_plugin_process_caps (plugin, caps);
	g_slist_free (caps);

	caps = brasero_caps_data_new (BRASERO_IMAGE_FS_ANY);
	brasero_plugin_process_caps (plugin, caps);
	g_slist_free (caps);

	brasero_plugin_set_process_flags (plugin, BRASERO_PLUGIN_RUN_FIRST);

	return BRASERO_BURN_OK;
}
