use std::env;

fn main() {
    println!("cargo:rerun-if-env-changed=DARWIN_ART_DL_PHDR_PROVIDER_LIBDIR");
    if env::var_os("CARGO_FEATURE_PROVIDER_LINK").is_none() {
        return;
    }
    let directory = env::var("DARWIN_ART_DL_PHDR_PROVIDER_LIBDIR")
        .expect("DARWIN_ART_DL_PHDR_PROVIDER_LIBDIR is required");
    println!("cargo:rustc-link-search=native={directory}");
    println!("cargo:rustc-link-lib=static=darwin-art-dl-phdr");
    println!("cargo:rustc-link-lib=c++");
}
