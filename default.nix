{ pkgs ? import <nixpkgs> { }
, buildType ? "Debug"
}:

pkgs.llvm_7.overrideAttrs (oldAttrs: rec {
  src = ./.;
  unpackPhase = ''
    cp -r ${src}/llvm/* .
    ls -alh tools/llvm-config/llvm-config.cpp
    chmod -R u+w .
  '';
  doCheck = false;
  preConfigure = ''
    # Make sure we're building the right LLVM
    ls lib/CodeGen/Wedlock.cpp || exit 1
  '';
  cmakeFlags = oldAttrs.cmakeFlags ++ [
    "-DLLVM_TARGETS_TO_BUILD=X86"
    "-DLLVM_BUILD_TYPE=${buildType}"
  ];
})
