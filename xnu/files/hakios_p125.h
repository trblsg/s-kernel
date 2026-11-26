/* P12.5 observation ABI. No power, timer or scheduling policy changes. */
#ifndef HAKIOS_P125_H
#define HAKIOS_P125_H
#include <stdint.h>
#include <stdbool.h>
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
/* Fixed OFFLINE window: active only for the new opt-in mode3 path. */
extern uint64_t hakios_p1273_offline_duration,hakios_p1273_offline_deadline;
extern uint32_t hakios_p1273_offline_fixed;
extern uint64_t hakios_p125_display_events;
/* P12.6 independent OFF-only experiments; lease low 6 bits = mask. */
#define P126_HAPTIC 1u
#define P126_WIFI_WORK 2u
#define P126_GNSS 4u
#define P126_TOUCH 8u
#define P126_TOUCH_LEEWAY 16u
#define P126_SHUB 32u
#define P126_MASK 63u
extern uint64_t hakios_p126_lease;
extern uint32_t hakios_p126_reason;
extern uint64_t hakios_p126_hits[6];
extern uint64_t hakios_p126_diag[9];
extern uint32_t hakios_p126_audio_state;
/* P12.7 independent lease, baseline P126 mask15, USB quiet, ABOX, PSR,
 * floor selector 0..5 in bits4..6. No CPU power-state changes. */
#define P127_BASE 1u
#define P127_USB 2u
#define P127_ABOX 4u
#define P127_PSR 8u
#define P127_WIFI 128u
#define P127_BT 256u
#define P127_TICKER 512u
#define P127_MASK 1023u
#define P127_FLOOR(M) (((M)>>4)&7u)
extern uint64_t hakios_p127_lease;
extern uint32_t hakios_p127_reason,hakios_p127_offline_aggregate;
extern uint64_t hakios_p127_wfi_n[8][9],hakios_p127_wfi_ticks[8][9],hakios_p127_bounds[8];
extern uint64_t hakios_p127_usb[5],hakios_p127_abox[4],hakios_p127_psr[4],hakios_p127_floor[2];
extern uint64_t hakios_p127_irq[8][3],hakios_p127_wifi[5],hakios_p127_bt[6],hakios_p127_ticker[3];
extern uint32_t hakios_p127_audio_event;
/* Functional qualification events, not workload generators. While a gate
 * is pending, separate events during the policy from post-restore events. */
enum p1273_event {
    P1273_USB_DOWN,P1273_USB_UP,P1273_AUDIO_FEED,P1273_AUDIO_CAPTURE,
    P1273_BT_RX,P1273_BT_TX,P1273_BT_ERROR,P1273_WIFI_RX,P1273_WIFI_TX,
    P1273_WIFI_HANDLED,P1273_TICK_CANCEL,P1273_TICK_FAIL,P1273_FLOOR_READBACK,P1273_FLOOR_STOCK,P1273_AUDIO_DMA,
    P1273_EVENT_COUNT
};
extern uint32_t hakios_p1273_gate_watch;
extern uint64_t hakios_p1273_events[2][P1273_EVENT_COUNT];
extern uint64_t hakios_p1273_last_stop_lease,hakios_p1273_fault_seq;
extern uint32_t hakios_p1273_last_stop_reason;
#define P127_FG_SLOTS 64u
struct p127_fg_row {uint64_t seq,ticks; int32_t ua,avg_ua; uint32_t uv,external,charger,qh;};
extern struct p127_fg_row hakios_p127_fg_rows[P127_FG_SLOTS];
extern uint64_t hakios_p127_fg_seq;
/* P12.7.3 bounded recount trace. State 0 empty, 1 preparing, 2 active,
 * 3 freezing, 4 frozen, 5 reader owns frozen storage. No policy writes. */
#define P1273_TRACE_BUCKETS 128u
#define P1273_TRACE_OWNERS 16u
struct p1273_trace_owner {
    uint64_t tid,task,user_ticks,system_ticks;
    uint32_t pid,kind,fragments,switches;
};
struct p1273_trace_bucket {
    uint64_t generation,bin,dropped_ticks;
    uint32_t mask,dropped;
    struct p1273_trace_owner owner[P1273_TRACE_OWNERS];
};
extern uint32_t hakios_p1273_trace_state,hakios_p1273_trace_reason;
extern uint64_t hakios_p1273_trace_generation,hakios_p1273_trace_start,hakios_p1273_trace_until;
extern uint64_t hakios_p1273_trace_trigger,hakios_p1273_trace_end,hakios_p1273_trace_span;
extern uint64_t hakios_p1273_trace_overhead[8],hakios_p1273_trace_clipped[8];
extern struct p1273_trace_bucket hakios_p1273_trace_rows[8][P1273_TRACE_BUCKETS];
/* Diagnostic indexes: BT idle/busy/RX/TTY TX/error, ABOX quiet/busy, SHUB quiet/busy. */
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
unsigned hakios_p126_active(unsigned bit);
void hakios_p126_hit(unsigned index);
void hakios_p126_stop(unsigned reason);
void hakios_p126_diag_add(unsigned index,uint64_t count);
void hakios_p126_audio_set(unsigned state);
unsigned hakios_p126_audio_get(void);
unsigned hakios_p127_mode(void);
void hakios_p127_stop(unsigned reason);
void hakios_p127_wfi(unsigned cpu,uint64_t ticks);
void hakios_p127_metric(unsigned group,unsigned index,uint64_t value);
void hakios_p127_audio_wake(void);
void hakios_p1273_event(unsigned event,uint64_t count);
void hakios_p127_fg_record(int32_t ua,int32_t avg_ua,uint32_t uv,unsigned external,unsigned charger,unsigned qh);
bool hakios_p1273_trace_arm(uint64_t start,uint64_t until,uint64_t bucket_span);
void hakios_p1273_trace_stop(unsigned reason,uint64_t now);
void hakios_p1273_recount(void *thread,void *task,uint64_t end,uint64_t ticks,bool from_user,bool switched);
bool hakios_p1273_trace_read_begin(void);
void hakios_p1273_identity(uint64_t *tid,uint64_t *task,uint32_t *pid);
void hakios_p1273_trace_read_end(void);
#ifdef __cplusplus
}
#endif
#endif
