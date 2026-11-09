#!/bin/bash
# 50-xpc-disable.sh ROOT : MessagesBlastDoorService.xpc (SIGTRAP loop in QuartzCore, spawned via imagent/IMCore) renamed, not deleted.
set -euo pipefail
X=$1/System/Library/PrivateFrameworks/MessagesBlastDoorSupport.framework/XPCServices
if [[ -d $X/MessagesBlastDoorService.xpc ]]; then mv "$X/MessagesBlastDoorService.xpc" "$X/MessagesBlastDoorService.xpc.disabled"; echo "MessagesBlastDoorService.xpc disabled"; else echo "MessagesBlastDoorService.xpc already disabled/absent"; fi
