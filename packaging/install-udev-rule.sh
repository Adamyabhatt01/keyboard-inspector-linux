#!/usr/bin/env bash
# Install a Keyboard Inspector evdev uaccess rule.
#
# Usage:
#   sudo packaging/install-udev-rule.sh --generic     # all keyboards/pointers/gamepads
#   sudo packaging/install-udev-rule.sh --targeted    # URX75 Core75 only
#   packaging/install-udev-rule.sh --auto             # scan connected devices, pick one
#   packaging/install-udev-rule.sh --scan             # list scan candidates only
#
# --generic is the recommended choice for most users: keyboards plugged in
# later are readable too. --auto scans unprivileged, asks which device to
# grant, and only asks for sudo when writing the rule. Nothing in the app
# ever elevates itself.
set -euo pipefail

mode="generic"
if [[ ${1:-} == "--apply-device" ]]; then
    mode="apply-device"
    shift
elif [[ ${1:-} == "--generic" ]]; then
    mode="generic"
elif [[ ${1:-} == "--targeted" ]]; then
    mode="targeted"
elif [[ ${1:-} == "--auto" ]]; then
    mode="auto"
elif [[ ${1:-} == "--scan" ]]; then
    mode="scan"
elif [[ ${1:-} == "-h" || ${1:-} == "--help" || ${1:-} == "" ]]; then
    sed -n '2,9p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
else
    echo "error: unknown option: $1 (use --generic, --targeted, --auto or --scan)" >&2
    exit 2
fi

scan_devices() {
    # Print <node>|<name>|<kinds>|<vid>|<pid> for every evdev device.
    # Reads sysfs and the udev database only — no special permission needed.
    local node n sysname name props kinds vid pid
    for node in /dev/input/event*; do
        [[ -e ${node} ]] || continue
        n=${node##*/}
        sysname="/sys/class/input/${n}/device/name"
        [[ -r ${sysname} ]] || continue
        name=$(<"${sysname}")
        props=$(udevadm info -q property --name "${node}" 2>/dev/null || true)
        kinds=""
        [[ ${props} == *ID_INPUT_KEYBOARD=1* ]] && kinds="${kinds}${kinds:+/}keyboard"
        [[ ${props} == *ID_INPUT_MOUSE=1* ]] && kinds="${kinds}${kinds:+/}mouse"
        [[ ${props} == *ID_INPUT_GAMEPAD=1* ]] && kinds="${kinds}${kinds:+/}gamepad"
        [[ ${props} == *ID_INPUT_JOYSTICK=1* ]] && kinds="${kinds}${kinds:+/}joystick"
        [[ ${props} == *ID_INPUT_TOUCHPAD=1* ]] && kinds="${kinds}${kinds:+/}touchpad"
        vid=$(sed -n 's/^ID_VENDOR_ID=//p' <<<"${props}")
        pid=$(sed -n 's/^ID_MODEL_ID=//p' <<<"${props}")
        printf '%s|%s|%s|%s|%s\n' "${n}" "${name}" "${kinds}" "${vid}" "${pid}"
    done
}

# Collect capturable devices (keyboards, pointers, pads) into arrays.
declare -a cand_node=() cand_name=() cand_vid=() cand_pid=()
while IFS='|' read -r node name kinds vid pid; do
    [[ -n ${kinds} ]] || continue
    cand_node+=("${node}")
    cand_name+=("${name}")
    cand_vid+=("${vid}")
    cand_pid+=("${pid}")
done < <(scan_devices)

case ${mode} in
    scan)
        echo "Capturable input devices (keyboard/mouse/gamepad/joystick/touchpad):"
        for i in "${!cand_node[@]}"; do
            if [[ -n ${cand_vid[i]} && -n ${cand_pid[i]} ]]; then
                printf '%2d) %-9s %-24s %-40s [%s:%s]\n' \
                    "$((i + 1))" "${cand_node[i]}" "${cand_name[i]}" \
                    "" "${cand_vid[i]}" "${cand_pid[i]}"
            else
                printf '%2d) %-9s %-24s %-40s (no USB ids)\n' \
                    "$((i + 1))" "${cand_node[i]}" "${cand_name[i]}" ""
            fi
        done
        exit 0
        ;;
    auto)
        c=${#cand_node[@]}
        if [[ ${c} -eq 0 ]]; then
            echo "no capturable input devices found (or udevadm unavailable); use --generic" >&2
            exit 1
        fi
        echo "Connected input devices:"
        for i in "${!cand_node[@]}"; do
            if [[ -n ${cand_vid[i]} && -n ${cand_pid[i]} ]]; then
                printf '%2d) %-9s %-24s %-40s [%s:%s]\n' \
                    "$((i + 1))" "${cand_node[i]}" "${cand_name[i]}" \
                    "" "${cand_vid[i]}" "${cand_pid[i]}"
            else
                printf '%2d) %-9s %-24s (no USB ids — use --generic instead)\n' \
                    "$((i + 1))" "${cand_node[i]}" "${cand_name[i]}"
            fi
        done
        echo
        echo "A physical device exposes several rows; picking any of them writes one rule"
        echo "covering the whole device (the rows share the same USB ids)."
        if [[ ! -t 0 ]]; then
            echo "error: no terminal for interactive selection" >&2
            echo "       use --scan, then run: sudo $0 --apply-device <vid> <pid> <name>" >&2
            exit 1
        fi
        read -rp "Rule for which device? [1-${c}] (empty = cancel): " choice
        [[ ${choice:-} =~ ^[0-9]+$ ]] || { echo "cancelled"; exit 0; }
        (( choice >= 1 && choice <= c )) || { echo "error: out of range" >&2; exit 2; }
        idx=$((choice - 1))
        vid=${cand_vid[idx]}
        pid=${cand_pid[idx]}
        if [[ -z ${vid} || -z ${pid} ]]; then
            echo "error: ${cand_node[idx]} has no USB ids (built-in or serial device); use --generic" >&2
            exit 1
        fi
        if [[ ${EUID} -eq 0 ]]; then
            exec "$0" --apply-device "${vid}" "${pid}" "${cand_name[idx]}"
        fi
        echo "Writing the rule needs root; sudo will not run the whole script as root, only this write:"
        exec sudo -- "$0" --apply-device "${vid}" "${pid}" "${cand_name[idx]}"
        ;;
    apply-device)
        if [[ ${EUID} -ne 0 ]]; then
            echo "error: --apply-device must run as root" >&2
            exit 1
        fi
        vid=${1:-} pid=${2:-} name=${3:-} rule=/etc/udev/rules.d/71-keyboard-inspector-device.rules
        [[ ${vid} =~ ^[0-9a-fA-F]{1,4}$ && ${pid} =~ ^[0-9a-fA-F]{1,4}$ ]] || {
            echo "error: invalid USB ids '${vid}:${pid}'" >&2
            exit 2
        }
        install -d /etc/udev/rules.d
        if [[ -f ${rule} ]] && grep -qF "ATTRS{id/vendor}==\"${vid}\"" "${rule}" \
            && grep -qF "ATTRS{id/product}==\"${pid}\"" "${rule}"; then
            echo "rule is already installed: ${rule}"
        else
            if [[ -f ${rule} ]]; then
                backup="${rule}.bak.$(date +%Y%m%d%H%M%S)"
                cp -a -- "${rule}" "${backup}"
                echo "backed up previous rule to ${backup}"
            fi
            cat > "${rule}" <<EOF
# Keyboard Inspector — targeted uaccess rule
# Device: ${name}
# Generated by install-udev-rule.sh --auto on $(date +%Y-%m-%d)
# Covers every evdev node of this physical device (shared USB ids).
# It does not grant access to every input device, does not use the
# input group, and does not change the application's privileges.
SUBSYSTEM=="input", KERNEL=="event*", \\
  ATTRS{id/vendor}=="${vid}", ATTRS{id/product}=="${pid}", \\
  MODE="0660", TAG+="uaccess"
EOF
            echo "installed ${rule}"
        fi
        if [[ -f /etc/udev/rules.d/71-keyboard-inspector-generic.rules ]]; then
            echo "note: the generic rule is also installed; this device rule is redundant but harmless"
        fi
        udevadm control --reload-rules
        udevadm trigger --subsystem-match=input --action=change
        echo "Reloaded udev. If the device was already connected, unplug/replug it."
        echo "Verify with: keyboard-inspector --list-devices"
        exit 0
        ;;
esac

# --- generic / targeted paths (unchanged behavior) -----------------------------
if [[ ${EUID} -ne 0 ]]; then
    echo "error: run this script with sudo (it writes to /etc/udev/rules.d), or use --auto/--scan instead" >&2
    exit 1
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ ${mode} == "generic" ]]; then
    rule_name="71-keyboard-inspector-generic.rules"
else
    rule_name="71-keyboard-inspector.rules"
fi
source_rule="${script_dir}/${rule_name}"
target_rule="/etc/udev/rules.d/${rule_name}"
if [[ ${mode} == "generic" ]]; then
    other_rule="/etc/udev/rules.d/71-keyboard-inspector.rules"
else
    other_rule="/etc/udev/rules.d/71-keyboard-inspector-generic.rules"
fi
legacy_rule="/etc/udev/rules.d/99-keyboard-inspector.rules"

if [[ ! -f ${source_rule} ]]; then
    echo "error: rule not found next to this script: ${source_rule}" >&2
    exit 1
fi

# 73-seat-late.rules applies the uaccess ACL, so this rule must sort before it.
if [[ -f ${legacy_rule} ]]; then
    backup="${legacy_rule}.bak.$(date +%Y%m%d%H%M%S)"
    cp -a -- "${legacy_rule}" "${backup}"
    rm -f -- "${legacy_rule}"
    echo "removed obsolete ${legacy_rule} (backed up to ${backup})"
fi

# Only one variant should be active; prefer the one just requested.
if [[ -f ${other_rule} && ${other_rule} != "${target_rule}" ]]; then
    backup="${other_rule}.bak.$(date +%Y%m%d%H%M%S)"
    cp -a -- "${other_rule}" "${backup}"
    rm -f -- "${other_rule}"
    echo "removed other rule variant ${other_rule} (backed up to ${backup})"
fi

if [[ -f ${target_rule} ]] && cmp -s -- "${source_rule}" "${target_rule}"; then
    echo "rule is already installed: ${target_rule}"
else
    if [[ -f ${target_rule} ]]; then
        backup="${target_rule}.bak.$(date +%Y%m%d%H%M%S)"
        cp -a -- "${target_rule}" "${backup}"
        echo "backed up existing rule to ${backup}"
    fi
    install -Dm644 -- "${source_rule}" "${target_rule}"
    echo "installed ${target_rule}"
fi

udevadm control --reload-rules
udevadm trigger --subsystem-match=input --action=change
echo
echo "Reloaded udev. If a device was already connected, unplug/replug it."
echo "Verify with: keyboard-inspector --list-devices"
if [[ ${mode} == "generic" ]]; then
    echo
    echo "This rule covers keyboards, pointers, touchpads and gamepads from any vendor."
else
    echo
    echo "This rule targets vendor 36b0 / product 3002 (URX75 Core75) only."
    echo "Use --generic to make devices from other vendors readable, or --auto to pick a device."
fi
