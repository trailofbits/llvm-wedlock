FROM nixos/nix:2.2.1

ARG BUILD_TYPE=Debug

USER root

WORKDIR /chess/llvm-wedlock
COPY ./ /chess/llvm-wedlock

# Get the dependencies first. Saves time in the build.
RUN nix-shell default.nix --run "exit 0"

# Actually run the build
RUN nix-build default.nix
