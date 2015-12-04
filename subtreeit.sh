#!/bin/sh
set -e -x
subtreeit() {
  local basename=$1 remote=${2:-$1} branch=${3:-master}
  [[ -n "$(git config remote.$remote.url)" ]] ||
    git remote add $remote https://github.com/apple/$basename.git
  git fetch $remote
  [[ -d $remote ]] ||
    git read-tree --prefix=$remote/ -u $remote/$branch
}

subtreeit swift swift
subtreeit swift-llvm llvm stable
subtreeit swift-clang clang stable
subtreeit swift-lldb lldb
subtreeit swift-cmark cmark
subtreeit swift-llbuild llbuild
subtreeit swift-package-manager swiftpm
subtreeit swift-corelibs-xctest
subtreeit swift-corelibs-foundation
