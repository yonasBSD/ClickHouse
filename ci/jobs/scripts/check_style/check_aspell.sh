#!/usr/bin/env bash

# force-enable double star globbing
shopt -s globstar

# Perform spell checking on the docs

if [[ ${1:-} == "--help" ]] || [[ ${1:-} == "-h" ]]; then
    echo "Usage $0 [--help|-h] [-i [filename]]"
    echo "  --help|-h: print this help"
    echo "  -i: interactive mode. If filename is specified, check only this file, otherwise check all files"
    exit 0
fi

ROOT_PATH="."

CHECK_LANG=en

ASPELL_IGNORE_PATH="${ROOT_PATH}/ci/jobs/scripts/check_style/aspell-ignore/${CHECK_LANG}"

# Use this to filter out lines we don't wanna consider in spell-check - slugs, imports
preprocess_file() {
    local file=$1
    sed -E 's/\{#[^}]*\}//g' "$file" | grep -Ev '^(slug:|import [[:alnum:]_]+ from)'
}

if [[ ${1:-} == "-i" ]]; then
    if [[ ! -z ${2:-} ]]; then
        FILES_TO_CHECK=${ROOT_PATH}/docs/${CHECK_LANG}/${2}
    else
        FILES_TO_CHECK=${ROOT_PATH}/docs/${CHECK_LANG}/**/*.md
    fi
    for fname in ${FILES_TO_CHECK}; do
        temp_file=$(mktemp)
        preprocess_file "$fname" > "$temp_file"
        echo "Checking $fname"
        aspell --personal=aspell-dict.txt --add-sgml-skip=code --encoding=utf-8 --mode=markdown -W 3 --lang=${CHECK_LANG} --home-dir=${ASPELL_IGNORE_PATH} -c "$fname"
        mv "$temp_file" "$fname"
    done
    exit
fi

STATUS=0
for fname in ${ROOT_PATH}/docs/${CHECK_LANG}/**/*.md; do
    errors=$(preprocess_file "$fname" \
        | aspell list \
            -W 3 \
            --personal=aspell-dict.txt \
            --add-sgml-skip=code \
            --encoding=utf-8 \
            --mode=markdown \
            --lang=${CHECK_LANG} \
            --home-dir=${ASPELL_IGNORE_PATH} \
        | sort | uniq)
    if [ ! -z "$errors" ]; then
        STATUS=1
        echo "====== $fname ======"
        echo "$errors"
    fi
done

if (( STATUS != 0 )); then
    echo "====== Errors found ======"
    echo "To exclude some words add them to the dictionary file \"${ASPELL_IGNORE_PATH}/aspell-dict.txt\""
    echo "You can also run '$(realpath --relative-base=${ROOT_PATH} ${0}) -i' to see the errors interactively and fix them or add to the dictionary file"
fi

exit ${STATUS}
