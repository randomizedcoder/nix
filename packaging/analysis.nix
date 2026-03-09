{
  lib,
  pkgs,
  src,
  nixComponents,
}:

let
  inherit (pkgs.buildPackages) meson ninja pkg-config bison flex cmake;
  deps = pkgs.nixDependencies2;

  # Generate a compilation database by running meson setup on the root project.
  # The root meson.build includes all subprojects, producing a comprehensive
  # compile_commands.json covering all source files.
  compilationDb = pkgs.stdenv.mkDerivation {
    pname = "nix-compilation-db";
    version = nixComponents.version;

    inherit src;

    nativeBuildInputs = [
      meson
      ninja
      pkg-config
      bison
      flex
      cmake
      pkgs.buildPackages.python3
    ];

    # External dependencies needed by the nix subprojects for meson configure.
    # These come from nixDependencies2 (overridden versions) where available,
    # otherwise from pkgs directly.
    buildInputs = [
      deps.boost
      deps.curl
      deps.libblake3
      deps.boehmgc
      pkgs.brotli
      pkgs.libarchive
      pkgs.libsodium
      pkgs.nlohmann_json
      pkgs.openssl
      pkgs.sqlite
      pkgs.libgit2
      pkgs.editline
      pkgs.lowdown
      pkgs.toml11
    ]
    ++ lib.optional pkgs.stdenv.hostPlatform.isLinux pkgs.libseccomp
    ++ lib.optionals (lib.meta.availableOn pkgs.stdenv.hostPlatform (pkgs.aws-c-common or null)) [
      pkgs.aws-c-common
      pkgs.aws-crt-cpp
    ];

    dontBuild = true;
    doCheck = false;
    dontFixup = true;

    # Run meson setup with minimal options — we only need compile_commands.json
    configurePhase = ''
      runHook preConfigure

      # Allow configure to succeed even if some optional deps are missing
      meson setup build \
        --prefix="$out" \
        -Dunit-tests=false \
        -Djson-schema-checks=false \
        -Dbindings=false \
        -Ddoc-gen=false \
        -Dbenchmarks=false \
        || echo "WARNING: meson configure had errors (compile_commands.json may be partial)"

      runHook postConfigure
    '';

    installPhase =
      let
        # Python script to rewrite compile_commands.json paths.
        # Meson generates relative "file" paths (e.g. "../src/foo.cc") resolved
        # against the build directory. We convert everything to absolute store paths.
        fixPathsScript = pkgs.writeText "fix-compile-db-paths.py" ''
          import json, os, sys

          store_src = sys.argv[1]
          source_root = sys.argv[2]
          input_file = sys.argv[3]
          output_file = sys.argv[4]

          build_prefix = "/build/" + source_root
          build_dir = build_prefix + "/build"

          with open(input_file) as f:
              db = json.load(f)

          for entry in db:
              d = entry.get("directory", "")
              fpath = entry.get("file", "")

              # Resolve relative file paths against the build directory
              if not os.path.isabs(fpath):
                  abs_file = os.path.normpath(os.path.join(d, fpath))
                  abs_file = abs_file.replace(build_prefix, store_src)
                  entry["file"] = abs_file

              # Set directory to the source root
              entry["directory"] = store_src

              # Fix paths in the command string
              cmd = entry.get("command", "")
              cmd = cmd.replace(build_dir, store_src)
              cmd = cmd.replace(build_prefix, store_src)
              entry["command"] = cmd

          with open(output_file, "w") as f:
              json.dump(db, f, indent=2)
        '';
      in
      ''
        mkdir -p $out
        if [ -f build/compile_commands.json ]; then
          ${pkgs.buildPackages.python3}/bin/python3 \
            ${fixPathsScript} \
            "${src}" \
            "$sourceRoot" \
            build/compile_commands.json \
            $out/compile_commands.json
        else
          echo "WARNING: compile_commands.json not found, creating empty one"
          echo "[]" > $out/compile_commands.json
        fi
      '';
  };

  # ── Tool runner scripts ────────────────────────────────────────────

  clang-tidy-runner = pkgs.writeShellApplication {
    name = "run-clang-tidy-analysis";
    runtimeInputs = with pkgs; [
      clang-tools
      coreutils
      findutils
      gnugrep
    ];
    text = ''
      compile_db="$1"
      source_dir="$2"
      output_dir="$3"

      echo "=== clang-tidy Analysis ==="
      echo "Using compilation database: $compile_db"

      # Find all .cc source files in library directories
      find "$source_dir/src" -name '*.cc' -not -path '*/test*' -print0 | \
        xargs -0 -P "$(nproc)" -I{} \
          clang-tidy -p "$compile_db" --header-filter='src/.*' {} \
        > "$output_dir/report.txt" 2>&1 || true

      findings=$(grep -c ': warning:\|: error:' "$output_dir/report.txt" || echo "0")
      echo "$findings" > "$output_dir/count.txt"
    '';
  };

  cppcheck-runner = pkgs.writeShellApplication {
    name = "run-cppcheck-analysis";
    runtimeInputs = with pkgs; [
      cppcheck
      coreutils
      gnugrep
    ];
    text = ''
      compile_db="$1"
      # shellcheck disable=SC2034
      source_dir="$2"
      output_dir="$3"

      echo "=== cppcheck Analysis ==="

      # Use --project for compilation database (cannot combine with source args)
      cppcheck \
        --project="$compile_db/compile_commands.json" \
        --enable=all \
        --std=c++20 \
        --suppress=missingInclude \
        --suppress=unusedFunction \
        --suppress=unmatchedSuppression \
        --xml \
        2> "$output_dir/report.xml" || true

      # Also produce a human-readable text report
      cppcheck \
        --project="$compile_db/compile_commands.json" \
        --enable=all \
        --std=c++20 \
        --suppress=missingInclude \
        --suppress=unusedFunction \
        --suppress=unmatchedSuppression \
        2> "$output_dir/report.txt" || true

      findings=$(grep -c '\(error\|warning\|style\|performance\|portability\)' "$output_dir/report.txt" || echo "0")
      echo "$findings" > "$output_dir/count.txt"
    '';
  };

  flawfinder-runner = pkgs.writeShellApplication {
    name = "run-flawfinder-analysis";
    runtimeInputs = with pkgs; [
      flawfinder
      coreutils
      gnugrep
    ];
    text = ''
      source_dir="$1"
      output_dir="$2"

      echo "=== flawfinder Analysis ==="

      flawfinder \
        --minlevel=1 \
        --columns \
        --context \
        --singleline \
        "$source_dir/src" \
        > "$output_dir/report.txt" 2>&1 || true

      # Extract hit count from flawfinder's summary line: "Hits = N"
      findings=$(grep -oP 'Hits = \K[0-9]+' "$output_dir/report.txt" || echo "0")
      echo "$findings" > "$output_dir/count.txt"
    '';
  };

  # Semgrep rules vendored for use inside the Nix sandbox (no network access).
  # Uses the auto config with --metrics=off and SEMGREP_ENABLE_VERSION_CHECK=0
  # to prevent any network calls.
  semgrep-runner = pkgs.writeShellApplication {
    name = "run-semgrep-analysis";
    runtimeInputs = with pkgs; [
      semgrep
      coreutils
      gnugrep
      cacert
    ];
    text = ''
      source_dir="$1"
      output_dir="$2"

      echo "=== semgrep Analysis ==="

      export SEMGREP_ENABLE_VERSION_CHECK=0
      export SEMGREP_SEND_METRICS=off
      export SSL_CERT_FILE="${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt"
      export OTEL_TRACES_EXPORTER=none
      # semgrep needs a writable HOME for its config/cache
      HOME="$(mktemp -d)"
      export HOME

      # Write inline semgrep rules for C/C++ security patterns
      # (avoids network access which fails in the Nix sandbox)
      cat > /tmp/semgrep-rules.yaml << 'RULES'
      rules:
        - id: dangerous-system-call
          pattern: system($ARG)
          message: Use of system() is dangerous — consider execve() or posix_spawn()
          languages: [cpp, c]
          severity: WARNING
        - id: unsafe-sprintf
          pattern: sprintf($BUF, ...)
          message: sprintf() has no bounds checking — use snprintf() instead
          languages: [cpp, c]
          severity: WARNING
        - id: unsafe-strcpy
          pattern: strcpy($DST, $SRC)
          message: strcpy() has no bounds checking — use strncpy() or std::string
          languages: [cpp, c]
          severity: WARNING
        - id: unsafe-strcat
          pattern: strcat($DST, $SRC)
          message: strcat() has no bounds checking — use strncat() or std::string
          languages: [cpp, c]
          severity: WARNING
        - id: potential-format-string
          patterns:
            - pattern: printf($FMT)
            - pattern-not: printf("...")
          message: Potential format string vulnerability — ensure format string is a literal
          languages: [cpp, c]
          severity: WARNING
      RULES

      semgrep \
        --config /tmp/semgrep-rules.yaml \
        --json \
        --metrics=off \
        --no-git-ignore \
        "$source_dir/src" \
        > "$output_dir/report.json" 2>&1 || true

      # Also produce a text report
      semgrep \
        --config /tmp/semgrep-rules.yaml \
        --metrics=off \
        --no-git-ignore \
        "$source_dir/src" \
        > "$output_dir/report.txt" 2>&1 || true

      # Count results from JSON output
      findings=$(grep -o '"check_id"' "$output_dir/report.json" | wc -l || echo "0")
      echo "$findings" > "$output_dir/count.txt"
    '';
  };

  # ── Helper for tools that need compilation database ─────────────

  mkCompileDbReport = name: script:
    pkgs.runCommand "nix-analysis-${name}" {
      nativeBuildInputs = [ script ];
    } ''
      mkdir -p $out
      ${lib.getExe script} ${compilationDb} ${src} $out
    '';

  # ── Helper for tools that work on raw source ────────────────────

  mkSourceReport = name: script:
    pkgs.runCommand "nix-analysis-${name}" {
      nativeBuildInputs = [ script ];
    } ''
      mkdir -p $out
      ${lib.getExe script} ${src} $out
    '';

  # ── GCC warning flags ──────────────────────────────────────────

  gccWarningFlags = [
    "-Wall"
    "-Wextra"
    "-Wpedantic"
    "-Wformat=2"
    "-Wformat-security"
    "-Wshadow"
    "-Wcast-qual"
    "-Wcast-align"
    "-Wwrite-strings"
    "-Wpointer-arith"
    "-Wconversion"
    "-Wsign-conversion"
    "-Wduplicated-cond"
    "-Wduplicated-branches"
    "-Wlogical-op"
    "-Wnull-dereference"
    "-Wdouble-promotion"
    "-Wfloat-equal"
    "-Walloca"
    "-Wvla"
    "-Werror=return-type"
    "-Werror=format-security"
  ];

  # ── Shared configuration for standalone source builds ─────────
  #
  # gcc-warnings and gcc-analyzer build from source in standalone derivations
  # so we can capture ninja output directly (component build logs are not
  # accessible from downstream derivations).

  mesonConfigureArgs = lib.concatStringsSep " " [
    "--prefix=$out"
    "-Dunit-tests=false"
    "-Djson-schema-checks=false"
    "-Dbindings=false"
    "-Ddoc-gen=false"
    "-Dbenchmarks=false"
  ];

  analysisNativeBuildInputs = [
    meson
    ninja
    pkg-config
    bison
    flex
    cmake
    pkgs.buildPackages.python3
  ];

  analysisBuildInputs = compilationDb.buildInputs;

  # ── GCC-based standalone builds ─────────────────────────────

  # Build the full project from source, capturing ninja output to extract warnings.
  mkGccAnalysisBuild = name: extraFlags: pkgs.stdenv.mkDerivation {
    pname = "nix-analysis-${name}";
    version = nixComponents.version;
    inherit src;

    nativeBuildInputs = analysisNativeBuildInputs;
    buildInputs = analysisBuildInputs;

    env.NIX_CXXFLAGS_COMPILE = lib.concatStringsSep " " extraFlags;

    dontFixup = true;
    doCheck = false;

    configurePhase = ''
      runHook preConfigure
      meson setup build ${mesonConfigureArgs} \
        || echo "WARNING: meson configure had errors"
      runHook postConfigure
    '';

    buildPhase = ''
      runHook preBuild
      # Build and capture all compiler output (ninja buffers subprocess output
      # and only prints it to stdout, so piping captures warnings too)
      ninja -C build -j''${NIX_BUILD_CORES:-1} 2>&1 | tee "$NIX_BUILD_TOP/build-output.log" || true
      runHook postBuild
    '';

    installPhase = ''
      mkdir -p $out
      # Extract warning/error lines from the build output
      grep -E ': warning:|: error:' "$NIX_BUILD_TOP/build-output.log" > $out/report.txt || true
      findings=$(wc -l < $out/report.txt)
      echo "$findings" > $out/count.txt

      # Include full build log for reference
      cp "$NIX_BUILD_TOP/build-output.log" $out/full-build.log

      {
        echo "=== ${name} Analysis ==="
        echo ""
        echo "Flags: ${lib.concatStringsSep " " extraFlags}"
        echo "Findings: $findings warnings/errors"
        if [ "$findings" -gt 0 ]; then
          echo ""
          echo "=== Warnings ==="
          cat $out/report.txt
        fi
      } > $out/summary.txt
    '';
  };

  # Sanitizers: reuse the existing buildWithSanitizers pattern from hydra.nix.
  # nix-everything has doCheck=true and checkInputs that trigger all tests.run
  # derivations, so building it actually RUNS all unit and functional tests
  # with sanitized binaries.
  sanitizer-components = nixComponents.overrideScope (
    self: super: {
      withASan = !pkgs.stdenv.buildPlatform.isDarwin;
      withUBSan = true;
      nix-expr = super.nix-expr.override { enableGC = false; };
      nix-perl-bindings = null;
    }
  );

  # ── Individual tool targets ────────────────────────────────────

  targets = rec {
    clang-tidy = mkCompileDbReport "clang-tidy" clang-tidy-runner;

    cppcheck = mkCompileDbReport "cppcheck" cppcheck-runner;

    flawfinder = mkSourceReport "flawfinder" flawfinder-runner;

    semgrep = mkSourceReport "semgrep" semgrep-runner;

    gcc-warnings = mkGccAnalysisBuild "gcc-warnings" gccWarningFlags;

    gcc-analyzer = mkGccAnalysisBuild "gcc-analyzer" [
      "-fanalyzer"
      "-fdiagnostics-plain-output"
    ];

    # Sanitizers: building nix-everything triggers all tests.run derivations
    # (via checkInputs with doCheck=true), executing the full test suite with
    # ASan + UBSan instrumented binaries. Any sanitizer violation causes a
    # test failure, which fails this derivation.
    sanitizers = pkgs.runCommand "nix-analysis-sanitizers" { } ''
      mkdir -p $out

      # nix-everything builds all components and runs all tests via checkInputs
      ln -s ${sanitizer-components.nix-everything} $out/build-output

      {
        echo "=== ASan + UBSan Analysis ==="
        echo ""
        echo "All components built with AddressSanitizer + UndefinedBehaviorSanitizer."
        echo "All unit tests (nix-util-tests, nix-store-tests, nix-expr-tests,"
        echo "nix-fetchers-tests, nix-flake-tests) and functional tests executed"
        echo "with sanitizer instrumentation."
        echo ""
        echo "Result: All tests passed — no sanitizer violations detected."
        echo ""
        echo "Sanitized build output: ${sanitizer-components.nix-everything}"
      } > $out/report.txt

      echo "0" > $out/count.txt
    '';

    # ── Combined targets ───────────────────────────────────────

    quick = pkgs.runCommand "nix-analysis-quick" { } ''
      mkdir -p $out
      ln -s ${clang-tidy} $out/clang-tidy
      ln -s ${cppcheck} $out/cppcheck
      {
        echo "=== Analysis Summary (quick) ==="
        echo ""
        echo "clang-tidy: $(cat ${clang-tidy}/count.txt) findings"
        echo "cppcheck:   $(cat ${cppcheck}/count.txt) findings"
        echo ""
        echo "Run 'nix build .#analysis-standard' for more thorough analysis."
      } > $out/summary.txt
      cat $out/summary.txt
    '';

    standard = pkgs.runCommand "nix-analysis-standard" { } ''
      mkdir -p $out
      ln -s ${clang-tidy} $out/clang-tidy
      ln -s ${cppcheck} $out/cppcheck
      ln -s ${flawfinder} $out/flawfinder
      ln -s ${gcc-warnings} $out/gcc-warnings
      {
        echo "=== Analysis Summary (standard) ==="
        echo ""
        echo "clang-tidy:   $(cat ${clang-tidy}/count.txt) findings"
        echo "cppcheck:     $(cat ${cppcheck}/count.txt) findings"
        echo "flawfinder:   $(cat ${flawfinder}/count.txt) findings"
        echo "gcc-warnings: $(cat ${gcc-warnings}/count.txt) findings"
        echo ""
        echo "Run 'nix build .#analysis-deep' for full analysis including"
        echo "GCC -fanalyzer, semgrep, and sanitizer builds."
      } > $out/summary.txt
      cat $out/summary.txt
    '';

    deep = pkgs.runCommand "nix-analysis-deep" { } ''
      mkdir -p $out
      ln -s ${clang-tidy} $out/clang-tidy
      ln -s ${cppcheck} $out/cppcheck
      ln -s ${flawfinder} $out/flawfinder
      ln -s ${gcc-warnings} $out/gcc-warnings
      ln -s ${gcc-analyzer} $out/gcc-analyzer
      ln -s ${semgrep} $out/semgrep
      ln -s ${sanitizers} $out/sanitizers
      {
        echo "=== Analysis Summary (deep) ==="
        echo ""
        echo "clang-tidy:   $(cat ${clang-tidy}/count.txt) findings"
        echo "cppcheck:     $(cat ${cppcheck}/count.txt) findings"
        echo "flawfinder:   $(cat ${flawfinder}/count.txt) findings"
        echo "gcc-warnings: $(cat ${gcc-warnings}/count.txt) findings"
        echo "gcc-analyzer: $(cat ${gcc-analyzer}/count.txt) findings"
        echo "semgrep:      $(cat ${semgrep}/count.txt) findings"
        echo "sanitizers:   $(cat ${sanitizers}/count.txt) findings"
        echo ""
        echo "All analysis tools completed."
      } > $out/summary.txt
      cat $out/summary.txt
    '';
  };

in
targets
