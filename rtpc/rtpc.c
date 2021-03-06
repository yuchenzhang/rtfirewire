/***
 *
 *  rtpc.c
 *
 *  adapted from RTnet <http://www.rts.uni-hannover.de/rtnet/>
 *
 *  Copyright (C) 2003 Jan Kiszka <jan.kiszka@web.de>
 * 			2005 Zhang Yuchen
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <asm/atomic.h>
#include <asm/uaccess.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/wait.h>

#include <rtpc.h>
#include <rt_serv.h>


static spinlock_t   pending_calls_lock = SPIN_LOCK_UNLOCKED;
static spinlock_t   processed_calls_lock = SPIN_LOCK_UNLOCKED;
static rtos_nrt_signal_t rtpc_nrt_signal;
struct rt_serv_struct *rtpc_dispatcher;
	
LIST_HEAD(pending_calls);
LIST_HEAD(processed_calls);


#ifndef __wait_event_interruptible_timeout
#define __wait_event_interruptible_timeout(wq, condition, ret)              \
do {                                                                        \
    wait_queue_t __wait;                                                    \
    init_waitqueue_entry(&__wait, current);                                 \
                                                                            \
    add_wait_queue(&wq, &__wait);                                           \
    for (;;) {                                                              \
        set_current_state(TASK_INTERRUPTIBLE);                              \
        if (condition)                                                      \
            break;                                                          \
        if (!signal_pending(current)) {                                     \
            ret = schedule_timeout(ret);                                    \
            if (!ret)                                                       \
                break;                                                      \
            continue;                                                       \
        }                                                                   \
        ret = -ERESTARTSYS;                                                 \
        break;                                                              \
    }                                                                       \
    current->state = TASK_RUNNING;                                          \
    remove_wait_queue(&wq, &__wait);                                        \
} while (0)
#endif

#ifndef wait_event_interruptible_timeout
#define wait_event_interruptible_timeout(wq, condition, timeout)            \
({                                                                          \
    long __ret = timeout;                                                   \
    if (!(condition))                                                       \
        __wait_event_interruptible_timeout(wq, condition, __ret);           \
    __ret;                                                                  \
})
#endif



int rtfw_rtpc_dispatch_call(rtpc_proc proc, unsigned int timeout,
                       void* priv_data, size_t priv_data_size,
                       rtpc_copy_back_proc copy_back_handler,
                       rtpc_cleanup_proc cleanup_handler)
{
    struct rt_proc_call *call;
    unsigned long       flags;
    int                 ret;

    call = kmalloc(sizeof(struct rt_proc_call) + priv_data_size, GFP_KERNEL);
    if (call == NULL) {
        if (call->cleanup_handler != NULL)
            call->cleanup_handler(priv_data);
        return -ENOMEM;
    }

    memcpy(call->priv_data, priv_data, priv_data_size);

    call->processed       = 0;
    call->proc            = proc;
    call->result          = 0;
    call->cleanup_handler = cleanup_handler;
    atomic_set(&call->ref_count, 2);    /* dispatcher + rt-procedure */
    init_waitqueue_head(&call->call_wq);

    rtos_spin_lock_irqsave(&pending_calls_lock, flags);
    list_add_tail(&call->list_entry, &pending_calls);
    rtos_spin_unlock_irqrestore(&pending_calls_lock, flags);

    rt_request_pend(rtpc_dispatcher, 0, 2000000, NULL, 0, NULL); //we make a delay of 2ms
    rt_serv_sync(rtpc_dispatcher);
	
    if (timeout > 0) {
        ret = wait_event_interruptible_timeout(call->call_wq,
            call->processed, (timeout * HZ) / 1000);
        if (ret == 0)
            ret = -ETIME;
    } else
        ret = wait_event_interruptible(call->call_wq, call->processed);
    
    if (ret >= 0) {
        if (copy_back_handler != NULL)
            copy_back_handler(call, priv_data);
        ret = call->result;
    }

    if (atomic_dec_and_test(&call->ref_count)) {
        if (call->cleanup_handler != NULL)
            call->cleanup_handler(&call->priv_data);
        kfree(call);
    }

    return ret;
}



static inline struct rt_proc_call *rtpc_dequeue_pending_call(void)
{
    unsigned long       flags;
    struct rt_proc_call *call;


    rtos_spin_lock_irqsave(&pending_calls_lock, flags);
    call = (struct rt_proc_call *)pending_calls.next;
    list_del(&call->list_entry);
    rtos_spin_unlock_irqrestore(&pending_calls_lock, flags);

    return call;
}



static inline void rtpc_queue_processed_call(struct rt_proc_call *call)
{
    unsigned long flags;


    rtos_spin_lock_irqsave(&processed_calls_lock, flags);
    list_add_tail(&call->list_entry, &processed_calls);
    rtos_spin_unlock_irqrestore(&processed_calls_lock, flags);

    rtos_nrt_signal_pend(&rtpc_nrt_signal);
}



static inline struct rt_proc_call *rtpc_dequeue_processed_call(void)
{
    unsigned long flags;
    struct rt_proc_call *call;


    rtos_spin_lock_irqsave(&processed_calls_lock, flags);
    if (!list_empty(&processed_calls)) {
        call = (struct rt_proc_call *)processed_calls.next;
        list_del(&call->list_entry);
    } else
        call = NULL;
    rtos_spin_unlock_irqrestore(&processed_calls_lock, flags);

    return call;
}



static void rtpc_dispatch_handler(unsigned long arg)
{
	struct rt_proc_call *call;
    int                 ret;
	
    if((call = rtpc_dequeue_pending_call())!=NULL)	
	ret = call->proc(call);
    else
	return;
        
    if (ret != -CALL_PENDING)
            rtpc_complete_call(call, ret);
}



static void rtpc_signal_handler(rtdm_nrtsig_t sig)
{
    struct rt_proc_call *call;


    while ((call = rtpc_dequeue_processed_call()) != NULL) {
        call->processed = 1;
        wake_up(&call->call_wq);

        if (atomic_dec_and_test(&call->ref_count)) {
            if (call->cleanup_handler != NULL)
                call->cleanup_handler(&call->priv_data);
            kfree(call);
        }
    }
}



void rtfw_rtpc_complete_call(struct rt_proc_call *call, int result)
{
    call->result = result;
    rtpc_queue_processed_call(call);
}



void rtfw_rtpc_complete_call_nrt(struct rt_proc_call *call, int result)
{
    call->processed = 1;
    wake_up(&call->call_wq);

    if (atomic_dec_and_test(&call->ref_count)) {
        if (call->cleanup_handler != NULL)
            call->cleanup_handler(&call->priv_data);
        kfree(call);
    }
}


int __init rtpc_init(void)
{
    int ret;
    unsigned char name[16];
    
    ret = rtos_nrt_signal_init(&rtpc_nrt_signal, rtpc_signal_handler);
    if(ret < 0)
	    return ret;
   
    sprintf(name, "rtfw_rtpc");
    rtpc_dispatcher = rt_serv_init(name, 1, rtpc_dispatch_handler, -1); 
    if(!rtpc_dispatcher){
	    rtdm_nrtsig_destroy(&rtpc_nrt_signal);
	    return -ENOMEM;
    }
    
    return 0;
}



void rtpc_cleanup(void)
{
    rt_serv_delete(rtpc_dispatcher);	
    rtos_nrt_signal_delete(&rtpc_nrt_signal);
}

module_init(rtpc_init);
module_exit(rtpc_cleanup);
MODULE_LICENSE("GPL");

EXPORT_SYMBOL(rtfw_rtpc_dispatch_call);
EXPORT_SYMBOL(rtfw_rtpc_complete_call);
EXPORT_SYMBOL(rtfw_rtpc_complete_call_nrt);

