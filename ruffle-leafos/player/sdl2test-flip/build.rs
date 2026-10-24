use std::env;
fn main() {

    let target = env::var("TARGET").unwrap();
    if target.contains("aarch64-unknown-linux-gnu") {

        cc::Build::new()
            .file("c_src/egl_drm.c")
            .include("../c_inc/")
            .flag("-fPIC")
            .compile("egl_drm_helper");
        // 链接系统库
        println!("cargo:rustc-link-lib=mali");
        println!("cargo:rustc-link-lib=drm");
    }
}
