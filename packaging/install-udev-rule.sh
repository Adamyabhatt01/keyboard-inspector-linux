#!/usr/bin/env bash
# Install a Keyboard Inspector evdev uaccess rule.
#
# Usage:
#   sudo packaging/install-udev-rule.sh --generic    # all keyboards/pointers/gamepads
#   sudo packaging/install-udev-rule.sh --targeted   # URX75 Core75 only
#
# --generic is the default, so keyboards plugged in later are readable too.
# Run explicitly with sudo; nothing in the application elevates itself.
set -euo pipefail

mode="generic"
case "${1:-}" in
    --generic) mode="generic" ;;
    --targeted) mode="targeted" ;;
    -h|--help)
        sed -n '2,9p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    "")
        ;;
    *)
        echo "error: unknown option: $1 (use --generic or --targeted)" >&2
        exit 2
        ;;
esac

if [[ ${EUID} -ne 0 ]]; then
    echo "error: run this script with sudo (it writes to /etc/udev/rules.d)." >&2
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
    echo "Use --generic to make devices from other vendors readable too."
fi
