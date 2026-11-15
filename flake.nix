{
  description = "An Unofficial Roblox Launcher for Linux";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      inherit (pkgs) lib;
    in
    {
      packages.${system} = rec {
        stud = pkgs.stdenv.mkDerivation rec {
          pname = "stud";
          version = "1.2.14";

          src = pkgs.fetchurl {
            url = "https://github.com/CatPieLeaf/Stud/archive/refs/tags/1.2.14.tar.gz";
            sha256 = "a10ed5340e5879ab961b5bdf0db235320abb8ae45675407ebf42669f9bb73e45";
          };

          # Rosemary, which CI builds: its cargo build fetches git
          # dependencies, which a Nix build cannot.
          prebuilt = pkgs.fetchurl {
            url = "https://github.com/CatPieLeaf/Stud/releases/download/1.2.14/stud-1.2.14-x86_64.tar.zst";
            sha256 = "13163081982513b3ab83063b194eb5bc8f4f5424915e2375bc87d3ca95e09b1a";
          };

          nlohmann_json = pkgs.fetchFromGitHub {
            owner = "nlohmann";
            repo = "json";
            rev = "v3.11.3";
            hash = "sha256-7F0Jon+1oWL7uqet5i1IgHX0fUw/+z0QwEcA3zs5xHg=";
          };
          miniz = pkgs.fetchFromGitHub {
            owner = "richgel999";
            repo = "miniz";
            rev = "3.1.2";
            hash = "sha256-/MAJWZXZ+pbelFduGE75rK/x9qEzxSFEj8RJWe3JUv0=";
          };

          nativeBuildInputs = with pkgs; [
            cmake
            ninja
            pkg-config
            zstd
            autoPatchelfHook
            qt6.wrapQtAppsHook
          ];

          buildInputs = with pkgs; [
            qt6.qtbase
            qt6.qtwayland
            qt6.qtwebengine
            qt6.qtwebchannel
            vulkan-headers
            # What Rosemary links.
            curl
            zlib
            libxkbcommon
            systemd
          ];

          cmakeFlags = [
            "-DSTUD_BUILD_ROSEMARY=OFF"
            "-DSTUD_BUILD_TESTS=OFF"
            "-DSTUD_VERSION=${version}"
            "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
            "-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=${nlohmann_json}"
            "-DFETCHCONTENT_SOURCE_DIR_MINIZ=${miniz}"
          ];

          # What Rosemary and the launcher open by name at runtime: SDL3 and,
          # through it, the window system and sound; Vulkan, EGL and GBM;
          # FFmpeg for video.
          runtimeDependencies = with pkgs; [
            sdl3
            vulkan-loader
            libGL
            mesa
            ffmpeg
            wayland
            libdecor
            libxkbcommon
            pipewire
            libpulseaudio
            alsa-lib
            xorg.libX11
            xorg.libXext
            xorg.libXi
            xorg.libXrandr
            xorg.libXcursor
          ];

          dontWrapQtApps = true;

          postInstall = ''
            mkdir prebuilt
            tar --zstd -xf ${prebuilt} -C prebuilt
            install -Dm755 prebuilt/usr/libexec/stud/rosemary $out/libexec/stud/rosemary
            cp -an prebuilt/usr/share/licenses/stud/. $out/share/licenses/stud/
            patchelf --add-rpath ${lib.makeLibraryPath [ pkgs.vulkan-loader ]} $out/bin/stud
          '';

          postFixup = ''
            wrapQtApp $out/bin/stud
          '';

          qtWrapperArgs = [
            "--prefix PATH : ${
              lib.makeBinPath (
                with pkgs;
                [
                  wl-clipboard
                  xdg-utils
                ]
              )
            }"
          ];

          meta = {
            description = "An Unofficial Roblox Launcher for Linux";
            homepage = "https://github.com/CatPieLeaf/Stud";
            license = lib.licenses.agpl3Only;
            sourceProvenance = with lib.sourceTypes; [
              fromSource
              binaryNativeCode
            ];
            platforms = [ "x86_64-linux" ];
            mainProgram = "stud";
          };
        };
        default = stud;
      };

      # pkgs.stud, for a NixOS configuration that wants it in nixpkgs:
      # nixpkgs.overlays = [ inputs.stud.overlays.default ];
      overlays.default = final: prev: {
        stud = self.packages.${final.stdenv.hostPlatform.system}.default;
      };
    };
}
