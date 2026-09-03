{
  description = "Local offline voice input plugin for Fcitx5";

  nixConfig = {
    extra-substituters = [ "https://fcitx5-vinput.cachix.org" ];
    extra-trusted-public-keys = [ "fcitx5-vinput.cachix.org-1:XpX3AA6+dDIX4qJhb1QM7sbTwX6/qSlGvW8Z5NK6XdU=" ];
  };

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-unstable";
    sherpa-onnx.url = "github:xifan2333/sherpa-onnx-flake";
    sherpa-onnx.inputs.nixpkgs.follows = "nixpkgs";
  };

  outputs =
    {
      self,
      nixpkgs,
      sherpa-onnx,
    }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      forAllSystems = nixpkgs.lib.genAttrs systems;
      pkgsFor = system: import nixpkgs { inherit system; };
      version = nixpkgs.lib.removeSuffix "\n" (builtins.readFile ./VERSION);
    in
    {
      inherit version;

      packages = forAllSystems (
        system:
        let
          pkgs = pkgsFor system;
          lib = pkgs.lib;
          sherpa-deps = sherpa-onnx.packages.${system};
          commandProviderPath = lib.makeBinPath [ pkgs.python3 ];
          commandProviderLibraryPath = lib.makeLibraryPath [ pkgs.libopus ];

          makeVinputPackage =
            {
              pname,
              enableLocalAsr,
            }:
            pkgs.stdenv.mkDerivation {
              inherit pname version;
              src = self;

              nativeBuildInputs = with pkgs; [
                cmake
                pkg-config
                gettext
                fcitx5
                makeBinaryWrapper
                qt6.wrapQtAppsHook
                autoPatchelfHook
              ];

              buildInputs =
                (with pkgs; [
                  fcitx5
                  systemdLibs
                  curl
                  libarchive
                  openssl
                  pipewire
                  qt6.qtbase
                  cli11
                  nlohmann_json
                  clang
                  mold
                ])
                ++ lib.optionals enableLocalAsr [
                  pkgs.onnxruntime
                  sherpa-deps.sherpa-onnx
                ];

              cmakeFlags = [
                "-DVINPUT_ENABLE_LOCAL_ASR=${if enableLocalAsr then "ON" else "OFF"}"
                "-DVINPUT_FETCH_CLI11=OFF"
                "-DCMAKE_BUILD_TYPE=Release"
                "-DCMAKE_C_COMPILER=clang"
                "-DCMAKE_CXX_COMPILER=clang++"
                "-DCMAKE_LINKER=mold"
                "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold"
                "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=mold"
              ];

              postInstall = ''
                ${lib.optionalString enableLocalAsr ''
                  rm -f $out/lib/fcitx5-vinput/libonnxruntime.so
                ''}

                for program in "$out"/bin/*; do
                  wrapProgram "$program" \
                    --prefix PATH : ${commandProviderPath} \
                    --prefix LD_LIBRARY_PATH : ${commandProviderLibraryPath}
                done
              '';

              meta = {
                description =
                  if enableLocalAsr then
                    "Voice input addon for Fcitx5 with local and cloud ASR"
                  else
                    "Voice input addon for Fcitx5, built without local ASR";
                homepage = "https://github.com/xifan2333/fcitx5-vinput";
                license = lib.licenses.gpl3Only;
                platforms = systems;
                mainProgram = "vinput";
              };
            };
        in
        rec {
          fcitx5-vinput = makeVinputPackage {
            pname = "fcitx5-vinput";
            enableLocalAsr = true;
          };

          fcitx5-vinput-lite = makeVinputPackage {
            pname = "fcitx5-vinput-lite";
            enableLocalAsr = false;
          };

          default = fcitx5-vinput;
        }
      );

      devShells = forAllSystems (
        system:
        let
          pkgs = pkgsFor system;
          sherpa-deps = sherpa-onnx.packages.${system};
        in
        {
          default = pkgs.mkShell {
            packages = with pkgs; [
              cmake
              fcitx5
              pkg-config
              gettext
              systemdLibs
              curl
              libarchive
              openssl
              onnxruntime
              pipewire
              qt6.qtbase
              qt6.wrapQtAppsHook
              cli11
              sherpa-deps.sherpa-onnx
              nlohmann_json
            ];
          };
        }
      );
    };
}
