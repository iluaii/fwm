{
  perSystem =
    { pkgs, ... }:
    let

      fwmDerivation =
        {
          lib,
          stdenv,
          cmake,
          pkg-config,
          wlroots_0_20,
          wayland,
          wayland-scanner,
          xwayland,
          wayland-protocols,
          libdrm,
          libxcb,
          libxcb-wm,
          libxkbcommon,
          libsysprof-capture,
          cairo,
          pango,
          libGL,
          box2d,
          ffmpeg,
          pipewire,
          pcre2,
          util-linux,
          libpulseaudio,
          cava,
          gdk-pixbuf,
          withCava ? true,
          ...
        }:

        let
          desktopItem = pkgs.makeDesktopItem {
            name = "fwm";
            desktopName = "fwm";
            comment = "Wayland compositor written in C where windows are physical objects";
            exec = "@out@/bin/fwm";
            destination = "/share/wayland-sessions";
            keywords = [
              "tiling"
              "wayland"
              "compositor"
            ];
          };
        in

        stdenv.mkDerivation {
          pname = "fwm";
          version = "0.3.0"; # There doesn't seem to be any version specified in the project so I'm going latest release tag from github. I would consider specifying current version in some VERSION file. Otherwise this will need to be updated every new release

          nativeBuildInputs = [
            cmake
            pkg-config
          ];

          src = lib.cleanSource ./..;

          buildInputs = [
            # display
            wlroots_0_20
            wayland
            wayland-scanner
            wayland-protocols
            xwayland
            libGL

            # keyboard
            libxcb
            libxcb-wm
            libxkbcommon

            # physics engine
            box2d

            # graphics lib
            cairo
            pango

            # images
            ffmpeg
            gdk-pixbuf
            libsysprof-capture

            # audio
            pipewire
            libpulseaudio

            # drm
            libdrm

            # regex
            pcre2

            # misc
            util-linux
          ]
          ++ lib.optionals withCava [ cava ];

          installPhase = ''
            mkdir -p $out/bin
            cp fwm $out/bin -r

            mkdir -p $out/share/wayland-sessions
            cp ${desktopItem}/share/wayland-sessions/* $out/share/wayland-sessions -r

            substituteInPlace $out/share/wayland-sessions/fwm.desktop \
                --replace-fail "@out@" "$out"
          '';

          passthru = {
            providedSessions = [ "fwm" ];
          };
        };
    in
    {
      packages.default = pkgs.callPackage fwmDerivation { };
    };
}
