/*
    This file is part of darktable,
    Copyright (C) 2010-2024 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/metadata.h"
#include "common/debug.h"
#include "common/collection.h"
#include "common/undo.h"
#include "common/grouping.h"
#include "control/conf.h"
#include "views/view.h"
#include "control/signal.h"

#include <stdlib.h>

static GList *_metadata_list = NULL;

GList *dt_metadata_get_list()
{
  return _metadata_list;
}

static gint _compare_display_order(gconstpointer a, gconstpointer b)
{
  return ((dt_metadata_t *) a)->display_order - ((dt_metadata_t *) b)->display_order;
}

void dt_metadata_sort()
{
  _metadata_list = g_list_sort(_metadata_list, _compare_display_order);
}

static void _set_default_import_flag(dt_metadata_t *metadata)
{
    const char *metadata_name = dt_metadata_get_tag_subkey(metadata->tagname);
    char *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", metadata_name);
    if(!dt_conf_key_exists(setting))
    {
      // per default should be imported - ignored if "write_sidecar_files" set
      uint32_t flag = DT_METADATA_FLAG_IMPORTED;
      dt_conf_set_int(setting, flag);
    }
    g_free(setting);
}

gboolean dt_metadata_add_metadata(dt_metadata_t *metadata)
{
  gboolean success = FALSE;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO data.meta_data "
                              " (key, tagname, name, internal, visible, private, display_order)"
                              " VALUES(NULL, ?1, ?2, ?3, ?4, ?5, ?6)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, metadata->tagname, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, metadata->name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, metadata->internal);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, metadata->visible);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, metadata->priv);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 6, metadata->display_order);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // get the new key
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT key FROM data.meta_data WHERE tagname = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, metadata->tagname, -1, SQLITE_TRANSIENT);
  success = sqlite3_step(stmt) == SQLITE_ROW;
  if(success)
  {
    metadata->key = sqlite3_column_int(stmt, 0);
    _metadata_list = g_list_prepend(_metadata_list, metadata);
    _set_default_import_flag(metadata);
  }
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return success;
}

dt_metadata_t *dt_metadata_get_metadata_by_keyid(const uint32_t keyid)
{
  for(GList *iter = _metadata_list; iter; iter = iter->next)
  {
    dt_metadata_t *metadata = (dt_metadata_t *)iter->data;
    if(metadata->key == keyid)
      return metadata;
  }
  return NULL;
}

dt_metadata_t *dt_metadata_get_metadata_by_tagname(const char *tagname)
{
  for(GList *iter = _metadata_list; iter; iter = iter->next)
  {
    dt_metadata_t *metadata = (dt_metadata_t *)iter->data;
    if(!g_strcmp0(metadata->tagname, tagname))
      return metadata;
  }
  return NULL;
}

uint32_t dt_metadata_get_keyid(const char* key)
{
  uint32_t result = -1;

  if(!key) return -1;
  for(GList *iter = _metadata_list; iter; iter = iter->next)
  {
    dt_metadata_t *metadata = (dt_metadata_t *)iter->data;
    if(strncmp(key, metadata->tagname, strlen(metadata->tagname)) == 0)
    {
      result = metadata->key;
      break;
    }
  }
  return result;
}

const char *dt_metadata_get_key(const uint32_t keyid)
{
  const char *result = NULL;

  for(GList *iter = _metadata_list; iter; iter = iter->next)
  {
    dt_metadata_t *metadata = (dt_metadata_t *)iter->data;
    if(metadata->key == keyid)
    {
      result = metadata->tagname;
      break;
    }
  }
  return result;
}

const char *dt_metadata_get_key_by_subkey(const char *subkey)
{
  const char *result = NULL;

  dt_pthread_mutex_lock(&darktable.metadata_threadsafe);
  if(subkey)
  {
    for(GList *iter = _metadata_list; iter; iter = iter->next)
    {
      dt_metadata_t *metadata = (dt_metadata_t *)iter->data;
      char *t = g_strrstr(metadata->tagname, ".");
      if(t && !g_strcmp0(t + 1, subkey))
      {
        result = metadata->tagname;
        break;
      }
    }
  }
  dt_pthread_mutex_unlock(&darktable.metadata_threadsafe);

  return result;
}

const char *dt_metadata_get_tag_subkey(const char *tagname)
{
  const char *t = g_strrstr(tagname, ".");
  if(t) return t + 1;
  return NULL;
}

static void _free_metadata_entry(dt_metadata_t *metadata, gpointer user_data)
{
  g_free(metadata->tagname);
  metadata->tagname = NULL;
  g_free(metadata->name);
  metadata->name = NULL;
}

void dt_metadata_init()
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                          "SELECT key, tagname, name, internal, visible, private, display_order"
                          " FROM data.meta_data"
                          " ORDER BY display_order",
                          -1, &stmt, NULL);

  g_list_foreach(_metadata_list, (GFunc)_free_metadata_entry, NULL);
  _metadata_list = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int key = sqlite3_column_int(stmt, 0);
    char *tagname = (char *)sqlite3_column_text(stmt, 1);
    char *name = (char *)sqlite3_column_text(stmt, 2);
    int internal = sqlite3_column_int(stmt, 3);
    gboolean visible = (gboolean) sqlite3_column_int(stmt, 4);
    gboolean private = (gboolean) sqlite3_column_int(stmt, 5);
    int display_order = sqlite3_column_int(stmt, 6);

    dt_metadata_t *metadata = calloc(1, sizeof(dt_metadata_t));
    metadata->key = key;
    metadata->tagname = g_strdup(tagname);
    metadata->name = g_strdup(name);
    metadata->internal = internal;
    metadata->visible = visible;
    metadata->priv = private;
    metadata->display_order = display_order;
    _metadata_list = g_list_prepend(_metadata_list, metadata);
    _set_default_import_flag(metadata);
  }
  _metadata_list = g_list_reverse(_metadata_list);

  sqlite3_finalize(stmt);
}

typedef struct dt_undo_metadata_t
{
  dt_imgid_t imgid;
  GList *before;      // list of key/value before
  GList *after;       // list of key/value after
} dt_undo_metadata_t;

static GList *_list_find_custom(GList *list, gpointer data)
{
  for(GList *i = list; i; i = g_list_next(i))
  {
    if(i->data && !g_strcmp0(i->data, data))
      return i;
    i = g_list_next(i);
  }
  return NULL;
}

static gchar *_get_tb_removed_metadata_string_values(GList *before, GList *after)
{
  GList *b = before;
  GList *a = after;
  gchar *metadata_list = NULL;

  while(b)
  {
    GList *same_key = _list_find_custom(a, b->data);
    GList *b2 = g_list_next(b);
    gboolean different_value = FALSE;
    const char *value = (char *)b2->data; // if empty we can remove it
    if(same_key)
    {
      GList *same2 = g_list_next(same_key);
      different_value = g_strcmp0(same2->data, b2->data);
    }
    if(!same_key || different_value || !value[0])
    {
      dt_util_str_cat(&metadata_list, "%d,", atoi(b->data));
    }
    b = g_list_next(b);
    b = g_list_next(b);
  }
  if(metadata_list) metadata_list[strlen(metadata_list) - 1] = '\0';
  return metadata_list;
}

static gchar *_get_tb_added_metadata_string_values(const dt_imgid_t imgid,
                                                   GList *before,
                                                   GList *after)
{
  GList *b = before;
  GList *a = after;
  gchar *metadata_list = NULL;

  while(a)
  {
    GList *same_key = _list_find_custom(b, a->data);
    GList *a2 = g_list_next(a);
    gboolean different_value = FALSE;
    const char *value = (char *)a2->data; // if empty we don't add it to database
    if(same_key)
    {
      GList *same2 = g_list_next(same_key);
      different_value = g_strcmp0(same2->data, a2->data);
    }
    if((!same_key || different_value) && value[0])
    {
      char *escaped_text = sqlite3_mprintf("%q", value);
      dt_util_str_cat(&metadata_list, "(%d,%d,'%s'),", GPOINTER_TO_INT(imgid), atoi(a->data), escaped_text);
      sqlite3_free(escaped_text);
    }
    a = g_list_next(a);
    a = g_list_next(a);
  }
  if(metadata_list) metadata_list[strlen(metadata_list) - 1] = '\0';
  return metadata_list;
}

static void _bulk_remove_metadata(const dt_imgid_t imgid, const gchar *metadata_list)
{
  if(dt_is_valid_imgid(imgid) && metadata_list)
  {
    sqlite3_stmt *stmt;
    gchar *query = g_strdup_printf("DELETE FROM main.meta_data WHERE id = %d AND key IN (%s)", imgid, metadata_list);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    g_free(query);
  }
}

static void _bulk_add_metadata(gchar *metadata_list)
{
  if(metadata_list)
  {
    sqlite3_stmt *stmt;
    gchar *query = g_strdup_printf("INSERT INTO main.meta_data (id, key, value) VALUES %s", metadata_list);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    g_free(query);
  }
}

static void _pop_undo_execute(const dt_imgid_t imgid, GList *before, GList *after)
{
  gchar *tobe_removed_list = _get_tb_removed_metadata_string_values(before, after);
  gchar *tobe_added_list = _get_tb_added_metadata_string_values(imgid, before, after);

  _bulk_remove_metadata(imgid, tobe_removed_list);
  _bulk_add_metadata(tobe_added_list);

  g_free(tobe_removed_list);
  g_free(tobe_added_list);
}

static void _pop_undo(gpointer user_data,
                      const dt_undo_type_t type,
                      dt_undo_data_t data,
                      const dt_undo_action_t action,
                      GList **imgs)
{
  if(type == DT_UNDO_METADATA)
  {
    for(GList *list = (GList *)data; list; list = g_list_next(list))
    {
      dt_undo_metadata_t *undometadata = list->data;

      GList *before = (action == DT_ACTION_UNDO) ? undometadata->after : undometadata->before;
      GList *after = (action == DT_ACTION_UNDO) ? undometadata->before : undometadata->after;
      _pop_undo_execute(undometadata->imgid, before, after);
      *imgs = g_list_prepend(*imgs, GINT_TO_POINTER(undometadata->imgid));
    }

    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
  }
}

GList *dt_metadata_get_list_id(const dt_imgid_t imgid)
{
  GList *metadata = NULL;
  if(!dt_is_valid_imgid(imgid))
    return NULL;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT key, value FROM main.meta_data WHERE id=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const gchar *value = (const char *)sqlite3_column_text(stmt, 1);
    gchar *ckey = g_strdup_printf("%d", sqlite3_column_int(stmt, 0));
    gchar *cvalue = g_strdup(value ? value : ""); // to avoid NULL value
    metadata = g_list_append(metadata, (gpointer)ckey);
    metadata = g_list_append(metadata, (gpointer)cvalue);
  }
  sqlite3_finalize(stmt);
  return metadata;
}

static void _undo_metadata_free(gpointer data)
{
  dt_undo_metadata_t *metadata = (dt_undo_metadata_t *)data;
  g_list_free_full(metadata->before, g_free);
  g_list_free_full(metadata->after, g_free);
  g_free(metadata);
}

static void _metadata_undo_data_free(gpointer data)
{
  GList *l = (GList *)data;
  g_list_free_full(l, _undo_metadata_free);
}

gchar *_cleanup_metadata_value(const gchar *value)
{
  char *v = NULL;
  char *c = NULL;
  if(value && value[0])
  {
    v = g_strdup(value);
    c = v + strlen(v) - 1;
    while(c >= v && *c == ' ') *c-- = '\0';
    c = v;
    while(*c == ' ') c++;
  }
  c = g_strdup(c ? c : ""); // avoid NULL value
  g_free(v);
  return c;
}

GList *dt_metadata_get_lock(const dt_imgid_t imgid,
                            const char *key,
                            uint32_t *count)
{
  GList *res = NULL;
  dt_pthread_mutex_lock(&darktable.metadata_threadsafe);
  res = dt_metadata_get(imgid, key, count);
  dt_pthread_mutex_unlock(&darktable.metadata_threadsafe);

  return res;
}

GList *dt_metadata_get(const dt_imgid_t imgid,
                       const char *key,
                       uint32_t *count)
{
  GList *result = NULL;
  sqlite3_stmt *stmt;
  uint32_t local_count = 0;

  const int keyid = dt_metadata_get_keyid(key);
  // key not found in db. Maybe it's one of our "special" keys (rating, tags and colorlabels)?
  if(keyid == -1)
  {
    if(strncmp(key, "Xmp.xmp.Rating", 14) == 0)
    {
      if(!dt_is_valid_imgid(imgid))
      {
        // clang-format off
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT flags FROM main.images WHERE id IN "
                                                                   "(SELECT imgid FROM main.selected_images)",
                                    -1, &stmt, NULL);
        // clang-format on
      }
      else // single image under mouse cursor
      {
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT flags FROM main.images WHERE id = ?1",
                                    -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
      }
      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        local_count++;
        int stars = sqlite3_column_int(stmt, 0);
        stars = (stars & 0x7) - 1;
        result = g_list_prepend(result, GINT_TO_POINTER(stars));
      }
      sqlite3_finalize(stmt);
    }
    else if(strncmp(key, "Xmp.dc.subject", 14) == 0)
    {
      if(!dt_is_valid_imgid(imgid))
      {
        // clang-format off
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "SELECT name FROM data.tags t JOIN main.tagged_images i ON "
                                    "i.tagid = t.id WHERE imgid IN "
                                    "(SELECT imgid FROM main.selected_images)",
                                    -1, &stmt, NULL);
        // clang-format on
      }
      else // single image under mouse cursor
      {
        // clang-format off
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "SELECT name FROM data.tags t JOIN main.tagged_images i ON "
                                    "i.tagid = t.id WHERE imgid = ?1",
                                    -1, &stmt, NULL);
        // clang-format on
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
      }
      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        local_count++;
        result = g_list_prepend(result, g_strdup((char *)sqlite3_column_text(stmt, 0)));
      }
      sqlite3_finalize(stmt);
    }
    else if(strncmp(key, "Xmp.darktable.colorlabels", 25) == 0)
    {
      if(!dt_is_valid_imgid(imgid))
      {
        // clang-format off
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "SELECT color FROM main.color_labels WHERE imgid IN "
                                    "(SELECT imgid FROM main.selected_images)",
                                    -1, &stmt, NULL);
        // clang-format on
      }
      else // single image under mouse cursor
      {
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "SELECT color FROM main.color_labels WHERE imgid=?1 ORDER BY color",
                                    -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
      }
      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        local_count++;
        result = g_list_prepend(result, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
      }
      sqlite3_finalize(stmt);
    }
    if(count != NULL) *count = local_count;
    return g_list_reverse(result);
  }

  // So we got this far -- it has to be a generic key-value entry from meta_data
  if(!dt_is_valid_imgid(imgid))
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2
      (dt_database_get(darktable.db),
       "SELECT value FROM main.meta_data WHERE id IN "
       "(SELECT imgid FROM main.selected_images) AND key = ?1 ORDER BY value",
       -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, keyid);
  }
  else // single image under mouse cursor
  {
    DT_DEBUG_SQLITE3_PREPARE_V2
      (dt_database_get(darktable.db),
       "SELECT value FROM main.meta_data WHERE id = ?1 AND key = ?2", -1,
       &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, keyid);
  }
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    local_count++;
    char *value = (char *)sqlite3_column_text(stmt, 0);
    result = g_list_prepend(result, g_strdup(value ? value : "")); // to avoid NULL value
  }
  sqlite3_finalize(stmt);
  if(count != NULL) *count = local_count;
  return g_list_reverse(result);  // list was built in reverse order, so un-reverse it
}

static void _metadata_add_metadata_to_list(GList **list,
                                           const GList *metadata)
{
  const GList *m = metadata;
  while(m)
  {
    GList *m2 = g_list_next(m);
    GList *same_key = _list_find_custom(*list, m->data);
    GList *same2 = g_list_next(same_key);
    gboolean different_value = FALSE;
    if(same_key) different_value = g_strcmp0(same2->data, m2->data);
    if(same_key && different_value)
    {
      // same key but different value - replace the old value by the new one
      g_free(same2->data);
      same2->data = g_strdup(m2->data);
    }
    else if(!same_key)
    {
      // new key for that image - append the new metadata item
      *list = g_list_append(*list, g_strdup(m->data));
      *list = g_list_append(*list, g_strdup(m2->data));
    }
    m = g_list_next(m);
    m = g_list_next(m);
  }
}

static void _metadata_remove_metadata_from_list(GList **list,
                                                const GList *metadata)
{
  // caution: metadata is a simple list here
  for(const GList *m = metadata; m; m = g_list_next(m))
  {
    GList *same_key = _list_find_custom(*list, m->data);
    if(same_key)
    {
      // same key for that image - remove metadata item
      GList *same2 = g_list_next(same_key);
      *list = g_list_remove_link(*list, same_key);
      g_free(same_key->data);
      g_list_free(same_key);
      *list = g_list_remove_link(*list, same2);
      g_free(same2->data);
      g_list_free(same2);
    }
  }
}

typedef enum dt_tag_actions_t
{
  DT_MA_SET = 0,
  DT_MA_ADD,
  DT_MA_REMOVE
} dt_tag_actions_t;

static void _metadata_execute(const GList *imgs,
                              const GList *metadata,
                              GList **undo,
                              const gboolean undo_on,
                              const gint action)
{
  for(const GList *images = imgs; images; images = g_list_next(images))
  {
    const dt_imgid_t imgid = GPOINTER_TO_INT(images->data);

    dt_undo_metadata_t *undometadata = malloc(sizeof(dt_undo_metadata_t));
    undometadata->imgid = imgid;
    undometadata->before = dt_metadata_get_list_id(imgid);
    switch(action)
    {
      case DT_MA_SET:
        undometadata->after = metadata ? g_list_copy_deep((GList *)metadata, (GCopyFunc)g_strdup, NULL) : NULL;
        break;
      case DT_MA_ADD:
        undometadata->after = g_list_copy_deep(undometadata->before, (GCopyFunc)g_strdup, NULL);
        _metadata_add_metadata_to_list(&undometadata->after, metadata);
        break;
      case DT_MA_REMOVE:
        undometadata->after = g_list_copy_deep(undometadata->before, (GCopyFunc)g_strdup, NULL);
        _metadata_remove_metadata_from_list(&undometadata->after, metadata);
        break;
      default:
        undometadata->after = g_list_copy_deep(undometadata->before, (GCopyFunc)g_strdup, NULL);
        break;
    }

    _pop_undo_execute(imgid, undometadata->before, undometadata->after);

    if(undo_on)
      *undo = g_list_append(*undo, undometadata);
    else
      _undo_metadata_free(undometadata);
  }
}

void dt_metadata_set(const dt_imgid_t imgid,
                     const char *key,
                     const char *value,
                     const gboolean undo_on)
{
  if(!key) return;

  int keyid = dt_metadata_get_keyid(key);
  if(keyid != -1) // known key
  {
    GList *imgs = NULL;
    if(!dt_is_valid_imgid(imgid))
      imgs = dt_act_on_get_images(TRUE, TRUE, FALSE);
    else
      imgs = g_list_prepend(imgs, GINT_TO_POINTER(imgid));
    if(!g_list_is_empty(imgs))
    {
      GList *undo = NULL;
      if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_METADATA);

      const gchar *ckey = g_strdup_printf("%d", keyid);
      const gchar *cvalue = _cleanup_metadata_value(value);
      GList *metadata = NULL;
      metadata = g_list_append(metadata, (gpointer)ckey);
      metadata = g_list_append(metadata, (gpointer)cvalue);

      _metadata_execute(imgs, metadata, &undo, undo_on, DT_MA_ADD);

      g_list_free_full(metadata, g_free);
      g_list_free(imgs);
      if(undo_on)
      {
        dt_undo_record(darktable.undo, NULL, DT_UNDO_METADATA, undo, _pop_undo, _metadata_undo_data_free);
        dt_undo_end_group(darktable.undo);
      }
    }
  }
}

void dt_metadata_set_import_lock(const dt_imgid_t imgid, const char *key, const char *value)
{
  dt_pthread_mutex_lock(&darktable.metadata_threadsafe);
  dt_metadata_set_import(imgid, key, value);
  dt_pthread_mutex_unlock(&darktable.metadata_threadsafe);
}

void dt_metadata_set_import(const dt_imgid_t imgid, const char *key, const char *value)
{
  if(!key || !dt_is_valid_imgid(imgid)) return;

  const dt_metadata_t *md = dt_metadata_get_metadata_by_tagname(key);

  if(md) // known key
  {
    gboolean imported = (dt_image_get_xmp_mode() != DT_WRITE_XMP_NEVER);
    if(!imported && !md->internal)
    {
      const gchar *name = dt_metadata_get_tag_subkey(md->tagname);
      char *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
      imported = dt_conf_get_int(setting) & DT_METADATA_FLAG_IMPORTED;
      g_free(setting);
    }
    if(imported)
    {
      GList *imgs = NULL;
      imgs = g_list_prepend(imgs, GINT_TO_POINTER(imgid));
      if(!g_list_is_empty(imgs))
      {
        GList *undo = NULL;

        const gchar *ckey = g_strdup_printf("%d", md->key);
        const gchar *cvalue = _cleanup_metadata_value(value);
        GList *metadata = NULL;
        metadata = g_list_append(metadata, (gpointer)ckey);
        metadata = g_list_append(metadata, (gpointer)cvalue);

        _metadata_execute(imgs, metadata, &undo, FALSE, DT_MA_ADD);

        g_list_free_full(metadata, g_free);
        g_list_free(imgs);
      }
    }
  }
}

void dt_metadata_set_list(const GList *imgs, GList *key_value, const gboolean undo_on)
{
  GList *metadata = NULL;
  GList *kv = key_value;

  dt_pthread_mutex_lock(&darktable.metadata_threadsafe);
  while(kv)
  {
    const gchar *key = (const gchar *)kv->data;
    const int keyid = dt_metadata_get_keyid(key);
    if(keyid != -1) // known key
    {
      const gchar *ckey = g_strdup_printf("%d", keyid);
      kv = g_list_next(kv);
      const gchar *value = (const gchar *)kv->data;
      kv = g_list_next(kv);
      if(value)
      {
        metadata = g_list_append(metadata, (gchar *)ckey);
        metadata = g_list_append(metadata, _cleanup_metadata_value(value));
      }
    }
    else
    {
      kv = g_list_next(kv);
      kv = g_list_next(kv);
    }
  }
  dt_pthread_mutex_unlock(&darktable.metadata_threadsafe);

  if(metadata && imgs)
  {
    GList *undo = NULL;
    if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_METADATA);

    _metadata_execute(imgs, metadata, &undo, undo_on, DT_MA_ADD);

    if(undo_on)
    {
      dt_undo_record(darktable.undo, NULL,
                     DT_UNDO_METADATA, undo, _pop_undo, _metadata_undo_data_free);
      dt_undo_end_group(darktable.undo);
    }

    g_list_free_full(metadata, g_free);
  }
}

void dt_metadata_clear(const GList *imgs, const gboolean undo_on)
{
  // do not clear internal or hidden metadata
  GList *metadata = NULL;
  for(GList *iter = dt_metadata_get_list(); iter; iter = iter->next)
  {
    const dt_metadata_t *md = (dt_metadata_t *)iter->data;
    if(!md->internal)
    {
      if(md->visible)
      {
        // caution: metadata is a simple list here
        metadata = g_list_prepend(metadata, g_strdup_printf("%u", md->key));
      }
    }
  }

  if(metadata)
  {
    metadata = g_list_reverse(metadata);  // list was built in reverse order, so un-reverse it
    GList *undo = NULL;
    if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_METADATA);

    _metadata_execute(imgs, metadata, &undo, undo_on, DT_MA_REMOVE);

    if(undo_on)
    {
      dt_undo_record(darktable.undo, NULL, DT_UNDO_METADATA, undo, _pop_undo, _metadata_undo_data_free);
      dt_undo_end_group(darktable.undo);
    }

    g_list_free_full(metadata, g_free);
  }
}

void dt_metadata_set_list_id(const GList *img,
                             const GList *metadata,
                             const gboolean clear_on,
                             const gboolean undo_on)
{
  if(!g_list_is_empty(img))
  {
    GList *undo = NULL;
    if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_METADATA);

    _metadata_execute(img, metadata, &undo, undo_on, clear_on ? DT_MA_SET : DT_MA_ADD);

    if(undo_on)
    {
      dt_undo_record(darktable.undo, NULL, DT_UNDO_METADATA, undo, _pop_undo, _metadata_undo_data_free);
      dt_undo_end_group(darktable.undo);
    }
  }
}

gboolean dt_metadata_already_imported(const char *filename, const char *datetime)
{
  if(!filename || !datetime)
    return FALSE;
  char *id = g_strconcat(filename, "-", datetime, NULL);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(*) FROM main.meta_data WHERE value=?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, id, -1, SQLITE_TRANSIENT);
  gboolean res = FALSE;
  if(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) != 0)
    res = TRUE;
  sqlite3_finalize(stmt);
  g_free(id);
  return res;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
