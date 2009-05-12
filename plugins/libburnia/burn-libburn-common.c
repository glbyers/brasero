/***************************************************************************
 *            burn-libburn-common.c
 *
 *  mer aoû 30 16:35:40 2006
 *  Copyright  2006  Rouquier Philippe
 *  bonfire-app@wanadoo.fr
 ***************************************************************************/

/*
 *  Brasero is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Brasero is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to:
 * 	The Free Software Foundation, Inc.,
 * 	51 Franklin Street, Fifth Floor
 * 	Boston, MA  02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <string.h>

#include <glib.h>
#include <glib-object.h>
#include <glib/gi18n-lib.h>

#include "burn-basics.h"
#include "burn-debug.h"
#include "burn-job.h"
#include "burn-libburn-common.h"

#include <libburn/libburn.h>

static void
brasero_libburn_common_ctx_free_real (BraseroLibburnCtx *ctx)
{
	BRASERO_BURN_LOG ("Drive stopped");

	if (ctx->disc) {
		burn_disc_free (ctx->disc);
		ctx->disc = NULL;
	}

	/* This must be done in this order since:
	 * ctx->drive = ctx->drive_info->drive */

	if (ctx->drive) {
		burn_drive_release (ctx->drive, 0);
		ctx->drive = NULL;
	}

	if (ctx->drive_info) {
		burn_drive_info_free (ctx->drive_info);
		ctx->drive_info = NULL;
	}

	g_free (ctx);

	/* Since the library is not needed any more call burn_finish ().
	 * NOTE: it itself calls burn_abort (). */
	burn_finish ();
}

static gboolean
brasero_libburn_common_ctx_wait_for_idle_drive (gpointer data)
{
	BraseroLibburnCtx *ctx = data;
	enum burn_drive_status status;

	/* try to properly cancel the drive */
	status = burn_drive_get_status (ctx->drive, NULL);
	if (status == BURN_DRIVE_WRITING || status == BURN_DRIVE_READING) {
		BRASERO_BURN_LOG ("Cancelling operation");
		burn_drive_cancel (ctx->drive);
	}

	if (status == BURN_DRIVE_GRABBING) {
		/* This should probably never happen */
		BRASERO_BURN_LOG ("Grabbing state, try to forget");
		burn_drive_info_forget (ctx->drive_info, 1);
	}

	if (status != BURN_DRIVE_IDLE) {
		BRASERO_BURN_LOG ("Drive not idle yet");
		return TRUE;
	}

	brasero_libburn_common_ctx_free_real (ctx);
	return FALSE;
}

void
brasero_libburn_common_ctx_free (BraseroLibburnCtx *ctx)
{
	enum burn_drive_status status;

	BRASERO_BURN_LOG ("Stopping Drive");

	/* try to properly cancel the drive */
	status = burn_drive_get_status (ctx->drive, NULL);
	if (status == BURN_DRIVE_WRITING || status == BURN_DRIVE_READING) {
		BRASERO_BURN_LOG ("Cancelling operation");
		burn_drive_cancel (ctx->drive);
	}

	if (status == BURN_DRIVE_GRABBING) {
		/* This should probably never happen */
		BRASERO_BURN_LOG ("Grabbing state, try to forget");
		burn_drive_info_forget (ctx->drive_info, 1);
	}
	
	if (status != BURN_DRIVE_IDLE) {
		/* otherwise wait for the drive to calm down */
		BRASERO_BURN_LOG ("Drive not idle yet");
		g_timeout_add (200,
			       brasero_libburn_common_ctx_wait_for_idle_drive,
			       ctx);
		return;
	}

	brasero_libburn_common_ctx_free_real (ctx);
}

BraseroLibburnCtx *
brasero_libburn_common_ctx_new (BraseroJob *job,
				GError **error)
{
	gchar libburn_device [BURN_DRIVE_ADR_LEN];
	BraseroLibburnCtx *ctx = NULL;
	gchar *device;
	int res;

	/* initialize the library */
	if (!burn_initialize ()) {
		g_set_error (error,
			     BRASERO_BURN_ERROR,
			     BRASERO_BURN_ERROR_GENERAL,
			     _("Libburn library could not be initialized"));
		return NULL;
	}

	/* We want all types of messages but not them printed */
	burn_msgs_set_severities ("ALL", "NEVER", "");

	/* we just want to scan the drive proposed by drive */
	brasero_job_get_device (job, &device);
	res = burn_drive_convert_fs_adr (device, libburn_device);
	g_free (device);
	if (res <= 0) {
		g_set_error (error,
			     BRASERO_BURN_ERROR,
			     BRASERO_BURN_ERROR_GENERAL,
			     _("The drive address could not be retrieved"));
		return NULL;
	}

	ctx = g_new0 (BraseroLibburnCtx, 1);
	res = burn_drive_scan_and_grab (&ctx->drive_info, libburn_device, 0);
	BRASERO_JOB_LOG (job, "Drive (%s) init result = %d", libburn_device, res);
	if (res <= 0) {
		g_free (ctx);
		g_set_error (error,
			     BRASERO_BURN_ERROR,
			     BRASERO_BURN_ERROR_DRIVE_BUSY,
			     _("The drive is busy"));
		return NULL;
	}

	ctx->drive = ctx->drive_info->drive;
	return ctx;	
}

static gboolean
brasero_libburn_common_process_message (BraseroJob *self)
{
	int ret;
	GError *error;
	int err_code = 0;
	int err_errno = 0;
	char err_sev [80];
	char err_txt [BURN_MSGS_MESSAGE_LEN] = {0};

	/* Get all messages, indicating an error */
	memset (err_txt, 0, sizeof (err_txt));
	ret = burn_msgs_obtain ("ALL",
				&err_code,
				err_txt,
				&err_errno,
				err_sev);
	if (ret == 0)
		return TRUE;

	if (strcmp ("FATAL", err_sev)
	&&  strcmp ("ABORT", err_sev)) {
		/* libburn didn't reported any FATAL message but maybe it did
		 * report some debugging output */
		BRASERO_JOB_LOG (self, err_txt);
	        return TRUE;
	}

	BRASERO_JOB_LOG (self, "Libburn reported an error %s", err_txt);
	error = g_error_new (BRASERO_BURN_ERROR,
			     BRASERO_BURN_ERROR_GENERAL,
			     err_txt);
	brasero_job_error (BRASERO_JOB (self), error);
	return FALSE;
}

static gboolean
brasero_libburn_common_status_changed (BraseroJob *self,
				       BraseroLibburnCtx *ctx,
				       enum burn_drive_status status,
				       struct burn_progress *progress)
{
	BraseroBurnAction action = BRASERO_BURN_ACTION_NONE;

	switch (status) {
		case BURN_DRIVE_WRITING:
			/* we ignore it if it happens after leadout */
			if (ctx->status == BURN_DRIVE_WRITING_LEADOUT
			||  ctx->status == BURN_DRIVE_CLOSING_TRACK
			||  ctx->status == BURN_DRIVE_CLOSING_SESSION)
				return TRUE;

			if (ctx->status == BURN_DRIVE_WRITING_LEADIN
			||  ctx->status == BURN_DRIVE_WRITING_PREGAP) {
				ctx->sectors += ctx->track_sectors;
				ctx->track_sectors = progress->sectors;
				ctx->track_num = progress->track;
			}

			action = BRASERO_BURN_ACTION_RECORDING;
			brasero_job_set_dangerous (BRASERO_JOB (self), TRUE);
			break;

		case BURN_DRIVE_WRITING_LEADIN:		/* DAO */
		case BURN_DRIVE_WRITING_PREGAP:		/* TAO */
			ctx->has_leadin = 1;
			action = BRASERO_BURN_ACTION_START_RECORDING;
			brasero_job_set_dangerous (BRASERO_JOB (self), FALSE);
			break;

		case BURN_DRIVE_WRITING_LEADOUT: 	/* DAO */
		case BURN_DRIVE_CLOSING_TRACK:		/* TAO */
		case BURN_DRIVE_CLOSING_SESSION:	/* Multisession end */
			ctx->sectors += ctx->track_sectors;
			ctx->track_sectors = progress->sectors;

			action = BRASERO_BURN_ACTION_FIXATING;
			brasero_job_set_dangerous (BRASERO_JOB (self), FALSE);
			break;

		case BURN_DRIVE_ERASING:
		case BURN_DRIVE_FORMATTING:
			action = BRASERO_BURN_ACTION_BLANKING;
			brasero_job_set_dangerous (BRASERO_JOB (self), TRUE);
			break;

		case BURN_DRIVE_IDLE:
			/* That's the end of activity */
			return FALSE;

		case BURN_DRIVE_SPAWNING:
			if (ctx->status == BURN_DRIVE_IDLE)
				action = BRASERO_BURN_ACTION_START_RECORDING;
			else
				action = BRASERO_BURN_ACTION_FIXATING;
			brasero_job_set_dangerous (BRASERO_JOB (self), FALSE);
			break;

		case BURN_DRIVE_READING:
			action = BRASERO_BURN_ACTION_DRIVE_COPY;
			brasero_job_set_dangerous (BRASERO_JOB (self), FALSE);
			break;

		default:
			BRASERO_JOB_LOG (self, "Unknown drive state (%i)", status);
			return TRUE;
	}

	ctx->status = status;
	brasero_job_set_current_action (self,
					action,
					NULL,
					FALSE);
	return TRUE;
}

BraseroBurnResult
brasero_libburn_common_status (BraseroJob *self,
			       BraseroLibburnCtx *ctx)
{
	enum burn_drive_status status;
	struct burn_progress progress;

	/* see if there is any pending message */
	if (!brasero_libburn_common_process_message (self))
		return BRASERO_BURN_ERR;

	if (!ctx->drive)
		return BRASERO_BURN_ERR;

	status = burn_drive_get_status (ctx->drive, &progress);

	/* FIXME! for some operations that libburn can't perform the drive stays
	 * idle and we've got no way to tell that kind of use case */
	if (ctx->status != status) {
		gboolean running;

		running = brasero_libburn_common_status_changed (self,
								 ctx,
								 status,
								 &progress);
		if (!running)
			return BRASERO_BURN_OK;
	}

	if (status == BURN_DRIVE_IDLE
	||  status == BURN_DRIVE_SPAWNING
	||  !progress.sectors
	||  !progress.sector) {
		ctx->sectors = 0;

		ctx->track_num = progress.track;
		ctx->track_sectors = progress.sectors;
		return BRASERO_BURN_RETRY;
	}

	if (status == BURN_DRIVE_CLOSING_SESSION
	||  status == BURN_DRIVE_WRITING_LEADOUT) {
		brasero_job_set_progress (self, 1.0);
	}
	else if (status != BURN_DRIVE_ERASING
	     &&  status != BURN_DRIVE_FORMATTING) {
		gint64 cur_sector;

		if (ctx->track_num != progress.track) {
			gchar *string;

			ctx->sectors += ctx->track_sectors;
			ctx->track_sectors = progress.sectors;
			ctx->track_num = progress.track;

			string = g_strdup_printf (_("Writing track %02i"), progress.track);
			brasero_job_set_current_action (self,
							BRASERO_BURN_ACTION_RECORDING,
							string,
							TRUE);
			g_free (string);
		}

		cur_sector = progress.sector + ctx->sectors;
		brasero_job_set_written_session (self, (gint64) ((gint64) cur_sector * 2048ULL));
	}
	else {
		gdouble fraction;

		/* when erasing only set progress */
		fraction = (gdouble) (progress.sector) /
			   (gdouble) (progress.sectors);

		brasero_job_set_progress (self, fraction);
	}

	brasero_job_start_progress (self, FALSE);
	return BRASERO_BURN_RETRY;
}