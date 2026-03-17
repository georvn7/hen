#!/bin/bash

set -euo pipefail

install_formula() {
    local formula="$1"
    brew install "$formula"
}

install_optional_formula() {
    local formula="$1"
    if brew info --formula "$formula" >/dev/null 2>&1; then
        brew install "$formula"
    else
        echo "Warning: Homebrew formula '$formula' is not available on this system. Skipping."
    fi
}

# Check if Homebrew is installed
if ! command -v brew &> /dev/null
then
    echo "Homebrew not found, installing..."
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install.sh)"
    (echo; echo 'eval "$(/opt/homebrew/bin/brew shellenv)"') >> ~/.zprofile
else
    echo "Homebrew is already installed."
fi

# Update Homebrew and install cpprestsdk
brew update
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
brew reinstall cpprestsdk

if [ "$(uname)" = "Darwin" ]; then
    cp "./Environment/cpprestsdk/streams.h" "/opt/homebrew/include/cpprest/streams.h"
    RED='\033[31m'
    RESET='\033[0m'
    #in case of a problem compiling didable the next line and reinstal cpprestsdk (brew uninstall cpprestsdk then brew install cpprestsdk)
    echo -e "${RED}WARNING: Changing file in CPPRESTSDK. This might break the build. Check for this message in build.sh${RESET}"
fi

tinyxml2_prefix="$(brew --prefix tinyxml2)"
cmake_args=(
    -B build
    -G "Xcode"
    -DCMAKE_OSX_ARCHITECTURES=arm64
    "-DCMAKE_PREFIX_PATH=${tinyxml2_prefix}"
)

if [ -f "${tinyxml2_prefix}/lib/cmake/tinyxml2/tinyxml2-config.cmake" ]; then
    cmake_args+=("-DTinyXML2_DIR=${tinyxml2_prefix}/lib/cmake/tinyxml2")
fi

cmake "${cmake_args[@]}"
cmake --build build --config Debug -j4

echo "Build script execution completed."
