import json, sys
man = json.load(open(sys.argv[1] if len(sys.argv) > 1 else 'manifest.json'))['files']
staged = {p: e for p, e in man.items() if e['status'] == 'staged'}
L1 = ['/usr/libexec/aonsensed', '/usr/libexec/safetycheckd', '/System/Library/PrivateFrameworks/PhotosFace.framework/photosfaced']
L1 = [p for p in staged if p.endswith(('/aonsensed', '/safetycheckd', '/photosfaced'))]
HOLD_EXACT = {
    '/sbin/launchd': 'PID 1',
    '/usr/lib/dyld': 'the dynamic linker, kernel-loaded into every process (A mode, 183 sites)',
    '/usr/lib/system/introspection/libdispatch.dylib': 'dyld-adjacent: substituted for libdispatch via DYLD_LIBRARY_PATH',
    '/usr/lib/libglInterpose.dylib': 'GL interpose library injected into GPU clients (9741 sites, A mode)',
    '/private/preboot/Cryptexes/OS/usr/lib/libglInterpose.dylib': 'cryptex copy of libglInterpose',
    '/usr/lib/libMetalMetricsInterpose.dylib': 'Metal interpose library injected into GPU clients',
    '/private/preboot/Cryptexes/OS/usr/lib/libMetalMetricsInterpose.dylib': 'cryptex copy of libMetalMetricsInterpose',
}
HOLD_NAMES = {n: why for n, why in {
    'logd': 'unified logging daemon: every process talks to it, launchd waits on it',
    'trustd': 'Security trust evaluation: needed by nearly every networked/system process',
    'fseventsd': 'filesystem events: early boot daemon',
    'watchdogd': 'system watchdog (a crash loop can reboot the phone)',
    'thermalmonitord': 'thermal manager: boot critical on a real device',
    'syslogd': 'early boot logging',
    'mDNSResponder': 'DNS/network name resolution, needed by every network client',
    'wifid': 'Wi-Fi manager (the only link of this phone)',
    'bluetoothd': 'Bluetooth manager',
    'lockdownd': 'host pairing/lockdown service',
    'installd': 'app installation',
    'mobileactivationd': 'activation state: gates SpringBoard setup flow',
    'diskimagesiod': 'disk image I/O (A mode, 758 sites)',
    'diskimagespawner': 'disk image service launcher',
    'locationd': 'CoreLocation: 2717 sites (A mode), many clients',
    'CommCenter': 'telephony daemon: 26791 sites (A mode), biggest file',
    'pipelined': '2652 sites (A mode)',
    'MapsOfflineService': '3159 sites, 40 MB (A mode)',
}.items()}
hold = {}
for p in staged:
    n = p.rsplit('/', 1)[1]
    if p in HOLD_EXACT: hold[p] = HOLD_EXACT[p]
    elif n in HOLD_NAMES and p.startswith(('/usr/libexec', '/usr/sbin', '/System/Library/Frameworks/CoreTelephony', '/System/Library/PrivateFrameworks/GeoServices')): hold[p] = HOLD_NAMES[n]
    elif p.startswith(('/System/Library/Extensions/', '/System/Library/DriverExtensions/', '/System/DriverKit/')):
        hold[p] = 'kernel-extension / DriverKit file: loaded by kxld/driverkitd, not by dyld (different loader)'
l2 = sorted(p for p in staged if p not in hold)
l3 = sorted(hold)
open('list-1.txt', 'w').write('\n'.join(sorted(L1)) + '\n')
open('list-2.txt', 'w').write('\n'.join(l2) + '\n')
open('list-3.txt', 'w').write('\n'.join(l3) + '\n')
json.dump(hold, open('list-3-reasons.json', 'w'), indent=1)
print('list-1', len(L1), [ (p, staged[p]['sites'], staged[p]['mode']) for p in L1])
print('list-2', len(l2), 'sites', sum(staged[p]['sites'] for p in l2), 'modes', {m: sum(1 for p in l2 if staged[p]['mode'] == m) for m in ('T', 'A')})
print('list-3', len(l3), 'sites', sum(staged[p]['sites'] for p in l3))
for p in l3: print('  hold', staged[p]['sites'], staged[p]['mode'], p.rsplit('/',1)[1])
