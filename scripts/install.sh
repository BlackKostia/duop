#!/usr/bin/env bash
# Duop installer: downloads the prebuilt static binary of a GitHub release (verified against SHA256SUMS),
# or builds from source when no release asset is available (DUOP_FROM_SOURCE=1 forces the source build).
#   curl -fsSL https://raw.githubusercontent.com/BlackKostia/duop/main/scripts/install.sh | bash
# Environment: DUOP_REPO (owner/name), DUOP_VERSION (e.g. 2.11.0, default: latest), PREFIX (default /usr/local).
set -euo pipefail
REPO="${DUOP_REPO:-BlackKostia/duop}"
PREFIX="${PREFIX:-/usr/local}"
VERSION="${DUOP_VERSION:-latest}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

case "$(uname -s)/$(uname -m)" in
    Linux/x86_64)          ARCH=x86_64 ;;
    Linux/aarch64|Linux/arm64) ARCH=aarch64 ;;
    *) echo "duop supports Linux x86_64 and aarch64 only (got $(uname -s)/$(uname -m))." >&2; exit 1 ;;
esac

SUDO=""
if [[ ! -w "$PREFIX" && ! -w "$(dirname "$PREFIX")" ]] && [[ $EUID -ne 0 ]]; then
    command -v sudo >/dev/null 2>&1 && SUDO="sudo"
fi

fetch() { # fetch URL OUT
    if command -v curl >/dev/null 2>&1; then curl -fsSL "$1" -o "$2"
    elif command -v wget >/dev/null 2>&1; then wget -qO "$2" "$1"
    else return 1; fi
}

install_tree() { # install_tree SRC_DIR (contains duop, lang/, layouts/, duop-cd.sh)
    $SUDO install -Dm755 "$1/duop" "$PREFIX/bin/duop"
    $SUDO install -Dm644 -t "$PREFIX/share/duop/layouts" "$1"/layouts/*.layout
    $SUDO install -Dm644 -t "$PREFIX/share/duop/lang" "$1"/lang/*.lang
    if [[ -f "$1/duop-cd.sh" ]]; then $SUDO install -Dm755 "$1/duop-cd.sh" "$PREFIX/share/duop/duop-cd.sh"; fi
    if [[ -f "$1/scripts/duop-cd.sh" ]]; then $SUDO install -Dm755 "$1/scripts/duop-cd.sh" "$PREFIX/share/duop/duop-cd.sh"; fi
}

from_release() {
    local base tag
    if [[ -n "${DUOP_RELEASE_URL:-}" ]]; then
        base="$DUOP_RELEASE_URL"   # mirror or local test directory (file://...)
    elif [[ "$VERSION" == latest ]]; then
        base="https://github.com/$REPO/releases/latest/download"
    else
        tag="v${VERSION#v}"
        base="https://github.com/$REPO/releases/download/$tag"
    fi
    local asset="duop-linux-$ARCH.tar.gz"
    echo "Downloading $asset from https://github.com/$REPO/releases ($VERSION) ..."
    fetch "$base/$asset" "$TMP/$asset" || return 1
    fetch "$base/SHA256SUMS" "$TMP/SHA256SUMS" || { echo "SHA256SUMS missing, refusing an unverified binary." >&2; return 1; }
    (cd "$TMP" && grep " $asset\$" SHA256SUMS | sha256sum -c -) || { echo "Checksum mismatch for $asset." >&2; exit 1; }
    mkdir -p "$TMP/pkg"
    tar -xzf "$TMP/$asset" -C "$TMP/pkg" --strip-components=1
    install_tree "$TMP/pkg"
}

from_source() {
    local cc=gcc; [[ $ARCH == aarch64 ]] && cc=clang
    for tool in git make python3 as ld "$cc"; do
        command -v "$tool" >/dev/null 2>&1 || { echo "Source build requires git, make, python3, binutils and $cc (missing: $tool)." >&2; exit 1; }
    done
    local ref=()
    [[ "$VERSION" != latest ]] && ref=(--branch "v${VERSION#v}")
    echo "Building duop from source (https://github.com/$REPO) ..."
    git clone --depth 1 "${ref[@]}" "https://github.com/$REPO.git" "$TMP/duop"
    if [[ $ARCH == aarch64 ]]; then
        make -C "$TMP/duop" release ARCH=aarch64 AS=as LD=ld
    else
        make -C "$TMP/duop" release ARCH=x86_64
    fi
    install_tree "$TMP/duop"
}

if [[ "${DUOP_FROM_SOURCE:-0}" == 1 ]] || ! from_release; then
    from_source
fi
echo "Installed $PREFIX/bin/duop ($("$PREFIX/bin/duop" --version | head -n1))"
echo "Optional cd-on-exit shell integration: source $PREFIX/share/duop/duop-cd.sh"
