#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <uuid/uuid.h>
#include <czmq.h>

#include "../resrc.h"
#include "../resrc_tree.h"
#include "src/common/libutil/jsonutil.h"
#include "src/common/libutil/xzmalloc.h"

#include <sys/time.h>
#include <sys/types.h>

static struct timeval start_time;

void init_time() {
              gettimeofday(&start_time, NULL);
}

u_int64_t get_time() {
    struct timeval t;
    gettimeofday(&t, NULL);
    return (u_int64_t) (t.tv_sec - start_time.tv_sec) * 1000000
        + (t.tv_usec - start_time.tv_usec);
}

int main (int argc, char** argv)
{
    /* char *resrc_id; */
    const char *filename = argv[1];
    int found = 0;
    resources_t *resrcs;
    resource_list_t *found_res = resrc_new_id_list ();

    if (filename == NULL || *filename == '\0')
        filename = getenv ("TESTRESRC_INPUT_FILE");

    init_time();
    resrcs = resrc_generate_resources (filename, "default");
    printf("resource generation took: %lf\n", ((double)get_time())/1000000);
    printf ("starting\n");
    /* resrc_print_resources (resrcs); */
    init_time();
    JSON child_sock = Jnew();
    JSON child_core = Jnew();
    JSON req_res = Jnew ();
    Jadd_str (child_core, "type", "core");
    Jadd_int (child_core, "req_qty", 6);

    Jadd_str (child_sock, "type", "socket");
    Jadd_int (child_sock, "req_qty", 4);
    json_object_object_add (child_sock, "req_child", child_core);

    Jadd_str (req_res, "type", "node");
    Jadd_int (req_res, "req_qty", 2);
    json_object_object_add (req_res, "req_child", child_sock);

    found = resrc_search_flat_resources (resrcs, found_res, req_res, false);
    found = resrc_tree_search (found_res, found_res,
                               req_res, false);

    Jput(req_res);
    printf ("found %d nodes\n", found);
    if (found) {
        const resrc_t * resrc = resrc_list_first (found_res);
        while (resrc) {
            printf ("resrc_id %ld\n", resrc_id(resrc));
            resrc = resrc_list_next (found_res);
        }
    }
    printf("find and scan took: %lf\n", ((double)get_time())/1000000);

    init_time();
    resrc_allocate_resources (resrcs, found_res, 1);
    resrc_allocate_resources (resrcs, found_res, 2);
    resrc_allocate_resources (resrcs, found_res, 3);
    resrc_reserve_resources (resrcs, found_res, 4);
    printf ("allocated\n");
    printf("allocate and reserve took: %lf\n", ((double)get_time())/1000000);
    /* resrc_print_resources (resrcs); */
    init_time();
    resrc_release_resources (resrcs, found_res, 1);
    printf ("released\n");
    printf("release took: %lf\n", ((double)get_time())/1000000);
    init_time();
    resrc_id_list_destroy (found_res);
    /* resrc_print_resources (resrcs); */

    resrc_destroy_resources (&resrcs);
    printf("destroy took: %lf\n", ((double)get_time())/1000000);

    return 0;
}

