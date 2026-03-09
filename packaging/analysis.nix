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
        # ── Category 1: Unsafe C String/Memory Functions ──────────────
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
        - id: unsafe-vsprintf
          pattern: vsprintf($BUF, ...)
          message: vsprintf() has no bounds checking — use vsnprintf() instead
          languages: [cpp, c]
          severity: WARNING
        - id: unsafe-gets
          pattern: gets($BUF)
          message: gets() is always unsafe (unbounded read) — use fgets() instead
          languages: [cpp, c]
          severity: ERROR
        - id: unsafe-strncpy-strlen
          pattern: strncpy($D, $S, strlen($S))
          message: strncpy with strlen(src) as length defeats the purpose of bounds checking
          languages: [cpp, c]
          severity: WARNING

        # ── Category 2: Memory Management ─────────────────────────────
        - id: raw-malloc
          pattern: malloc(...)
          message: Prefer C++ allocation (new/make_unique) over raw malloc in C++ code
          languages: [cpp]
          severity: INFO
        - id: raw-realloc
          pattern: realloc($PTR, $SIZE)
          message: Prefer C++ containers over raw realloc — risk of memory leak on failure
          languages: [cpp]
          severity: INFO
        - id: raw-free-in-cpp
          pattern: free($PTR)
          message: Prefer C++ RAII (unique_ptr/shared_ptr) over raw free() in C++ code
          languages: [cpp]
          severity: INFO
        - id: delete-this
          pattern: delete this
          message: "'delete this' is dangerous — ensure no member access after this point"
          languages: [cpp]
          severity: WARNING
        - id: memset-zero-length
          pattern: memset($B, $V, 0)
          message: memset with length 0 is a no-op — check arguments
          languages: [cpp, c]
          severity: WARNING
        - id: memcpy-sizeof-pointer
          pattern: memcpy($D, $S, sizeof($PTR))
          message: memcpy with sizeof(pointer) likely copies only pointer size, not data
          languages: [cpp, c]
          severity: WARNING

        # ── Category 3: Race Conditions / TOCTOU ─────────────────────
        - id: toctou-access
          pattern: access($PATH, ...)
          message: access() is prone to TOCTOU races — use faccessat() or open-then-check
          languages: [cpp, c]
          severity: WARNING
        - id: chmod-on-pathname
          pattern: chmod($PATH, $MODE)
          message: chmod on pathname is TOCTOU-prone — prefer fchmod() on an open fd
          languages: [cpp, c]
          severity: INFO
        - id: chown-on-pathname
          patterns:
            - pattern-either:
                - pattern: chown($PATH, $UID, $GID)
                - pattern: lchown($PATH, $UID, $GID)
          message: chown/lchown on pathname is TOCTOU-prone — prefer fchown() on an open fd
          languages: [cpp, c]
          severity: INFO
        - id: insecure-rand
          patterns:
            - pattern-either:
                - pattern: rand()
                - pattern: srand(...)
          message: rand()/srand() are not cryptographically secure — use <random> or getrandom()
          languages: [cpp, c]
          severity: WARNING
        - id: toctou-stat
          patterns:
            - pattern-either:
                - pattern: stat($PATH, $BUF)
                - pattern: lstat($PATH, $BUF)
          message: stat/lstat on pathname is TOCTOU-prone — prefer fstat() on an open fd
          languages: [cpp, c]
          severity: INFO

        # ── Category 4: Type Safety and Casts ─────────────────────────
        - id: const-cast
          pattern: const_cast<$T>($E)
          message: const_cast removes const — ensure this is intentional and safe
          languages: [cpp]
          severity: WARNING
        - id: reinterpret-cast
          pattern: reinterpret_cast<$T>($E)
          message: reinterpret_cast is unsafe — verify type punning is valid
          languages: [cpp]
          severity: INFO
        - id: c-style-pointer-cast
          pattern: ($TYPE *) $EXPR
          message: C-style pointer cast — prefer static_cast/reinterpret_cast for clarity
          languages: [cpp]
          severity: INFO
        - id: stoi-unchecked
          patterns:
            - pattern-either:
                - pattern: std::stoi(...)
                - pattern: std::stol(...)
                - pattern: std::stoul(...)
          message: std::stoi/stol/stoul throw on invalid input — ensure exception handling
          languages: [cpp]
          severity: INFO

        # ── Category 5: Error Handling ────────────────────────────────
        - id: catch-all-no-rethrow
          patterns:
            - pattern: "catch (...) { ... }"
            - pattern-not-inside: "catch (...) { ... throw; ... }"
          message: Catch-all without rethrow may silently swallow errors
          languages: [cpp]
          severity: WARNING
        - id: empty-catch-block
          pattern: "catch ($T $E) { }"
          message: Empty catch block silently ignores exception
          languages: [cpp]
          severity: WARNING
        - id: throw-in-destructor
          pattern: |
            ~$CLASS(...) { ... throw $EXC; ... }
          message: Throwing in destructor can cause std::terminate — use noexcept
          languages: [cpp]
          severity: ERROR
        - id: strerror-thread-unsafe
          pattern: strerror($E)
          message: strerror() is not thread-safe — use strerror_r() or std::system_category()
          languages: [cpp, c]
          severity: INFO

        # ── Category 6: Resource Management ──────────────────────────
        - id: fopen-raw-file-pointer
          pattern: fopen($P, $M)
          message: Raw FILE* from fopen — prefer RAII wrapper or std::fstream in C++
          languages: [cpp]
          severity: INFO
        - id: signal-not-sigaction
          pattern: signal($SIG, $H)
          message: signal() has portability issues — prefer sigaction()
          languages: [cpp, c]
          severity: WARNING
        - id: vfork-usage
          pattern: vfork()
          message: vfork() shares address space with parent — prefer posix_spawn() or fork()
          languages: [cpp, c]
          severity: WARNING
        - id: detached-thread
          pattern: $T.detach()
          message: Detached threads are hard to clean up — ensure this is intentional
          languages: [cpp]
          severity: WARNING
        - id: popen-usage
          pattern: popen($CMD, $MODE)
          message: popen() invokes shell — risk of command injection, prefer posix_spawn()
          languages: [cpp, c]
          severity: WARNING

        # ── Category 7: Privilege and Command Execution ───────────────
        - id: setuid-setgid
          patterns:
            - pattern-either:
                - pattern: setuid(...)
                - pattern: setgid(...)
          message: setuid/setgid changes process privileges — ensure proper error checking
          languages: [cpp, c]
          severity: WARNING
        - id: chroot-usage
          pattern: chroot($PATH)
          message: chroot alone is not a security boundary — ensure chdir+drop privileges
          languages: [cpp, c]
          severity: WARNING
        - id: getenv-unchecked
          pattern: getenv($VAR)
          message: getenv() returns nullable pointer — check for NULL before use
          languages: [cpp, c]
          severity: INFO
        - id: sqlite-exec-non-literal
          patterns:
            - pattern: sqlite3_exec($DB, $SQL, ...)
            - pattern-not: sqlite3_exec($DB, "...", ...)
          message: Non-literal SQL in sqlite3_exec — risk of SQL injection, use prepared statements
          languages: [cpp, c]
          severity: WARNING
        - id: exec-family
          patterns:
            - pattern-either:
                - pattern: execvp(...)
                - pattern: execv(...)
                - pattern: execve(...)
          message: exec-family call — ensure arguments are validated and paths are absolute
          languages: [cpp, c]
          severity: INFO

        # ── Category 8: Concurrency ───────────────────────────────────
        - id: lock-guard-temporary
          pattern: std::lock_guard<$T>($M);
          message: Lock guard as temporary is destroyed immediately — assign to a variable
          languages: [cpp]
          severity: ERROR
        - id: relaxed-memory-order
          patterns:
            - pattern-either:
                - pattern: $X.load(std::memory_order_relaxed)
                - pattern: $X.store(..., std::memory_order_relaxed)
          message: memory_order_relaxed provides no synchronization — ensure this is intentional
          languages: [cpp]
          severity: INFO
        - id: thread-creation
          pattern: std::thread($FN, ...)
          message: std::thread creation — ensure the thread is joined or detached before scope exit
          languages: [cpp]
          severity: INFO

        # ── Category 9: Code Quality / Defensive Programming ─────────
        - id: goto-usage
          pattern: goto $LABEL
          message: goto usage — consider structured control flow alternatives
          languages: [cpp, c]
          severity: INFO
        - id: assert-side-effect
          patterns:
            - pattern-either:
                - pattern: assert($X = $Y)
                - pattern: assert($X++)
          message: Side effect in assert() — expression is removed in release builds (NDEBUG)
          languages: [cpp, c]
          severity: ERROR
        - id: dangling-c-str
          pattern: $E.str().c_str()
          message: Dangling pointer — temporary std::string from str() is destroyed after c_str()
          languages: [cpp]
          severity: WARNING
        - id: using-namespace-std
          pattern: using namespace std;
          message: "'using namespace std' pollutes the global namespace — use specific imports"
          languages: [cpp]
          severity: INFO
        - id: fprintf-stderr
          pattern: fprintf(stderr, ...)
          message: Direct stderr output — consider using the project logging infrastructure
          languages: [cpp, c]
          severity: INFO
        - id: atoi-atol-usage
          patterns:
            - pattern-either:
                - pattern: atoi(...)
                - pattern: atol(...)
                - pattern: atof(...)
          message: atoi/atol/atof have no error checking — use strtol/strtod or std::stoi with try/catch
          languages: [cpp, c]
          severity: WARNING
        - id: alloca-usage
          pattern: alloca($SIZE)
          message: alloca() allocates on stack with no overflow check — prefer heap allocation
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
