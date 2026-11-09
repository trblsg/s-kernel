#!/usr/bin/env python3
"""edit-prefs.py ROOT : mobile preferences of the full root (idempotent; mobile:mobile 501:501, 0600).
  Accessibility: Reduce Motion / Reduce Transparency / increased contrast / slide-transition reduction all OFF (real iOS 27 look)
  .GlobalPreferences + PosterBoard: CAEnableIOSurfaceCompression / CAEnableFramebufferCompression = NO (the S9 GPU path has no AFBC)
  springboard + BacklightServices: watchdogs off (a missed check-in must not kill SpringBoard / panic)
"""
import os, plistlib, sys
P = sys.argv[1] + '/private/var/mobile/Library/Preferences'
os.makedirs(P, exist_ok=True); os.chown(sys.argv[1] + '/private/var/mobile', 501, 501)
def setkeys(name, kv, mode=0o600):
    p = P + '/' + name
    try: d = plistlib.load(open(p, 'rb'))
    except (FileNotFoundError, plistlib.InvalidFileException): d = {}
    d.update(kv)
    open(p, 'wb').write(plistlib.dumps(d, fmt=plistlib.FMT_BINARY)); os.chmod(p, mode); os.chown(p, 501, 501)
    print(f'{name}: {sorted(kv)}')
setkeys('com.apple.Accessibility.plist', {'ReduceMotionEnabled': False, 'ReduceTransparencyEnabled': False,
        'EnhancedBackgroundContrastEnabled': False, 'ReduceMotionReduceSlideTransitionsPreference': False,
        'ReduceMotionAutoplayAnimatedImagesEnabled': False}, 0o644)
setkeys('.GlobalPreferences.plist', {'CAEnableFramebufferCompression': False, 'CAEnableIOSurfaceCompression': False})
setkeys('com.apple.PosterBoard.plist', {'CAEnableDeepFramebuffer': False, 'CAEnableFramebufferCompression': False,
        'CAEnableIOSurfaceCompression': False})
setkeys('com.apple.springboard.plist', {'BKNoWatchdogs': True, 'disableWatchdog': True, 'FBDisableWatchdog': True, 'SBDisableWatchdog': True})
setkeys('com.apple.BacklightServices.plist', {'backlightWatchdogsDisabled': True, 'BacklightWatchdogsDisabled': True,
        'ignoreWatchdogPanicRequests': True})
