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
COMMIT=0
STASH_OK=0
INDEX=0

function usage()
{
    cat<<EOF
${SCRIPTNAME} [-h] [-f] [-v] [-c] [-s] [-i]

Runs autoformatters on the Tor source code.

arguments:
   -h: show this help text
   -v: verbose output
   -f: force-run the script (even if the working tree is dirty)
   -c: commit the results of running the script.
   -s: If committing, stash the working tree if it's dirty.
   -i: Run the formatters on the git index, stashing unstaged changes
       first.

env vars:
   PERL, PYTHON, CARGO_FMT: paths to tools

   abs_top_srcdir: where to look for the Tor source
EOF
}

while getopts "hvfcis" opt; do
    case "$opt" in
        h) usage
           exit 0
           ;;
        f) FORCE=1
           ;;
        v) VERBOSE=1
           ;;
        c) COMMIT=1
           ;;
        s) STASH_OK=1
           ;;
        i) INDEX=1
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

if [[ $COMMIT = 1 && $FORCE = 1 ]]; then
    warn "Can't use -c (commit) with -f (force)"
    exit 1
elif [[ $INDEX = 1 && $FORCE = 1 ]]; then
    warn "Can't use -i (index) with -f (force)"
    exit 1
fi

function tree_is_clean()
{
    output="$(git status --untracked-files=no --porcelain)" && [[ -z "$output" ]]
}

cd "${SRCDIR}"

# ======================================================================
# Check whether the working tree is dirty.
#
# If it isn't dirty, great.
#
# If it is dirty, then we don't procede unless we got -f or -cs or -i

WANT_TO_STASH=0
STASH_ARGS=

if [[ $INDEX = 1 ]]; then
    if git diff --exit-code >/dev/null ; then
        note "No un-added changes to stash."
    else
        WANT_TO_STASH=1
        STASH_ARGS=--keep-index
    fi
elif [[ "$FORCE" = 0 ]]; then
   if tree_is_clean; then
       note "Working tree is clean"
   elif [[ $COMMIT = 1 && $STASH_OK = 1 ]]; then
       note "Working tree is dirty, going to try git-stash before committing."
       WANT_TO_STASH=1
   elif [[ $COMMIT = 1 ]]; then
       warn "There are uncommitted changes in the working tree, and -s (stash) was not specified. Can't procede."
       exit 1
   else
       warn "There are uncommitted changes in the working tree. Use -f (force) if you want to autostyle anyway."
       exit 1
   fi
else
    note "Received the -f flag: not checking whether tree is clean."
fi

# ======================================================================
# Stash if needed.

STASHED=0
if [[ $WANT_TO_STASH = 1 ]]; then
    git stash push -m 'About to run autostyle.' $STASH_ARGS
    STASHED=1
fi

# ======================================================================
# 1. update versions.

note "Updating versions"
abs_top_srcdir="${SRCDIR}" "${PYTHON}" "${SCRIPTDIR}/update_versions.py" \
              --quiet

# ======================================================================
# 1.2. Update copyrights.

note "Updating copyrights"
"${PERL}" "${SCRIPTDIR}/updateCopyright.pl" "${OWNED_TOR_C_FILES[@]}"

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

# ======================================================================
# DONE: Commit if we are supposed to commit.

if [[ $COMMIT = 1 ]] && tree_is_clean; then
    echo "Nothing to commit."
elif [[ $COMMIT = 1 ]]; then
    N=$(git status --untracked-files=no --porcelain | wc -l)
    git commit -a -F - <<EOF
Ran ${SCRIPTNAME} on the respository: $N files changed.

Files changed:
$(git status --untracked-files=no --porcelain | cut -c 4- | sed -e 's/^/    /')

This commit was automatically generated by ${SCRIPTNAME} $@
EOF
fi

# Add if we are supposed to add.
if [[ $INDEX = 1 ]]; then
    git add --update .
fi

# Now retrieve anything that we had stashed.

if [[ $STASHED = 1 ]]; then
    git stash pop
fi
