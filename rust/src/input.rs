use crate::{codecs::Decoder, signals};
use std::{
    fs::{File, OpenOptions},
    io::{self, Write},
    os::unix::{
        ffi::OsStrExt,
        fs::OpenOptionsExt,
        io::{AsRawFd, FromRawFd},
    },
    path::{Path, PathBuf},
    sync::mpsc,
    time::Instant,
};
const CHUNK: usize = 8 * 1024 * 1024;
pub struct Mapped {
    ptr: *mut libc::c_void,
    len: usize,
}
impl Mapped {
    fn new(file: &File) -> Result<Self, String> {
        let metadata = file.metadata().map_err(|e| e.to_string())?;
        if !metadata.is_file() {
            return Err("A regular file fitting the address space is required".into());
        }
        let len = usize::try_from(metadata.len()).map_err(|_| "File exceeds address space")?;
        if len > isize::MAX as usize {
            return Err("File exceeds address space".into());
        }
        let ptr = if len == 0 {
            std::ptr::null_mut()
        } else {
            let ptr = unsafe {
                libc::mmap(
                    std::ptr::null_mut(),
                    len,
                    libc::PROT_READ,
                    libc::MAP_PRIVATE,
                    file.as_raw_fd(),
                    0,
                )
            };
            if ptr == libc::MAP_FAILED {
                return Err(io::Error::last_os_error().to_string());
            }
            ptr
        };
        Ok(Self { ptr, len })
    }
    pub fn bytes(&self) -> &[u8] {
        if self.len == 0 {
            &[]
        } else {
            // Mapping outlives this borrow. Source files must remain stable.
            unsafe { std::slice::from_raw_parts(self.ptr.cast(), self.len) }
        }
    }
}
impl Drop for Mapped {
    fn drop(&mut self) {
        if self.len > 0 {
            unsafe {
                libc::munmap(self.ptr, self.len);
            }
        }
    }
}
fn detect(data: &[u8]) -> &'static str {
    if data.starts_with(b"\x1f\x8b") {
        "gzip"
    } else if data.starts_with(b"\xfd7zXZ\0") {
        "xz"
    } else if data.starts_with(b"\x28\xb5\x2f\xfd")
        || (data.len() >= 4 && data[0] & 0xf0 == 0x50 && data[1..4] == [0x2a, 0x4d, 0x18])
    {
        "zstd"
    } else {
        "none"
    }
}
fn temporary(dir: Option<&Path>) -> Result<File, String> {
    let default = std::env::var_os("TMPDIR")
        .filter(|s| !s.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/tmp"));
    let dir = dir
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(&default);
    let mut name = dir.as_os_str().as_bytes().to_vec();
    name.extend_from_slice(b"/fastless-XXXXXX\0");
    let fd = unsafe { libc::mkstemp(name.as_mut_ptr().cast()) };
    if fd < 0 {
        return Err(format!(
            "Cannot create temporary file in {}: {}",
            dir.display(),
            io::Error::last_os_error()
        ));
    }
    let file = unsafe { File::from_raw_fd(fd) };
    if unsafe { libc::unlink(name.as_ptr().cast()) } != 0 {
        return Err(format!(
            "Cannot unlink temporary file: {}",
            io::Error::last_os_error()
        ));
    }
    Ok(file)
}
fn write_output(file: &File, bytes: &[u8]) -> Result<(), String> {
    let mut pos = 0;
    while pos < bytes.len() {
        if signals::cancelled() {
            return Err("Decompression interrupted".into());
        }
        let n = unsafe {
            libc::write(
                file.as_raw_fd(),
                bytes[pos..].as_ptr().cast(),
                bytes.len() - pos,
            )
        };
        if n < 0 {
            let error = io::Error::last_os_error();
            if error.kind() == io::ErrorKind::Interrupted {
                continue;
            }
            return Err(format!("Temporary-file write failed: {error}"));
        }
        if n == 0 {
            return Err("Temporary-file write failed: zero-length write".into());
        }
        pos += n as usize;
    }
    Ok(())
}
struct Progress {
    enabled: bool,
    last: Instant,
}
impl Drop for Progress {
    fn drop(&mut self) {
        if self.enabled {
            eprint!("\r\x1b[K");
        }
    }
}
pub fn open(path: &Path, mode: &str, dir: Option<&Path>, threads: u32) -> Result<Mapped, String> {
    let file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NONBLOCK)
        .open(path)
        .map_err(|e| format!("{}: {e}", path.display()))?;
    let source = Mapped::new(&file)?;
    drop(file);
    let format = if mode == "auto" {
        detect(source.bytes())
    } else {
        mode
    };
    if format == "none" {
        return Ok(source);
    }
    let output_file = temporary(dir)?;
    let threads = if threads == 0 {
        unsafe { libc::sysconf(libc::_SC_NPROCESSORS_ONLN) }.clamp(1, 32) as u32
    } else {
        threads
    };
    // Decoder must drop before the source map whose pointers it retains; the
    // output buffers are only touched inside `step`.
    let mut decoder = Decoder::new(format, threads).map_err(|e| format!("{format}: {e}"))?;
    if source.len > 0 {
        unsafe {
            libc::madvise(source.ptr, source.len, libc::MADV_SEQUENTIAL);
        }
    }
    let mut progress = Progress {
        enabled: unsafe { libc::isatty(2) } == 1,
        last: Instant::now(),
    };
    if progress.enabled {
        eprint!("Decompressing {format}... (Ctrl-C cancels)");
        let _ = io::stderr().flush();
    }
    let data = source.bytes();
    // Decoding continues into one buffer while the writer thread writes the
    // other out.
    let (full, filled) = mpsc::sync_channel::<(Vec<u8>, usize)>(1);
    let (emptied, empty) = mpsc::channel::<Vec<u8>>();
    let _ = emptied.send(vec![0u8; CHUNK]);
    let output = &output_file;
    std::thread::scope(|scope| {
        let writer = scope.spawn(move || {
            for (buffer, length) in filled {
                write_output(output, &buffer[..length])?;
                if emptied.send(buffer).is_err() {
                    break;
                }
            }
            Ok::<(), String>(())
        });
        let decoded = decode(format, data, &mut decoder, &mut progress, full, empty);
        let wrote = writer.join().unwrap().map_err(|e| format!("{format}: {e}"));
        match decoded {
            Err(e) if e.is_empty() => wrote.and(Err(format!("{format}: Temporary-file write failed"))),
            other => other.and(wrote),
        }
    })?;
    if signals::cancelled() {
        return Err("Decompression interrupted".into());
    }
    Mapped::new(&output_file)
}
/// Runs the decoder over `data`, passing each filled buffer to the writer
/// through `full` and taking a free one back from `empty`. An empty error
/// means the writer stopped, in which case the writer holds the reason.
fn decode(
    format: &str,
    data: &[u8],
    decoder: &mut Decoder,
    progress: &mut Progress,
    full: mpsc::SyncSender<(Vec<u8>, usize)>,
    empty: mpsc::Receiver<Vec<u8>>,
) -> Result<(), String> {
    let mut buffer = vec![0u8; CHUNK];
    let (mut pos, mut written) = (0usize, 0usize);
    loop {
        if signals::cancelled() {
            return Err(format!("{format}: Decompression interrupted"));
        }
        let end = pos + (data.len() - pos).min(CHUNK);
        let step = decoder
            .step(&data[pos..end], &mut buffer, end == data.len())
            .map_err(|e| format!("{format}: {e}"))?;
        pos += step.consumed;
        written = written
            .checked_add(step.produced)
            .filter(|n| *n <= isize::MAX as usize)
            .ok_or("Decompressed file exceeds address/file size limits")?;
        if step.produced > 0 {
            // Hand the full buffer to the writer and decode on into the other.
            // Either channel closing means the writer failed; it reports why.
            if full.send((buffer, step.produced)).is_err() {
                return Err(String::new());
            }
            buffer = empty.recv().map_err(|_| String::new())?;
        }
        if progress.enabled && progress.last.elapsed().as_secs_f64() >= 0.25 {
            eprint!(
                "\rDecompressing {format}: {:.1}% input, {:.2} GiB output\x1b[K",
                if data.is_empty() {
                    100.0
                } else {
                    100.0 * pos as f64 / data.len() as f64
                },
                written as f64 / (1024.0 * 1024.0 * 1024.0)
            );
            let _ = io::stderr().flush();
            progress.last = Instant::now();
        }
        if step.finished {
            if format == "xz" || pos == data.len() {
                break;
            }
            if format == "gzip" {
                if data[pos] == 0 {
                    for (offset, &byte) in data[pos..].iter().enumerate() {
                        if offset & 65535 == 0 && signals::cancelled() {
                            return Err("Decompression interrupted".into());
                        }
                        if byte != 0 {
                            return Err("gzip: Invalid trailing data after gzip member".into());
                        }
                    }
                    break;
                }
                decoder.reset_gzip_member()?;
            }
        } else if step.consumed == 0 && step.produced == 0 && format != "xz" {
            return Err(format!("{format}: Truncated or invalid {format} stream"));
        }
    }
    Ok(())
}
