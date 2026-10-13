use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};
static INTERRUPTED: AtomicBool = AtomicBool::new(false);
static RESIZED: AtomicBool = AtomicBool::new(false);
static EXITING: AtomicI32 = AtomicI32::new(0);
extern "C" fn handler(sig: libc::c_int) {
    if sig == libc::SIGWINCH {
        RESIZED.store(true, Ordering::Relaxed);
    } else {
        INTERRUPTED.store(true, Ordering::Relaxed);
        if sig == libc::SIGTERM || sig == libc::SIGHUP {
            EXITING.store(sig, Ordering::Relaxed);
        }
    }
}
pub fn install() -> Result<(), String> {
    // Only lock-free atomics are touched by the signal handler.
    unsafe {
        let mut action: libc::sigaction = std::mem::zeroed();
        action.sa_sigaction = handler as *const () as usize;
        libc::sigemptyset(&mut action.sa_mask);
        for sig in [libc::SIGINT, libc::SIGTERM, libc::SIGHUP, libc::SIGWINCH] {
            if libc::sigaction(sig, &action, std::ptr::null_mut()) != 0 {
                return Err(std::io::Error::last_os_error().to_string());
            }
        }
    }
    Ok(())
}
#[inline]
pub fn cancelled() -> bool {
    INTERRUPTED.load(Ordering::Relaxed)
}
pub fn resized() -> bool {
    RESIZED.load(Ordering::Relaxed)
}
pub fn exiting() -> i32 {
    EXITING.load(Ordering::Relaxed)
}
pub fn clear() {
    INTERRUPTED.store(false, Ordering::Relaxed);
    RESIZED.store(false, Ordering::Relaxed);
}
pub fn code() -> i32 {
    if exiting() != 0 {
        128 + exiting()
    } else {
        130
    }
}
