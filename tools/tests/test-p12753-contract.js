const assert=require('assert'),fs=require('fs'),path=require('path');
const read=p=>fs.readFileSync(path.join(__dirname,'../..',p),'utf8');
const h=read('drivers/power/exynos/hakios.p12753.h'),gov=read('drivers/power/exynos/hakios.exynos_cpufreq.c'),abi=read('xnu/files/hakios_p125_impl.h'),old=read('drivers/power/exynos/hakios.p125_sysctl.h'),host=read('tools/s9-p125-timer-serial.ps1'),dual=read('tools/s9-p12-dual-serial.ps1');
assert(read('drivers/map').includes('power/exynos bsd/dev/hakios.p12753.h'));
assert(gov.includes('#include "hakios.p12753.h"'));
assert(gov.includes('max=p12753_ceiling(c,max)'));
assert(h.includes('return cap?MIN(stock,cap):stock'));
assert(h.includes('PE_parse_boot_argn("p12753"'));
assert(h.includes('p1274_ticks_ms(connected?1000:20000)') && h.includes('p1274_ticks_ms(90000)'));
assert(h.includes('down>p12753_arm_at && down>up && fresh && !(fg&1u)'));
assert(h.indexOf('now-p12753_quiet<p1274_ticks_ms(connected?1000:20000)')<h.indexOf('p127_arm_mode((int)policy)'));
assert(h.includes('p12753_request==9 && p12753_bus<13'));
assert(h.includes('hakios_acpm_dvfs_get') && h.includes('hakios_acpm_dvfs_table'));
assert(!/hakios_acpm_dvfs_set|CPU_SUSPEND|CPU_OFF|DCS|ml_io_write|pmic_write/.test(h));
assert(h.includes('r->dpu!=UINT32_MAX && (r->dpu&15u)==15u?hakios_scanout_refreshes()'));
// Native build regression: exported machine_routines.h lacks ml_io_map.
// Match the existing MFC PMU mapper's ABI; declare before the first call.
const mapDecl=/extern\s+vm_offset_t\s+ml_io_map\(vm_offset_t(?:\s+\w+)?,\s*vm_size_t(?:\s+\w+)?\);/;
assert(mapDecl.test(read('drivers/media/mfc/hakios.exynos_mfc.c')));
const decl=h.match(mapDecl), call=h.indexOf('p12753_pmu=ml_io_map(EXYNOS_PMU_PA,0x5000)');
assert(decl && call>decl.index,'P12753 typed PMU mapping declaration missing before use');
assert(h.includes('p12753_policy_lease=orig') && h.includes('p12753_qualified(p12753_request)'));
assert(abi.includes('hakios_p12753_observe') && abi.includes('hakios_p12753_fg_link'));
assert(old.includes('Keep legacy hard guards'));
assert(read('drivers/power/exynos/hakios.p12752.h').includes('r.driver==15u'));
for(const s of ['Audit53Recover','Audit53Compare','P12753-Power','P12753-ReadAudit','P12753-Compare'])assert(host.includes(s));
for(const s of ['p12753_revision','p12753_guard','p12753_status','p12753_arm','p12753_select','p12753_page'])assert(dual.includes(s));
console.log('P12753 source contract: delayed ownership, lower-only cap, passive FG/WFI, bounded read-only bus, host recovery PASS (not native compile)');
