
#ifndef GANG_RQ
#define GANG_RQ

#include "linux/types.h"
struct gang_rq{
    u64 ipi_time;
    u64 queue_rotate_time;
    int gang_id_to_pick;
    struct list_head run_list;
    int registered_cnt; /* This is not the same the as the size of run_list due to enqueue dequeues in case of sleep ..etc */
};
#endif
