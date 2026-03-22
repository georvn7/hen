#!/bin/bash

set -euo pipefail

require_supported_host() {
    local os
    local arch

    os="$(uname -s)"
    arch="$(uname -m)"

    if [ "$os" != "Darwin" ]; then
        echo "ERROR: build.sh currently supports only macOS on Apple Silicon." >&2
        exit 1
    fi

    if [ "$arch" != "arm64" ]; then
        echo "ERROR: build.sh currently supports only Apple Silicon Macs (arm64)." >&2
        exit 1
    fi

    if ! xcodebuild -version >/dev/null 2>&1; then
        echo "ERROR: Xcode must be installed and selected before running build.sh." >&2
        echo "Install Xcode from the App Store, then run:" >&2
        echo "  sudo xcode-select -s /Applications/Xcode.app/Contents/Developer" >&2
        exit 1
    fi
}

install_formula() {
    local formula="$1"
    "$BREW_BIN" install "$formula"
}

install_optional_formula() {
    local formula="$1"
    if "$BREW_BIN" info --formula "$formula" >/dev/null 2>&1; then
        "$BREW_BIN" install "$formula"
    else
        echo "Warning: Homebrew formula '$formula' is not available on this system. Skipping."
    fi
}

find_brew_bin() {
    if command -v brew >/dev/null 2>&1; then
        command -v brew
        return 0
    fi

    local candidate
    for candidate in /opt/homebrew/bin/brew /usr/local/bin/brew; do
        if [ -x "$candidate" ]; then
            echo "$candidate"
            return 0
        fi
    done

    return 1
}

require_supported_host

# Check if Homebrew is installed
BREW_BIN="$(find_brew_bin || true)"
if [ -z "$BREW_BIN" ]
then
    echo "Homebrew not found, installing..."
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install.sh)"

    BREW_BIN="$(find_brew_bin || true)"
    if [ -z "$BREW_BIN" ]; then
        echo "ERROR: Homebrew installation completed, but brew could not be found." >&2
        exit 1
    fi

    eval "$("$BREW_BIN" shellenv)"

    brew_shellenv_cmd='eval "$('"$BREW_BIN"' shellenv)"'
    echo "Homebrew was installed for this build session."
    echo "For future shells, add this line to your shell profile manually:"
    echo "  $brew_shellenv_cmd"
else
    echo "Homebrew is already installed."
fi

# Update Homebrew and install cpprestsdk
"$BREW_BIN" update
install_formula coreutils
install_formula cmake
install_formula boost
install_formula cpprestsdk
install_formula llvm
install_formula node
install_formula ninja
install_formula tinyxml2
install_optional_formula xinetd
install_formula nginx
install_formula pkg-config
install_formula openssl
install_formula sqlite
"$BREW_BIN" reinstall cpprestsdk

tinyxml2_prefix="$("$BREW_BIN" --prefix tinyxml2)"
boost_prefix="$("$BREW_BIN" --prefix boost)"
sqlite_prefix="$("$BREW_BIN" --prefix sqlite)"
llvm_prefix="$("$BREW_BIN" --prefix llvm)"
openssl_prefix="$("$BREW_BIN" --prefix openssl)"
cpprestsdk_prefix="$("$BREW_BIN" --prefix cpprestsdk)"

cmake_prefix_path=(
    "${tinyxml2_prefix}"
    "${boost_prefix}"
    "${sqlite_prefix}"
    "${llvm_prefix}"
    "${openssl_prefix}"
    "${cpprestsdk_prefix}"
)

cmake_prefix_path_str=""
for prefix in "${cmake_prefix_path[@]}"; do
    if [ -z "${cmake_prefix_path_str}" ]; then
        cmake_prefix_path_str="${prefix}"
    else
        cmake_prefix_path_str="${cmake_prefix_path_str};${prefix}"
    fi
done

cmake_args=(
    -B build
    -G "Xcode"
    -DCMAKE_OSX_ARCHITECTURES=arm64
    "-DCMAKE_PREFIX_PATH=${cmake_prefix_path_str}"
    "-DBoost_ROOT=${boost_prefix}"
    "-DSQLite3_ROOT=${sqlite_prefix}"
    "-DOPENSSL_ROOT_DIR=${openssl_prefix}"
    "-DLLVM_DIR=${llvm_prefix}/lib/cmake/llvm"
)

if [ -f "${tinyxml2_prefix}/lib/cmake/tinyxml2/tinyxml2-config.cmake" ]; then
    cmake_args+=("-DTinyXML2_DIR=${tinyxml2_prefix}/lib/cmake/tinyxml2")
fi

if [ -f "${cpprestsdk_prefix}/lib/cmake/cpprestsdk/cpprestsdk-config.cmake" ]; then
    cmake_args+=("-Dcpprestsdk_DIR=${cpprestsdk_prefix}/lib/cmake/cpprestsdk")
fi

cmake "${cmake_args[@]}"
cmake --build build --config Debug -j4

echo "Build script execution completed."
