#!/bin/sh
set -eu
app=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
config=/storage/.config/rgbox-screen-color
units=/storage/.config/system.d
unit=rgbox-screen-color.service
[ "$(id -u)" = 0 ]
grep -qx 'OS_NAME="AisOS"' /etc/os-release
grep -qx 'HW_DEVICE="RK3562"' /etc/os-release

umask 022
mkdir -p "$config" "$units/graphical.target.wants"
cp "$app/screen_color.py" "$config/screen_color.py.new"
mv -f "$config/screen_color.py.new" "$config/screen_color.py"
cat > "$units/$unit.new" <<'UNIT'
[Unit]
Description=RGBox built-in screen color
After=weston.service

[Service]
ExecStart=/usr/bin/python3 -B /storage/.config/rgbox-screen-color/screen_color.py --watch
Restart=on-failure
RestartSec=2

[Install]
WantedBy=graphical.target
UNIT
mv -f "$units/$unit.new" "$units/$unit"
ln -sfn "../$unit" "$units/graphical.target.wants/$unit"
systemctl daemon-reload
systemctl restart "$unit"
