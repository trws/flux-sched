/*****************************************************************************\
 *  Copyright (c) 2014 Lawrence Livermore National Security, LLC.  Produced at
 *  the Lawrence Livermore National Laboratory (cf, AUTHORS, DISCLAIMER.LLNS).
 *  LLNL-CODE-658032 All rights reserved.
 *
 *  This file is part of the Flux resource manager framework.
 *  For details, see https://github.com/flux-framework.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the license, or (at your option)
 *  any later version.
 *
 *  Flux is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *  See also:  http://www.gnu.org/licenses/
 \*****************************************************************************/

#include "resrc_sqlite_priv.h"

#include <stdarg.h>
// for asprintf
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <czmq.h>

#include "sqlite/sqlite3.h"

#include "rdl.h"
#include "resrc.h"
#include "resrc_tree.h"
#include "src/common/libutil/jsonutil.h"
#include "src/common/libutil/shortjson.h"
#include "src/common/libutil/xzmalloc.h"

/***************************************************************************
 *  API
 ***************************************************************************/

const char *resrc_type (const resrc_t *resrc)
{
    return resrc->type;
}

void jobid_destroy (void *object)
{
    int64_t *tmp = (int64_t *)object;
    free (tmp);
}

resource_list_t *resrc_new_id_list ()
{
    resource_list_t *ret = xzmalloc (sizeof(resource_list_t));
    memset (ret, 0, sizeof(resource_list_t));
    return ret;
}

void resrc_id_list_destroy (resource_list_t *resrc_ids_in)
{
    if (resrc_ids_in) {
        if (resrc_ids_in->query)
            sqlite3_free ((void *)resrc_ids_in->query);
        if (resrc_ids_in->stmt)
            SQLITE_CHECK (resrc_ids_in->db,
                          sqlite3_finalize (resrc_ids_in->stmt));
    }
    free (resrc_ids_in);
}

const resrc_t *resrc_list_next (resource_list_t *rl)
{
    if (!rl)
        return NULL;

    if (rl->cursor == NULL)
        rl->cursor = resrc_new_resource (NULL, NULL, 0, NULL);

    // If this is null, it's programmer error
    assert (rl->stmt != NULL);

    if (sqlite3_step (rl->stmt) == SQLITE_ROW) {
        rl->cursor->initialized = false;
        rl->cursor->id = sqlite3_column_int64 (rl->stmt, 0);
        rl->cursor->resource_database.db = rl->db;
    } else {
        // No row is available
        return NULL;
    }
    return rl->cursor;
}

const resrc_t *resrc_list_first (resource_list_t *rl)
{
    if (!rl)
        return NULL;

    if (!rl->stmt)
        SQLITE_CHECK (rl->db,
                      sqlite3_prepare_v2 (rl->db,
                                          rl->query,
                                          strlen (rl->query) + 1,
                                          &rl->stmt,
                                          NULL));
    else
        sqlite3_reset (rl->stmt);

    return resrc_list_next (rl);
}

size_t resrc_list_size (resource_list_t *rl)
{
    return zlist_size ((zlist_t *)rl);
}

void create_base_schema (sqlite3 *db)
{
    const char * schema =
        "PRAGMA foreign_keys = ON;"
        "DROP TABLE IF EXISTS resource_types;"
        "CREATE TABLE resource_types("
        "name TEXT PRIMARY KEY NOT NULL,"
        "pool INT NOT NULL,"
        "WITHOUT ROWID);"

        "DROP TABLE IF EXISTS resources;"
        "CREATE TABLE resources(id INTEGER PRIMARY KEY,"
        "local_id INT,"
        "type TEXT,"
        "name TEXT,"
        "pool BOOLEAN,"
        "pool_size INT,"
        "parent INT,"
        "uuid TEXT,"
        "state INT,"
        "FOREIGN KEY(type) REFERENCES resource_types(name)"
        ");"

        "DROP TABLE IF EXISTS job_link;"
        "CREATE TABLE job_link("
        "job_id INTEGER,"
        "resource_id INTEGER,"
        "job_type INTEGER NOT NULL DEFAULT(0),"
        "FOREIGN KEY(resource_id) REFERENCES resources(id)"
        /* "FOREIGN KEY(job_id) REFERENCES jobs(id)" */ //TODO: nothing in
        //here actually adds jobs...
        ");"
        "CREATE INDEX job_link_id_index ON job_link(job_id);"

        "DROP TABLE IF EXISTS ancestors;"
        "CREATE TABLE ancestors("
        "ancestor INTEGER NOT NULL,"
        "resource_id INTEGER NOT NULL,"
        "depth INTEGER,"
        "FOREIGN KEY(resource_id) REFERENCES resources(id),"
        "FOREIGN KEY(ancestor) REFERENCES resources(id),"
        "PRIMARY KEY(ancestor, resource_id)"
        //here actually adds jobs...
        ");"
        "CREATE INDEX ancestors_ancestor ON ancestors(ancestor);"
        "CREATE INDEX ancestors_id ON ancestors(resource_id);"
        ;

    SQLITE_CHECK (db, sqlite3_exec (db, schema, 0, 0, NULL));
}

int64_t add_resource_type (sqlite3 *db, const char *name, bool pool)
{
    const char * sql = 
        "INSERT INTO resource_types "
        "      (name, pool) "
        "VALUES(?,    ?)";
    sqlite3_stmt *stmt = NULL;
    SQLITE_CHECK (db,
                  sqlite3_prepare_v2 (db, sql, strlen (sql) + 1, &stmt, NULL));
    SQLITE_CHECK (
        db,
        sqlite3_bind_text (stmt, 1, name, strlen (name) + 1, SQLITE_STATIC));
    SQLITE_CHECK (db, sqlite3_bind_int (stmt, 2, pool));

    SQLITE_CHECK_EXPECT (db, sqlite3_step (stmt), SQLITE_DONE);
    SQLITE_CHECK (db, sqlite3_finalize (stmt));
    return sqlite3_last_insert_rowid (db);
}

void add_ancestors (sqlite3 *db, int64_t resource_id, zlist_t *ancestors)
{
    const char * sql = 
        "INSERT INTO ancestors "
        "      (resource_id, ancestor, depth) "
        "VALUES(          ?,        ?, ?)";
    static sqlite3_stmt *stmt = NULL;
    if (!stmt)
        SQLITE_CHECK (
            db, sqlite3_prepare_v2 (db, sql, strlen (sql) + 1, &stmt, NULL));

    int64_t *ancestor = zlist_first (ancestors);
    int64_t depth = 0;
    while (ancestor) {
        SQLITE_CHECK (db, sqlite3_bind_int64 (stmt, 1, resource_id));
        SQLITE_CHECK (db, sqlite3_bind_int64 (stmt, 2, *ancestor));
        SQLITE_CHECK (db, sqlite3_bind_int64 (stmt, 3, depth));

        SQLITE_CHECK_EXPECT (db, sqlite3_step (stmt), SQLITE_DONE);
        SQLITE_CHECK (db, sqlite3_reset (stmt));
        ancestor = zlist_next (ancestors);
        depth++;
    }
}

int64_t add_resource (sqlite3 *db, resrc_t *r)
{
    static const char * sql =
        "INSERT INTO resources"
        "      (name, type, local_id, pool, pool_size, parent, uuid, state) "
        "VALUES("
        "       ?,"
        "       ?,"
        "       ?,"
        "       ?,"
        "       ?,"
        "       ?,"
        "       ?,"
        "       ?);";
    // NOTE: static to keep the prepared insertion statement around for
    // performance
    static sqlite3_stmt *stmt = NULL;
    int id = 0;

    if (stmt == NULL) {
        SQLITE_CHECK (
            db, sqlite3_prepare_v2 (db, sql, strlen (sql) + 1, &stmt, NULL));
    }

    if (r->name)
        SQLITE_CHECK (db,
                      sqlite3_bind_text (stmt,
                                         ++id,
                                         r->name,
                                         strlen (r->name) + 1,
                                         SQLITE_STATIC));
    else
        SQLITE_CHECK (db, sqlite3_bind_null (stmt, ++id));

    SQLITE_CHECK (db,
                  sqlite3_bind_text (stmt,
                                     ++id,
                                     r->type,
                                     strlen (r->type) + 1,
                                     SQLITE_STATIC));

    SQLITE_CHECK (db, sqlite3_bind_int64 (stmt, ++id, r->local_id));
    SQLITE_CHECK (db, sqlite3_bind_int (stmt, ++id, r->pool));
    SQLITE_CHECK (db, sqlite3_bind_int64 (stmt, ++id, r->pool_size));

    if (zlist_size (r->ancestors) > 0) {
        int64_t *parent = zlist_first (r->ancestors);
        SQLITE_CHECK (db, sqlite3_bind_int64 (stmt, ++id, *parent));
    } else {
        SQLITE_CHECK (db, sqlite3_bind_null (stmt, ++id));
    }

    if (uuid_is_null (r->uuid)) {
        SQLITE_CHECK (db, sqlite3_bind_null (stmt, ++id));
    } else {
        char uuid_str[37] = {0};
        uuid_unparse (r->uuid, uuid_str);
        SQLITE_CHECK (db,
                      sqlite3_bind_text (stmt,
                                         ++id,
                                         uuid_str,
                                         strlen (uuid_str) + 1,
                                         SQLITE_STATIC));
    }

    SQLITE_CHECK (db, sqlite3_bind_int (stmt, ++id, r->state));

    int err = sqlite3_step (stmt);
    if (err != SQLITE_OK && err == SQLITE_CONSTRAINT) {
        // Chances are this is a type that hasn't been added, add it now
        /* printf ("constraint failed, adding type %s\n", r->type); */
        add_resource_type (db, r->type, r->pool);
        sqlite3_reset (stmt);
        SQLITE_CHECK_EXPECT (db, sqlite3_step (stmt), SQLITE_DONE);
    } else {
        SQLITE_CHECK_EXPECT (db, err, SQLITE_DONE);
    }

    sqlite3_reset (stmt);

    int64_t new_resource_id = sqlite3_last_insert_rowid (db);
    add_ancestors (db, new_resource_id, r->ancestors);

    return new_resource_id;
}

resrc_t *resrc_new_resource (const char *type,
                             const char *name,
                             int64_t id,
                             uuid_t uuid)
{
    resrc_t *ret = xzmalloc (sizeof(resrc_t));
    if (ret && type && name && !uuid_is_null (uuid)) {
        *ret = (resrc_t){.type = type,
                         .name = name,
                         .id = id,
                         .initialized = true};
        uuid_copy (ret->uuid, uuid);
    } else if (ret) {
        *ret = (resrc_t){.id = id, .initialized = false};
    }
    return ret;
}

resrc_t *resrc_copy_resource (resrc_t *resrc)
{
    // TODO: nothing uses this yet
    return NULL;
}

void resrc_resource_destroy (resrc_t *object)
{
    if (object) {
        if (object->initialized) {
            zlist_destroy (&object->ancestors);
        }
    }
    free (object);
}

static int64_t resrc_add_resource (resources_t *resource_database,
                                   zlist_t *ancestors,
                                   struct resource *rdl_resource)
{
    const char *name = NULL;
    const char *tmp = NULL;
    const char *type = NULL;
    int64_t database_id = 0;
    int64_t local_id = 0;
    json_object *o = NULL;
    o = rdl_resource_json (rdl_resource);
    Jget_str (o, "type", &type);
    Jget_str (o, "uuid", &tmp);
    Jget_str (o, "name", &name);

    if (!(Jget_int64 (o, "id", &local_id)))
        local_id = 0;

    resrc_t new_resource = {.type = type,
                            .name = name,
                            .local_id = local_id,
                            .ancestors = ancestors,
                            .state = RESOURCE_IDLE};

    uuid_parse (tmp, new_resource.uuid);

    database_id = add_resource (resource_database->db, &new_resource);
    json_object_put (o);
    return database_id;
}

static void resrc_add_resource_from_rdl_tree_inner (
    resources_t *resource_database,
    struct resource *r,
    zlist_t *ancestors)
{
    struct resource *c;
    int64_t *current_id = malloc (sizeof(int64_t));
    *current_id = resrc_add_resource (resource_database, ancestors, r);

    zlist_append (ancestors, current_id);
    while ((c = rdl_resource_next_child (r))) {
        resrc_add_resource_from_rdl_tree_inner (resource_database,
                                                c,
                                                ancestors);
        rdl_resource_destroy (c);
    }
    zlist_remove (ancestors, current_id);
    free (current_id);
}
static void resrc_add_resource_from_rdl_tree (resources_t *resource_database,
                                              struct resource *r)
{
    zlist_t *ancestors = zlist_new ();
    resrc_add_resource_from_rdl_tree_inner (resource_database, r, ancestors);
    zlist_destroy (&ancestors);
}

resources_t *resrc_generate_resources (const char *path, char *resource)
{
    struct rdl *rdl = NULL;
    struct rdllib *l = NULL;
    struct resource *r = NULL;
    resources_t *resource_database = xzmalloc (sizeof(resources_t));
    const char *filename = NULL;

    if (!(l = rdllib_open ()) || !(rdl = rdl_loadfile (l, path)))
        goto ret;

    if (!(r = rdl_resource_get (rdl, resource)))
        goto ret;

    if ((filename = getenv ("FLUX_SQLITE_USE_FILE"))) {
        // TODO: consider a default file location rather than in-memory
        sqlite3_open_v2 (filename,
                         &resource_database->db,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                         NULL);
    } else {
        sqlite3_open_v2 (":memory:",
                         &resource_database->db,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                         NULL);
    }
    create_base_schema (resource_database->db);

    resrc_add_resource_from_rdl_tree (resource_database, r);
    rdl_destroy (rdl);
    rdllib_close (l);
ret:
    return (resources_t *)resource_database;
}

void resrc_destroy_resources (resources_t **resources)
{
    sqlite3_close ((*resources)->db);
}
static sqlite3_stmt *get_jobs_from_id = NULL;
static const char *get_jobs_from_id_query
    = "SELECT job_id FROM job_link WHERE resource_id = ?";

static sqlite3_stmt *get_rjobs_from_id = NULL;
static const char *get_rjobs_from_id_query
    = "SELECT job_id FROM job_link WHERE resource_id = ?";

int print_columns_and_data (void *dat,
                            int ncols,
                            char **coldata,
                            char **colnames)
{
    resources_t *resource_database = (resources_t *)dat;
    int ret = 0;
    if (ncols == 0 || coldata[0] == NULL) {
        ret = 1;
        goto done;
    }

    fputs ("resrc", stdout);
    for (int column_id = 0; column_id < ncols; column_id++) {
        putchar (' ');
        fputs (colnames[column_id], stdout);
        putchar (':');
        fputs (coldata[column_id] ? coldata[column_id] : "NULL", stdout);
    }
    fputs (", jobs", stdout);
    if (!get_jobs_from_id) {
        SQLITE_CHECK (resource_database->db,
                      sqlite3_prepare_v2 (resource_database->db,
                                          get_jobs_from_id_query,
                                          strlen (get_jobs_from_id_query) + 1,
                                          &get_jobs_from_id,
                                          NULL));
    }

    sqlite3_bind_text (get_jobs_from_id,
                       1,
                       coldata[0],
                       strlen (coldata[0]) + 1,
                       SQLITE_STATIC);

    while (sqlite3_step (get_jobs_from_id) == SQLITE_ROW) {
        printf (", %lld", sqlite3_column_int64 (get_jobs_from_id, 0));
    }
    sqlite3_reset (get_jobs_from_id);

    fputs (", reserved jobs", stdout);
    if (!get_rjobs_from_id) {
        SQLITE_CHECK (resource_database->db,
                      sqlite3_prepare_v2 (resource_database->db,
                                          get_rjobs_from_id_query,
                                          strlen (get_rjobs_from_id_query) + 1,
                                          &get_rjobs_from_id,
                                          NULL));
    }
    sqlite3_bind_text (get_rjobs_from_id,
                       1,
                       coldata[0],
                       strlen (coldata[0]) + 1,
                       SQLITE_STATIC);

    while (sqlite3_step (get_rjobs_from_id) == SQLITE_ROW) {
        printf (", %lld", sqlite3_column_int64 (get_rjobs_from_id, 0));
    }
    sqlite3_reset (get_rjobs_from_id);

    putchar ('\n');

done:
    return ret;
}

void resrc_print_resources (resources_t *resource_database)
{
    if (!resource_database) {
        return;
    }

    const char * const sql = "SELECT id, type, name, local_id, state, uuid "
        "FROM resources "
        "ORDER BY id ASC ";

    SQLITE_CHECK (resource_database->db,
                  sqlite3_exec (resource_database->db,
                                sql,
                                print_columns_and_data,
                                resource_database,
                                NULL));
}

int resrc_search_flat_resources_for_count (sqlite3 *db,
                                           const char *const type,
                                           bool available)
{
    if (!db || !type) {
        return -1;
    }
    const char * sql = 
        "SELECT count(*) "
        "FROM resources "
        "WHERE type like ? AND (? OR state = ? )";
    sqlite3_stmt *stmt;
    SQLITE_CHECK (db,
                  sqlite3_prepare_v2 (db, sql, strlen (sql) + 1, &stmt, NULL));
    SQLITE_CHECK (
        db,
        sqlite3_bind_text (stmt, 1, type, strlen (type) + 1, SQLITE_STATIC));
    SQLITE_CHECK (db, sqlite3_bind_int (stmt, 2, available));
    SQLITE_CHECK (db, sqlite3_bind_int (stmt, 3, RESOURCE_IDLE));
    SQLITE_CHECK_EXPECT (db, sqlite3_step (stmt), SQLITE_ROW);

    // FIXME: this is slicing, but the interface of resrc assumes int
    int ret = sqlite3_column_int64 (stmt, 0);
    SQLITE_CHECK (db, sqlite3_finalize (stmt));
    return ret;
}

void resrc_search_by_id (const resources_t *resource_database,
                         resource_list_t *found,
                         int64_t id)
{
    if (!resource_database || !found) {
        return;
    }

    const char * sql = 
        "SELECT id "
        "FROM resources "
        "WHERE parent = %ld ";
    // the extra OR construct allows me to only prepare this statement once,
    // even if state shouldn't be considered

    found->query = sqlite3_mprintf (sql, id);
    found->db = resource_database->db;
    found->stmt = NULL;
}

int resrc_search_flat_resources (resources_t *resource_database,
                                 resource_list_t *found,
                                 JSON req_res,
                                 bool available)
{
    const char *type = NULL;
    int nfound = 0;
    int req_qty = 0;

    if (!resource_database || !found || !req_res) {
        goto ret;
    }

    Jget_str (req_res, "type", &type);
    Jget_int (req_res, "req_qty", &req_qty);

    nfound = resrc_search_flat_resources_for_count (resource_database->db,
                                                    type,
                                                    available);

    if (nfound == 0)
        goto ret;

    const char * sql = 
        "SELECT id "
        "FROM resources "
        "WHERE type like %Q AND (%d OR state = %d )";
    // the extra OR construct allows me to only prepare this statement once,
    // even if state shouldn't be considered

    found->query
        = sqlite3_mprintf (sql, type, available ? 1 : 0, RESOURCE_IDLE);
    found->db = resource_database->db;
    found->stmt = NULL;

ret:
    return nfound;
}

void resrc_update_state (resources_t *resource_database,
                         resource_list_t *resrc_ids,
                         resource_state_t state)
{
    const char *const sql_up
        = "UPDATE resources SET state = %d WHERE resources.id in (%s)";
    const char *const updater
        = sqlite3_mprintf (sql_up, state, resrc_ids->query);
    /* printf ("running updater: %s\n", updater); */
    SQLITE_CHECK (resource_database->db,
                  sqlite3_exec (resource_database->db, updater, 0, 0, NULL));
}

int resrc_allocate_resources (resources_t *resource_database,
                              resource_list_t *resrc_ids,
                              int64_t job_id)
{
    int rc = 0;

    if (!resource_database || !resrc_ids || !job_id) {
        rc = -1;
        goto ret;
    }

    const char * const sql = "INSERT INTO job_link (job_id, resource_id) SELECT (%lld) as job_id, id as resource_id from (%s)";
    const char *const inserter
        = sqlite3_mprintf (sql, job_id, resrc_ids->query);
    /* printf ("running inserter: %s\n", inserter); */
    SQLITE_CHECK (resource_database->db,
                  sqlite3_exec (resource_database->db, inserter, 0, 0, NULL));

    resrc_update_state (resource_database, resrc_ids, RESOURCE_ALLOCATED);

ret:
    return rc;
}

int resrc_reserve_resources (resources_t *resource_database,
                             resource_list_t *resrc_ids,
                             int64_t job_id)
{
    int rc = 0;

    if (!resource_database || !resrc_ids || !job_id) {
        rc = -1;
        goto ret;
    }

    const char * const sql = "INSERT INTO job_link (job_id, resource_id, job_type) SELECT (%lld) as job_id, id as resource_id, 1 as job_type from (%s)";
    const char *const inserter
        = sqlite3_mprintf (sql, job_id, resrc_ids->query);
    /* printf ("running inserter: %s\n", inserter); */
    SQLITE_CHECK (resource_database->db,
                  sqlite3_exec (resource_database->db, inserter, 0, 0, NULL));

    const char * const sql_up = "UPDATE resources SET state = %d WHERE state != %d AND resources.id in (%s)";
    const char *const updater = sqlite3_mprintf (sql_up,
                                                 RESOURCE_RESERVED,
                                                 RESOURCE_ALLOCATED,
                                                 resrc_ids->query);
    /* printf ("running updater: %s\n", updater); */
    SQLITE_CHECK (resource_database->db,
                  sqlite3_exec (resource_database->db, updater, 0, 0, NULL));

ret:
    return rc;
}

json_object *resrc_serialize (resources_t *resource_database,
                              resource_list_t *resrc_ids)
{
    JSON o = NULL;
    if (!resource_database || !resrc_ids) {
        goto ret;
    }

    char *sql = sqlite3_mprintf (
        "SELECT resources.id as id, name FROM resources JOIN (%s) as query on "
        "resources.id = query.id",
        resrc_ids->query);

    sqlite3_stmt *stmt;
    SQLITE_CHECK (resource_database->db,
                  sqlite3_prepare_v2 (
                      resource_database->db, sql, strlen (sql), &stmt, NULL));
    o = Jnew ();
    JSON ja = Jnew_ar();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Jadd_ar_str (ja, (const char *)sqlite3_column_text(stmt, 1));

    }
    json_object_object_add (o, "resrcs", ja);
    sqlite3_free (sql);
ret:
    return (json_object*)o;
}
int resrc_release_resources (resources_t *resource_database,
                             resource_list_t *resrc_ids,
                             int64_t rel_job)
{
    int rc = 0;

    if (!resource_database || !resrc_ids || !rel_job) {
        rc = -1;
        goto ret;
    }

    const char * const sql = "DELETE FROM job_link WHERE job_id = %lld AND job_link.resource_id in (SELECT id as resource_id from (%s))";
    const char *const deleter
        = sqlite3_mprintf (sql, rel_job, resrc_ids->query);
    /* printf ("running deleter: %s\n", deleter); */
    SQLITE_CHECK (resource_database->db,
                  sqlite3_exec (resource_database->db, deleter, 0, 0, NULL));

    const char * const sql_up = "UPDATE resources "
                                "SET state = CASE WHEN (SELECT count(*) FROM job_link where resource_id = resources.id) = 0 "
                                                      "THEN %d "
                                                 "ELSE %d END "
                                "WHERE resources.id in (%s) ";
    const char *const updater = sqlite3_mprintf (sql_up,
                                                 RESOURCE_IDLE,
                                                 RESOURCE_RESERVED,
                                                 resrc_ids->query);
    printf ("running updater: %s\n", updater);
    SQLITE_CHECK (resource_database->db,
                  sqlite3_exec (resource_database->db, updater, 0, 0, NULL));

ret:
    return rc;
}

void resrc_tree_print (const resrc_tree_t *resrc_tree)
{
    resrc_t *resource = (resrc_t *)resrc_tree;

    if (!resrc_tree) {
        return;
    }

    resources_t *resource_database = &resource->resource_database;

    char *sql = sqlite3_mprintf (
        "SELECT id, type, name, local_id, state, uuid "
        "FROM resources join ancestors on ancestors.resource_id = resources.id "
        "WHERE ancestors.ancestor = %ld "
        "ORDER BY id ASC ",
        resource->id);

    // Print all descendents of the resource passed in, in order of ID

    SQLITE_CHECK (resource_database->db,
                  sqlite3_exec (resource_database->db,
                                sql,
                                print_columns_and_data,
                                resource_database,
                                NULL));
}

char *build_query_string (const char *fields,
                          JSON request,
                          bool group_each_level)
{
    char *base_from = "resources as r_l0 ";
    char *base_where = "r_l0.type LIKE '%s'";
    char *base_group = group_each_level ? "GROUP BY r_l0.id" : "";

    const char *base_type = NULL;
    int base_qty = 0;
    Jget_str (request, "type", &base_type);
    Jget_int (request, "req_qty", &base_qty);

    char *from = xstrdup (base_from);
    char *where;
    asprintf (&where, base_where, base_type);
    char *group = xstrdup (base_group);

    JSON child_walker = request;
    Jget_obj (request, "req_child", &child_walker);
    for (int level = 1; child_walker;
         child_walker = Jobj_get (child_walker, "req_child"), level++) {
        const char *type = NULL;
        int qty = 0;

        Jget_str (child_walker, "type", &type);
        Jget_int (child_walker, "req_qty", &qty);

        printf ("Got type: %s\n", type);

        char *new_from;
        asprintf (&new_from,
                  " %3$s "
                  " JOIN ancestors as a_l%1$d ON r_l%2$d.id = a_l%1$d.ancestor "
                  " JOIN resources as r_l%1$d ON r_l%1$d.id = "
                  "a_l%1$d.resource_id ",
                  level,
                  level - 1,
                  from);
        free (from);
        from = new_from;

        // FIXME: would much rather use mprintf here, but no positional
        // arguments
        char *new_where;
        asprintf (&new_where,
                  " %1$s "
                  "AND r_l%2$d.type like '%3$s' ",
                  where,
                  level,
                  type);
        free (where);
        where = new_where;

        if (group_each_level) {
            char *new_group;
            asprintf (&new_group,
                      " %1$s "
                      ",  r_l%2$d.type ",
                      group,
                      level);
            free (group);
            group = new_group;
        }
    }

    const char *sql = "SELECT %s FROM %s WHERE %s %s";
    char *ret = sqlite3_mprintf (sql, fields, from, where, group);
    free (from);
    free (where);
    free (group);
    printf ("New multilevel query string: %s\n", ret);
    return ret;
}

int resrc_tree_search_multilevel_count (const resource_list_t *ids,
                                        JSON req_res,
                                        bool available)
{
    sqlite3 *db = ids->db;

    // Build a counting string
    char *sql = build_query_string ("count(DISTINCT r_l0.id)", req_res, false);
    sqlite3_stmt *stmt = NULL;
    SQLITE_CHECK (db, sqlite3_prepare_v2 (db, sql, strlen (sql), &stmt, NULL));
    if (sqlite3_step (stmt) != SQLITE_ROW)
        return 0;

    // FIXME: this is slicing, but the interface of resrc assumes int
    int ret = sqlite3_column_int64 (stmt, 0);
    SQLITE_CHECK (db, sqlite3_finalize (stmt));
    sqlite3_free (sql);
    return ret;
}

int resrc_tree_search_multilevel (const resource_list_t *ids,
                                  resource_list_t *found,
                                  JSON req_res,
                                  bool available)
{
    sqlite3 *db = ids->db;
    int ret = resrc_tree_search_multilevel_count (ids, req_res, available);

    if (ret <= 0) {
        goto done;
    }

    char *sql = build_query_string ("r_l0.id as id", req_res, true);

    found->query = sql;
    found->db = db;
    found->stmt = NULL;

done:
    return ret;
}

/* returns the number of composites found */
int resrc_tree_search (const resource_list_t *ids,
                       resource_list_t *found,
                       JSON req_res,
                       bool available)
{
    if (!ids || !req_res) {
        return -1;
    }

    struct resources resource_pack = {.db = ids->db};
    JSON req_child = NULL;
    Jget_obj (req_res, "req_child", &req_child);

    if (!req_child) {
        printf ("TREE: running flat search\n");
        return resrc_search_flat_resources (&resource_pack,
                                            found,
                                            req_res,
                                            available);
    } else {
        printf ("TREE: running multilevel search\n");
        return resrc_tree_search_multilevel (ids, found, req_res, available);
    }
}

const resrc_tree_t *resrc_phys_tree (const resrc_t *resrc)
{
    return (resrc_tree_t *)resrc;
}

resource_list_t *resrc_tree_children (const resrc_tree_t *resrc_tree)
{
    const resrc_t *r = (resrc_t *)resrc_tree;
    resource_list_t *list = resrc_new_id_list ();
    resrc_search_by_id (&r->resource_database, list, r->id);
    return list;
}

int64_t resrc_id (const resrc_t *resrc)
{
    return resrc->id;
}

/*
 * vi: ts=4 sw=4 expandtab
 */
