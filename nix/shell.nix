{
  perSystem =
    { pkgs, config, ... }:
    {
      devShells.default = pkgs.mkShell {
        inputsFrom = [
          (config.packages.default)
        ];
      };
    };
}
