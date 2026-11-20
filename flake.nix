{
  description = "Play and create games on Roblox";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    # Rosemary, built from source by its own flake: Stud's fork, branch stud.
    # On the same nixpkgs, so Stud and Rosemary share one glibc.
    rosemary = {
      url = "github:CatPieLeaf/rosemary-stud/stud";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    { self, nixpkgs, rosemary }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      inherit (pkgs) lib;
      rosemaryPackage = rosemary.packages.${system}.default;
    in
    {
      packages.${system} = rec {
        stud = pkgs.stdenv.mkDerivation rec {
          pname = "stud";
          version = "2.0.5";

          src = pkgs.fetchurl {
            url = "https://github.com/CatPieLeaf/Stud/archive/refs/tags/2.0.5.tar.gz";
            sha256 = "675a372dd31930115dd4b3ba69b622b7de2f63f5b123d4e8cfa5a28893b9b7cf";
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
            qt6.wrapQtAppsHook
          ];

          buildInputs = with pkgs; [
            qt6.qtbase
            qt6.qtwayland
            openssl
            libsecret
          ];

          cmakeFlags = [
            "-DSTUD_BUILD_ROSEMARY=OFF"
            "-DSTUD_BUILD_TESTS=OFF"
            "-DSTUD_VERSION=${version}"
            "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
            "-DFETCHCONTENT_SOURCE_DIR_MINIZ=${miniz}"
          ];

          dontWrapQtApps = true;

          # Rosemary where Stud looks for it, beside its own binary. Its
          # package carries what it opens at runtime: SDL3, Vulkan, EGL,
          # FFmpeg and the rest.
          postInstall = ''
            mkdir -p $out/libexec/stud
            ln -s ${lib.getExe rosemaryPackage} $out/libexec/stud/rosemary
            install -Dm644 ${rosemary}/LICENSE $out/share/licenses/stud/rosemary/LICENSE-Rosemary.txt
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
            description = "Play and create games on Roblox";
            homepage = "https://github.com/CatPieLeaf/Stud";
            license = lib.licenses.agpl3Plus;
            sourceProvenance = [ lib.sourceTypes.fromSource ];
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
