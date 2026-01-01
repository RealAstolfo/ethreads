{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  buildInputs = with pkgs; [
    valgrind
    gnumake
    gcc
  ];
}
