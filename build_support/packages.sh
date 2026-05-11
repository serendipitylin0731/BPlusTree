#!/usr/bin/env bash

## =================================================================
## BUSTUB PACKAGE INSTALLATION
##
## This script will install all the packages that are needed to
## build and run the DBMS.
##
## Supported environments:
##  * Ubuntu / WSL (x86-64)
##  * macOS with Homebrew (x86-64 or ARM)
## =================================================================

main() {
  set -o errexit

    if [ "${1:-}" == "-y" ] 
    then 
        install
    else
        echo "PACKAGES WILL BE INSTALLED. THIS MAY BREAK YOUR EXISTING TOOLCHAIN."
        echo "YOU ACCEPT ALL RESPONSIBILITY BY PROCEEDING."
        read -p "Proceed? [y/N] : " yn
    
        case $yn in
            Y|y) install;;
            *) ;;
        esac
    fi

    echo "Script complete."
}

install() {
  set -x
  UNAME=$(uname | tr "[:lower:]" "[:upper:]" )

  case $UNAME in
    DARWIN) install_mac ;;

    LINUX)
      if [ -f /etc/os-release ]; then
        . /etc/os-release
      fi
      case ${ID:-} in
        ubuntu) install_linux ;;
        *) give_up ;;
      esac
      ;;

    *) give_up ;;
  esac
}

give_up() {
  set +x
  echo "Unsupported distribution '$UNAME'"
  echo "Please contact our support team for additional help."
  echo "Be sure to include the contents of this message."
  echo "Platform: $(uname -a)"
  echo
  echo "https://github.com/DB/bustub/issues"
  echo
  exit 1
}

install_mac() {
  # Install Homebrew.
  if ! command -v brew >/dev/null 2>&1; then
    echo "Installing Homebrew (https://brew.sh/)"
    bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install.sh)"
  fi
  # Update Homebrew.
  brew update
  # Install packages.
  brew ls --versions cmake || brew install cmake
  brew ls --versions coreutils || brew install coreutils
  brew ls --versions doxygen || brew install doxygen
  brew ls --versions git || brew install git
  brew ls --versions llvm || brew install llvm
  brew ls --versions libelf || brew install libelf
}

install_linux() {
  # Update apt-get.
  apt-get -y update
  # Install packages.
  packages=(
      build-essential
      cmake
      doxygen
      git
      pkg-config
      zlib1g-dev
      libelf-dev
      libdwarf-dev
  )

  for package in clang-14 clang-format-14 clang-tidy-14; do
    if apt-cache show "$package" >/dev/null 2>&1; then
      packages+=("$package")
    else
      packages+=("${package%-14}")
    fi
  done

  apt-get -y install "${packages[@]}"
}

main "$@"
