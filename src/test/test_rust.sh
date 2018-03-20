#!/bin/sh
# Test all Rust crates

set -e

manifests=`find ${abs_top_srcdir:-.}/src/rust/ -mindepth 2 -maxdepth 2 -name 'Cargo.toml'`

exitcode=0

for manifest in $manifests; do
    cd "${abs_top_builddir:-../../..}/src/rust"
    CARGO_TARGET_DIR="${abs_top_builddir:-../../..}/src/rust/target" \
      CARGO_HOME="${abs_top_builddir:-../../..}/src/rust" \
      "${CARGO:-cargo}" test --all-features ${CARGO_ONLINE-"--frozen"} \
      --manifest-path "${manifest}" \
	|| exitcode=1
    cd -
done

exit $exitcode
