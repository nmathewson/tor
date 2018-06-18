
use std::collections::HashMap;
use std::env;
use std::fs::File;
use std::io::prelude::*;
use std::io;

fn load_cfg() -> io::Result<HashMap<String,String>> {
    let mut path = env::var("OUT_DIR").unwrap().to_owned();
    path.push_str("/../../../../../../../config.cargo");

    let f = File::open(&path)?;
    let reader = io::BufReader::new(f);
    let mut map = HashMap::new();
    for line in reader.lines() {
        let s = line?;
        if s.starts_with("#") {
            continue;
        }
        let idx = match s.find("=") {
            None => continue,
            Some(x) => x
        };
        let (var,eq_val) = s.split_at(idx);
        let val = &eq_val[1..];
        map.insert(var.to_owned(), val.to_owned());
    }
    Ok(map)
}

fn component(s : &str) {
        println!("cargo:rustc-link-lib=static={}", s);
}

fn dependency(s : &str) {
        println!("cargo:rustc-link-lib={}", s);
}

fn link_relpath(builddir : &str, s : &str) {
    println!("cargo:rustc-link-search=native={}/{}", builddir, s);
}

fn link_path(s : &str) {
    println!("cargo:rustc-link-search=native={}", s);
}

fn from_c(s : &str) {
    let mut next_is_lib = false;
    let mut next_is_path = false;
    for ent in s.split_whitespace() {
        if next_is_lib {
            dependency(ent);
            next_is_lib = false;
        } else if next_is_path {
            link_path(ent);
            next_is_path = false;
        } else if ent == "-l" {
            next_is_lib = true;
        } else if ent == "-L" {
            next_is_path = true;
        } else if ent.starts_with("-L") {
            link_path(&ent[2..]);
        } else if ent.starts_with("-l") {
            dependency(&ent[2..]);
        }
    }
}

pub fn main() {
    let cfg = load_cfg().unwrap();

    let builddir = cfg.get("BUILDDIR").unwrap();

    from_c(cfg.get("TOR_LDFLAGS_zlib").unwrap());
    from_c(cfg.get("TOR_LDFLAGS_openssl").unwrap());
    from_c(cfg.get("TOR_LDFLAGS_libevent").unwrap());

    link_relpath(builddir, "src/common");
    link_relpath(builddir, "src/or");
    link_relpath(builddir, "src/ext/keccak-tiny");
    link_relpath(builddir, "src/ext/keccak-tiny");
    link_relpath(builddir, "src/ext/ed25519/ref10");
    link_relpath(builddir, "src/ext/ed25519/donna");
    link_relpath(builddir, "src/trunnel");
    link_relpath(builddir, "src/trace");

    component("tor-testing");
    component("or-crypto-testing");
    component("or-ctime-testing");
    component("or-testing");
    component("or-ctime-testing");
    component("or-event-testing");
    component("or-trunnel-testing");
    component("or-trace");
    component("curve25519_donna");
    component("keccak-tiny");
    component("ed25519_ref10");
    component("ed25519_donna");

    from_c(cfg.get("TOR_ZLIB_LIBS").unwrap());
    from_c(cfg.get("TOR_LIB_MATH").unwrap());
    from_c(cfg.get("TOR_LIBEVENT_LIBS").unwrap());
    from_c(cfg.get("TOR_OPENSSL_LIBS").unwrap());
    from_c(cfg.get("TOR_LIB_WS32").unwrap());
    from_c(cfg.get("TOR_LIB_GDI").unwrap());
    from_c(cfg.get("TOR_LIB_USERENV").unwrap());
    from_c(cfg.get("CURVE25519_LIBS").unwrap());
    from_c(cfg.get("TOR_SYSTEMD_LIBS").unwrap());
    from_c(cfg.get("TOR_LZMA_LIBS").unwrap());
    from_c(cfg.get("TOR_ZSTD_LIBS").unwrap());
    from_c(cfg.get("LIBS").unwrap())

}
