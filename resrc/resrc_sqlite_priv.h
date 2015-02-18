#ifndef __RESRC_SQLITE_PRIV_H
#define __RESRC_SQLITE_PRIV_H

#include "resrc.h"

#define SQLITE_CHECK_EXPECT(db, f, x)\
    do{                                                        \
        int i;                                                   \
        i = (f);                                                 \
        if (i != x) {                                            \
            fprintf (stderr, __FILE__ ":%d:"                       \
                    "%s failed with status %d: %s\n",     \
                    __LINE__, #f, i, sqlite3_errmsg (db));\
            exit (i);                                              \
        }                                                        \
    }while(0)

#define SQLITE_CHECK(db, f)\
    SQLITE_CHECK_EXPECT(db, f, SQLITE_OK)


struct resources{
    struct sqlite3 * db;
};

struct resource_list{
    struct sqlite3 * db;
    struct sqlite3_stmt * stmt;
    const char * query;
};

typedef struct {
    char *type;
    int64_t items;
} resrc_pool_t;

struct resrc{
    int64_t id;
    int64_t local_id;
    const char *type;
    const char *name;
    bool pool;
    int64_t pool_size;
    int64_t parent;
    int64_t max_jobs;
    uuid_t uuid;
    resource_state_t state;
};

struct resrc_tree {
  int64_t root_id;
};

#endif /* __RESRC_SQLITE_PRIV_H */
