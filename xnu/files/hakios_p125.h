/* P12.5 observation ABI. No power, timer or scheduling policy changes. */
#ifndef HAKIOS_P125_H
#define HAKIOS_P125_H
#include <stdint.h>
#define P125_CPUS 8u
#define P125_SLOTS 512u
#define P125_BANK_SLOTS 128u
#define P125_BANKS 4u
#define P125_KINDS 15u
#define P125_PAGE_ROWS 2u
#define P125_NCOUNT 28u
/* Count domains are separate; do not sum nested layers as IRQs. */
enum p125_counter {
    P125_IRQ27, P125_IRQ_QUEUE, P125_QUEUE_ZERO, P125_QUEUE_NONZERO,
    P125_QUEUE_CB, P125_OTHER_QUEUE, P125_OTHER_CB, P125_PAUSE,
    P125_POSTPONE, P125_LOCK_SKIP, P125_RATE_BREAK, P125_EMPTY,
    P125_ENTER, P125_REENTER, P125_LOCAL, P125_REQ_LEEWAY_ZERO,
    P125_EFF_LEEWAY_ZERO, P125_SHORT, P125_WAIT_ARMED, P125_WAIT_STALE,
    P125_DELAYED_READY, P125_WORKER, P125_MK_SEND, P125_DROP,
    P125_SETPOP, P125_POP_SHORT, P125_RAW_EARLY, P125_CANCEL
};
enum p125_kind {
    P125_K_QUEUE=1, P125_K_ENTER, P125_K_WAIT_ARM, P125_K_WAIT_EXPIRE,
    P125_K_DELAYED, P125_K_WORKER, P125_K_MK_ARM, P125_K_MK_SEND,
    P125_K_IOTIMER, P125_K_OTHER_QUEUE, P125_K_ASSIGN,
    P125_K_MK_CANCEL, P125_K_CANCEL, P125_K_WAIT_STALE
};
/* Keys immutable after release publication. No pointer identity for objects,
 * no reuse/reset while kernel is running. Count drops instead of evicting.
 * Addresses are unslid code only. owner/aux are type-specific scalar IDs. */
struct p125_row {
    uint32_t state, kind;
    uint64_t func, owner, aux;
    uint64_t count, short_n, reqzero, effzero, flags_or, sample_n, wall_ticks;
};
extern uint64_t hakios_p125_counts[P125_CPUS][P125_NCOUNT];
extern struct p125_row hakios_p125_rows[P125_CPUS][P125_SLOTS];
extern uint64_t hakios_p125_pages[P125_CPUS][P125_BANKS];
extern uint64_t hakios_p125_kind_total[P125_CPUS][P125_KINDS];
extern uint64_t hakios_p125_kind_drop[P125_CPUS][P125_KINDS];
extern uint64_t hakios_p125_thread_map_drops;
extern uint64_t hakios_p125_intervals[P125_CPUS][5];
extern uint64_t hakios_p125_lease, hakios_p125_start, hakios_p125_end;
extern uint32_t hakios_p125_stop_reason;
extern uint64_t hakios_p125_usb_down_at,hakios_p125_usb_up_at;
extern uint64_t hakios_p125_display_events;
/* 0 off, 1 aggregate, 2 detail, 3 USB-offline detail (pending until link-down).
 * Lease encodes expiry and arm mode. active() returns 0/1/2. */
#ifdef __cplusplus
extern "C" {
#endif
unsigned hakios_p125_active(void);
unsigned hakios_p125_cpu(void);
uint64_t hakios_p125_code(uintptr_t);
void hakios_p125_inc(unsigned cpu,unsigned idx,uint64_t n);
void hakios_p125_interval(unsigned cpu,uint64_t ticks);
unsigned hakios_p125_short(uint64_t ticks);
void hakios_p125_record(unsigned cpu,unsigned kind,uint64_t func,
    uint64_t owner,uint64_t aux,uint64_t delta,uint64_t requested_leeway,
    uint64_t effective_leeway,uint32_t flags);
/* Sampling token references immutable slot. Zero means not sampled. */
uint32_t hakios_p125_sample(unsigned cpu,unsigned kind,uint64_t func,
    uint64_t owner,uint64_t aux);
void hakios_p125_wall(uint32_t token,uint64_t elapsed);
void hakios_p125_owner(void *thread,uint64_t *owner,uint64_t *aux,uint64_t *site);
uint64_t hakios_p125_task_id(void *task);
void hakios_p125_thread_register(uint64_t tid,uintptr_t entry);
void hakios_p125_usb_event(unsigned connected);
void hakios_p125_display_event(unsigned on);
#ifdef __cplusplus
}
#endif
#endif
