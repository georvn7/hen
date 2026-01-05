#!/bin/bash

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
brew install coreutils
brew install cmake
brew install boost
brew install cpprestsdk llvm node ninja tinyxml2 xinetd nginx
brew install pkg-config openssl
brew install sqlite
brew reinstall cpprestsdk

if [ "$(uname)" = "Darwin" ]; then
    cp "./Environment/cpprestsdk/streams.h" "/opt/homebrew/include/cpprest/streams.h"
    RED='\033[31m'
    RESET='\033[0m'
    #in case of a problem compiling didable the next line and reinstal cpprestsdk (brew uninstall cpprestsdk then brew install cpprestsdk)
    echo -e "${RED}WARNING: Changing file in CPPRESTSDK. This might break the build. Check for this message in build.sh${RESET}"
fi

cmake -B build -G "Xcode" \
    -DCMAKE_OSX_ARCHITECTURES=arm64

echo "Build script execution completed."
