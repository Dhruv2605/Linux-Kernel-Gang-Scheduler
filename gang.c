// #include "linux/sched.h"
#include "asm/current.h"
#include "asm/uaccess.h"
#include "gang_sched.h"
#include "linux/compiler.h"
#include "linux/container_of.h"
#include "linux/cpumask.h"
#include "linux/gfp_types.h"
#include "linux/list.h"
#include "linux/sched.h"
#include "linux/sched/task.h"
#include "linux/slab.h"
#include "linux/smp.h"
#include "linux/spinlock.h"
#include "linux/spinlock_types.h"
#include "linux/types.h"
#include <linux/energy_model.h>
#include <linux/mmap_lock.h>
#include <linux/hugetlb_inline.h>
#include <linux/jiffies.h>
#include <linux/mm_api.h>
#include <linux/highmem.h>
#include <linux/spinlock_api.h>
#include <linux/cpumask_api.h>
#include <linux/lockdep_api.h>
#include <linux/softirq.h>
#include <linux/refcount_api.h>
#include <linux/topology.h>
#include <linux/sched/clock.h>
#include <linux/sched/cond_resched.h>
#include <linux/sched/cputime.h>
#include <linux/sched/isolation.h>
#include <linux/sched/nohz.h>

#include <linux/cpuidle.h>
#include <linux/interrupt.h>
#include <linux/memory-tiers.h>
#include <linux/mempolicy.h>
#include <linux/mutex_api.h>
#include <linux/profile.h>
#include <linux/psi.h>
#include <linux/ratelimit.h>
#include <linux/task_work.h>
#include <linux/rbtree_augmented.h>

#include <asm/switch_to.h>

#include "sched.h"

const struct sched_class gang_sched_class;

#define PID_NOT_FOUND -23
#define CPUS_EXCEEDED -22



struct list_head gang_entries;

struct gang_entry {
    int gang_id;
    int size;
    struct list_head next_prev;
    struct list_head thread_entries;
};

struct gang_thread_entry{
    int pid;
    struct list_head next_prev;
};

rwlock_t gang_entries_lock;

void init_gang_scheduler(void){
    INIT_LIST_HEAD(&gang_entries);
    rwlock_init(&gang_entries_lock);
}

static void add_thread_to_gang_entry(struct gang_entry *gang_entry, int pid){
    struct gang_thread_entry *gang_thread_entry=kzalloc(sizeof(struct gang_thread_entry),GFP_KERNEL);
    gang_thread_entry->pid=pid;
    list_add(&gang_thread_entry->next_prev,&gang_entry->thread_entries);
    gang_entry->size++;
}

static void add_gang_thread_entry(int gangId, int pid){
    struct gang_entry *gang_entry;
    list_for_each_entry(gang_entry, &gang_entries, next_prev){
        if(gang_entry->gang_id==gangId){
            add_thread_to_gang_entry(gang_entry,pid);
            return;
        }
    }

    /* Gang entry not in list */
    gang_entry=kzalloc(sizeof(struct gang_entry),GFP_KERNEL);
    gang_entry->gang_id=gangId;
    INIT_LIST_HEAD(&gang_entry->thread_entries);
    list_add(&gang_entry->next_prev, &gang_entries);
    add_thread_to_gang_entry(gang_entry, pid);
}

static void remove_thread_from_gang_entry(struct gang_entry *gang_entry, int pid){
    struct gang_thread_entry *gang_thread_entry;
    list_for_each_entry(gang_thread_entry, &gang_entry->thread_entries, next_prev){
        if(gang_thread_entry->pid==pid){
            list_del(&gang_thread_entry->next_prev);
            gang_entry->size--;
            return;
        }
    }
}

static void remove_gang_thread_entry(int gangId, int pid){
    struct gang_entry *gang_entry;
    list_for_each_entry(gang_entry, &gang_entries, next_prev){
        if(gang_entry->gang_id==gangId){
            remove_thread_from_gang_entry(gang_entry,pid);
            if(gang_entry->size==0){
                /* all entries are gone */
                list_del(&gang_entry->next_prev);
            }
            return;
        }
    }
}

static int num_tasks_in_gang(int gangId){
    struct gang_entry *gang_entry;
    list_for_each_entry(gang_entry, &gang_entries, next_prev){
        if(gang_entry->gang_id==gangId){
            return gang_entry->size;
        }
    }
    return 0;
}

/* must be called with rq lock help*/
static int switch_to_gang_sched(struct task_struct *p,struct rq *rq, int gang_id,int exec_time){
    write_lock(&gang_entries_lock);
    int num_registered_tasks_to_gang=num_tasks_in_gang(gang_id);
    if(num_registered_tasks_to_gang>=num_possible_cpus()) {
        write_unlock(&gang_entries_lock);
        return CPUS_EXCEEDED;
    }
    add_gang_thread_entry(gang_id, p->pid);

    bool queued=task_on_rq_queued(p);
    bool running=rq->curr==p;

    if(queued){
        dequeue_task(rq, p,DEQUEUE_MOVE| DEQUEUE_SAVE);
    }
    if(running){
        p->sched_class->put_prev_task(rq,p,NULL);
    }
    
    const struct sched_class *prev_class=p->sched_class;
    p->sched_class=&gang_sched_class;
    p->policy=SCHED_GANG;
    p->gang.gang_id=gang_id;
    p->gang.exec_time=((u64)exec_time)*1000ull*1000ull*1000ull; /* exec_time is provided in seconds but we measure in ns*/
    p->gang.runtime=0; /* Required as the task might register back again */

    check_class_changing(rq, p, prev_class);
    if(queued){
        enqueue_task(rq, p, ENQUEUE_MOVE | ENQUEUE_RESTORE);
    }
    if(running){
        if(rq->cpu==0) rq->gang.ipi_time=0; /* This should force the ipi by governor to be sent on set_next_task below */
        set_next_task(rq, p);
    }
    check_class_changed(rq, p, prev_class, p->prio);
    write_unlock(&gang_entries_lock);

    return 0;
}

SYSCALL_DEFINE3(register_gang,int,pid,int,gangid,int,exec_time)
{
    struct task_struct *p=find_get_task_by_vpid(pid);
    if(!p){
        return PID_NOT_FOUND;
    }

    struct rq_flags rf_flags;
    struct rq *rq=task_rq_lock(p,&rf_flags);

    int retvalue=switch_to_gang_sched(p, rq, gangid, exec_time);

    task_rq_unlock(rq, p, &rf_flags);
    put_task_struct(p);

	return retvalue;
}

/* must be called with rq lock help*/
static void switch_to_fair_sched(struct task_struct *p,struct rq *rq){
    bool queued=task_on_rq_queued(p);
    bool running=rq->curr==p;

    if(queued){
        dequeue_task(rq, p,DEQUEUE_MOVE| DEQUEUE_SAVE);
    }
    if(running){
        p->sched_class->put_prev_task(rq,p,NULL);
    }
    
    const struct sched_class *prev_class=p->sched_class;
    p->sched_class=&fair_sched_class;
    p->policy=SCHED_NORMAL;
    check_class_changing(rq, p, prev_class);
    if(queued){
        enqueue_task(rq, p, ENQUEUE_MOVE | ENQUEUE_RESTORE);
    }

    if(running){
        set_next_task(rq, p);
    }
    check_class_changed(rq, p, prev_class, p->prio);
    
    write_lock(&gang_entries_lock);
    remove_gang_thread_entry(p->gang.gang_id, p->pid);
    write_unlock(&gang_entries_lock);

}

SYSCALL_DEFINE1(exit_gang,int,pid)
{
    struct task_struct *p=find_get_task_by_vpid(pid);
    if(!p){
        return PID_NOT_FOUND;
    }

    struct rq_flags rf_flags;
    struct rq *rq=task_rq_lock(p,&rf_flags);

    switch_to_fair_sched(p, rq);

    task_rq_unlock(rq, p, &rf_flags);
    put_task_struct(p);

	return 0;
}

SYSCALL_DEFINE2(list,int,gang_Id,int*,pids)
{
    read_lock(&gang_entries_lock);
    struct gang_entry * gang_entry;
    list_for_each_entry(gang_entry, &gang_entries, next_prev){
        if(gang_entry->gang_id==gang_Id){

            struct gang_thread_entry *gang_thread_entry;
            list_for_each_entry(gang_thread_entry, &gang_entry->thread_entries, next_prev){
                put_user(gang_thread_entry->pid, pids++);
            }
            read_unlock(&gang_entries_lock);
            return gang_entry->size;
        }
    }
    read_unlock(&gang_entries_lock);
	return 0;
}

static void gang_start_call(void *info){
    unsigned long long gangId=(unsigned long long)info;
    int cpu=smp_processor_id();
    cpu_rq(cpu)->gang.ipi_time=sched_clock();
    cpu_rq(cpu)->gang.gang_id_to_pick=gangId;
    struct rq *rq=cpu_rq(cpu);

    if(rq->gang.registered_cnt){
        if(sched_class_above(&gang_sched_class,current->sched_class) || (current->sched_class==&gang_sched_class && current->gang.gang_id!=gangId)){
            resched_curr(cpu_rq(cpu));
        }
    }
}


/* Called per cpu to initialize its gang queue */
void init_gang_rq(struct gang_rq *gang_rq){
    INIT_LIST_HEAD(&gang_rq->run_list);
}

static void enqueue_task_gang(struct rq *rq,struct task_struct *p, int flags){
    struct sched_gang_entity *gang_se=&p->gang;
    list_add(&gang_se->run_list, &rq->gang.run_list);
    add_nr_running(rq, 1);
}

static inline struct task_struct * task_of_ge(struct sched_gang_entity *ge){
    return container_of(ge, struct task_struct, gang);
}

static inline struct sched_gang_entity * gang_entity_of_list(struct list_head *list){
    return container_of(list, struct sched_gang_entity, run_list);
}

static bool dequeue_task_gang(struct rq *rq,struct task_struct *p, int flags){
    struct list_head *gang_entity_lh;
    struct list_head *gang_entity_nxt_lh;
    list_for_each_safe(gang_entity_lh, gang_entity_nxt_lh, &rq->gang.run_list)
    {
        struct sched_gang_entity *gang_entity=gang_entity_of_list(gang_entity_lh);
        struct task_struct *gang_task=task_of_ge(gang_entity);
        if(gang_task==p){
            list_del(gang_entity_lh);
            break;
        }
    }
    sub_nr_running(rq, 1);
    return true;
}

static void round_robbin_role_queue(struct gang_rq *gang_rq){
    if(likely(!list_empty(&gang_rq->run_list))){
        struct list_head *first_element=gang_rq->run_list.next;
        list_del(first_element); // remove first element
        list_add_tail(first_element, &gang_rq->run_list); // add the element back at end
    }
}

const u64 time_quanta_ns=10*1000*1000; // 10 ms
static struct task_struct * pick_task_gang(struct rq *rq){
    struct gang_rq *gang_rq=&rq->gang;
    u64 current_sched_clock_time=sched_clock();
    bool is_gang_governor=rq->cpu==0;
    if(is_gang_governor){
        if(unlikely(!list_empty(&gang_rq->run_list))){
            if((current_sched_clock_time-gang_rq->queue_rotate_time)>time_quanta_ns){
                round_robbin_role_queue(gang_rq);
                gang_rq->queue_rotate_time=current_sched_clock_time;
            }
            return task_of_ge(gang_entity_of_list(gang_rq->run_list.next));
        }
    }
    else if(unlikely((current_sched_clock_time-gang_rq->ipi_time)<time_quanta_ns)){
        struct sched_gang_entity *ge;
        list_for_each_entry(ge, &gang_rq->run_list,run_list)
        {
            if(ge->gang_id==gang_rq->gang_id_to_pick){
                struct task_struct *gang_task=task_of_ge(ge);
                return gang_task;
            }
        }
    }
    return NULL;
}

static void yield_task_gang(struct rq *rq)
{
    // We don't need to do anything here
}

static void wakeup_preempt_gang(struct rq *rq, struct task_struct *p, int flags)
{   
    if(rq->gang.registered_cnt && (rq->cpu==0 || (sched_clock()-rq->gang.ipi_time)<time_quanta_ns) && sched_class_above(&gang_sched_class,rq->curr->sched_class)){
        resched_curr(rq);
    }
}
static void put_prev_task_gang(struct rq *rq, struct task_struct *p, struct task_struct *next)
{
    /* gang thread put back */
    update_curr_common(rq);

}
static inline void set_next_task_gang(struct rq *rq, struct task_struct *p, bool first)
{
    p->se.exec_start=rq_clock_task(rq);
    u64 current_sched_clock_time=sched_clock();
    if(rq->cpu==0){
        /* Started execution of gang governor task*/
        if(current_sched_clock_time-rq->gang.ipi_time>time_quanta_ns)
        {
            rq->gang.ipi_time=current_sched_clock_time;
            smp_call_function(gang_start_call, (void *)((unsigned long long)p->gang.gang_id), false);
        }
    }
}



static int select_task_rq_gang(struct task_struct *p, int cpu, int flags)
{
    return cpu;
}

static void update_curr_gang(struct rq *rq)
{
    if(rq->curr->sched_class!=&gang_sched_class) return;

    rq->curr->gang.runtime+=update_curr_common(rq);
}
static void task_tick_gang(struct rq *rq, struct task_struct *p, int queued)
{
    update_curr_gang(rq);

    /* check if the time limit is exceeded for the gang */
    if(p->gang.runtime > p->gang.exec_time){
        printk("pid %d gangId %d exceeded its registered gang exec_time limit of %lld ns by using %lld ns by %lld ns",p->pid,p->gang.gang_id,p->gang.exec_time,p->gang.runtime,(p->gang.runtime-p->gang.exec_time));
        switch_to_fair_sched(p, rq);
        printk("pid %d gangId %d moved to fair scheduler\n",p->pid,p->gang.gang_id);
        
        resched_curr(rq);
    }

    if(rq->cpu==0 && sched_clock()-rq->gang.queue_rotate_time>time_quanta_ns){
        resched_curr(rq);
    }

}

static void switched_to_gang(struct rq *rq, struct task_struct *p)
{
    rq->gang.registered_cnt++;
}
static void switched_from_gang(struct rq *rq, struct task_struct *p)
{
    rq->gang.registered_cnt--;
}

DEFINE_SCHED_CLASS(gang)={
    .enqueue_task       = enqueue_task_gang,
    .dequeue_task       = dequeue_task_gang,
	.yield_task		    = yield_task_gang,

	.wakeup_preempt		= wakeup_preempt_gang,

    .pick_task          = pick_task_gang,
	.put_prev_task		= put_prev_task_gang,
    .set_next_task      = set_next_task_gang,

#ifdef CONFIG_SMP
	.select_task_rq		= select_task_rq_gang,
	.set_cpus_allowed	= set_cpus_allowed_common,
#endif

	.task_tick		    = task_tick_gang,

	.switched_to		= switched_to_gang,
	.switched_from		= switched_from_gang,
	.update_curr		= update_curr_gang,
};
