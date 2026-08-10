{
  description = "fwm is a Wayland compositor written in C where windows are physical objects — Box2D physics, a 10-desktop world with parallax, and Hyprland-style tiling";

  inputs = {
    nixpkgs = {
      url = "github:nixos/nixpkgs/nixos-unstable";
    };

    flake-parts.url = "github:hercules-ci/flake-parts";
    import-tree.url = "github:vic/import-tree";

  };

  outputs =
    inputs:
    inputs.flake-parts.lib.mkFlake { inherit inputs; } (
      { ... }:
      {

        systems = [
          "x86_64-linux"
          "aarch64-linux"
        ];

        imports = [
          (inputs.import-tree ./nix)
        ];
      }
    );
}
