{ self, ... }:
{
  flake.nixosModules.default =
    {
      pkgs,
      config,
      lib,
      ...
    }:
    let
      cfg = config.programs.fwm;

      pkg = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
    in
    {
      options.programs.fwm = {
        enable = lib.mkEnableOption "fwm is a Wayland compositor written in C where windows are physical objects";
        package = lib.mkOption {
          default = pkg;
          description = "Package to use";
          type = lib.types.package;
        };
      };

      config = lib.mkIf cfg.enable {
        environment.systemPackages = [ cfg.package ];

        services.displayManager.sessionPackages = [
          cfg.package
        ];

        # wayland session

        security = {
          polkit.enable = true;
        };

        programs = {
          dconf.enable = lib.mkDefault true;
          xwayland.enable = lib.mkDefault true;
        };

        services.graphical-desktop.enable = true;

        xdg.portal = {
          wlr.enable = true;
          extraPortals = [
            pkgs.xdg-desktop-portal-gtk
          ];

          configPackages = lib.mkDefault [
            cfg.package
          ];
        };

        # Window manager only sessions (unlike DEs) don't handle XDG
        # autostart files, so force them to run the service
        services.xserver.desktopManager.runXdgAutostartIfNone = lib.mkDefault true;
      };
    };
}
