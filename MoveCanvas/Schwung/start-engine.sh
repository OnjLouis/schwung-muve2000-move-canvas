#!/bin/sh
# Both arguments are absolute paths supplied by the native worker.
set -eu
umask 077
runtime=$1
instance=$2
export SCBRIDGE_MOVE_PARENT_PID=${3:-}
schema="$instance/root/opt/wine/lib/wine/i386-windows"
mkdir -p "$schema"
cp "$runtime/apisetschema.dll" "$schema/"
digest=$(sha256sum "$runtime/SCCore.dll")
digest=${digest%% *}
export SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=dummy
cd "$instance"
exec "$runtime/CanvasBoxedWine" -root "$instance/root" -zip "$runtime/boxedwine.zip" \
    -mount_drive "$runtime" d -mount_drive "$instance" e \
    -env WINEDEBUG=-all -env SCBRIDGE_DISABLE_GUEST_WORKGROUP=1 \
    -env "SCBRIDGE_VERIFIED_CORE_SHA256=$digest" -novideo /bin/wine \
    'd:\SoundCanvasGuest.exe' 'd:\SCCore.dll' 'e:\commands' 'e:\responses'
