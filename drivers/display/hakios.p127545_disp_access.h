/* Scope fence for all Exynos9810 display MMIO entry points.
 * Never sleep or invoke ACPM in a scanout/IRQ/PSR scope. A denied
 * demand asks the owner to restore first; passive reads are unavailable.
 * The cleanup attribute releases the scope on every early return.
 */
#ifndef HAKIOS_P127545_DISP_ACCESS_H
#define HAKIOS_P127545_DISP_ACCESS_H
#include <stdbool.h>
extern bool hakios_p545_disp_enter(bool demand);
extern void hakios_p545_disp_leave(void);
static inline void p545_disp_scope_exit(bool *held){
    if(*held)hakios_p545_disp_leave();
}
#define P545_DISP_GUARD(demand, failure) \
    bool p545_scope __attribute__((cleanup(p545_disp_scope_exit)))=hakios_p545_disp_enter(demand); \
    if(!p545_scope)return failure
#define P545_DISP_GUARD_VOID(demand) \
    bool p545_scope __attribute__((cleanup(p545_disp_scope_exit)))=hakios_p545_disp_enter(demand); \
    if(!p545_scope)return
#endif
