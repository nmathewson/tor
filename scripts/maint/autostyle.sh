#!/usr/bin/env bash

# Script to run all of our automatic code-styling tools.

set -e

# Set up some reasonable defaults.
PYTHON=${PYTHON:-python}
PERL=${PERL:-perl}
CARGO_FMT=${CARGO_FMT:-cargo-fmt}

# Note that we look for scripts in the same directory as _this_ script,
# not in the git repository.
SCRIPTDIR=$(dirname "$0")
SCRIPTNAME=$(basename "$0")

if [[ -z "${abs_top_srcdir:=}" ]]; then
    SRCDIR=$(git rev-parse --show-toplevel)
else
    SRCDIR=${abs_top_srcdir}
fi

OWNED_TOR_C_FILES=( \
                "${SRCDIR}"/src/lib/*/*.[ch] \
                "${SRCDIR}"/src/core/*/*.[ch] \
                "${SRCDIR}"/src/feature/*/*.[ch] \
                "${SRCDIR}"/src/app/*/*.[ch] \
                "${SRCDIR}"/src/test/*.[ch] \
                "${SRCDIR}"/src/test/*/*.[ch] \
                "${SRCDIR}"/src/tools/*.[ch] \
    )

VERBOSE=0
FORCE=0

function usage()
{
    cat<<EOF
${SCRIPTNAME} [-h] [-f] [-v]

Runs autoformatters on the Tor source code.

arguments:
   -h: show this help text
   -v: verbose output
   -f: force-run the script (even if the working tree is dirty)

env vars:
   PERL, PYTHON, CARGO_FMT: paths to tools

   abs_top_srcdir: where to look for the Tor source
EOF
}

while getopts "hvf" opt; do
    case "$opt" in
        h) usage
           exit 0
           ;;
        f) FORCE=1
           ;;
        v) VERBOSE=1
           ;;
        *) echo
           usage
           exit 1
           ;;
    esac
done

if [[ "$VERBOSE" = 1 ]]; then
    function note()
    {
        echo "$@"
    }
else
    function note()
    {
        true
    }
fi

function warn()
{
    echo "$@" 1>&2
}

function tree_is_clean()
{
    output="$(git status --untracked-files=no --porcelain)" && [[ -z "$output" ]]
}

cd "${SRCDIR}"

if [[ "$FORCE" = 0 ]]; then
   if tree_is_clean; then
       note "Working tree is clean"
   else
       warn "There are uncommitted changes in the working tree. Use -f if you want to autostyle anyway."
       exit 1
   fi
else
    note "Received the -f flag: not checking whether tree is clean."
fi

# ======================================================================
# 1. update versions.

note "Updating versions"
abs_top_srcdir="${SRCDIR}" "${PYTHON}" "${SCRIPTDIR}/update_versions.py" \
              --quiet

# ======================================================================
# 2. rustfmt.

if [[ -x $(command -v "${CARGO_FMT}") ]]; then
    note "Running rustfmt"
    cd "${SRCDIR}"/src/rust
    "${CARGO_FMT}" fmt --all --
    cd "${SRCDIR}"
fi

# ======================================================================
# 3. autostyle-ifdefs

note "Annotating ifdef directives"
"${PYTHON}" "${SCRIPTDIR}/annotate_ifdef_directives.py" \
            "${OWNED_TOR_C_FILES[@]}"

# ======================================================================
# 4. autostyle-operators

note "Replacing operators with OP_ macros"
"${PERL}" "${SCRIPTDIR}/test-operator-cleanup.pl" \
            "${OWNED_TOR_C_FILES[@]}"

# ======================================================================
# 5. Rectify includes.

note "Putting include paths in canonical form"
"${PYTHON}" "${SCRIPTDIR}/rectify_include_paths.py"
