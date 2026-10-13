use std::process::Command;
fn main() {
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_PATH");
    println!("cargo:rerun-if-env-changed=PKG_CONFIG");
    let tool = std::env::var("PKG_CONFIG").unwrap_or_else(|_| "pkg-config".into());
    if let Ok(output) = Command::new(tool)
        .args(["--libs", "zlib", "libzstd", "liblzma"])
        .output()
    {
        if output.status.success() {
            for flag in String::from_utf8_lossy(&output.stdout).split_whitespace() {
                if let Some(path) = flag.strip_prefix("-L") {
                    println!("cargo:rustc-link-search=native={path}");
                }
                if let Some(lib) = flag.strip_prefix("-l") {
                    println!("cargo:rustc-link-lib={lib}");
                }
            }
            return;
        }
    }
    for lib in ["z", "zstd", "lzma"] {
        println!("cargo:rustc-link-lib={lib}");
    }
}
