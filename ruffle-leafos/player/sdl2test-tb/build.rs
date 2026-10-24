use std::env;
fn main() {

    let target = env::var("TARGET").unwrap();
    if target.contains("aarch64-unknown-linux-gnu") {

        cc::Build::new()
            .file("c_src/egl_tb.c")
            .include("../c_inc/")
            .flag("-fPIC")
            .compile("egl_gl_helper");
        println!("cargo:rustc-link-lib=EGL");
        println!("cargo:rustc-link-lib=GLESv2");
    }
}
