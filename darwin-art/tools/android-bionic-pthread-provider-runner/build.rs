use std::env;

fn main() {
    let directory = env::var("DARWIN_ART_PTHREAD_PROVIDER_LIBDIR")
        .expect("DARWIN_ART_PTHREAD_PROVIDER_LIBDIR is required");
    println!("cargo:rustc-link-search=native={directory}");
    println!("cargo:rustc-link-lib=static=darwin-art-bionic-pthread");
    println!("cargo:rustc-link-lib=c++");
    println!("cargo:rerun-if-env-changed=DARWIN_ART_PTHREAD_PROVIDER_LIBDIR");
}
