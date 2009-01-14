/***************************************************************************
 *            burn-iso9660.h
 *
 *  Sat Oct  7 17:10:09 2006
 *  Copyright  2006  Rouquier Philippe
 *  <bonfire-app@wanadoo.fr>
 ****************************************************************************/

/*
 * Brasero is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * Brasero is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor Boston, MA 02110-1301,  USA
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>

#include <glib.h>

#include "burn-volume.h"
#include "burn-volume-source.h"

#ifndef _BURN_ISO9660_H
#define _BURN_ISO9660_H

G_BEGIN_DECLS

#define ISO9660_BLOCK_SIZE 2048

typedef enum {
	BRASERO_ISO_FLAG_NONE	= 0,
	BRASERO_ISO_FLAG_RR	= 1
} BraseroIsoFlag;

gboolean
brasero_iso9660_is_primary_descriptor (const gchar *buffer,
				       GError **error);

gboolean
brasero_iso9660_get_size (const gchar *block,
			  gint64 *nb_blocks,
			  GError **error);

gboolean
brasero_iso9660_get_label (const gchar *block,
			   gchar **label,
			   GError **error);

BraseroVolFile *
brasero_iso9660_get_contents (BraseroVolSrc *src,
			      const gchar *block,
			      gint64 *nb_blocks,
			      GError **error);

/**
 * Address to -1 for root
 */
GList *
brasero_iso9660_get_directory_contents (BraseroVolSrc *vol,
					const gchar *vol_desc,
					gint address,
					GError **error);

BraseroVolFile *
brasero_iso9660_get_file (BraseroVolSrc *src,
			  const gchar *path,
			  const gchar *block,
			  GError **error);

G_END_DECLS

#endif /* _BURN_ISO9660_H */

 