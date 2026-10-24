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
          version = "1.2.6";

          src = pkgs.fetchurl {
            url = "https://github.com/CatPieLeaf/Stud/archive/refs/tags/1.2.6.tar.gz";
            sha256 = "f991e2a9d82781ff5118e16fe57cdca0d8dd39d9cbebea7eb8c1364a5cee7d88";
          };

          # The Android runtime and render-host, which need the NDK and AOSP to build.
          prebuilt = pkgs.fetchurl {
            url = "https://github.com/CatPieLeaf/Stud/releases/download/1.2.6/stud-1.2.6-x86_64.tar.zst";
            sha256 = "65c8bf7b43a5984dd3734f86dcd65fd4e7c624acdbef0f298135c2c8dea54c33";
          };

          libjnivm = pkgs.fetchFromGitHub {
            owner = "ChristopherHX";
            repo = "libjnivm";
            rev = "f24b98c198fcc5c59a68d1efadd3f5791eb01c2e";
            hash = "sha256-K4+1Zo7AJIouIqnHn2ZE4GyrNrYOczmWYblLAnGYA5w=";
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
            python3
            wayland-scanner
            zstd
            # render-host's layout (code before .interp) needs patchelf 0.18.
            patchelfUnstable
            autoPatchelfHook
            qt6.wrapQtAppsHook
          ];

          buildInputs = with pkgs; [
            qt6.qtbase
            qt6.qtwayland
            qt6.qtwebengine
            kdePackages.qtkeychain
            vulkan-headers
            openssl
            wayland
            wayland-protocols
            libxkbcommon
            freetype
            libdrm
            libGL
          ];

          cmakeFlags = [
            "-DSTUD_BUILD_QT_ONLY=ON"
            "-DSTUD_BUILD_TESTS=OFF"
            "-DSTUD_VERSION=${version}"
            "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
            "-DFETCHCONTENT_SOURCE_DIR_LIBJNIVM=${libjnivm}"
            "-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=${nlohmann_json}"
            "-DFETCHCONTENT_SOURCE_DIR_MINIZ=${miniz}"
          ];

          # Libraries render-host and the present layer open at runtime.
          runtimeDependencies = with pkgs; [
            vulkan-loader
            libGL
            libdecor
            libxkbcommon
            libpulseaudio
            alsa-lib
            libjack2
            xorg.libX11
            xorg.libXext
            xorg.libXi
            xorg.libXrandr
            xorg.libXrender
            xorg.libxcb
          ];

          # The Android binaries must keep their bionic linker and libraries.
          dontAutoPatchelf = true;
          dontPatchELF = true;
          dontWrapQtApps = true;
          stripDebugList = [ "bin" ];

          postInstall = ''
            mkdir prebuilt
            tar --zstd -xf ${prebuilt} -C prebuilt
            mkdir -p $out/lib $out/libexec/stud
            cp -a prebuilt/usr/lib/stud $out/lib/
            cp -a prebuilt/usr/libexec/stud/{lib64,stud-render-host,stud-runtime-bionic,libstud_present_layer.so,VK_LAYER_STUD_present.json} $out/libexec/stud/
            cp -an prebuilt/usr/share/licenses/stud/. $out/share/licenses/stud/
            patchelf --add-rpath ${lib.makeLibraryPath [ pkgs.vulkan-loader ]} $out/bin/stud
          '';

          postFixup = ''
            autoPatchelf $out/libexec/stud/stud-render-host $out/libexec/stud/libstud_present_layer.so
            wrapQtApp $out/bin/stud
            wrapQtApp $out/libexec/stud/stud-webview
          '';

          qtWrapperArgs = [
            "--prefix PATH : ${
              lib.makeBinPath (
                with pkgs;
                [
                  bubblewrap
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
