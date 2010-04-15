/*
 * Copyright (C) 2009-2010 Julien BLACHE <jb@jblache.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <inttypes.h>

#include <assert.h>

#include <pthread.h>

#include <sqlite3.h>

#include <unicode/utypes.h>
#include <unicode/ucnv.h>

#include "conffile.h"
#include "logger.h"
#include "misc.h"
#include "db.h"

#define STR(x) ((x) ? (x) : "")

/* Inotify cookies are uint32_t */
#define INOTIFY_FAKE_COOKIE ((int64_t)1 << 32)

enum group_type {
  G_ALBUMS = 1,
};

#define DB_TYPE_CHAR    1
#define DB_TYPE_INT     2
#define DB_TYPE_INT64   3
#define DB_TYPE_STRING  4

struct col_type_map {
  ssize_t offset;
  short type;
};

/* This list must be kept in sync with
 * - the order of the columns in the files table
 * - the type and name of the fields in struct media_file_info
 */
static const struct col_type_map mfi_cols_map[] =
  {
    { mfi_offsetof(id),                 DB_TYPE_INT },
    { mfi_offsetof(path),               DB_TYPE_STRING },
    { mfi_offsetof(fname),              DB_TYPE_STRING },
    { mfi_offsetof(title),              DB_TYPE_STRING },
    { mfi_offsetof(artist),             DB_TYPE_STRING },
    { mfi_offsetof(album),              DB_TYPE_STRING },
    { mfi_offsetof(genre),              DB_TYPE_STRING },
    { mfi_offsetof(comment),            DB_TYPE_STRING },
    { mfi_offsetof(type),               DB_TYPE_STRING },
    { mfi_offsetof(composer),           DB_TYPE_STRING },
    { mfi_offsetof(orchestra),          DB_TYPE_STRING },
    { mfi_offsetof(conductor),          DB_TYPE_STRING },
    { mfi_offsetof(grouping),           DB_TYPE_STRING },
    { mfi_offsetof(url),                DB_TYPE_STRING },
    { mfi_offsetof(bitrate),            DB_TYPE_INT },
    { mfi_offsetof(samplerate),         DB_TYPE_INT },
    { mfi_offsetof(song_length),        DB_TYPE_INT },
    { mfi_offsetof(file_size),          DB_TYPE_INT64 },
    { mfi_offsetof(year),               DB_TYPE_INT },
    { mfi_offsetof(track),              DB_TYPE_INT },
    { mfi_offsetof(total_tracks),       DB_TYPE_INT },
    { mfi_offsetof(disc),               DB_TYPE_INT },
    { mfi_offsetof(total_discs),        DB_TYPE_INT },
    { mfi_offsetof(bpm),                DB_TYPE_INT },
    { mfi_offsetof(compilation),        DB_TYPE_CHAR },
    { mfi_offsetof(rating),             DB_TYPE_INT },
    { mfi_offsetof(play_count),         DB_TYPE_INT },
    { mfi_offsetof(data_kind),          DB_TYPE_INT },
    { mfi_offsetof(item_kind),          DB_TYPE_INT },
    { mfi_offsetof(description),        DB_TYPE_STRING },
    { mfi_offsetof(time_added),         DB_TYPE_INT },
    { mfi_offsetof(time_modified),      DB_TYPE_INT },
    { mfi_offsetof(time_played),        DB_TYPE_INT },
    { mfi_offsetof(db_timestamp),       DB_TYPE_INT },
    { mfi_offsetof(disabled),           DB_TYPE_INT },
    { mfi_offsetof(sample_count),       DB_TYPE_INT64 },
    { mfi_offsetof(codectype),          DB_TYPE_STRING },
    { mfi_offsetof(index),              DB_TYPE_INT },
    { mfi_offsetof(has_video),          DB_TYPE_INT },
    { mfi_offsetof(contentrating),      DB_TYPE_INT },
    { mfi_offsetof(bits_per_sample),    DB_TYPE_INT },
    { mfi_offsetof(album_artist),       DB_TYPE_STRING },
    { mfi_offsetof(media_kind),         DB_TYPE_INT },
    { mfi_offsetof(tv_series_name),     DB_TYPE_STRING },
    { mfi_offsetof(tv_episode_num_str), DB_TYPE_STRING },
    { mfi_offsetof(tv_network_name),    DB_TYPE_STRING },
    { mfi_offsetof(tv_episode_sort),    DB_TYPE_INT },
    { mfi_offsetof(tv_season_num),      DB_TYPE_INT },
    { mfi_offsetof(songalbumid),        DB_TYPE_INT64 },
  };

/* This list must be kept in sync with
 * - the order of the columns in the playlists table
 * - the type and name of the fields in struct playlist_info
 */
static const struct col_type_map pli_cols_map[] =
  {
    { pli_offsetof(id),           DB_TYPE_INT },
    { pli_offsetof(title),        DB_TYPE_STRING },
    { pli_offsetof(type),         DB_TYPE_INT },
    { pli_offsetof(query),        DB_TYPE_STRING },
    { pli_offsetof(db_timestamp), DB_TYPE_INT },
    { pli_offsetof(disabled),     DB_TYPE_INT },
    { pli_offsetof(path),         DB_TYPE_STRING },
    { pli_offsetof(index),        DB_TYPE_INT },
    { pli_offsetof(special_id),   DB_TYPE_INT },

    /* items is computed on the fly */
  };

/* This list must be kept in sync with
 * - the order of the columns in the files table
 * - the name of the fields in struct db_media_file_info
 */
static const ssize_t dbmfi_cols_map[] =
  {
    dbmfi_offsetof(id),
    dbmfi_offsetof(path),
    dbmfi_offsetof(fname),
    dbmfi_offsetof(title),
    dbmfi_offsetof(artist),
    dbmfi_offsetof(album),
    dbmfi_offsetof(genre),
    dbmfi_offsetof(comment),
    dbmfi_offsetof(type),
    dbmfi_offsetof(composer),
    dbmfi_offsetof(orchestra),
    dbmfi_offsetof(conductor),
    dbmfi_offsetof(grouping),
    dbmfi_offsetof(url),
    dbmfi_offsetof(bitrate),
    dbmfi_offsetof(samplerate),
    dbmfi_offsetof(song_length),
    dbmfi_offsetof(file_size),
    dbmfi_offsetof(year),
    dbmfi_offsetof(track),
    dbmfi_offsetof(total_tracks),
    dbmfi_offsetof(disc),
    dbmfi_offsetof(total_discs),
    dbmfi_offsetof(bpm),
    dbmfi_offsetof(compilation),
    dbmfi_offsetof(rating),
    dbmfi_offsetof(play_count),
    dbmfi_offsetof(data_kind),
    dbmfi_offsetof(item_kind),
    dbmfi_offsetof(description),
    dbmfi_offsetof(time_added),
    dbmfi_offsetof(time_modified),
    dbmfi_offsetof(time_played),
    dbmfi_offsetof(db_timestamp),
    dbmfi_offsetof(disabled),
    dbmfi_offsetof(sample_count),
    dbmfi_offsetof(codectype),
    dbmfi_offsetof(idx),
    dbmfi_offsetof(has_video),
    dbmfi_offsetof(contentrating),
    dbmfi_offsetof(bits_per_sample),
    dbmfi_offsetof(album_artist),
    dbmfi_offsetof(media_kind),
    dbmfi_offsetof(tv_series_name),
    dbmfi_offsetof(tv_episode_num_str),
    dbmfi_offsetof(tv_network_name),
    dbmfi_offsetof(tv_episode_sort),
    dbmfi_offsetof(tv_season_num),
    dbmfi_offsetof(songalbumid),
  };

/* This list must be kept in sync with
 * - the order of the columns in the playlists table
 * - the name of the fields in struct playlist_info
 */
static const ssize_t dbpli_cols_map[] =
  {
    dbpli_offsetof(id),
    dbpli_offsetof(title),
    dbpli_offsetof(type),
    dbpli_offsetof(query),
    dbpli_offsetof(db_timestamp),
    dbpli_offsetof(disabled),
    dbpli_offsetof(path),
    dbpli_offsetof(index),
    dbpli_offsetof(special_id),

    /* items is computed on the fly */
  };

/* This list must be kept in sync with
 * - the order of fields in the Q_GROUPS query
 * - the name of the fields in struct group_info
 */
static const ssize_t dbgri_cols_map[] =
  {
    dbgri_offsetof(itemcount),
    dbgri_offsetof(id),
    dbgri_offsetof(persistentid),
    dbgri_offsetof(songalbumartist),
    dbgri_offsetof(itemname),
  };

/* This list must be kept in sync with
 * - the order of the columns in the inotify table
 * - the name and type of the fields in struct watch_info
 */
static const struct col_type_map wi_cols_map[] =
  {
    { wi_offsetof(wd),     DB_TYPE_INT },
    { wi_offsetof(cookie), DB_TYPE_INT },
    { wi_offsetof(path),   DB_TYPE_STRING },
  };

static char *db_path;
static __thread sqlite3 *hdl;


/* Forward */
static int
db_pl_count_items(int id);

static int
db_smartpl_count_items(const char *smartpl_query);

struct playlist_info *
db_pl_fetch_byid(int id);

void ensure_all_utf8(struct media_file_info *mfi);
void ensure_utf8(char *str);

char *
db_escape_string(const char *str)
{
  char *escaped;
  char *ret;

  escaped = sqlite3_mprintf("%q", str);
  if (!escaped)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for escaped string\n");

      return NULL;
    }

  ret = strdup(escaped);

  sqlite3_free(escaped);

  return ret;
}

void
free_pi(struct pairing_info *pi, int content_only)
{
  if (pi->remote_id)
    free(pi->remote_id);

  if (pi->name)
    free(pi->name);

  if (pi->guid)
    free(pi->guid);

  if (!content_only)
    free(pi);
}

void
free_mfi(struct media_file_info *mfi, int content_only)
{
  if (mfi->path)
    free(mfi->path);

  if (mfi->fname)
    free(mfi->fname);

  if (mfi->title)
    free(mfi->title);

  if (mfi->artist)
    free(mfi->artist);

  if (mfi->album)
    free(mfi->album);

  if (mfi->genre)
    free(mfi->genre);

  if (mfi->comment)
    free(mfi->comment);

  if (mfi->type)
    free(mfi->type);

  if (mfi->composer)
    free(mfi->composer);

  if (mfi->orchestra)
    free(mfi->orchestra);

  if (mfi->conductor)
    free(mfi->conductor);

  if (mfi->grouping)
    free(mfi->grouping);

  if (mfi->description)
    free(mfi->description);

  if (mfi->codectype)
    free(mfi->codectype);

  if (mfi->album_artist)
    free(mfi->album_artist);

  if (mfi->tv_series_name)
    free(mfi->tv_series_name);

  if (mfi->tv_episode_num_str)
    free(mfi->tv_episode_num_str);

  if (mfi->tv_network_name)
    free(mfi->tv_network_name);

  if (!content_only)
    free(mfi);
}

void
free_pli(struct playlist_info *pli, int content_only)
{
  if (pli->title)
    free(pli->title);

  if (pli->query)
    free(pli->query);

  if (pli->path)
    free(pli->path);

  if (!content_only)
    free(pli);
}

void
db_purge_cruft(time_t ref)
{
  char *errmsg;
  int i;
  int ret;
  char *queries[3] = { NULL, NULL, NULL };
  char *queries_tmpl[3] =
    {
      "DELETE FROM playlistitems WHERE playlistid IN (SELECT id FROM playlists WHERE type <> 1 AND db_timestamp < %" PRIi64 ");",
      "DELETE FROM playlists WHERE type <> 1 AND db_timestamp < %" PRIi64 ";",
      "DELETE FROM files WHERE db_timestamp < %" PRIi64 ";"
    };

  if (sizeof(queries) != sizeof(queries_tmpl))
    {
      DPRINTF(E_LOG, L_DB, "db_purge_cruft(): queries out of sync with queries_tmpl\n");
      return;
    }

  for (i = 0; i < (sizeof(queries_tmpl) / sizeof(queries_tmpl[0])); i++)
    {
      queries[i] = sqlite3_mprintf(queries_tmpl[i], (int64_t)ref);
      if (!queries[i])
	{
	  DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
	  goto purge_fail;
	}
    }

  for (i = 0; i < (sizeof(queries) / sizeof(queries[0])); i++)
    {
      DPRINTF(E_DBG, L_DB, "Running purge query '%s'\n", queries[i]);

      ret = sqlite3_exec(hdl, queries[i], NULL, NULL, &errmsg);
      if (ret != SQLITE_OK)
	{
	  DPRINTF(E_LOG, L_DB, "Purge query %d error: %s\n", i, errmsg);

	  sqlite3_free(errmsg);
	}
      else
	DPRINTF(E_DBG, L_DB, "Purged %d rows\n", sqlite3_changes(hdl));
    }

 purge_fail:
  for (i = 0; i < (sizeof(queries) / sizeof(queries[0])); i++)
    {
      sqlite3_free(queries[i]);
    }

}

static int
db_get_count(char *query)
{
  sqlite3_stmt *stmt;
  int ret;

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = sqlite3_prepare_v2(hdl, query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));
      return -1;
    }

  ret = sqlite3_step(stmt);
  if (ret != SQLITE_ROW)
    {
      DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));	

      sqlite3_finalize(stmt);
      return -1;
    }

  ret = sqlite3_column_int(stmt, 0);

  sqlite3_finalize(stmt);

  return ret;
}


/* Queries */
static int
db_build_query_index_clause(struct query_params *qp, char **i)
{
  char *idx;

  switch (qp->idx_type)
    {
      case I_FIRST:
	idx = sqlite3_mprintf("LIMIT %d", qp->limit);
	break;

      case I_LAST:
	idx = sqlite3_mprintf("LIMIT -1 OFFSET %d", qp->limit, qp->results - qp->limit);
	break;

      case I_SUB:
	idx = sqlite3_mprintf("LIMIT %d OFFSET %d", qp->limit, qp->offset);
	break;

      case I_NONE:
	*i = NULL;
	return 0;

      default:
	DPRINTF(E_LOG, L_DB, "Unknown index type\n");
	return -1;
    }

  if (!idx)
    {
      DPRINTF(E_LOG, L_DB, "Could not build index string; out of memory");
      return -1;
    }

  *i = idx;

  return 0;
}

static int
db_build_query_items(struct query_params *qp, char **q)
{
  char *query;
  char *count;
  char *idx;
  int ret;

  if (qp->filter)
    count = sqlite3_mprintf("SELECT COUNT(*) FROM files WHERE disabled = 0 AND %s;", qp->filter);
  else
    count = sqlite3_mprintf("SELECT COUNT(*) FROM files WHERE disabled = 0;");

  if (!count)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for count query string\n");

      return -1;
    }

  qp->results = db_get_count(count);
  sqlite3_free(count);

  if (qp->results < 0)
    return -1;

  /* Get index clause */
  ret = db_build_query_index_clause(qp, &idx);
  if (ret < 0)
    return -1;

  if (idx && qp->filter)
    query = sqlite3_mprintf("SELECT * FROM files WHERE disabled = 0 AND %s %s;", qp->filter, idx);
  else if (idx)
    query = sqlite3_mprintf("SELECT * FROM files WHERE disabled = 0 %s;", idx);
  else if (qp->filter)
    query = sqlite3_mprintf("SELECT * FROM files WHERE disabled = 0 AND %s;", qp->filter);
  else
    query = sqlite3_mprintf("SELECT * FROM files WHERE disabled = 0;");

  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return -1;
    }

  *q = query;

  return 0;
}

static int
db_build_query_pls(struct query_params *qp, char **q)
{
  char *query;
  char *idx;
  int ret;

  qp->results = db_get_count("SELECT COUNT(*) FROM playlists WHERE disabled = 0;");
  if (qp->results < 0)
    return -1;

  /* Get index clause */
  ret = db_build_query_index_clause(qp, &idx);
  if (ret < 0)
    return -1;

  if (idx && qp->filter)
    query = sqlite3_mprintf("SELECT * FROM playlists WHERE disabled = 0 AND %s %s;", qp->filter, idx);
  else if (idx)
    query = sqlite3_mprintf("SELECT * FROM playlists WHERE disabled = 0 %s;", idx);
  else if (qp->filter)
    query = sqlite3_mprintf("SELECT * FROM playlists WHERE disabled = 0 AND %s;", qp->filter);
  else
    query = sqlite3_mprintf("SELECT * FROM playlists WHERE disabled = 0;");

  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return -1;
    }

  *q = query;

  return 0;
}

static int
db_build_query_plitems_plain(struct query_params *qp, char **q)
{
  char *query;
  char *count;
  char *idx;
  int ret;

  if (qp->filter)
    count = sqlite3_mprintf("SELECT COUNT(*) FROM files JOIN playlistitems ON files.path = playlistitems.filepath"
			    " WHERE playlistitems.playlistid = %d AND files.disabled = 0 AND %s;", qp->id, qp->filter);
  else
    count = sqlite3_mprintf("SELECT COUNT(*) FROM files JOIN playlistitems ON files.path = playlistitems.filepath"
			    " WHERE playlistitems.playlistid = %d AND files.disabled = 0;", qp->id);

  if (!count)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for count query string\n");

      return -1;
    }

  qp->results = db_get_count(count);
  sqlite3_free(count);

  if (qp->results < 0)
    return -1;

  /* Get index clause */
  ret = db_build_query_index_clause(qp, &idx);
  if (ret < 0)
    return -1;

  if (idx && qp->filter)
    query = sqlite3_mprintf("SELECT files.* FROM files JOIN playlistitems ON files.path = playlistitems.filepath"
			    " WHERE playlistitems.playlistid = %d AND files.disabled = 0 AND %s ORDER BY playlistitems.id ASC %s;",
			    qp->id, qp->filter, idx);
  else if (idx)
    query = sqlite3_mprintf("SELECT files.* FROM files JOIN playlistitems ON files.path = playlistitems.filepath"
			    " WHERE playlistitems.playlistid = %d AND files.disabled = 0 ORDER BY playlistitems.id ASC %s;",
			    qp->id, idx);
  else if (qp->filter)
    query = sqlite3_mprintf("SELECT files.* FROM files JOIN playlistitems ON files.path = playlistitems.filepath"
			    " WHERE playlistitems.playlistid = %d AND files.disabled = 0 AND %s ORDER BY playlistitems.id ASC;",
			    qp->id, qp->filter);
  else
    query = sqlite3_mprintf("SELECT files.* FROM files JOIN playlistitems ON files.path = playlistitems.filepath"
			    " WHERE playlistitems.playlistid = %d AND files.disabled = 0 ORDER BY playlistitems.id ASC;",
			    qp->id);

  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return -1;
    }

  *q = query;

  return 0;
}

static int
db_build_query_plitems_smart(struct query_params *qp, char *smartpl_query, char **q)
{
  char *query;
  char *count;
  char *filter;
  char *idx;
  int ret;

  if (qp->filter)
    filter = qp->filter;
  else
    filter = "1 = 1";

  count = sqlite3_mprintf("SELECT COUNT(*) FROM files WHERE disabled = 0 AND %s AND %s;", filter, smartpl_query);
  if (!count)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for count query string\n");
      return -1;
    }

  qp->results = db_get_count(count);

  sqlite3_free(count);

  if (qp->results < 0)
    return -1;

  /* Get index clause */
  ret = db_build_query_index_clause(qp, &idx);
  if (ret < 0)
    return -1;

  if (!idx)
    idx = "";

  query = sqlite3_mprintf("SELECT * FROM files WHERE disabled = 0 AND %s AND %s %s;", smartpl_query, filter, idx);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return -1;
    }

  *q = query;

  return 0;
}

static int
db_build_query_plitems(struct query_params *qp, char **q)
{
  struct playlist_info *pli;
  int ret;

  if (qp->id <= 0)
    {
      DPRINTF(E_LOG, L_DB, "No playlist id specified in playlist items query\n");
      return -1;
    }

  pli = db_pl_fetch_byid(qp->id);
  if (!pli)
    return -1;

  switch (pli->type)
    {
      case PL_SMART:
	ret = db_build_query_plitems_smart(qp, pli->query, q);
	break;

      case PL_PLAIN:
	ret = db_build_query_plitems_plain(qp, q);
	break;

      default:
	DPRINTF(E_LOG, L_DB, "Unknown playlist type %d in playlist items query\n", pli->type);
	ret = -1;
	break;
    }

  free_pli(pli, 0);

  return ret;
}

static int
db_build_query_groups(struct query_params *qp, char **q)
{
  char *query;
  char *idx;
  int ret;

  qp->results = db_get_count("SELECT COUNT(DISTINCT songalbumid) FROM files WHERE disabled = 0;");
  if (qp->results < 0)
    return -1;

  /* Get index clause */
  ret = db_build_query_index_clause(qp, &idx);
  if (ret < 0)
    return -1;

  if (idx && qp->filter)
    query = sqlite3_mprintf("SELECT COUNT(*), g.id, g.persistentid, f.album_artist, g.name FROM files f JOIN groups g ON f.songalbumid = g.persistentid GROUP BY f.album_artist, g.name HAVING g.type = %d AND disabled = 0 AND %s %s;", G_ALBUMS, qp->filter, idx);
  else if (idx)
    query = sqlite3_mprintf("SELECT COUNT(*), g.id, g.persistentid, f.album_artist, g.name FROM files f JOIN groups g ON f.songalbumid = g.persistentid GROUP BY f.album_artist, g.name HAVING g.type = %d AND disabled = 0 %s;", G_ALBUMS, idx);
  else if (qp->filter)
    query = sqlite3_mprintf("SELECT COUNT(*), g.id, g.persistentid, f.album_artist, g.name FROM files f JOIN groups g ON f.songalbumid = g.persistentid GROUP BY f.album_artist, g.name HAVING g.type = %d AND disabled = 0 AND %s;", G_ALBUMS, qp->filter);
  else
    query = sqlite3_mprintf("SELECT COUNT(*), g.id, g.persistentid, f.album_artist, g.name FROM files f JOIN groups g ON f.songalbumid = g.persistentid GROUP BY f.album_artist, g.name HAVING g.type = %d AND disabled = 0;", G_ALBUMS);

  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return -1;
    }

  *q = query;

  return 0;
}

static int
db_build_query_groupitems(struct query_params *qp, char **q)
{
  char *query;
  char *count;
  enum group_type gt;

  gt = db_group_type_byid(qp->id);

  switch (gt)
    {
      case G_ALBUMS:
	count = sqlite3_mprintf("SELECT COUNT(*) FROM files JOIN groups ON files.songalbumid = groups.persistentid"
				" WHERE groups.id = %d AND files.disabled = 0;", qp->id);
	break;

      default:
	DPRINTF(E_LOG, L_DB, "Unsupported group type %d for group id %d\n", gt, qp->id);
	return -1;
    }

  if (!count)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for count query string\n");

      return -1;
    }

  qp->results = db_get_count(count);
  sqlite3_free(count);

  if (qp->results < 0)
    return -1;

  switch (gt)
    {
      case G_ALBUMS:
	query = sqlite3_mprintf("SELECT files.* FROM files JOIN groups ON files.songalbumid = groups.persistentid"
				" WHERE groups.id = %d AND files.disabled = 0;", qp->id);
	break;
    }

  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return -1;
    }

  *q = query;

  return 0;
}

static int
db_build_query_group_dirs(struct query_params *qp, char **q)
{
  char *query;
  char *count;
  enum group_type gt;

  gt = db_group_type_byid(qp->id);

  switch (gt)
    {
      case G_ALBUMS:
	count = sqlite3_mprintf("SELECT COUNT(DISTINCT(SUBSTR(files.path, 1, LENGTH(files.path) - LENGTH(files.fname) - 1)))"
				" FROM files JOIN groups ON files.songalbumid = groups.persistentid"
				" WHERE groups.id = %d AND files.disabled = 0;", qp->id);
	break;

      default:
	DPRINTF(E_LOG, L_DB, "Unsupported group type %d for group id %d\n", gt, qp->id);
	return -1;
    }

  if (!count)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for count query string\n");

      return -1;
    }

  qp->results = db_get_count(count);
  sqlite3_free(count);

  if (qp->results < 0)
    return -1;

  switch (gt)
    {
      case G_ALBUMS:
	query = sqlite3_mprintf("SELECT DISTINCT(SUBSTR(files.path, 1, LENGTH(files.path) - LENGTH(files.fname) - 1))"
				" FROM files JOIN groups ON files.songalbumid = groups.persistentid"
				" WHERE groups.id = %d AND files.disabled = 0;", qp->id);
	break;
    }

  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return -1;
    }

  *q = query;

  return 0;
}

static int
db_build_query_browse(struct query_params *qp, char *field, char **q)
{
  char *query;
  char *count;
  char *idx;
  int ret;

  if (qp->filter)
    count = sqlite3_mprintf("SELECT COUNT(DISTINCT %s) FROM files WHERE data_kind = 0 AND disabled = 0 AND %s != '' AND %s;",
			    field, field, qp->filter);
  else
    count = sqlite3_mprintf("SELECT COUNT(DISTINCT %s) FROM files WHERE data_kind = 0 AND disabled = 0 AND %s != '';",
			    field, field);

  if (!count)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for count query string\n");

      return -1;
    }

  qp->results = db_get_count(count);
  sqlite3_free(count);

  if (qp->results < 0)
    return -1;

  /* Get index clause */
  ret = db_build_query_index_clause(qp, &idx);
  if (ret < 0)
    return -1;

  if (idx && qp->filter)
    query = sqlite3_mprintf("SELECT DISTINCT %s FROM files WHERE data_kind = 0 AND disabled = 0 AND %s != ''"
			    " AND %s %s;", field, field, qp->filter, idx);
  else if (idx)
    query = sqlite3_mprintf("SELECT DISTINCT %s FROM files WHERE data_kind = 0 AND disabled = 0 AND %s != ''"
			    " %s;", field, field, idx);
  else if (qp->filter)
    query = sqlite3_mprintf("SELECT DISTINCT %s FROM files WHERE data_kind = 0 AND disabled = 0 AND %s != ''"
			    " AND %s;", field, field, qp->filter);
  else
    query = sqlite3_mprintf("SELECT DISTINCT %s FROM files WHERE data_kind = 0 AND disabled = 0 AND %s != ''",
			    field, field);

  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return -1;
    }

  *q = query;

  return 0;
}

int
db_query_start(struct query_params *qp)
{
  char *query;
  int ret;

  qp->stmt = NULL;

  switch (qp->type)
    {
      case Q_ITEMS:
	ret = db_build_query_items(qp, &query);
	break;

      case Q_PL:
	ret = db_build_query_pls(qp, &query);
	break;

      case Q_PLITEMS:
	ret = db_build_query_plitems(qp, &query);
	break;

      case Q_GROUPS:
	ret = db_build_query_groups(qp, &query);
	break;

      case Q_GROUPITEMS:
	ret = db_build_query_groupitems(qp, &query);
	break;

      case Q_GROUP_DIRS:
	ret = db_build_query_group_dirs(qp, &query);
	break;

      case Q_BROWSE_ALBUMS:
	ret = db_build_query_browse(qp, "album", &query);
	break;

      case Q_BROWSE_ARTISTS:
	ret = db_build_query_browse(qp, "artist", &query);
	break;

      case Q_BROWSE_GENRES:
	ret = db_build_query_browse(qp, "genre", &query);
	break;

      case Q_BROWSE_COMPOSERS:
	ret = db_build_query_browse(qp, "composer", &query);
	break;

      default:
	DPRINTF(E_LOG, L_DB, "Unknown query type\n");
	return -1;
    }

  if (ret < 0)
    return -1;

  DPRINTF(E_DBG, L_DB, "Starting query '%s'\n", query);

  ret = sqlite3_prepare_v2(hdl, query, -1, &qp->stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      sqlite3_free(query);
      return -1;
    }

  sqlite3_free(query);

  return 0;
}

void
db_query_end(struct query_params *qp)
{
  if (!qp->stmt)
    return;

  qp->results = -1;

  sqlite3_finalize(qp->stmt);
  qp->stmt = NULL;
}

int
db_query_fetch_file(struct query_params *qp, struct db_media_file_info *dbmfi)
{
  int ncols;
  char **strcol;
  int i;
  int ret;

  memset(dbmfi, 0, sizeof(struct db_media_file_info));

  if (!qp->stmt)
    {
      DPRINTF(E_LOG, L_DB, "Query not started!\n");
      return -1;
    }

  if ((qp->type != Q_ITEMS) && (qp->type != Q_PLITEMS) && (qp->type != Q_GROUPITEMS))
    {
      DPRINTF(E_LOG, L_DB, "Not an items, playlist or group items query!\n");
      return -1;
    }

  ret = sqlite3_step(qp->stmt);
  if (ret == SQLITE_DONE)
    {
      DPRINTF(E_INFO, L_DB, "End of query results\n");
      dbmfi->id = NULL;
      return 0;
    }
  else if (ret != SQLITE_ROW)
    {
      DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));	
      return -1;
    }

  ncols = sqlite3_column_count(qp->stmt);

  if (sizeof(dbmfi_cols_map) / sizeof(dbmfi_cols_map[0]) != ncols)
    {
      DPRINTF(E_LOG, L_DB, "BUG: dbmfi column map out of sync with schema\n");
      return -1;
    }

  for (i = 0; i < ncols; i++)
    {
      strcol = (char **) ((char *)dbmfi + dbmfi_cols_map[i]);

      *strcol = (char *)sqlite3_column_text(qp->stmt, i);
    }

  return 0;
}

int
db_query_fetch_pl(struct query_params *qp, struct db_playlist_info *dbpli)
{
  int ncols;
  char **strcol;
  int id;
  int type;
  int nitems;
  int i;
  int ret;

  memset(dbpli, 0, sizeof(struct db_playlist_info));

  if (!qp->stmt)
    {
      DPRINTF(E_LOG, L_DB, "Query not started!\n");
      return -1;
    }

  if (qp->type != Q_PL)
    {
      DPRINTF(E_LOG, L_DB, "Not a playlist query!\n");
      return -1;
    }

  ret = sqlite3_step(qp->stmt);
  if (ret == SQLITE_DONE)
    {
      DPRINTF(E_INFO, L_DB, "End of query results\n");
      dbpli->id = NULL;
      return 0;
    }
  else if (ret != SQLITE_ROW)
    {
      DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));	
      return -1;
    }

  ncols = sqlite3_column_count(qp->stmt);

  if (sizeof(dbpli_cols_map) / sizeof(dbpli_cols_map[0]) != ncols)
    {
      DPRINTF(E_LOG, L_DB, "BUG: dbpli column map out of sync with schema\n");
      return -1;
    }

  for (i = 0; i < ncols; i++)
    {
      strcol = (char **) ((char *)dbpli + dbpli_cols_map[i]);

      *strcol = (char *)sqlite3_column_text(qp->stmt, i);
    }

  type = sqlite3_column_int(qp->stmt, 2);

  switch (type)
    {
      case PL_PLAIN:
	id = sqlite3_column_int(qp->stmt, 0);
	nitems = db_pl_count_items(id);
	break;

      case PL_SMART:
	nitems = db_smartpl_count_items(dbpli->query);
	break;

      default:
	DPRINTF(E_LOG, L_DB, "Unknown playlist type %d while fetching playlist\n", type);
	return -1;
    }

  dbpli->items = qp->buf;
  ret = snprintf(qp->buf, sizeof(qp->buf), "%d", nitems);
  if ((ret < 0) || (ret >= sizeof(qp->buf)))
    {
      DPRINTF(E_LOG, L_DB, "Could not convert items, buffer too small\n");

      strcpy(qp->buf, "0");
    }

  return 0;
}

int
db_query_fetch_group(struct query_params *qp, struct db_group_info *dbgri)
{
  int ncols;
  char **strcol;
  int i;
  int ret;

  memset(dbgri, 0, sizeof(struct db_group_info));

  if (!qp->stmt)
    {
      DPRINTF(E_LOG, L_DB, "Query not started!\n");
      return -1;
    }

  if (qp->type != Q_GROUPS)
    {
      DPRINTF(E_LOG, L_DB, "Not a groups query!\n");
      return -1;
    }

  ret = sqlite3_step(qp->stmt);
  if (ret == SQLITE_DONE)
    {
      DPRINTF(E_INFO, L_DB, "End of query results\n");
      return 1;
    }
  else if (ret != SQLITE_ROW)
    {
      DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));
      return -1;
    }

  ncols = sqlite3_column_count(qp->stmt);

  if (sizeof(dbgri_cols_map) / sizeof(dbgri_cols_map[0]) != ncols)
    {
      DPRINTF(E_LOG, L_DB, "BUG: dbgri column map out of sync with schema\n");
      return -1;
    }

  for (i = 0; i < ncols; i++)
    {
      strcol = (char **) ((char *)dbgri + dbgri_cols_map[i]);

      *strcol = (char *)sqlite3_column_text(qp->stmt, i);
    }

  return 0;
}

int
db_query_fetch_string(struct query_params *qp, char **string)
{
  int ret;

  *string = NULL;

  if (!qp->stmt)
    {
      DPRINTF(E_LOG, L_DB, "Query not started!\n");
      return -1;
    }

  if (!(qp->type & Q_F_BROWSE))
    {
      DPRINTF(E_LOG, L_DB, "Not a browse query!\n");
      return -1;
    }

  ret = sqlite3_step(qp->stmt);
  if (ret == SQLITE_DONE)
    {
      DPRINTF(E_INFO, L_DB, "End of query results\n");
      *string = NULL;
      return 0;
    }
  else if (ret != SQLITE_ROW)
    {
      DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));	
      return -1;
    }

  *string = (char *)sqlite3_column_text(qp->stmt, 0);

  return 0;
}


/* Files */
int
db_files_get_count(void)
{
  return db_get_count("SELECT COUNT(*) FROM files WHERE disabled = 0;");
}

void
db_files_update_songalbumid(void)
{
#define Q_SONGALBUMID "UPDATE files SET songalbumid = daap_songalbumid(album_artist, album);"
  char *errmsg;
  int ret;

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", Q_SONGALBUMID);

  errmsg = NULL;
  ret = sqlite3_exec(hdl, Q_SONGALBUMID, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    DPRINTF(E_LOG, L_DB, "Error updating songalbumid: %s\n", errmsg);

  sqlite3_free(errmsg);

#undef Q_SONGALBUMID
}

void
db_file_inc_playcount(int id)
{
#define Q_TMPL "UPDATE files SET play_count = play_count + 1, time_played = %" PRIi64 " WHERE id = %d;"
  char *query;
  char *errmsg;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, (int64_t)time(NULL), id);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  errmsg = NULL;
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    DPRINTF(E_LOG, L_DB, "Error incrementing play count on %d: %s\n", id, errmsg);

  sqlite3_free(errmsg);
  sqlite3_free(query);

#undef Q_TMPL
}

void
db_file_ping(char *path)
{
#define Q_TMPL "UPDATE files SET db_timestamp = %" PRIi64 ", disabled = 0 WHERE path = '%q';"
  char *query;
  char *errmsg;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, (int64_t)time(NULL), path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  errmsg = NULL;
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    DPRINTF(E_LOG, L_DB, "Error pinging file '%s': %s\n", path, errmsg);

  sqlite3_free(errmsg);
  sqlite3_free(query);

#undef Q_TMPL
}

char *
db_file_path_byid(int id)
{
#define Q_TMPL "SELECT path FROM files WHERE id = %d;"
  char *query;
  sqlite3_stmt *stmt;
  char *res;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, id);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return NULL;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = sqlite3_prepare_v2(hdl, query, strlen(query) + 1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      sqlite3_free(query);
      return NULL;
    }

  ret = sqlite3_step(stmt);
  if (ret != SQLITE_ROW)
    {
      if (ret == SQLITE_DONE)
	DPRINTF(E_INFO, L_DB, "No results\n");
      else
	DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));	

      sqlite3_finalize(stmt);
      sqlite3_free(query);
      return NULL;
    }

  res = (char *)sqlite3_column_text(stmt, 0);
  if (res)
    res = strdup(res);

  sqlite3_finalize(stmt);
  sqlite3_free(query);

  return res;

#undef Q_TMPL
}

static int
db_file_id_byquery(char *query)
{
  sqlite3_stmt *stmt;
  int ret;

  if (!query)
    return 0;

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = sqlite3_prepare_v2(hdl, query, strlen(query) + 1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      return 0;
    }

  ret = sqlite3_step(stmt);
  if (ret != SQLITE_ROW)
    {
      if (ret == SQLITE_DONE)
	DPRINTF(E_INFO, L_DB, "No results\n");
      else
	DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));	

      sqlite3_finalize(stmt);
      return 0;
    }

  ret = sqlite3_column_int(stmt, 0);

  sqlite3_finalize(stmt);

  return ret;
}

int
db_file_id_bypath(char *path)
{
#define Q_TMPL "SELECT id FROM files WHERE path = '%q';"
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return 0;
    }

  ret = db_file_id_byquery(query);

  sqlite3_free(query);

  return ret;

#undef Q_TMPL
}

int
db_file_id_byfilebase(char *filename, char *base)
{
#define Q_TMPL "SELECT id FROM files WHERE path LIKE '%q/%%/%q';"
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, base, filename);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return 0;
    }

  ret = db_file_id_byquery(query);

  sqlite3_free(query);

  return ret;

#undef Q_TMPL
}

int
db_file_id_byfile(char *filename)
{
#define Q_TMPL "SELECT id FROM files WHERE fname = '%q';"
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, filename);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return 0;
    }

  ret = db_file_id_byquery(query);

  sqlite3_free(query);

  return ret;

#undef Q_TMPL
}

int
db_file_id_byurl(char *url)
{
#define Q_TMPL "SELECT id FROM files WHERE url = '%q';"
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, url);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return 0;
    }

  ret = db_file_id_byquery(query);

  sqlite3_free(query);

  return ret;

#undef Q_TMPL
}

time_t
db_file_stamp_bypath(char *path)
{
#define Q_TMPL "SELECT db_timestamp FROM files WHERE path = '%q';"
  char *query;
  sqlite3_stmt *stmt;
  time_t stamp;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return 0;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = sqlite3_prepare_v2(hdl, query, strlen(query) + 1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      sqlite3_free(query);
      return 0;
    }

  ret = sqlite3_step(stmt);
  if (ret != SQLITE_ROW)
    {
      if (ret == SQLITE_DONE)
	DPRINTF(E_INFO, L_DB, "No results\n");
      else
	DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));	

      sqlite3_finalize(stmt);
      sqlite3_free(query);
      return 0;
    }

  stamp = (time_t)sqlite3_column_int64(stmt, 0);

  sqlite3_finalize(stmt);
  sqlite3_free(query);

  return stamp;

#undef Q_TMPL
}

static struct media_file_info *
db_file_fetch_byquery(char *query)
{
  struct media_file_info *mfi;
  sqlite3_stmt *stmt;
  int ncols;
  char *cval;
  uint32_t *ival;
  uint64_t *i64val;
  char **strval;
  uint64_t disabled;
  int i;
  int ret;

  if (!query)
    return NULL;

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  mfi = (struct media_file_info *)malloc(sizeof(struct media_file_info));
  if (!mfi)
    {
      DPRINTF(E_LOG, L_DB, "Could not allocate struct media_file_info, out of memory\n");
      return NULL;
    }
  memset(mfi, 0, sizeof(struct media_file_info));

  ret = sqlite3_prepare_v2(hdl, query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      free(mfi);
      return NULL;
    }

  ret = sqlite3_step(stmt);

  if (ret != SQLITE_ROW)
    {
      if (ret == SQLITE_DONE)
	DPRINTF(E_INFO, L_DB, "No results\n");
      else
	DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));

      sqlite3_finalize(stmt);
      free(mfi);
      return NULL;
    }

  ncols = sqlite3_column_count(stmt);

  if (sizeof(mfi_cols_map) / sizeof(mfi_cols_map[0]) != ncols)
    {
      DPRINTF(E_LOG, L_DB, "BUG: mfi column map out of sync with schema\n");

      sqlite3_finalize(stmt);
      /* Can't risk free()ing what's inside the mfi in this case... */
      free(mfi);
      return NULL;
    }

  for (i = 0; i < ncols; i++)
    {
      switch (mfi_cols_map[i].type)
	{
	  case DB_TYPE_CHAR:
	    cval = (char *)mfi + mfi_cols_map[i].offset;

	    *cval = sqlite3_column_int(stmt, i);
	    break;

	  case DB_TYPE_INT:
	    ival = (uint32_t *) ((char *)mfi + mfi_cols_map[i].offset);

	    if (mfi_cols_map[i].offset == mfi_offsetof(disabled))
	      {
		disabled = sqlite3_column_int64(stmt, i);
		*ival = (disabled != 0);
	      }
	    else
	      *ival = sqlite3_column_int(stmt, i);
	    break;

	  case DB_TYPE_INT64:
	    i64val = (uint64_t *) ((char *)mfi + mfi_cols_map[i].offset);

	    *i64val = sqlite3_column_int64(stmt, i);
	    break;

	  case DB_TYPE_STRING:
	    strval = (char **) ((char *)mfi + mfi_cols_map[i].offset);

	    cval = (char *)sqlite3_column_text(stmt, i);
	    if (cval)
	      *strval = strdup(cval);
	    break;

	  default:
	    DPRINTF(E_LOG, L_DB, "BUG: Unknown type %d in mfi column map\n", mfi_cols_map[i].type);

	    free_mfi(mfi, 0);
	    sqlite3_finalize(stmt);
	    return NULL;
	}
    }

  sqlite3_finalize(stmt);

  return mfi;
}

struct media_file_info *
db_file_fetch_byid(int id)
{
#define Q_TMPL "SELECT * FROM files WHERE id = %d;"
  struct media_file_info *mfi;
  char *query;

  query = sqlite3_mprintf(Q_TMPL, id);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return NULL;
    }

  mfi = db_file_fetch_byquery(query);

  sqlite3_free(query);

  return mfi;

#undef Q_TMPL
}

int
db_file_add(struct media_file_info *mfi)
{
#define Q_TMPL "INSERT INTO files (id, path, fname, title, artist, album, genre, comment, type, composer," \
               " orchestra, conductor, grouping, url, bitrate, samplerate, song_length, file_size, year, track," \
               " total_tracks, disc, total_discs, bpm, compilation, rating, play_count, data_kind, item_kind," \
               " description, time_added, time_modified, time_played, db_timestamp, disabled, sample_count," \
               " codectype, idx, has_video, contentrating, bits_per_sample, album_artist," \
               " media_kind, tv_series_name, tv_episode_num_str, tv_network_name, tv_episode_sort, tv_season_num, " \
               " songalbumid" \
               " ) " \
               " VALUES (NULL, '%q', '%q', %Q, %Q, %Q, %Q, %Q, %Q, %Q," \
               " %Q, %Q, %Q, %Q, %d, %d, %d, %" PRIi64 ", %d, %d," \
               " %d, %d, %d, %d, %d, %d, %d, %d, %d," \
               " %Q, %" PRIi64 ", %" PRIi64 ", %" PRIi64 ", %" PRIi64 ", %d, %" PRIi64 "," \
               " %Q, %d, %d, %d, %d, %Q, %d, %Q, %Q, %Q, %d, %d, daap_songalbumid(%Q, %Q));"
  char *query;
  char *errmsg;
  int ret;


  if (mfi->id != 0)
    {
      DPRINTF(E_WARN, L_DB, "Trying to update file with id > 0; use db_file_update()?\n");
      return -1;
    }

  mfi->db_timestamp = (uint64_t)time(NULL);
  mfi->time_added = mfi->db_timestamp;

  if (mfi->time_modified == 0)
    mfi->time_modified = mfi->db_timestamp;

  ensure_all_utf8(mfi);

  query = sqlite3_mprintf(Q_TMPL,
			  STR(mfi->path), STR(mfi->fname), mfi->title, mfi->artist, mfi->album,
			  mfi->genre, mfi->comment, mfi->type, mfi->composer,
			  mfi->orchestra, mfi->conductor, mfi->grouping, mfi->url, mfi->bitrate,
			  mfi->samplerate, mfi->song_length, mfi->file_size, mfi->year, mfi->track,
			  mfi->total_tracks, mfi->disc, mfi->total_discs, mfi->bpm, mfi->compilation,
			  mfi->rating, mfi->play_count, mfi->data_kind, mfi->item_kind,
			  mfi->description, (int64_t)mfi->time_added, (int64_t)mfi->time_modified,
			  (int64_t)mfi->time_played, (int64_t)mfi->db_timestamp, mfi->disabled, mfi->sample_count,
			  mfi->codectype, mfi->index, mfi->has_video,
			  mfi->contentrating, mfi->bits_per_sample, mfi->album_artist,
                          mfi->media_kind, mfi->tv_series_name, mfi->tv_episode_num_str, 
                          mfi->tv_network_name, mfi->tv_episode_sort, mfi->tv_season_num,
			  mfi->album_artist, mfi->album);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  errmsg = NULL;
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Query error: %s\n", errmsg);

      sqlite3_free(errmsg);
      sqlite3_free(query);
      return -1;
    }

  sqlite3_free(query);

  return 0;

#undef Q_TMPL
}

int
db_file_update(struct media_file_info *mfi)
{
#define Q_TMPL "UPDATE files SET path = '%q', fname = '%q', title = %Q, artist = %Q, album = %Q, genre = %Q," \
               " comment = %Q, type = %Q, composer = %Q, orchestra = %Q, conductor = %Q, grouping = %Q," \
               " url = %Q, bitrate = %d, samplerate = %d, song_length = %d, file_size = %" PRIi64 "," \
               " year = %d, track = %d, total_tracks = %d, disc = %d, total_discs = %d, bpm = %d," \
               " compilation = %d, rating = %d, data_kind = %d, item_kind = %d," \
               " description = %Q, time_modified = %" PRIi64 "," \
               " db_timestamp = %" PRIi64 ", sample_count = %d," \
               " codectype = %Q, idx = %d, has_video = %d," \
               " bits_per_sample = %d, album_artist = %Q," \
               " media_kind = %d, tv_series_name = %Q, tv_episode_num_str = %Q," \
               " tv_network_name = %Q, tv_episode_sort = %d, tv_season_num = %d," \
               " songalbumid = daap_songalbumid(%Q, %Q) " \
               " WHERE id = %d;"
  char *query;
  char *errmsg;
  int ret;

  if (mfi->id == 0)
    {
      DPRINTF(E_WARN, L_DB, "Trying to update file with id 0; use db_file_add()?\n");
      return -1;
    }

  mfi->db_timestamp = (uint64_t)time(NULL);

  if (mfi->time_modified == 0)
    mfi->time_modified = mfi->db_timestamp;

  ensure_all_utf8(mfi);

  query = sqlite3_mprintf(Q_TMPL,
			  STR(mfi->path), STR(mfi->fname), mfi->title, mfi->artist, mfi->album, mfi->genre,
			  mfi->comment, mfi->type, mfi->composer, mfi->orchestra, mfi->conductor, mfi->grouping, 
			  mfi->url, mfi->bitrate, mfi->samplerate, mfi->song_length, mfi->file_size,
			  mfi->year, mfi->track, mfi->total_tracks, mfi->disc, mfi->total_discs, mfi->bpm,
			  mfi->compilation, mfi->rating, mfi->data_kind, mfi->item_kind,
			  mfi->description, (int64_t)mfi->time_modified,
			  (int64_t)mfi->db_timestamp, mfi->sample_count,
			  mfi->codectype, mfi->index, mfi->has_video,
			  mfi->bits_per_sample, mfi->album_artist,
			  mfi->media_kind, mfi->tv_series_name, mfi->tv_episode_num_str, 
			  mfi->tv_network_name, mfi->tv_episode_sort, mfi->tv_season_num,
			  mfi->album_artist, mfi->album,
			  mfi->id);

  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  errmsg = NULL;
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Query error: %s\n", errmsg);

      sqlite3_free(errmsg);
      sqlite3_free(query);
      return -1;
    }

  sqlite3_free(query);

  return 0;

#undef Q_TMPL
}

void
db_file_delete_bypath(char *path)
{
#define Q_TMPL "DELETE FROM files WHERE path = '%q';"
  char *query;
  char *errmsg;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  errmsg = NULL;
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    DPRINTF(E_LOG, L_DB, "Error deleting file: %s\n", errmsg);

  sqlite3_free(errmsg);
  sqlite3_free(query);

#undef Q_TMPL
}

static void
db_file_disable_byquery(char *query)
{
  char *errmsg;
  int ret;

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  errmsg = NULL;
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    DPRINTF(E_LOG, L_DB, "Error disabling file: %s\n", errmsg);

  sqlite3_free(errmsg);
}

void
db_file_disable_bypath(char *path, char *strip, uint32_t cookie)
{
#define Q_TMPL "UPDATE files SET path = substr(path, %d), disabled = %" PRIi64 " WHERE path = '%q';"
  char *query;
  int64_t disabled;
  int striplen;

  disabled = (cookie != 0) ? cookie : INOTIFY_FAKE_COOKIE;
  striplen = strlen(strip) + 1;

  query = sqlite3_mprintf(Q_TMPL, striplen, disabled, path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return;
    }

  db_file_disable_byquery(query);

  sqlite3_free(query);

#undef Q_TMPL
}

void
db_file_disable_bymatch(char *path, char *strip, uint32_t cookie)
{
#define Q_TMPL "UPDATE files SET path = substr(path, %d), disabled = %" PRIi64 " WHERE path LIKE '%q/%%';"
  char *query;
  int64_t disabled;
  int striplen;

  disabled = (cookie != 0) ? cookie : INOTIFY_FAKE_COOKIE;
  striplen = strlen(strip) + 1;

  query = sqlite3_mprintf(Q_TMPL, striplen, disabled, path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return;
    }

  db_file_disable_byquery(query);

  sqlite3_free(query);

#undef Q_TMPL
}

int
db_file_enable_bycookie(uint32_t cookie, char *path)
{
#define Q_TMPL "UPDATE files SET path = '%q' || path, disabled = 0 WHERE disabled = %" PRIi64 ";"
  char *query;
  char *errmsg;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, path, (int64_t)cookie);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  errmsg = NULL;
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Error enabling files: %s\n", errmsg);

      sqlite3_free(errmsg);
      sqlite3_free(query);
      return -1;
    }

  sqlite3_free(query);

  return sqlite3_changes(hdl);

#undef Q_TMPL
}


/* Playlists */
int
db_pl_get_count(void)
{
  return db_get_count("SELECT COUNT(*) FROM playlists WHERE disabled = 0;");
}

static int
db_pl_count_items(int id)
{
#define Q_TMPL "SELECT COUNT(*) FROM playlistitems JOIN files" \
               " ON playlistitems.filepath = files.path WHERE files.disabled = 0 AND playlistitems.playlistid = %d;"
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, id);

  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return 0;
    }

  ret = db_get_count(query);

  sqlite3_free(query);

  return ret;

#undef Q_TMPL
}

static int
db_smartpl_count_items(const char *smartpl_query)
{
#define Q_TMPL "SELECT COUNT(*) FROM files WHERE disabled = 0 AND %s;"
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, smartpl_query);

  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return 0;
    }

  ret = db_get_count(query);

  sqlite3_free(query);

  return ret;

#undef Q_TMPL
}

void
db_pl_ping(int id)
{
#define Q_TMPL "UPDATE playlists SET db_timestamp = %" PRIi64 ", disabled = 0 WHERE id = %d;"
  char *query;
  char *errmsg;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, (int64_t)time(NULL), id);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  errmsg = NULL;
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    DPRINTF(E_LOG, L_DB, "Error pinging playlist %d: %s\n", id, errmsg);

  sqlite3_free(errmsg);
  sqlite3_free(query);

#undef Q_TMPL
}

static int
db_pl_id_bypath(char *path, int *id)
{
#define Q_TMPL "SELECT id FROM playlists WHERE path = '%q';"
  char *query;
  sqlite3_stmt *stmt;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = sqlite3_prepare_v2(hdl, query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      sqlite3_free(query);
      return -1;
    }

  ret = sqlite3_step(stmt);
  if (ret != SQLITE_ROW)
    {
      if (ret == SQLITE_DONE)
	DPRINTF(E_INFO, L_DB, "No results\n");
      else
	DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));	

      sqlite3_finalize(stmt);
      sqlite3_free(query);
      return -1;
    }

  *id = sqlite3_column_int(stmt, 0);

  sqlite3_finalize(stmt);
  sqlite3_free(query);

  return 0;

#undef Q_TMPL
}

static struct playlist_info *
db_pl_fetch_byquery(char *query)
{
  struct playlist_info *pli;
  sqlite3_stmt *stmt;
  int ncols;
  char *cval;
  uint32_t *ival;
  char **strval;
  uint64_t disabled;
  int i;
  int ret;

  if (!query)
    return NULL;

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  pli = (struct playlist_info *)malloc(sizeof(struct playlist_info));
  if (!pli)
    {
      DPRINTF(E_LOG, L_DB, "Could not allocate struct playlist_info, out of memory\n");
      return NULL;
    }
  memset(pli, 0, sizeof(struct playlist_info));

  ret = sqlite3_prepare_v2(hdl, query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      free(pli);
      return NULL;
    }

  ret = sqlite3_step(stmt);
  if (ret != SQLITE_ROW)
    {
      if (ret == SQLITE_DONE)
	DPRINTF(E_INFO, L_DB, "No results\n");
      else
	DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));

      sqlite3_finalize(stmt);
      free(pli);
      return NULL;
    }

  ncols = sqlite3_column_count(stmt);

  if (sizeof(pli_cols_map) / sizeof(pli_cols_map[0]) != ncols)
    {
      DPRINTF(E_LOG, L_DB, "BUG: pli column map out of sync with schema\n");

      sqlite3_finalize(stmt);
      /* Can't risk free()ing what's inside the pli in this case... */
      free(pli);
      return NULL;
    }

  for (i = 0; i < ncols; i++)
    {
      switch (pli_cols_map[i].type)
	{
	  case DB_TYPE_INT:
	    ival = (uint32_t *) ((char *)pli + pli_cols_map[i].offset);

	    if (pli_cols_map[i].offset == pli_offsetof(disabled))
	      {
		disabled = sqlite3_column_int64(stmt, i);
		*ival = (disabled != 0);
	      }
	    else
	      *ival = sqlite3_column_int(stmt, i);
	    break;

	  case DB_TYPE_STRING:
	    strval = (char **) ((char *)pli + pli_cols_map[i].offset);

	    cval = (char *)sqlite3_column_text(stmt, i);
	    if (cval)
	      *strval = strdup(cval);
	    break;

	  default:
	    DPRINTF(E_LOG, L_DB, "BUG: Unknown type %d in pli column map\n", pli_cols_map[i].type);

	    sqlite3_finalize(stmt);
	    free_pli(pli, 0);
	    return NULL;
	}
    }

  ret = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (ret != SQLITE_DONE)
    {
      DPRINTF(E_WARN, L_DB, "Query had more than a single result!\n");

      free_pli(pli, 0);
      return NULL;
    }

  switch (pli->type)
    {
      case PL_PLAIN:
	pli->items = db_pl_count_items(pli->id);
	break;

      case PL_SMART:
	pli->items = db_smartpl_count_items(pli->query);
	break;

      default:
	DPRINTF(E_LOG, L_DB, "Unknown playlist type %d while fetching playlist\n", pli->type);

	free_pli(pli, 0);
	return NULL;
    }

  return pli;
}

struct playlist_info *
db_pl_fetch_bypath(char *path)
{
#define Q_TMPL "SELECT * FROM playlists WHERE path = '%q';"
  struct playlist_info *pli;
  char *query;

  query = sqlite3_mprintf(Q_TMPL, path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return NULL;
    }

  pli = db_pl_fetch_byquery(query);

  sqlite3_free(query);

  return pli;

#undef Q_TMPL
}

struct playlist_info *
db_pl_fetch_byid(int id)
{
#define Q_TMPL "SELECT * FROM playlists WHERE id = %d;"
  struct playlist_info *pli;
  char *query;

  query = sqlite3_mprintf(Q_TMPL, id);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return NULL;
    }

  pli = db_pl_fetch_byquery(query);

  sqlite3_free(query);

  return pli;

#undef Q_TMPL
}

struct playlist_info *
db_pl_fetch_bytitlepath(char *title, char *path)
{
#define Q_TMPL "SELECT * FROM playlists WHERE title = '%q' AND path = '%q';"
  struct playlist_info *pli;
  char *query;

  query = sqlite3_mprintf(Q_TMPL, title, path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return NULL;
    }

  pli = db_pl_fetch_byquery(query);

  sqlite3_free(query);

  return pli;

#undef Q_TMPL
}

int
db_pl_add(char *title, char *path, int *id)
{
#define QDUP_TMPL "SELECT COUNT(*) FROM playlists WHERE title = '%q' AND path = '%q';"
#define QADD_TMPL "INSERT INTO playlists (title, type, query, db_timestamp, disabled, path, idx, special_id)" \
                  " VALUES ('%q', 0, NULL, %" PRIi64 ", 0, '%q', 0, 0);"
  char *query;
  char *errmsg;
  int ret;

  /* Check duplicates */
  query = sqlite3_mprintf(QDUP_TMPL, title, path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return -1;
    }

  ret = db_get_count(query);

  sqlite3_free(query);

  if (ret > 0)
    {
      DPRINTF(E_WARN, L_DB, "Duplicate playlist with title '%s' path '%s'\n", title, path);
      return -1;
    }

  /* Add */
  query = sqlite3_mprintf(QADD_TMPL, title, (int64_t)time(NULL), path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  errmsg = NULL;
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Query error: %s\n", errmsg);

      sqlite3_free(errmsg);
      sqlite3_free(query);
      return -1;
    }

  sqlite3_free(query);

  *id = (int)sqlite3_last_insert_rowid(hdl);
  if (*id == 0)
    {
      DPRINTF(E_LOG, L_DB, "Successful insert but no last_insert_rowid!\n");
      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Added playlist %s (path %s) with id %d\n", title, path, *id);

  return 0;

#undef QDUP_TMPL
#undef QADD_TMPL
}

int
db_pl_add_item_bypath(int plid, char *path)
{
#define Q_TMPL "INSERT INTO playlistitems (playlistid, filepath) VALUES (%d, '%q');"
  char *query;
  char *errmsg;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, plid, path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  errmsg = NULL;
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Query error: %s\n", errmsg);

      sqlite3_free(errmsg);
      sqlite3_free(query);
      return -1;
    }

  sqlite3_free(query);

  return 0;

#undef Q_TMPL
}

int
db_pl_add_item_byid(int plid, int fileid)
{
#define Q_TMPL "INSERT INTO playlistitems (playlistid, filepath) VALUES (%d, (SELECT path FROM files WHERE id = %d));"
  char *query;
  char *errmsg;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, plid, fileid);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  errmsg = NULL;
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Query error: %s\n", errmsg);

      sqlite3_free(errmsg);
      sqlite3_free(query);
      return -1;
    }

  sqlite3_free(query);

  return 0;

#undef Q_TMPL
}

void
db_pl_clear_items(int id)
{
#define Q_TMPL "DELETE FROM playlistitems WHERE playlistid = %d;"
  char *query;
  char *errmsg;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, id);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  errmsg = NULL;
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    DPRINTF(E_LOG, L_DB, "Error clearing playlist %d items: %s\n", id, errmsg);

  sqlite3_free(errmsg);
  sqlite3_free(query);

#undef Q_TMPL
}

void
db_pl_delete(int id)
{
#define Q_TMPL "DELETE FROM playlists WHERE id = %d;"
  char *query;
  char *errmsg;
  int ret;

  if (id == 1)
    return;

  query = sqlite3_mprintf(Q_TMPL, id);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  errmsg = NULL;
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    DPRINTF(E_LOG, L_DB, "Error deleting playlist %d: %s\n", id, errmsg);

  sqlite3_free(errmsg);
  sqlite3_free(query);

  db_pl_clear_items(id);

#undef Q_TMPL
}

void
db_pl_delete_bypath(char *path)
{
  int id;
  int ret;

  ret = db_pl_id_bypath(path, &id);
  if (ret < 0)
    return;

  db_pl_delete(id);
}

static void
db_pl_disable_byquery(char *query)
{
  char *errmsg;
  int ret;

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  errmsg = NULL;
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    DPRINTF(E_LOG, L_DB, "Error disabling playlist: %s\n", errmsg);

  sqlite3_free(errmsg);
}

void
db_pl_disable_bypath(char *path, char *strip, uint32_t cookie)
{
#define Q_TMPL "UPDATE playlists SET path = substr(path, %d), disabled = %" PRIi64 " WHERE path = '%q';"
  char *query;
  int64_t disabled;
  int striplen;

  disabled = (cookie != 0) ? cookie : INOTIFY_FAKE_COOKIE;
  striplen = strlen(strip) + 1;

  query = sqlite3_mprintf(Q_TMPL, striplen, disabled, path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return;
    }

  db_pl_disable_byquery(query);

  sqlite3_free(query);

#undef Q_TMPL
}

void
db_pl_disable_bymatch(char *path, char *strip, uint32_t cookie)
{
#define Q_TMPL "UPDATE playlists SET path = substr(path, %d), disabled = %" PRIi64 " WHERE path LIKE '%q/%%';"
  char *query;
  int64_t disabled;
  int striplen;

  disabled = (cookie != 0) ? cookie : INOTIFY_FAKE_COOKIE;
  striplen = strlen(strip) + 1;

  query = sqlite3_mprintf(Q_TMPL, striplen, disabled, path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return;
    }

  db_pl_disable_byquery(query);

  sqlite3_free(query);

#undef Q_TMPL
}

int
db_pl_enable_bycookie(uint32_t cookie, char *path)
{
#define Q_TMPL "UPDATE playlists SET path = '%q' || path, disabled = 0 WHERE disabled = %" PRIi64 ";"
  char *query;
  char *errmsg;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, path, (int64_t)cookie);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  errmsg = NULL;
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Error enabling playlists: %s\n", errmsg);

      sqlite3_free(errmsg);
      sqlite3_free(query);
      return -1;
    }

  sqlite3_free(query);

  return sqlite3_changes(hdl);

#undef Q_TMPL
}


/* Groups */
int
db_groups_clear(void)
{
  char *query = "DELETE FROM groups;";
  char *errmsg;
  int ret;

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  errmsg = NULL;
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Query error: %s\n", errmsg);

      sqlite3_free(errmsg);
      return -1;
    }

  return 0;
}

enum group_type
db_group_type_byid(int id)
{
#define Q_TMPL "SELECT type FROM groups WHERE id = '%d';"
  char *query;
  sqlite3_stmt *stmt;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, id);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return 0;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = sqlite3_prepare_v2(hdl, query, strlen(query) + 1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      sqlite3_free(query);
      return 0;
    }

  ret = sqlite3_step(stmt);
  if (ret != SQLITE_ROW)
    {
      if (ret == SQLITE_DONE)
	DPRINTF(E_INFO, L_DB, "No results\n");
      else
	DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));

      sqlite3_finalize(stmt);
      sqlite3_free(query);
      return 0;
    }

  ret = sqlite3_column_int(stmt, 0);

  sqlite3_finalize(stmt);
  sqlite3_free(query);

  return ret;

#undef Q_TMPL
}

/* Remotes */
static int
db_pairing_delete_byremote(char *remote_id)
{
#define Q_TMPL "DELETE FROM pairings WHERE remote = '%q';"
  char *query;
  char *errmsg;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, remote_id);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  errmsg = NULL;
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Error deleting pairing: %s\n", errmsg);

      sqlite3_free(errmsg);
      sqlite3_free(query);
      return -1;
    }

  sqlite3_free(query);

  return 0;

#undef Q_TMPL
}

int
db_pairing_add(struct pairing_info *pi)
{
#define Q_TMPL "INSERT INTO pairings (remote, name, guid) VALUES ('%q', '%q', '%q');"
  char *query;
  char *errmsg;
  int ret;

  ret = db_pairing_delete_byremote(pi->remote_id);
  if (ret < 0)
    return ret;

  query = sqlite3_mprintf(Q_TMPL, pi->remote_id, pi->name, pi->guid);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  errmsg = NULL;
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Error adding pairing: %s\n", errmsg);

      sqlite3_free(errmsg);
      sqlite3_free(query);
      return -1;
    }

  sqlite3_free(query);

  return 0;

#undef Q_TMPL
}

int
db_pairing_fetch_byguid(struct pairing_info *pi)
{
#define Q_TMPL "SELECT * FROM pairings WHERE guid = '%q';"
  char *query;
  sqlite3_stmt *stmt;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, pi->guid);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = sqlite3_prepare_v2(hdl, query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));
      return -1;
    }

  ret = sqlite3_step(stmt);
  if (ret != SQLITE_ROW)
    {
      if (ret == SQLITE_DONE)
	DPRINTF(E_INFO, L_DB, "Pairing GUID %s not found\n", pi->guid);
      else
	DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));

      sqlite3_finalize(stmt);
      sqlite3_free(query);
      return -1;
    }

  pi->remote_id = strdup((char *)sqlite3_column_text(stmt, 0));
  pi->name = strdup((char *)sqlite3_column_text(stmt, 1));

  sqlite3_finalize(stmt);
  sqlite3_free(query);

  return 0;

#undef Q_TMPL
}


/* Inotify */
int
db_watch_clear(void)
{
  char *query = "DELETE FROM inotify;";
  char *errmsg;
  int ret;

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  errmsg = NULL;
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Query error: %s\n", errmsg);

      sqlite3_free(errmsg);
      return -1;
    }

  return 0;
}

int
db_watch_add(struct watch_info *wi)
{
#define Q_TMPL "INSERT INTO inotify (wd, cookie, path) VALUES (%d, 0, '%q');"
  char *query;
  char *errmsg;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, wi->wd, wi->path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  errmsg = NULL;
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Error adding watch: %s\n", errmsg);

      sqlite3_free(errmsg);
      sqlite3_free(query);
      return -1;
    }

  sqlite3_free(query);

  return 0;

#undef Q_TMPL
}

static int
db_watch_delete_byquery(char *query)
{
  char *errmsg;
  int ret;

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  errmsg = NULL;
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Error deleting watch: %s\n", errmsg);

      sqlite3_free(errmsg);
      sqlite3_free(query);
      return -1;
    }

  return 0;
}

int
db_watch_delete_bywd(uint32_t wd)
{
#define Q_TMPL "DELETE FROM inotify WHERE wd = %d;"
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, wd);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return -1;
    }

  ret = db_watch_delete_byquery(query);

  sqlite3_free(query);

  return ret;

#undef Q_TMPL
}

int
db_watch_delete_bypath(char *path)
{
#define Q_TMPL "DELETE FROM inotify WHERE path = '%q';"
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return -1;
    }

  ret = db_watch_delete_byquery(query);

  sqlite3_free(query);

  return ret;

#undef Q_TMPL
}

int
db_watch_delete_bymatch(char *path)
{
#define Q_TMPL "DELETE FROM inotify WHERE path LIKE '%q/%%';"
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return -1;
    }

  ret = db_watch_delete_byquery(query);

  sqlite3_free(query);

  return ret;

#undef Q_TMPL
}

int
db_watch_delete_bycookie(uint32_t cookie)
{
#define Q_TMPL "DELETE FROM inotify WHERE cookie = %" PRIi64 ";"
  char *query;
  int ret;

  if (cookie == 0)
    return -1;

  query = sqlite3_mprintf(Q_TMPL, (int64_t)cookie);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return -1;
    }

  ret = db_watch_delete_byquery(query);

  sqlite3_free(query);

  return ret;

#undef Q_TMPL
}

int
db_watch_get_bywd(struct watch_info *wi)
{
#define Q_TMPL "SELECT * FROM inotify WHERE wd = %d;"
  char *query;
  sqlite3_stmt *stmt;
  char **strval;
  char *cval;
  uint32_t *ival;
  int64_t cookie;
  int ncols;
  int i;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, wi->wd);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = sqlite3_prepare_v2(hdl, query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));
      return -1;
    }

  ret = sqlite3_step(stmt);
  if (ret != SQLITE_ROW)
    {
      DPRINTF(E_LOG, L_DB, "Watch wd %d not found\n", wi->wd);

      sqlite3_finalize(stmt);
      sqlite3_free(query);
      return -1;
    }

  ncols = sqlite3_column_count(stmt);

  if (sizeof(wi_cols_map) / sizeof(wi_cols_map[0]) != ncols)
    {
      DPRINTF(E_LOG, L_DB, "BUG: wi column map out of sync with schema\n");

      sqlite3_finalize(stmt);
      sqlite3_free(query);
      return -1;
    }

  for (i = 0; i < ncols; i++)
    {
      switch (wi_cols_map[i].type)
	{
	  case DB_TYPE_INT:
	    ival = (uint32_t *) ((char *)wi + wi_cols_map[i].offset);

	    if (wi_cols_map[i].offset == wi_offsetof(cookie))
	      {
		cookie = sqlite3_column_int64(stmt, i);
		*ival = (cookie == INOTIFY_FAKE_COOKIE) ? 0 : cookie;
	      }
	    else
	      *ival = sqlite3_column_int(stmt, i);
	    break;

	  case DB_TYPE_STRING:
	    strval = (char **) ((char *)wi + wi_cols_map[i].offset);

	    cval = (char *)sqlite3_column_text(stmt, i);
	    if (cval)
	      *strval = strdup(cval);
	    break;

	  default:
	    DPRINTF(E_LOG, L_DB, "BUG: Unknown type %d in wi column map\n", wi_cols_map[i].type);
	    sqlite3_finalize(stmt);
	    sqlite3_free(query);
	    return -1;
	}
    }

  sqlite3_finalize(stmt);
  sqlite3_free(query);

  return 0;

#undef Q_TMPL
}

static void
db_watch_mark_byquery(char *query)
{
  char *errmsg;
  int ret;

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  errmsg = NULL;
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    DPRINTF(E_LOG, L_DB, "Error marking watch: %s\n", errmsg);

  sqlite3_free(errmsg);
}

void
db_watch_mark_bypath(char *path, char *strip, uint32_t cookie)
{
#define Q_TMPL "UPDATE inotify SET path = substr(path, %d), cookie = %" PRIi64 " WHERE path = '%q';"
  char *query;
  int64_t disabled;
  int striplen;

  disabled = (cookie != 0) ? cookie : INOTIFY_FAKE_COOKIE;
  striplen = strlen(strip) + 1;

  query = sqlite3_mprintf(Q_TMPL, striplen, disabled, path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return;
    }

  db_watch_mark_byquery(query);

  sqlite3_free(query);

#undef Q_TMPL
}

void
db_watch_mark_bymatch(char *path, char *strip, uint32_t cookie)
{
#define Q_TMPL "UPDATE inotify SET path = substr(path, %d), cookie = %" PRIi64 " WHERE path LIKE '%q/%%';"
  char *query;
  int64_t disabled;
  int striplen;

  disabled = (cookie != 0) ? cookie : INOTIFY_FAKE_COOKIE;
  striplen = strlen(strip) + 1;

  query = sqlite3_mprintf(Q_TMPL, striplen, disabled, path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return;
    }

  db_watch_mark_byquery(query);

  sqlite3_free(query);

#undef Q_TMPL
}

void
db_watch_move_bycookie(uint32_t cookie, char *path)
{
#define Q_TMPL "UPDATE inotify SET path = '%q' || path, cookie = 0 WHERE cookie = %" PRIi64 ";"
  char *query;
  char *errmsg;
  int ret;

  if (cookie == 0)
    return;

  query = sqlite3_mprintf(Q_TMPL, path, (int64_t)cookie);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  errmsg = NULL;
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    DPRINTF(E_LOG, L_DB, "Error moving watch: %s\n", errmsg);

  sqlite3_free(errmsg);
  sqlite3_free(query);

#undef Q_TMPL
}

int
db_watch_cookie_known(uint32_t cookie)
{
#define Q_TMPL "SELECT COUNT(*) FROM inotify WHERE cookie = %" PRIi64 ";"
  char *query;
  int ret;

  if (cookie == 0)
    return 0;

  query = sqlite3_mprintf(Q_TMPL, (int64_t)cookie);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return 0;
    }

  ret = db_get_count(query);

  sqlite3_free(query);

  return (ret > 0);

#undef Q_TMPL
}

int
db_watch_enum_start(struct watch_enum *we)
{
#define Q_MATCH_TMPL "SELECT wd FROM inotify WHERE path LIKE '%q/%%';"
#define Q_COOKIE_TMPL "SELECT wd FROM inotify WHERE cookie = %" PRIi64 ";"
  char *query;
  int ret;

  we->stmt = NULL;

  if (we->match)
    query = sqlite3_mprintf(Q_MATCH_TMPL, we->match);
  else if (we->cookie != 0)
    query = sqlite3_mprintf(Q_COOKIE_TMPL, we->cookie);
  else
    {
      DPRINTF(E_LOG, L_DB, "Could not start enum, no parameter given\n");
      return -1;
    }

  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Starting enum '%s'\n", query);

  ret = sqlite3_prepare_v2(hdl, query, -1, &we->stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      sqlite3_free(query);
      return -1;
    }

  sqlite3_free(query);

  return 0;

#undef Q_MATCH_TMPL
#undef Q_COOKIE_TMPL
}

void
db_watch_enum_end(struct watch_enum *we)
{
  if (!we->stmt)
    return;

  sqlite3_finalize(we->stmt);
  we->stmt = NULL;
}

int
db_watch_enum_fetchwd(struct watch_enum *we, uint32_t *wd)
{
  int ret;

  *wd = 0;

  if (!we->stmt)
    {
      DPRINTF(E_LOG, L_DB, "Watch enum not started!\n");
      return -1;
    }

  ret = sqlite3_step(we->stmt);
  if (ret == SQLITE_DONE)
    {
      DPRINTF(E_INFO, L_DB, "End of watch enum results\n");
      return 0;
    }
  else if (ret != SQLITE_ROW)
    {
      DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));	
      return -1;
    }

  *wd = (uint32_t)sqlite3_column_int(we->stmt, 0);

  return 0;
}


static void
db_daap_songalbumid_xfunc(sqlite3_context *pv, int n, sqlite3_value **ppv)
{
  const char *album_artist;
  const char *album;
  char *hashbuf;
  sqlite3_int64 result;

  if (n != 2)
    {
      sqlite3_result_error(pv, "daap_songalbumid() requires 2 parameters, album_artist and album", -1);
      return;
    }

  if ((sqlite3_value_type(ppv[0]) != SQLITE_TEXT)
      || (sqlite3_value_type(ppv[1]) != SQLITE_TEXT))
    {
      sqlite3_result_error(pv, "daap_songalbumid() requires 2 text parameters", -1);
      return;
    }

  album_artist = (const char *)sqlite3_value_text(ppv[0]);
  album = (const char *)sqlite3_value_text(ppv[1]);

  hashbuf = sqlite3_mprintf("%s==%s", (album_artist) ? album_artist : "", (album) ? album : "");
  if (!hashbuf)
    {
      sqlite3_result_error(pv, "daap_songalbumid() out of memory for hashbuf", -1);
      return;
    }

  /* Limit hash length to 63 bits, due to signed type in sqlite */
  result = murmur_hash64(hashbuf, strlen(hashbuf), 0) >> 1;

  sqlite3_free(hashbuf);

  sqlite3_result_int64(pv, result);
}


int
db_perthread_init(void)
{
  int ret;

  ret = sqlite3_open(db_path, &hdl);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not open database: %s\n", sqlite3_errmsg(hdl));

      sqlite3_close(hdl);
      return -1;
    }

  ret = sqlite3_create_function(hdl, "daap_songalbumid", 2, SQLITE_UTF8, NULL, db_daap_songalbumid_xfunc, NULL, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not create daap_songalbumid function: %s\n", sqlite3_errmsg(hdl));

      sqlite3_close(hdl);
      return -1;
    }

  return 0;
}

void
db_perthread_deinit(void)
{
  sqlite3_stmt *stmt;

  /* Tear down anything that's in flight */
  while ((stmt = sqlite3_next_stmt(hdl, 0)))
    sqlite3_finalize(stmt);

  sqlite3_close(hdl);
}


#define T_ADMIN					\
  "CREATE TABLE IF NOT EXISTS admin("		\
  "   key   VARCHAR(32) NOT NULL,"		\
  "   value VARCHAR(32) NOT NULL"		\
  ");"

#define T_FILES						\
  "CREATE TABLE IF NOT EXISTS files ("			\
  "   id                 INTEGER PRIMARY KEY NOT NULL,"	\
  "   path               VARCHAR(4096) NOT NULL,"	\
  "   fname              VARCHAR(255) NOT NULL,"	\
  "   title              VARCHAR(1024) DEFAULT NULL,"	\
  "   artist             VARCHAR(1024) DEFAULT NULL,"	\
  "   album              VARCHAR(1024) NOT NULL,"	\
  "   genre              VARCHAR(255) DEFAULT NULL,"	\
  "   comment            VARCHAR(4096) DEFAULT NULL,"	\
  "   type               VARCHAR(255) DEFAULT NULL,"	\
  "   composer           VARCHAR(1024) DEFAULT NULL,"	\
  "   orchestra          VARCHAR(1024) DEFAULT NULL,"	\
  "   conductor          VARCHAR(1024) DEFAULT NULL,"	\
  "   grouping           VARCHAR(1024) DEFAULT NULL,"	\
  "   url                VARCHAR(1024) DEFAULT NULL,"	\
  "   bitrate            INTEGER DEFAULT 0,"		\
  "   samplerate         INTEGER DEFAULT 0,"		\
  "   song_length        INTEGER DEFAULT 0,"		\
  "   file_size          INTEGER DEFAULT 0,"		\
  "   year               INTEGER DEFAULT 0,"		\
  "   track              INTEGER DEFAULT 0,"		\
  "   total_tracks       INTEGER DEFAULT 0,"		\
  "   disc               INTEGER DEFAULT 0,"		\
  "   total_discs        INTEGER DEFAULT 0,"		\
  "   bpm                INTEGER DEFAULT 0,"		\
  "   compilation        INTEGER DEFAULT 0,"		\
  "   rating             INTEGER DEFAULT 0,"		\
  "   play_count         INTEGER DEFAULT 0,"		\
  "   data_kind          INTEGER DEFAULT 0,"		\
  "   item_kind          INTEGER DEFAULT 0,"		\
  "   description        INTEGER DEFAULT 0,"		\
  "   time_added         INTEGER DEFAULT 0,"		\
  "   time_modified      INTEGER DEFAULT 0,"		\
  "   time_played        INTEGER DEFAULT 0,"		\
  "   db_timestamp       INTEGER DEFAULT 0,"		\
  "   disabled           INTEGER DEFAULT 0,"		\
  "   sample_count       INTEGER DEFAULT 0,"		\
  "   codectype          VARCHAR(5) DEFAULT NULL,"	\
  "   idx                INTEGER NOT NULL,"		\
  "   has_video          INTEGER DEFAULT 0,"		\
  "   contentrating      INTEGER DEFAULT 0,"		\
  "   bits_per_sample    INTEGER DEFAULT 0,"		\
  "   album_artist       VARCHAR(1024) NOT NULL,"	\
  "   media_kind         INTEGER NOT NULL,"		\
  "   tv_series_name     VARCHAR(1024) DEFAULT NULL,"	\
  "   tv_episode_num_str VARCHAR(1024) DEFAULT NULL,"	\
  "   tv_network_name    VARCHAR(1024) DEFAULT NULL,"	\
  "   tv_episode_sort    INTEGER NOT NULL,"		\
  "   tv_season_num      INTEGER NOT NULL,"		\
  "   songalbumid        INTEGER NOT NULL"		\
  ");"

#define T_PL					\
  "CREATE TABLE IF NOT EXISTS playlists ("		\
  "   id             INTEGER PRIMARY KEY NOT NULL,"	\
  "   title          VARCHAR(255) NOT NULL,"		\
  "   type           INTEGER NOT NULL,"			\
  "   query          VARCHAR(1024),"			\
  "   db_timestamp   INTEGER NOT NULL,"			\
  "   disabled       INTEGER DEFAULT 0,"		\
  "   path           VARCHAR(4096),"			\
  "   idx            INTEGER NOT NULL,"			\
  "   special_id     INTEGER DEFAULT 0"			\
  ");"

#define T_PLITEMS				\
  "CREATE TABLE IF NOT EXISTS playlistitems ("		\
  "   id             INTEGER PRIMARY KEY NOT NULL,"	\
  "   playlistid     INTEGER NOT NULL,"			\
  "   filepath       VARCHAR(4096) NOT NULL"		\
  ");"

#define T_GROUPS							\
  "CREATE TABLE IF NOT EXISTS groups ("					\
  "   id             INTEGER PRIMARY KEY NOT NULL,"			\
  "   type           INTEGER NOT NULL,"					\
  "   name           VARCHAR(1024) NOT NULL,"				\
  "   persistentid   INTEGER NOT NULL,"					\
  "CONSTRAINT groups_type_unique_persistentid UNIQUE (type, persistentid)" \
  ");"

#define T_PAIRINGS					\
  "CREATE TABLE IF NOT EXISTS pairings("		\
  "   remote         VARCHAR(64) PRIMARY KEY NOT NULL,"	\
  "   name           VARCHAR(255) NOT NULL,"		\
  "   guid           VARCHAR(16) NOT NULL"		\
  ");"

#define T_INOTIFY					\
  "CREATE TABLE IF NOT EXISTS inotify ("		\
  "   wd          INTEGER PRIMARY KEY NOT NULL,"	\
  "   cookie      INTEGER NOT NULL,"			\
  "   path        VARCHAR(4096) NOT NULL"		\
  ");"

#define I_PATH							\
  "CREATE INDEX IF NOT EXISTS idx_path ON files(path, idx);"

#define I_FILEPATH							\
  "CREATE INDEX IF NOT EXISTS idx_filepath ON playlistitems(filepath ASC);"

#define I_PLITEMID							\
  "CREATE INDEX IF NOT EXISTS idx_playlistid ON playlistitems(playlistid, filepath);"

#define I_PAIRING				\
  "CREATE INDEX IF NOT EXISTS idx_pairingguid ON pairings(guid);"

#define TRG_GROUPS_INSERT_FILES						\
  "CREATE TRIGGER update_groups_new_file AFTER INSERT ON files FOR EACH ROW" \
  " BEGIN"								\
  "   INSERT OR IGNORE INTO groups (type, name, persistentid) VALUES (1, NEW.album, NEW.songalbumid);" \
  " END;"

#define TRG_GROUPS_UPDATE_FILES						\
  "CREATE TRIGGER update_groups_update_file AFTER UPDATE OF songalbumid ON files FOR EACH ROW" \
  " BEGIN"								\
  "   INSERT OR IGNORE INTO groups (type, name, persistentid) VALUES (1, NEW.album, NEW.songalbumid);" \
  " END;"

#define Q_PL1								\
  "INSERT INTO playlists (id, title, type, query, db_timestamp, path, idx, special_id)" \
  " VALUES(1, 'Library', 1, '1 = 1', 0, '', 0, 0);"

#define Q_PL2								\
  "INSERT INTO playlists (id, title, type, query, db_timestamp, path, idx, special_id)" \
  " VALUES(2, 'Music', 1, 'media_kind = 1', 0, '', 0, 6);"

#define Q_PL3								\
  "INSERT INTO playlists (id, title, type, query, db_timestamp, path, idx, special_id)" \
  " VALUES(3, 'Movies', 1, 'media_kind = 2', 0, '', 0, 4);"

#define Q_PL4								\
  "INSERT INTO playlists (id, title, type, query, db_timestamp, path, idx, special_id)" \
  " VALUES(4, 'TV Shows', 1, 'media_kind = 64', 0, '', 0, 5);"

/* These are the remaining automatically-created iTunes playlists, but
 * their query is unknown
  " VALUES(5, 'Podcasts', 0, 'media_kind = 128 ', 0, '', 0, 1);"
  " VALUES(6, 'iTunes U', 0, 'media_kind = 256', 0, '', 0, 13);"
  " VALUES(7, 'Audiobooks', 0, 'media_kind = 512', 0, '', 0, 7);"
  " VALUES(8, 'Purchased', 0, 'media_kind = 1024', 0, '', 0, 8);"
 */


#define SCHEMA_VERSION 9
#define Q_SCVER					\
  "INSERT INTO admin (key, value) VALUES ('schema_version', '9');"

struct db_init_query {
  char *query;
  char *desc;
};

static const struct db_init_query db_init_queries[] =
  {
    { T_ADMIN,     "create table admin" },
    { T_FILES,     "create table files" },
    { T_PL,        "create table playlists" },
    { T_PLITEMS,   "create table playlistitems" },
    { T_GROUPS,    "create table groups" },
    { T_PAIRINGS,  "create table pairings" },
    { T_INOTIFY,   "create table inotify" },

    { I_PATH,      "create file path index" },
    { I_FILEPATH,  "create file path index" },
    { I_PLITEMID,  "create playlist id index" },
    { I_PAIRING,   "create pairing guid index" },

    { TRG_GROUPS_INSERT_FILES,    "create trigger update_groups_new_file" },
    { TRG_GROUPS_UPDATE_FILES,    "create trigger update_groups_update_file" },

    { Q_PL1,       "create default playlist" },
    { Q_PL2,       "create default smart playlist 'Music'" },
    { Q_PL3,       "create default smart playlist 'Movies'" },
    { Q_PL4,       "create default smart playlist 'TV Shows'" },

    { Q_SCVER,     "set schema version" },
  };

static int
db_create_tables(void)
{
  char *errmsg;
  int i;
  int ret;

  for (i = 0; i < (sizeof(db_init_queries) / sizeof(db_init_queries[0])); i++)
    {
      DPRINTF(E_DBG, L_DB, "DB init query: %s\n", db_init_queries[i].desc);

      ret = sqlite3_exec(hdl, db_init_queries[i].query, NULL, NULL, &errmsg);
      if (ret != SQLITE_OK)
	{
	  DPRINTF(E_FATAL, L_DB, "DB init error: %s\n", errmsg);

	  sqlite3_free(errmsg);
	  return -1;
	}
    }

  return 0;
}

static int
db_generic_upgrade(const struct db_init_query *queries, int nqueries)
{
  char *errmsg;
  int i;
  int ret;

  for (i = 0; i < nqueries; i++, queries++)
    {
      DPRINTF(E_DBG, L_DB, "DB upgrade query: %s\n", queries->desc);

      ret = sqlite3_exec(hdl, queries->query, NULL, NULL, &errmsg);
      if (ret != SQLITE_OK)
	{
	  DPRINTF(E_FATAL, L_DB, "DB upgrade error: %s\n", errmsg);

	  sqlite3_free(errmsg);
	  return -1;
	}
    }

  return 0;
}


/* Upgrade from schema v1 to v2 */

#define U_V2_FILES					\
  "ALTER TABLE files ADD COLUMN media_kind INTEGER NOT NULL DEFAULT 0;"		\
  "ALTER TABLE files ADD COLUMN tv_series_name VARCHAR(1024) DEFAULT NULL;"	\
  "ALTER TABLE files ADD COLUMN tv_episode_num_str VARCHAR(1024) DEFAULT NULL;"	\
  "ALTER TABLE files ADD COLUMN tv_network_name VARCHAR(1024) DEFAULT NULL;"	\
  "ALTER TABLE files ADD COLUMN tv_episode_sort INTEGER NOT NULL DEFAULT 0;"	\
  "ALTER TABLE files ADD COLUMN tv_season_num INTEGER NOT NULL DEFAULT 0;"

#define U_V2_RESCAN					\
  "UPDATE files SET db_timestamp = 1;"

#define U_V2_SCVER					\
  "UPDATE admin SET value = '2' WHERE key = 'schema_version';"

static const struct db_init_query db_upgrade_v2_queries[] =
  {
    { U_V2_FILES,     "upgrade table files" },
    { U_V2_RESCAN,    "force library rescan" },
    { U_V2_SCVER,     "set schema_version to 2" },
  };

/* Upgrade from schema v2 to v3 */

#define U_V3_FILES					\
  "UPDATE files SET album_artist = COALESCE(artist, '') WHERE album_artist IS NULL;"

#define U_V3_SCVER					\
  "UPDATE admin SET value = '3' WHERE key = 'schema_version';"

static const struct db_init_query db_upgrade_v3_queries[] =
  {
    { U_V3_FILES,     "upgrade table files" },
    { U_V3_SCVER,     "set schema_version to 3" },
  };

/* Upgrade from schema v3 to v4 */

#define U_V4_PLAYLISTS								\
  "ALTER TABLE playlists ADD COLUMN special_id INTEGER NOT NULL DEFAULT 0;"

#define U_V4_PL1								\
  "UPDATE playlists SET query = '1 = 1' WHERE id = 1;"

#define U_V4_PL2								\
  "INSERT INTO playlists (title, type, query, db_timestamp, path, idx, special_id)" \
  " VALUES('Music', 1, 'media_kind = 1', 0, '', 0, 6);"

#define U_V4_PL3								\
  "INSERT INTO playlists (title, type, query, db_timestamp, path, idx, special_id)" \
  " VALUES('Movies', 1, 'media_kind = 32', 0, '', 0, 4);"

#define U_V4_PL4								\
  "INSERT INTO playlists (title, type, query, db_timestamp, path, idx, special_id)" \
  " VALUES('TV Shows', 1, 'media_kind = 64', 0, '', 0, 5);"

#define U_V4_SCVER					\
  "UPDATE admin SET value = '4' WHERE key = 'schema_version';"

static const struct db_init_query db_upgrade_v4_queries[] =
  {
    { U_V4_PLAYLISTS, "upgrade table playlists" },
    { U_V4_PL1,       "update playlist 1" },
    { U_V4_PL2,       "add smart playlist 'Music'" },
    { U_V4_PL3,       "add smart playlist 'Movies'" },
    { U_V4_PL4,       "add smart playlist 'TV Shows'" },
    { U_V4_SCVER,     "set schema_version to 4" },
  };

/* Upgrade from schema v4 to v5 */

#define U_V5_FIXPL					\
  "UPDATE playlists SET query = 'media_kind = 2' WHERE title = 'Movies' and type = 1;"

#define U_V5_RESCAN					\
  "UPDATE files SET db_timestamp = 1;"

#define U_V5_SCVER					\
  "UPDATE admin SET value = '5' WHERE key = 'schema_version';"

static const struct db_init_query db_upgrade_v5_queries[] =
  {
    { U_V5_FIXPL,     "fix 'Movies' smart playlist" },
    { U_V5_RESCAN,    "force library rescan" },
    { U_V5_SCVER,     "set schema_version to 5" },
  };

/* Upgrade from schema v5 to v6 */

#define U_V6_PAIRINGS					\
  "CREATE TABLE IF NOT EXISTS pairings("		\
  "   remote         VARCHAR(64) PRIMARY KEY NOT NULL,"	\
  "   name           VARCHAR(255) NOT NULL,"		\
  "   guid           VARCHAR(16) NOT NULL"		\
  ");"

#define U_V6_PAIRINGGUID				\
  "CREATE INDEX IF NOT EXISTS idx_pairingguid ON pairings(guid);"

#define U_V6_SCVER					\
  "UPDATE admin SET value = '6' WHERE key = 'schema_version';"

static const struct db_init_query db_upgrade_v6_queries[] =
  {
    { U_V6_PAIRINGS,    "create pairings table" },
    { U_V6_PAIRINGGUID, "create pairing guid index" },
    { U_V6_SCVER,       "set schema_version to 6" },
  };

/* Upgrade from schema v6 to v7 */

#define U_V7_FILES								\
  "ALTER TABLE files ADD COLUMN songalbumid INTEGER NOT NULL DEFAULT 0;"

#define U_V7_RESCAN					\
  "UPDATE files SET db_timestamp = 1;"

#define U_V7_SCVER					\
  "UPDATE admin SET value = '7' WHERE key = 'schema_version';"

static const struct db_init_query db_upgrade_v7_queries[] =
  {
    { U_V7_FILES,     "upgrade table files" },
    { U_V7_RESCAN,    "force library rescan" },
    { U_V7_SCVER,     "set schema_version to 7" },
  };

/* Upgrade from schema v7 to v8 */

#define U_V8_GROUPS							\
  "CREATE TABLE IF NOT EXISTS groups ("					\
  "   id             INTEGER PRIMARY KEY NOT NULL,"			\
  "   type           INTEGER NOT NULL,"					\
  "   name           VARCHAR(1024) NOT NULL,"				\
  "   persistentid   INTEGER NOT NULL,"					\
  "CONSTRAINT groups_type_unique_persistentid UNIQUE (type, persistentid)" \
  ");"

#define U_V8_TRG1							\
  "CREATE TRIGGER update_groups_new_file AFTER INSERT ON files FOR EACH ROW" \
  " BEGIN"								\
  "   INSERT OR IGNORE INTO groups (type, name, persistentid) VALUES (1, NEW.album, NEW.songalbumid);" \
  " END;"

#define U_V8_TRG2							\
  "CREATE TRIGGER update_groups_update_file AFTER UPDATE OF songalbumid ON files FOR EACH ROW" \
  " BEGIN"								\
  "   INSERT OR IGNORE INTO groups (type, name, persistentid) VALUES (1, NEW.album, NEW.songalbumid);" \
  " END;"

#define U_V8_SCVER						\
  "UPDATE admin SET value = '8' WHERE key = 'schema_version';"

static const struct db_init_query db_upgrade_v8_queries[] =
  {
    { U_V8_GROUPS,    "create groups table" },
    { U_V8_TRG1,      "create trigger update_groups_new_file" },
    { U_V8_TRG2,      "create trigger update_groups_update_file" },
    { U_V8_SCVER,     "set schema_version to 8" },
  };

/* Upgrade from schema v8 to v9 */

#define U_V9_INOTIFY1				\
  "DROP TABLE inotify;"

#define U_V9_INOTIFY2					\
  "CREATE TABLE inotify ("				\
  "   wd          INTEGER PRIMARY KEY NOT NULL,"	\
  "   cookie      INTEGER NOT NULL,"			\
  "   path        VARCHAR(4096) NOT NULL"		\
  ");"

#define U_V9_SCVER					\
  "UPDATE admin SET value = '9' WHERE key = 'schema_version';"

static const struct db_init_query db_upgrade_v9_queries[] =
  {
    { U_V9_INOTIFY1,  "drop table inotify" },
    { U_V9_INOTIFY2,  "create new table inotify" },
    { U_V9_SCVER,     "set schema_version to 9" },
  };

static int
db_check_version(void)
{
#define Q_VER "SELECT value FROM admin WHERE key = 'schema_version';"
#define Q_VACUUM "VACUUM;"
  sqlite3_stmt *stmt;
  char *errmsg;
  int cur_ver;
  int ret;

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", Q_VER);

  ret = sqlite3_prepare_v2(hdl, Q_VER, strlen(Q_VER) + 1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s %d\n", sqlite3_errmsg(hdl), ret);
      return -1;
    }

  ret = sqlite3_step(stmt);
  if (ret != SQLITE_ROW)
    {
      DPRINTF(E_LOG, L_DB, "Could not step: %s %d\n", sqlite3_errmsg(hdl), ret);

      sqlite3_finalize(stmt);
      return -1;
    }

  cur_ver = sqlite3_column_int(stmt, 0);

  sqlite3_finalize(stmt);

  if (cur_ver < SCHEMA_VERSION)
    {
      DPRINTF(E_LOG, L_DB, "Database schema outdated, schema upgrade needed v%d -> v%d\n", cur_ver, SCHEMA_VERSION);

      switch (cur_ver)
	{
	  case 1:
	    ret = db_generic_upgrade(db_upgrade_v2_queries, sizeof(db_upgrade_v2_queries) / sizeof(db_upgrade_v2_queries[0]));
	    if (ret < 0)
	      return -1;

	    /* FALLTHROUGH */

	  case 2:
	    ret = db_generic_upgrade(db_upgrade_v3_queries, sizeof(db_upgrade_v3_queries) / sizeof(db_upgrade_v3_queries[0]));
	    if (ret < 0)
	      return -1;

	    /* FALLTHROUGH */

	  case 3:
	    ret = db_generic_upgrade(db_upgrade_v4_queries, sizeof(db_upgrade_v4_queries) / sizeof(db_upgrade_v4_queries[0]));
	    if (ret < 0)
	      return -1;

	    /* FALLTHROUGH */

	  case 4:
	    ret = db_generic_upgrade(db_upgrade_v5_queries, sizeof(db_upgrade_v5_queries) / sizeof(db_upgrade_v5_queries[0]));
	    if (ret < 0)
	      return -1;

	    /* FALLTHROUGH */

	  case 5:
	    ret = db_generic_upgrade(db_upgrade_v6_queries, sizeof(db_upgrade_v6_queries) / sizeof(db_upgrade_v6_queries[0]));
	    if (ret < 0)
	      return -1;

	    /* FALLTHROUGH */

	  case 6:
	    ret = db_generic_upgrade(db_upgrade_v7_queries, sizeof(db_upgrade_v7_queries) / sizeof(db_upgrade_v7_queries[0]));
	    if (ret < 0)
	      return -1;

	    /* FALLTHROUGH */

	  case 7:
	    ret = db_generic_upgrade(db_upgrade_v8_queries, sizeof(db_upgrade_v8_queries) / sizeof(db_upgrade_v8_queries[0]));
	    if (ret < 0)
	      return -1;

	    /* FALLTHROUGH */

	  case 8:
	    ret = db_generic_upgrade(db_upgrade_v9_queries, sizeof(db_upgrade_v9_queries) / sizeof(db_upgrade_v9_queries[0]));
	    if (ret < 0)
	      return -1;

	    break;

	  default:
	    DPRINTF(E_LOG, L_DB, "No upgrade path from DB schema v%d to v%d\n", cur_ver, SCHEMA_VERSION);
	    return -1;
	}

      /* What about some housekeeping work, eh? */
      DPRINTF(E_INFO, L_DB, "Now vacuuming database, this may take some time...\n");

      ret = sqlite3_exec(hdl, Q_VACUUM, NULL, NULL, &errmsg);
      if (ret != SQLITE_OK)
	{
	  DPRINTF(E_LOG, L_DB, "Could not VACUUM database: %s\n", errmsg);

	  sqlite3_free(errmsg);
	  return -1;
	}
    }
  else if (cur_ver > SCHEMA_VERSION)
    {
      DPRINTF(E_LOG, L_DB, "Database schema is newer than the supported version\n");
      return -1;
    }

  return 0;

#undef Q_VER
#undef Q_VACUUM
}

int
db_init(void)
{
  int files;
  int pls;
  int ret;

  db_path = cfg_getstr(cfg_getsec(cfg, "general"), "db_path");

  if (!sqlite3_threadsafe())
    {
      DPRINTF(E_FATAL, L_DB, "The SQLite3 library is not built with a threadsafe configuration\n");
      return -1;
    }

  ret = db_perthread_init();
  if (ret < 0)
    return ret;

  ret = db_check_version();
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_DB, "Could not check database version, trying DB init\n");

      ret = db_create_tables();
      if (ret < 0)
	{
	  DPRINTF(E_FATAL, L_DB, "Could not create tables\n");
	  db_perthread_deinit();
	  return -1;
	}
    }

  files = db_files_get_count();
  pls = db_pl_get_count();

  db_perthread_deinit();

  DPRINTF(E_INFO, L_DB, "Database OK with %d active files and %d active playlists\n", files, pls);

  return 0;
}

void ensure_all_utf8(struct media_file_info *mfi)
{
  ensure_utf8(mfi->title);
  ensure_utf8(mfi->artist);
  ensure_utf8(mfi->album);
  ensure_utf8(mfi->genre);
  ensure_utf8(mfi->comment);
  ensure_utf8(mfi->type);
  ensure_utf8(mfi->composer);
  ensure_utf8(mfi->orchestra);
  ensure_utf8(mfi->conductor);
  ensure_utf8(mfi->url);
  ensure_utf8(mfi->description);
  ensure_utf8(mfi->codectype);
  ensure_utf8(mfi->codectype);
}

void ensure_utf8(char *str)
{
  if (str == NULL)
    return;

  UErrorCode status = U_ZERO_ERROR;

  /* First convert to unicode */

  UConverter *conv = ucnv_open(NULL, &status);
  assert(U_SUCCESS(status));

  uint32_t ucharsLen = ucnv_toUChars(conv, NULL, 0, str, strlen(str), &status);
  if (status != U_BUFFER_OVERFLOW_ERROR) {
    DPRINTF(E_LOG, L_SCAN, "Error: Preflight ucnv_toUChars ICU failed: %s (%d)\n", u_errorName(status), status);
    return;
  };

  status = U_ZERO_ERROR;

  UChar *ucharBuf = 0;
  ucharBuf = (UChar *)realloc(ucharBuf, (ucharsLen+1) * sizeof(UChar));

  ucnv_toUChars(conv, ucharBuf, ucharsLen + 1, str, strlen(str), &status);
  if (!U_SUCCESS(status)) {
    DPRINTF(E_LOG, L_SCAN, "Error: ucnv_toUChars (2) failed: %s (%d)\n", u_errorName(status), status);
    assert(U_SUCCESS(status));
  }

  ucnv_close(conv);

  /* Then to UTF-8 */

  char bytes[2000];

  status = U_ZERO_ERROR;

  conv = ucnv_open("utf-8", &status);
  assert(U_SUCCESS(status));

  status = U_ZERO_ERROR;

  ucnv_fromUChars(conv, bytes, sizeof(bytes), ucharBuf, ucharsLen, &status);
  assert(U_SUCCESS(status));

  ucnv_close(conv);

  free(ucharBuf);

  strcpy(str, bytes);
}
