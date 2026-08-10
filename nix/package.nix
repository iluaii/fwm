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
          gdk-pixbuf,
          ...
        }:

        let
          desktopItem = pkgs.makeDesktopItem {
            name = "fwm";
            desktopName = "fwm";
            comment = "Wayland compositor written in C where windows are physical objects";
            exec = "@out@/bin/fwm-session";
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
          version = lib.strings.trim (builtins.readFile ../VERSION);

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
          ];

          installPhase = ''
            mkdir -p $out/bin

            cp fwm $out/bin -r
            cp fwmctl $out/bin -r
            cp $src/session/fwm-session $out/bin/fwm-session

            mkdir -p $out/share/wayland-sessions
            cp ${desktopItem}/share/wayland-sessions/* $out/share/wayland-sessions -r

            mkdir -p $out/share/xdg-desktop-portal
            cp $src/session/fwm-portals.conf $out/share/xdg-desktop-portal/fwm-portals.conf

            substituteInPlace $out/share/wayland-sessions/fwm.desktop \
                --replace-fail "@out@" "$out"
          '';

          passthru = {
            providedSessions = [ "fwm" ];
          };

          meta = {
            description = "Wayland compositor written in C where windows are physical objects";
            longDescription = ''
              A Wayland compositor written in C (wlroots) where windows behave as physical objects with mass, momentum, inertia, and velocity — simulated by a real rigid-body engine (Box2D v3). Drag a window and throw it — it slides, bounces off walls, stacks under gravity, and comes to rest like a real object.
            '';
            homepage = "https://fwm-website.vercel.app/";
            license = lib.licenses.gpl2Only;
            mainProgram = "fwm";
            platforms = lib.platforms.linux;
          };
        };
    in
    {
      packages.default = pkgs.callPackage fwmDerivation { };
    };
}
