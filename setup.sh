#!/usr/bin/env bash
# Keyboard Inspector for Linux — one-step setup.
#
# Usage:
#   ./setup.sh                  # deps check, build, tests, install to ~/.local
#   ./setup.sh --system         # same, but install to /usr/local (asks for sudo)
#   ./setup.sh --prefix DIR     # install into a custom prefix
#   ./setup.sh --no-tests       # skip the Catch2 suites
#   ./setup.sh --skip-udev      # do not ask about device access at the end
#   ./setup.sh --help
#
# Safe to re-run; every step is idempotent. Missing dependencies are printed
# with install hints, never installed automatically.
set -euo pipefail

prefix=""
system=0
run_tests=1
ask_udev=1

while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            sed -n '2,11p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        --system) system=1 ;;
        --prefix)
            [[ $# -ge 2 ]] || { echo "error: --prefix needs a directory" >&2; exit 2; }
            prefix=$2
            shift
            ;;
        --prefix=*) prefix=${1#--prefix=} ;;
        --no-tests) run_tests=0 ;;
        --skip-udev) ask_udev=0 ;;
        *)
            echo "error: unknown option: $1" >&2
            exit 2
            ;;
    esac
    shift
done

if [[ -n ${prefix} && ${system} -eq 1 ]]; then
    echo "error: --prefix and --system cannot be combined" >&2
    exit 2
fi
if [[ ${system} -eq 1 ]]; then
    prefix="/usr/local"
elif [[ -z ${prefix} ]]; then
    prefix="${HOME}/.local"
fi

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cd "${repo_root}"

banner() { echo; echo "== $* =="; }
missing=()

have() { command -v "$1" >/dev/null 2>&1; }
need_cmd() { have "$1" || missing+=("$1"); }

# ---------------------------------------------------------------- dependencies
banner "Checking dependencies (build deps: cmake, git, compiler, libevdev, Qt6)"

# Package set per detected package manager. Every listed package is included
# even if some are present; managers no-op already-installed ones
# (apt needs --no-install-recommends to keep the payload lean).
pkg_manager=""
install_set=""
if have pacman; then
    pkg_manager="pacman"
    install_set="cmake git gcc make qt6-base libevdev"
elif have apt-get; then
    pkg_manager="apt"
    install_set="cmake git build-essential qt6-base-dev libevdev-dev"
elif have dnf; then
    pkg_manager="dnf"
    install_set="cmake git gcc-c++ make qt6-qtbase-devel libevdev-devel"
elif have zypper; then
    pkg_manager="zypper"
    install_set="cmake git gcc-c++ make qt6-base-devel libevdev-devel"
elif have apk; then
    pkg_manager="apk"
    install_set="cmake git g++ make qt6-qtbase-dev libevdev-dev"
fi

need_cmd cmake
need_cmd git          # CMake FetchContent vendors kissfft + Catch2 via git
# A C++ compiler: c++, g++, clang++ in that order.
have c++ || have g++ || have clang++ || missing+=("C++ compiler (c++/g++/clang++)")
have pkg-config || missing+=(pkg-config)

qt_ok=0 evdev_ok=0
if have pkg-config; then
    pkg-config --exists Qt6Core && qt_ok=1 || missing+=("Qt6 development files (pkg-config Qt6Core not found)")
    pkg-config --exists libevdev && evdev_ok=1 || missing+=("libevdev development files (pkg-config libevdev not found)")
fi

resolve_deps() {
    missing=()
    need_cmd cmake
    need_cmd git
    have c++ || have g++ || have clang++ || missing+=("C++ compiler")
    have pkg-config || missing+=(pkg-config)
    if have pkg-config; then
        pkg-config --exists Qt6Core || missing+=("Qt6 development files")
        pkg-config --exists libevdev || missing+=("libevdev development files")
    else
        missing+=(pkg-config)
    fi
}

if [[ ${#missing[@]} -gt 0 && -n ${install_set} ]]; then
    echo
    echo "Missing dependencies detected on this system:"
    printf '  - %s\n' "${missing[@]}"
    if [[ -t 0 ]]; then
        echo
        echo "Packaging system detected: ${pkg_manager}"
        read -rp "Install them automatically with sudo now? [y/N]: " yn
        case ${yn:-n} in
            [yY]|[yY][eE][sS])
                echo
                echo "Installing ${install_set} (the password prompt covers only these system packages)..."
                case ${pkg_manager} in
                    pacman) sudo -- pacman -Syu --needed ${install_set} ;;
                    apt)    sudo -- apt-get update; sudo -- apt-get install -y --no-install-recommends ${install_set} ;;
                    dnf)    sudo -- dnf install -y ${install_set} ;;
                    zypper) sudo -- zypper install -y ${install_set} ;;
                    apk)    sudo -- apk add ${install_set} ;;
                esac
                resolve_deps
                ;;
            *)
                echo "Declined — manual instructions below."
                ;;
        esac
    else
        echo "No terminal available for the sudo prompt; manual instructions below."
    fi
fi

if [[ ${#missing[@]} -gt 0 ]]; then
    echo
    echo "Dependencies still missing:"
    printf '  - %s\n' "${missing[@]}"
    if [[ -n ${pkg_manager} ]]; then
        echo
        echo "Install manually:"
        case ${pkg_manager} in
            pacman) echo "  sudo pacman -S --needed ${install_set}" ;;
            apt)    echo "  sudo apt-get install -y ${install_set}" ;;
            dnf)    echo "  sudo dnf install -y ${install_set}" ;;
            zypper) echo "  sudo zypper install -y ${install_set}" ;;
            apk)    echo "  sudo apk add ${install_set}" ;;
        esac
    else
        echo
        echo "No supported package manager found; install cmake, git, a C++20 compiler,"
        echo "Qt 6 development files (Core/Widgets/Gui/Concurrent/Network) and libevdev manually."
    fi
    echo
    echo "Re-run ./setup.sh after installing them."
    exit 1
fi
echo "All dependencies present."

# ---------------------------------------------------------------- build
banner "Building (cmake + ${BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)} jobs, Release)"
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
jobs="${BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}"
cmake --build build -j "${jobs}"

# ---------------------------------------------------------------- tests
if [[ ${run_tests} -eq 1 ]]; then
    banner "Running tests (core + UI smoke)"
    QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
else
    banner "Skipping tests (--no-tests)"
fi

# ---------------------------------------------------------------- install
banner "Installing to ${prefix}"$([[ ${system} -eq 1 ]] && echo " (sudo will ask for your password)")
install_cmd=()
if [[ ${system} -eq 1 ]]; then
    install_cmd=(sudo)
fi
"${install_cmd[@]}" cmake --install build --prefix "${prefix}"
pkg_share="${prefix}/share"
"${install_cmd[@]}" install -Dm644 packaging/keyboard-inspector.desktop \
    "${pkg_share}/applications/keyboard-inspector.desktop"
"${install_cmd[@]}" install -Dm644 packaging/keyboard-inspector.png \
    "${pkg_share}/icons/hicolor/256x256/apps/keyboard-inspector.png"
"${install_cmd[@]}" install -Dm644 README.md \
    "${pkg_share}/doc/keyboard-inspector/README.md"
"${install_cmd[@]}" install -Dm644 LICENSE \
    "${pkg_share}/doc/keyboard-inspector/LICENSE"
"${install_cmd[@]}" install -Dm644 packaging/71-keyboard-inspector.rules \
    "${pkg_share}/doc/keyboard-inspector/71-keyboard-inspector.rules.example"
"${install_cmd[@]}" install -Dm644 packaging/71-keyboard-inspector-generic.rules \
    "${pkg_share}/doc/keyboard-inspector/71-keyboard-inspector-generic.rules.example"

# ---------------------------------------------------------------- device access
if [[ ${ask_udev} -eq 1 ]]; then
    banner "Device access setup"
    echo "The app captures /dev/input/event* unprivileged; it needs read access."
    echo "  [1] Generic rule  — all keyboards/mice/gamepads, including future ones (recommended)"
    echo "  [2] Auto-scan     — pick one connected device, targeted rule for it"
    echo "  [3] Skip          — decide later: sudo packaging/install-udev-rule.sh"
    if [[ -t 0 ]]; then
        read -rp "Choose [1/2/3] (default 1): " choice
        choice=${choice:-1}
        case ${choice} in
            1) sudo -- packaging/install-udev-rule.sh --generic ;;
            2) sudo -- packaging/install-udev-rule.sh --auto ;;
            3) echo "Skipped. Devices may show as locked until a rule is installed." ;;
            *) echo "unknown choice; skipping" ;;
        esac
    else
        echo "No terminal available; skipping. Run:"
        echo "  sudo packaging/install-udev-rule.sh --generic   (or --auto)"
    fi
fi

# ---------------------------------------------------------------- summary
banner "Setup complete"
echo "Binary:      ${prefix}/bin/keyboard-inspector"
echo "Desktop:     ${pkg_share}/applications/keyboard-inspector.desktop"
echo "Icon:        ${pkg_share}/icons/hicolor/256x256/apps/keyboard-inspector.png"
echo
case ":${PATH}:" in
    *":${prefix}/bin:"*) ;;
    *)
        echo "NOTE: ${prefix}/bin is not in your PATH. Add it:"
        echo "  echo 'export PATH=\"${prefix}/bin:\$PATH\"' >> ~/.bashrc   (or your shell profile)"
        echo
        ;;
esac
echo "Verify:"
echo "  keyboard-inspector --list-devices    # devices, access state, USB descriptors"
echo "  keyboard-inspector                   # start the app"
echo "If a rule was installed, unplug/replug the device once (or run udevadm trigger)."
