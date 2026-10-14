//! Native codec ABI bindings. Decode control and I/O are implemented in Rust;
//! no C pager/search/input implementation is linked into the Rust executable.
use libc::{c_char, c_int, c_uint, c_ulong, c_void};
use std::{ffi::CStr, mem};

#[repr(C)]
pub struct ZStream {
    next_in: *const u8,
    avail_in: c_uint,
    total_in: c_ulong,
    next_out: *mut u8,
    avail_out: c_uint,
    total_out: c_ulong,
    msg: *const c_char,
    state: *mut c_void,
    zalloc: Option<unsafe extern "C" fn(*mut c_void, c_uint, c_uint) -> *mut c_void>,
    zfree: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
    opaque: *mut c_void,
    data_type: c_int,
    adler: c_ulong,
    reserved: c_ulong,
}
#[repr(C)]
pub struct LzmaStream {
    next_in: *const u8,
    avail_in: usize,
    total_in: u64,
    next_out: *mut u8,
    avail_out: usize,
    total_out: u64,
    allocator: *const c_void,
    internal: *mut c_void,
    reserved_ptr: [*mut c_void; 4],
    seek_pos: u64,
    reserved_int2: u64,
    reserved_int3: usize,
    reserved_int4: usize,
    reserved_enum: [c_int; 2],
}
#[repr(C)]
struct LzmaMt {
    flags: u32,
    threads: u32,
    block_size: u64,
    timeout: u32,
    preset: u32,
    filters: *const c_void,
    check: c_int,
    reserved_enum: [c_int; 3],
    reserved_int: [u32; 4],
    memlimit_threading: u64,
    memlimit_stop: u64,
    reserved_int7: u64,
    reserved_int8: u64,
    reserved_ptr: [*mut c_void; 4],
}
#[repr(C)]
struct ZstdIn {
    src: *const c_void,
    size: usize,
    pos: usize,
}
#[repr(C)]
struct ZstdOut {
    dst: *mut c_void,
    size: usize,
    pos: usize,
}
extern "C" {
    fn zlibVersion() -> *const c_char;
    fn inflateInit2_(
        stream: *mut ZStream,
        bits: c_int,
        version: *const c_char,
        size: c_int,
    ) -> c_int;
    fn inflate(stream: *mut ZStream, flush: c_int) -> c_int;
    fn inflateReset2(stream: *mut ZStream, bits: c_int) -> c_int;
    fn inflateEnd(stream: *mut ZStream) -> c_int;
    fn ZSTD_createDStream() -> *mut c_void;
    fn ZSTD_freeDStream(stream: *mut c_void) -> usize;
    fn ZSTD_initDStream(stream: *mut c_void) -> usize;
    fn ZSTD_decompressStream(stream: *mut c_void, out: *mut ZstdOut, input: *mut ZstdIn) -> usize;
    fn ZSTD_isError(code: usize) -> c_uint;
    fn ZSTD_getErrorName(code: usize) -> *const c_char;
    fn lzma_stream_decoder(stream: *mut LzmaStream, limit: u64, flags: u32) -> c_int;
    fn lzma_code(stream: *mut LzmaStream, action: c_int) -> c_int;
    fn lzma_end(stream: *mut LzmaStream);
}
fn xz_error(code: i32) -> String {
    match code {
        // lzma_ret values from lzma/base.h.
        5 => "Cannot allocate xz decoder memory",
        6 => "xz decoder needs more than the 512 MiB memory limit",
        7 => "Invalid xz format",
        8 => "Unsupported xz compression options",
        3 => "Unsupported xz integrity check",
        9 => "Corrupt xz stream or checksum",
        10 => "Truncated xz stream",
        _ => "xz decoder error",
    }
    .into()
}
unsafe fn zstd_error(code: usize) -> Result<(), String> {
    if ZSTD_isError(code) != 0 {
        Err(CStr::from_ptr(ZSTD_getErrorName(code))
            .to_string_lossy()
            .into_owned())
    } else {
        Ok(())
    }
}
pub enum Decoder {
    Gzip(Box<ZStream>),
    Zstd(*mut c_void),
    Xz(Box<LzmaStream>),
}
pub struct Step {
    pub consumed: usize,
    pub produced: usize,
    pub finished: bool,
}
impl Decoder {
    pub fn new(format: &str, threads: u32) -> Result<Self, String> {
        unsafe {
            match format {
                "gzip" => {
                    // zlib's internal state refers back to its stream address:
                    // Box keeps that address stable when Decoder is moved.
                    let mut z: Box<ZStream> = Box::new(mem::zeroed());
                    let rc = inflateInit2_(
                        &mut *z,
                        31,
                        zlibVersion(),
                        mem::size_of::<ZStream>() as c_int,
                    );
                    if rc != 0 {
                        if !z.state.is_null() {
                            inflateEnd(&mut *z);
                        }
                        return Err("Cannot initialize zlib".into());
                    }
                    Ok(Self::Gzip(z))
                }
                "zstd" => {
                    let z = ZSTD_createDStream();
                    if z.is_null() {
                        return Err("Cannot allocate zstd decoder".into());
                    }
                    if let Err(err) = zstd_error(ZSTD_initDStream(z)) {
                        ZSTD_freeDStream(z);
                        return Err(err);
                    }
                    Ok(Self::Zstd(z))
                }
                "xz" => {
                    let mut z: Box<LzmaStream> = Box::new(mem::zeroed());
                    let flags = 8 | 2; // CONCATENATED | TELL_UNSUPPORTED_CHECK
                                       // Runtime lookup preserves support for pre-5.4 liblzma.
                    let mt = libc::dlsym(libc::RTLD_DEFAULT, c"lzma_stream_decoder_mt".as_ptr());
                    let rc = if threads > 1 && !mt.is_null() {
                        let mut options: LzmaMt = mem::zeroed();
                        options.flags = flags;
                        options.threads = threads;
                        options.timeout = 100;
                        options.memlimit_threading = 256 * 1024 * 1024;
                        options.memlimit_stop = 512 * 1024 * 1024;
                        let init: unsafe extern "C" fn(*mut LzmaStream, *const LzmaMt) -> c_int =
                            mem::transmute(mt);
                        init(&mut *z, &options)
                    } else {
                        lzma_stream_decoder(&mut *z, 512 * 1024 * 1024, flags)
                    };
                    if rc != 0 {
                        lzma_end(&mut *z);
                        return Err(xz_error(rc));
                    }
                    Ok(Self::Xz(z))
                }
                _ => Err("Invalid compression format".into()),
            }
        }
    }
    pub fn step(&mut self, input: &[u8], output: &mut [u8], finish: bool) -> Result<Step, String> {
        // ABI functions receive slices' exact lengths and retain no buffer past
        // this call. Input is immutable; the output is uniquely borrowed.
        unsafe {
            match self {
                Self::Gzip(z) => {
                    z.next_in = input.as_ptr();
                    z.avail_in = input.len() as c_uint;
                    z.next_out = output.as_mut_ptr();
                    z.avail_out = output.len() as c_uint;
                    let rc = inflate(&mut **z, 0);
                    if ![0, 1, -5].contains(&rc) {
                        return Err(if z.msg.is_null() {
                            "Invalid gzip stream".into()
                        } else {
                            CStr::from_ptr(z.msg).to_string_lossy().into_owned()
                        });
                    }
                    Ok(Step {
                        consumed: input.len() - z.avail_in as usize,
                        produced: output.len() - z.avail_out as usize,
                        finished: rc == 1,
                    })
                }
                Self::Zstd(z) => {
                    let mut i = ZstdIn {
                        src: input.as_ptr().cast(),
                        size: input.len(),
                        pos: 0,
                    };
                    let mut o = ZstdOut {
                        dst: output.as_mut_ptr().cast(),
                        size: output.len(),
                        pos: 0,
                    };
                    let rc = ZSTD_decompressStream(*z, &mut o, &mut i);
                    zstd_error(rc)?;
                    Ok(Step {
                        consumed: i.pos,
                        produced: o.pos,
                        finished: rc == 0,
                    })
                }
                Self::Xz(z) => {
                    z.next_in = input.as_ptr();
                    z.avail_in = input.len();
                    z.next_out = output.as_mut_ptr();
                    z.avail_out = output.len();
                    let rc = lzma_code(&mut **z, if finish { 3 } else { 0 });
                    if rc != 0 && rc != 1 {
                        return Err(xz_error(rc));
                    }
                    Ok(Step {
                        consumed: input.len() - z.avail_in,
                        produced: output.len() - z.avail_out,
                        finished: rc == 1,
                    })
                }
            }
        }
    }
    pub fn reset_gzip_member(&mut self) -> Result<(), String> {
        if let Self::Gzip(z) = self {
            if unsafe { inflateReset2(&mut **z, 31) } != 0 {
                return Err("Cannot reset zlib".into());
            }
        }
        Ok(())
    }
}
impl Drop for Decoder {
    fn drop(&mut self) {
        unsafe {
            match self {
                Self::Gzip(z) => {
                    inflateEnd(&mut **z);
                }
                Self::Zstd(z) => {
                    ZSTD_freeDStream(*z);
                }
                Self::Xz(z) => lzma_end(&mut **z),
            }
        }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn native_64_bit_abi_sizes() {
        assert_eq!(mem::size_of::<ZStream>(), 112);
        assert_eq!(mem::size_of::<LzmaStream>(), 136);
        assert_eq!(mem::size_of::<LzmaMt>(), 128);
        assert_eq!(mem::size_of::<ZstdIn>(), 24);
    }
}
