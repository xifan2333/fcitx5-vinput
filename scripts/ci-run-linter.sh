#!/usr/bin/env bash
# Unified Clang-Format and Clang-Tidy quality gate runner.
# Aligned between local development and CI execution.
#
# Usage:
#   scripts/ci-run-linter.sh                      # Incremental: check modified files & affected translation units
#   scripts/ci-run-linter.sh --mode=full          # Full audit: scan all compilation units in compile_commands.json
#   scripts/ci-run-linter.sh --report-dir=DIR     # Directory for logs and summary (default: pr-report)
#   scripts/ci-run-linter.sh --build-dir=DIR      # Build directory with compile_commands.json (default: build)
#   scripts/ci-run-linter.sh --base-ref=REF       # Base git ref for diff calculation
#   scripts/ci-run-linter.sh --pr-number=NUM      # Record PR number for downstream comment bot
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

MODE="incremental"
BUILD_DIR="${REPO_ROOT}/build"
REPORT_DIR="${REPO_ROOT}/pr-report"
BASE_REF=""
PR_NUMBER=""

log_info() {
  printf "\033[1;34m[linter]\033[0m %s\n" "$*"
}

log_warn() {
  printf "\033[1;33m[linter:warn]\033[0m %s\n" "$*" >&2
}

log_error() {
  printf "\033[1;31m[linter:error]\033[0m %s\n" "$*" >&2
}

show_help() {
  cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Options:
  --mode=incremental|full   Linter scope: incremental PR diff (default) or full repository audit
  --build-dir=DIR           CMake build directory containing compile_commands.json (default: build)
  --report-dir=DIR          Directory to write report artifacts and logs (default: pr-report)
  --base-ref=REF            Base ref for diff calculation (auto-detected if omitted)
  -h, --help                Show this help message
EOF
}

parse_args() {
  for arg in "$@"; do
    case "$arg" in
    --mode=incremental) MODE="incremental" ;;
    --mode=full) MODE="full" ;;
    --build-dir=*) BUILD_DIR="${arg#--build-dir=}" ;;
    --report-dir=*) REPORT_DIR="${arg#--report-dir=}" ;;
    --base-ref=*) BASE_REF="${arg#--base-ref=}" ;;
    --pr-number=*) PR_NUMBER="${arg#--pr-number=}" ;;
    -h | --help)
      show_help
      exit 0
      ;;
    *)
      log_error "Unknown argument: $arg"
      show_help
      exit 1
      ;;
    esac
  done
}

find_tool() {
  local preferred="$1"
  local fallback="$2"
  if command -v "$preferred" >/dev/null 2>&1; then
    command -v "$preferred"
  elif command -v "$fallback" >/dev/null 2>&1; then
    command -v "$fallback"
  else
    return 1
  fi
}

resolve_base_ref() {
  if [ -n "$BASE_REF" ]; then
    echo "$BASE_REF"
    return 0
  fi

  # GitHub Actions Pull Request
  if [ -n "${GITHUB_BASE_REF:-}" ]; then
    local target="origin/${GITHUB_BASE_REF}"
    if ! git rev-parse --verify "$target" >/dev/null 2>&1; then
      git fetch origin "${GITHUB_BASE_REF}" --depth=1 >/dev/null 2>&1 || true
    fi
    echo "$target"
    return 0
  fi

  # GitHub Actions Push with before SHA (from env or GITHUB_EVENT_PATH)
  local before_sha="${GITHUB_EVENT_BEFORE:-}"
  if [ -z "$before_sha" ] && [ -n "${GITHUB_EVENT_PATH:-}" ] && [ -f "$GITHUB_EVENT_PATH" ]; then
    before_sha="$(jq -r '.before // empty' "$GITHUB_EVENT_PATH" 2>/dev/null || true)"
  fi
  if [ -n "$before_sha" ] && [ "$before_sha" != "0000000000000000000000000000000000000000" ]; then
    echo "$before_sha"
    return 0
  fi

  # Local git repository: merge-base with origin/main or HEAD~1
  local mb
  mb="$(git merge-base HEAD origin/main 2>/dev/null || true)"
  if [ -n "$mb" ]; then
    echo "$mb"
  elif git rev-parse --verify origin/main >/dev/null 2>&1; then
    echo "origin/main"
  else
    echo "HEAD~1"
  fi
}

collect_targets() {
  cd "${REPO_ROOT}"
  FORMAT_FILES=()
  TIDY_UNITS=()

  if [ "$MODE" = "full" ]; then
    log_info "Mode: full repository scan"
    mapfile -t FORMAT_FILES < <(find src tests -type f \( -name '*.cpp' -o -name '*.h' \) | sort)
    mapfile -t TIDY_UNITS < <(find src tests -type f -name '*.cpp' | sort)
    return 0
  fi

  local base
  base="$(resolve_base_ref)"
  log_info "Mode: incremental diff against ${base}"

  if ! git rev-parse --verify --quiet "${base}^{commit}" >/dev/null 2>&1; then
    log_warn "Base ref '${base}' not resolved locally; attempting shallow fetch..."
    git fetch origin "${base}" --depth=1 >/dev/null 2>&1 || true
    if ! git rev-parse --verify --quiet "${base}^{commit}" >/dev/null 2>&1; then
      log_error "Failed to resolve base ref '${base}' for incremental diff!"
      return 1
    fi
  fi

  local changed_files_raw=""
  if [ "${GITHUB_EVENT_NAME:-}" = "push" ] || [ -n "${GITHUB_EVENT_BEFORE:-}" ]; then
    if ! changed_files_raw="$(git diff --name-only "${base}" HEAD 2>/dev/null)"; then
      log_error "git diff against push base '${base}' failed!"
      return 1
    fi
  else
    if ! changed_files_raw="$(git diff --name-only "${base}...HEAD" 2>/dev/null)"; then
      if ! changed_files_raw="$(git diff --name-only "${base}" 2>/dev/null)"; then
        log_error "git diff against base '${base}' failed!"
        return 1
      fi
    fi
  fi

  # Also include unstaged and staged local changes if any (for local developer convenience)
  if [ -z "${GITHUB_ACTIONS:-}" ]; then
    local local_changes
    local_changes="$(git diff --name-only 2>/dev/null || true)"
    local cached_changes
    cached_changes="$(git diff --cached --name-only 2>/dev/null || true)"
    changed_files_raw=$(printf "%s\n%s\n%s\n" "$changed_files_raw" "$local_changes" "$cached_changes" | sort -u | grep -v '^$' || true)
  fi

  # Filter existing C++ files for format check
  mapfile -t FORMAT_FILES < <(echo "$changed_files_raw" | grep -E '^(src|tests)/.*\.(cpp|h)$' | while read -r f; do [ -f "$f" ] && echo "$f"; done || true)

  # Filter changed sources and headers for tidy analysis
  local changed_cpp=()
  local changed_h=()
  mapfile -t changed_cpp < <(echo "$changed_files_raw" | grep -E '^(src|tests)/.*\.cpp$' | while read -r f; do [ -f "$f" ] && echo "$f"; done || true)
  mapfile -t changed_h < <(echo "$changed_files_raw" | grep -E '^(src|tests)/.*\.h$' | while read -r f; do [ -f "$f" ] && echo "$f"; done || true)

  local candidate_units=("${changed_cpp[@]}")

  if [ "${#changed_h[@]}" -gt 0 ]; then
    log_info "Analyzing ${#changed_h[@]} modified header(s) for transitive translation units..."
    local all_h
    all_h=$(find src tests -type f -name '*.h')
    local all_cpp
    all_cpp=$(find src tests -type f -name '*.cpp')

    # Transitive closure of affected headers using BFS
    local affected_headers=("${changed_h[@]}")
    local queue=("${changed_h[@]}")

    while [ "${#queue[@]}" -gt 0 ]; do
      local current="${queue[0]}"
      queue=("${queue[@]:1}")
      local cur_base
      cur_base="$(basename "$current")"
      while IFS= read -r inc_h; do
        if [ -n "$inc_h" ]; then
          if [[ ! " ${affected_headers[*]} " =~ " ${inc_h} " ]]; then
            affected_headers+=("$inc_h")
            queue+=("$inc_h")
          fi
        fi
      done < <(grep -l -E "#include [\"<].*${cur_base}[\">]" $all_h 2>/dev/null || true)
    done

    log_info "Transitive header expansion found ${#affected_headers[@]} affected header(s)."

    for h in "${affected_headers[@]}"; do
      local h_base
      h_base="$(basename "$h")"
      while IFS= read -r inc_cpp; do
        [ -n "$inc_cpp" ] && candidate_units+=("$inc_cpp")
      done < <(grep -l -E "#include [\"<].*${h_base}[\">]" $all_cpp 2>/dev/null || true)
    done
  fi

  mapfile -t TIDY_UNITS < <(printf "%s\n" "${candidate_units[@]}" | sort -u | grep -v '^$' || true)
}

run_format() {
  local clang_format="$1"
  log_info "Running Clang-Format (${clang_format})..."

  if [ "${#FORMAT_FILES[@]}" -eq 0 ]; then
    log_info "No C++ files modified for formatting."
    echo "0" >"${REPORT_DIR}/format_failed"
    return 0
  fi

  log_info "Checking formatting on ${#FORMAT_FILES[@]} file(s)..."
  if ! "$clang_format" --dry-run --Werror "${FORMAT_FILES[@]}"; then
    log_error "Clang-Format detected formatting violations!"
    echo "1" >"${REPORT_DIR}/format_failed"
    return 1
  fi

  echo "0" >"${REPORT_DIR}/format_failed"
  return 0
}

run_tidy() {
  local run_tidy="$1"
  local clang_tidy="$2"
  log_info "Running Clang-Tidy (${run_tidy} using binary ${clang_tidy})..."

  if [ "${#TIDY_UNITS[@]}" -eq 0 ]; then
    log_info "No compilation units affected. Skipping Clang-Tidy analysis."
    echo "0" >"${REPORT_DIR}/tidy_failed"
    return 0
  fi

  local compile_db="${BUILD_DIR}/compile_commands.json"
  if [ ! -f "$compile_db" ]; then
    log_error "Compilation database not found: ${compile_db}"
    echo "Error: ${compile_db} does not exist. Run cmake configure first." >"${REPORT_DIR}/tidy.log"
    echo "1" >"${REPORT_DIR}/tidy_failed"
    return 1
  fi

  local target_regex=""
  if [ "$MODE" = "full" ]; then
    target_regex="^${REPO_ROOT}/(src|tests)/.*"
  else
    local regex_parts=()
    for f in "${TIDY_UNITS[@]}"; do
      local escaped
      escaped=$(echo "$f" | sed 's/\./\\./g')
      regex_parts+=("${escaped}")
    done
    local joined
    joined=$(
      IFS='|'
      echo "${regex_parts[*]}"
    )
    target_regex="^${REPO_ROOT}/(${joined})$"
  fi

  log_info "Target regex for Clang-Tidy: ${target_regex}"

  local matched_count
  if ! matched_count=$(jq --arg r "$target_regex" '[.[] | select(.file | test($r))] | length' "$compile_db" 2>&1); then
    log_error "Failed to query compile_commands.json: ${matched_count}"
    echo "Error: Failed to query compile_commands.json: ${matched_count}" >"${REPORT_DIR}/tidy.log"
    echo "1" >"${REPORT_DIR}/tidy_failed"
    return 1
  fi
  log_info "Matched ${matched_count} compilation unit(s) in compile_commands.json"

  if [ "$matched_count" -eq 0 ]; then
    if [ "$MODE" = "full" ]; then
      log_error "Zero compilation units matched in full mode!"
      echo "Error: 0 compilation units matched in full mode!" >"${REPORT_DIR}/tidy.log"
      echo "1" >"${REPORT_DIR}/tidy_failed"
      return 1
    else
      log_warn "Target units not present in compile_commands.json; skipping execution."
      echo "0" >"${REPORT_DIR}/tidy_failed"
      return 0
    fi
  fi

  local header_filter="^${REPO_ROOT}/(src|tests)/.*"
  local tidy_failed=0

  if ! "$run_tidy" -clang-tidy-binary "$clang_tidy" \
    -header-filter="${header_filter}" \
    -p "${BUILD_DIR}" \
    -j "$(nproc)" \
    "${target_regex}" >"${REPORT_DIR}/tidy.log" 2>&1; then
    tidy_failed=1
  fi

  if grep -q "warning:" "${REPORT_DIR}/tidy.log" || grep -q "error:" "${REPORT_DIR}/tidy.log"; then
    tidy_failed=1
  fi

  echo "${tidy_failed}" >"${REPORT_DIR}/tidy_failed"
  return "$tidy_failed"
}

generate_summary() {
  local format_status="unknown"
  [ -f "${REPORT_DIR}/format_failed" ] && format_status="$(cat "${REPORT_DIR}/format_failed")"

  local tidy_status="unknown"
  [ -f "${REPORT_DIR}/tidy_failed" ] && tidy_status="$(cat "${REPORT_DIR}/tidy_failed")"

  {
    echo "### Clang-Tidy & Clang-Format Report"
    echo ""

    if [ "$format_status" = "0" ]; then
      if [ "${#FORMAT_FILES[@]}" -eq 0 ]; then
        echo "- **Clang-Format**: Skipped (no modified C++ files)"
      elif [ "$MODE" = "incremental" ]; then
        echo "- **Clang-Format**: Passed cleanly (${#FORMAT_FILES[@]} file(s) checked)"
      else
        echo "- **Clang-Format**: Passed cleanly (full scan)"
      fi
    elif [ "$format_status" = "unknown" ]; then
      echo "- **Clang-Format**: Not run or failed during setup"
    else
      echo "- **Clang-Format**: Formatting violations detected"
    fi

    if [ "$tidy_status" = "0" ]; then
      if [ "${#TIDY_UNITS[@]}" -eq 0 ]; then
        echo "- **Clang-Tidy**: Skipped (no affected translation units)"
      elif [ "$MODE" = "incremental" ]; then
        echo "- **Clang-Tidy**: 0 warnings (${#TIDY_UNITS[@]} unit(s) checked)"
      else
        echo "- **Clang-Tidy**: 0 warnings (full repository scan)"
      fi
    elif [ "$tidy_status" = "unknown" ]; then
      echo "- **Clang-Tidy**: Not run or failed during setup"
    else
      echo "- **Clang-Tidy**: Violations detected"
      echo ""
      echo '```'
      if [ -f "${REPORT_DIR}/tidy.log" ]; then
        tail -n 200 "${REPORT_DIR}/tidy.log"
      fi
      echo '```'
    fi
  } >"${REPORT_DIR}/summary.md"

  # GitHub Actions step summary integration
  if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    cat "${REPORT_DIR}/summary.md" >>"${GITHUB_STEP_SUMMARY}"
  fi

  # Console output
  cat "${REPORT_DIR}/summary.md"
}

main() {
  parse_args "$@"
  mkdir -p "${REPORT_DIR}"

  if [ -n "$PR_NUMBER" ]; then
    echo "$PR_NUMBER" >"${REPORT_DIR}/pr_number"
  fi

  log_info "Repository root: ${REPO_ROOT}"
  log_info "Report directory: ${REPORT_DIR}"

  local clang_format
  clang_format="$(find_tool clang-format-21 clang-format || true)"
  if [ -z "$clang_format" ]; then
    log_error "Neither clang-format-21 nor clang-format found in PATH!"
    echo "1" >"${REPORT_DIR}/format_failed"
    echo "1" >"${REPORT_DIR}/tidy_failed"
    generate_summary
    exit 1
  fi

  local run_tidy
  run_tidy="$(find_tool run-clang-tidy-21 run-clang-tidy || true)"
  local clang_tidy
  clang_tidy="$(find_tool clang-tidy-21 clang-tidy || true)"
  if [ -z "$run_tidy" ] || [ -z "$clang_tidy" ]; then
    log_error "run-clang-tidy or clang-tidy binary not found in PATH!"
    echo "1" >"${REPORT_DIR}/format_failed"
    echo "1" >"${REPORT_DIR}/tidy_failed"
    generate_summary
    exit 1
  fi

  collect_targets

  local format_rc=0
  run_format "$clang_format" || format_rc=$?

  local tidy_rc=0
  run_tidy "$run_tidy" "$clang_tidy" || tidy_rc=$?

  generate_summary

  if [ "$format_rc" -ne 0 ] || [ "$tidy_rc" -ne 0 ]; then
    log_error "Linter quality gate failed!"
    exit 1
  fi

  log_info "All linter checks passed cleanly!"
  exit 0
}

main "$@"
