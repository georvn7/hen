#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Upload selected .jsonl files to a Hugging Face dataset repo.

Usage:
  ./upload_hf_jsonl.sh --repo <username/dataset_repo> [options] <file1.jsonl> [file2.jsonl ...]

Options:
  --repo <id>           Hugging Face repo id, e.g. georvn/my-debug-dataset (required)
  --private             Create repo as private (default if creating: public)
  --public              Create repo as public
  --path-prefix <path>  Prefix path inside repo, e.g. runs/run_2026_02_22
  --no-create           Do not create repo automatically
  -h, --help            Show this help

Examples:
  ./upload_hf_jsonl.sh --repo georvn/std-rave-debug \
    /Users/georvn/projects/std-rave/SimpleC/dataset/S0.../train_dbg_sft.jsonl \
    /Users/georvn/projects/std-rave/SimpleC/dataset/S0.../train_run_sft.jsonl

  ./upload_hf_jsonl.sh --repo georvn/std-rave-debug --private \
    --path-prefix S0_return_constant_generate_assembly \
    ./SimpleC/dataset/S0_return_constant_generate_assembly/train_dbg_sft.jsonl
USAGE
}

CLI_MODE=""

detect_cli() {
  if command -v hf >/dev/null 2>&1; then
    CLI_MODE="hf"
  elif command -v huggingface-cli >/dev/null 2>&1; then
    CLI_MODE="legacy"
  elif python3 - <<'PY' >/dev/null 2>&1
import importlib.util, sys
sys.exit(0 if importlib.util.find_spec('huggingface_hub.commands.huggingface_cli') else 1)
PY
  then
    CLI_MODE="py"
  else
    echo "ERROR: No Hugging Face CLI found." >&2
    echo "Install in venv: python3 -m pip install -U huggingface_hub" >&2
    exit 1
  fi
}

hf_whoami() {
  case "$CLI_MODE" in
    hf) hf auth whoami ;;
    legacy) huggingface-cli whoami ;;
    py) python3 -m huggingface_hub.commands.huggingface_cli whoami ;;
  esac
}

hf_repo_create() {
  local repo_id="$1"
  local visibility="$2"

  case "$CLI_MODE" in
    hf)
      local args=(repo create "$repo_id" --repo-type dataset --exist-ok)
      if [[ "$visibility" == "private" ]]; then
        args+=(--private)
      fi
      hf "${args[@]}"
      ;;
    legacy)
      local args=(repo create "$repo_id" --type dataset)
      if [[ "$visibility" == "private" ]]; then
        args+=(--private)
      fi
      huggingface-cli "${args[@]}"
      ;;
    py)
      local args=(repo create "$repo_id" --type dataset)
      if [[ "$visibility" == "private" ]]; then
        args+=(--private)
      fi
      python3 -m huggingface_hub.commands.huggingface_cli "${args[@]}"
      ;;
  esac
}

hf_upload_file() {
  local repo_id="$1"
  local local_path="$2"
  local path_in_repo="$3"

  case "$CLI_MODE" in
    hf)
      hf upload "$repo_id" "$local_path" "$path_in_repo" --repo-type dataset
      ;;
    legacy)
      huggingface-cli upload "$repo_id" "$local_path" "$path_in_repo" --repo-type dataset
      ;;
    py)
      python3 -m huggingface_hub.commands.huggingface_cli upload "$repo_id" "$local_path" "$path_in_repo" --repo-type dataset
      ;;
  esac
}

REPO_ID=""
CREATE_REPO=1
VISIBILITY="public"
PATH_PREFIX=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo)
      [[ $# -ge 2 ]] || { echo "ERROR: --repo requires a value" >&2; exit 1; }
      REPO_ID="$2"
      shift 2
      ;;
    --private)
      VISIBILITY="private"
      shift
      ;;
    --public)
      VISIBILITY="public"
      shift
      ;;
    --path-prefix)
      [[ $# -ge 2 ]] || { echo "ERROR: --path-prefix requires a value" >&2; exit 1; }
      PATH_PREFIX="$2"
      shift 2
      ;;
    --no-create)
      CREATE_REPO=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    -*)
      echo "ERROR: Unknown option: $1" >&2
      usage
      exit 1
      ;;
    *)
      break
      ;;
  esac
done

if [[ -z "$REPO_ID" ]]; then
  echo "ERROR: --repo is required" >&2
  usage
  exit 1
fi

if [[ $# -lt 1 ]]; then
  echo "ERROR: Provide at least one .jsonl file path" >&2
  usage
  exit 1
fi

detect_cli

# Validate auth early
if ! hf_whoami >/dev/null 2>&1; then
  if [[ "$CLI_MODE" == "hf" ]]; then
    echo "ERROR: Not logged in to Hugging Face. Run: hf auth login" >&2
  else
    echo "ERROR: Not logged in to Hugging Face. Run: huggingface-cli login" >&2
  fi
  exit 1
fi

if [[ "$CREATE_REPO" -eq 1 ]]; then
  set +e
  create_output=$(hf_repo_create "$REPO_ID" "$VISIBILITY" 2>&1)
  create_code=$?
  set -e

  if [[ $create_code -ne 0 ]]; then
    if echo "$create_output" | grep -Eiq "already exists|409|Conflict|exist-ok|already created"; then
      echo "Repo already exists: $REPO_ID"
    else
      echo "ERROR: Failed to create repo '$REPO_ID'" >&2
      echo "$create_output" >&2
      exit 1
    fi
  else
    if echo "$create_output" | grep -Eiq "already exists|already created"; then
      echo "Repo already exists: $REPO_ID"
    else
      echo "Created repo: $REPO_ID ($VISIBILITY)"
    fi
  fi
fi

repo_url="https://huggingface.co/datasets/${REPO_ID}"
uploaded=0
uploaded_paths=()
uploaded_urls=()

for file in "$@"; do
  if [[ ! -f "$file" ]]; then
    echo "ERROR: File not found: $file" >&2
    exit 1
  fi

  if [[ "${file##*.}" != "jsonl" ]]; then
    echo "ERROR: Not a .jsonl file: $file" >&2
    exit 1
  fi

  # Keep structure when file is under current working directory, else use basename.
  path_in_repo=""
  if [[ "$file" = /* ]]; then
    if [[ "$file" == "$PWD/"* ]]; then
      path_in_repo="${file#$PWD/}"
    else
      path_in_repo="$(basename "$file")"
    fi
  else
    path_in_repo="$file"
  fi

  if [[ -n "$PATH_PREFIX" ]]; then
    path_in_repo="${PATH_PREFIX%/}/${path_in_repo}"
  fi

  echo "Uploading: $file -> $path_in_repo"
  hf_upload_file "$REPO_ID" "$file" "$path_in_repo"

  safe_path="${path_in_repo// /%20}"
  uploaded_paths+=("$path_in_repo")
  uploaded_urls+=("${repo_url}/resolve/main/${safe_path}")
  uploaded=$((uploaded + 1))
done

echo ""
echo "Done. Uploaded $uploaded file(s) to dataset repo: $REPO_ID"
echo "Dataset URL: $repo_url"
echo "Uploaded file links:"
for i in "${!uploaded_paths[@]}"; do
  echo "- ${uploaded_paths[$i]}"
  echo "  ${uploaded_urls[$i]}"
done
