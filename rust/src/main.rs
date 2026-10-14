mod codecs;
mod input;
mod literal;
mod pager;
mod search;
mod signals;
use literal::Simd;
use search::{Mode, Search};
use std::{
    ffi::OsString,
    io::{self, Write},
    os::unix::ffi::OsStrExt,
    path::PathBuf,
};

fn usage() {
    print!(
        "Usage: fastless-rs [--regex|--literal|--glob] [-i|-I|--case-sensitive] FILE\n\
            fastless-rs [options] --find PATTERN [--all] [--offset BYTE] FILE\n\
Search: POSIX extended regex (default), literal bytes, or whole-line glob (* ? \\).\n\
Keys: Space/f page, b previous page, j/k line, d/u half page, g/< or ESC-< start,\n\
      G/> or ESC-> end, arrow/Page/Home/End keys, / and ? search,\n\
      n/N repeat/reverse, r/l/w regex/literal/glob mode, q quit.\n\
LESS -i/-I supplies smart/always ignore-case defaults; CLI options override.\n\
--compression auto|none|gzip|zstd|xz: detect/force format (default: auto).\n\
--tmpdir DIR: decoded temporary file (default: TMPDIR or /tmp).\n\
--threads N: search and xz threads (0: auto up to 32; maximum: 256).\n\
--simd auto|off|avx2: search acceleration (default: auto).\n\
Compressed input is fully decoded before paging/searching; offsets are decoded bytes.\n"
    );
}
fn less_options() -> u8 {
    let mut mode = 0;
    let Some(value) = std::env::var_os("LESS") else {
        return mode;
    };
    // As in less: the leading '-' is optional, flags may be clustered, and a
    // string-valued option (or +command) runs to the next '$' or the end.
    let text = value.as_bytes();
    let mut i = 0;
    while i < text.len() {
        let c = text[i];
        i += 1;
        if c == b'-' && text.get(i) == Some(&b'-') {
            let name = i + 1;
            while i < text.len() && !text[i].is_ascii_whitespace() {
                i += 1;
            }
            match &text[name..i] {
                b"ignore-case" => mode = 1,
                b"IGNORE-CASE" => mode = 2,
                _ => (),
            }
        } else if b"+DkoOpPtT\"".contains(&c) {
            while i < text.len() && text[i] != b'$' {
                i += 1;
            }
            i += 1;
        } else if c == b'i' {
            mode = 1;
        } else if c == b'I' {
            mode = 2;
        }
    }
    mode
}
fn value(args: &[OsString], i: &mut usize) -> Result<OsString, String> {
    *i += 1;
    args.get(*i)
        .cloned()
        .ok_or_else(|| "Missing option value; see --help".into())
}
fn execute() -> Result<i32, String> {
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    let mut mode = Mode::Regex;
    let mut case_mode = less_options();
    let (mut file, mut pattern, mut temp_dir): (Option<PathBuf>, Option<Vec<u8>>, Option<PathBuf>) =
        (None, None, None);
    let (mut all, mut positional) = (false, false);
    let mut offset = 0usize;
    let mut threads = 0u32;
    let mut compression = "auto".to_string();
    let mut simd_mode = "auto".to_string();
    let mut i = 0;
    while i < args.len() {
        let a = args[i].as_bytes();
        if !positional {
            match a {
                b"--" => {
                    positional = true;
                    i += 1;
                    continue;
                }
                b"--help" | b"-h" => {
                    usage();
                    return Ok(0);
                }
                b"--regex" => mode = Mode::Regex,
                b"--literal" => mode = Mode::Literal,
                b"--glob" => mode = Mode::Glob,
                b"-i" => case_mode = 1,
                b"-I" => case_mode = 2,
                b"--case-sensitive" => case_mode = 0,
                b"--all" => all = true,
                b"--find" => pattern = Some(value(&args, &mut i)?.as_bytes().to_vec()),
                b"--tmpdir" => temp_dir = Some(PathBuf::from(value(&args, &mut i)?)),
                b"--simd" => {
                    simd_mode = value(&args, &mut i)?
                        .into_string()
                        .map_err(|_| "Invalid SIMD mode")?
                }
                b"--compression" => {
                    compression = value(&args, &mut i)?
                        .into_string()
                        .map_err(|_| "Invalid compression format")?
                }
                b"--threads" => {
                    threads = value(&args, &mut i)?
                        .to_str()
                        .ok_or("Invalid threads")?
                        .parse()
                        .map_err(|_| "Threads must be between 0 and 256")?;
                    if threads > 256 {
                        return Err("Threads must be between 0 and 256".into());
                    }
                }
                b"--offset" => {
                    offset = value(&args, &mut i)?
                        .to_str()
                        .ok_or("Invalid byte offset")?
                        .parse()
                        .map_err(|_| "Invalid byte offset")?
                }
                _ => {
                    if a.starts_with(b"-") {
                        return Err("Unknown option; see --help".into());
                    }
                    if file.is_some() {
                        return Err("Specify exactly one file".into());
                    }
                    file = Some(PathBuf::from(&args[i]));
                }
            }
        } else {
            if file.is_some() {
                return Err("Specify exactly one file".into());
            }
            file = Some(PathBuf::from(&args[i]));
        }
        i += 1;
    }
    let path = file.ok_or("Specify a file; see --help")?;
    if all && pattern.is_none() {
        return Err("--all requires --find".into());
    }
    if !["auto", "none", "gzip", "zstd", "xz"].contains(&compression.as_str()) {
        return Err("Invalid compression format".into());
    }
    if std::mem::size_of::<usize>() < 8 {
        return Err("A 64-bit build is required".into());
    }
    let simd = Simd::configure(&simd_mode)?;
    if threads == 0 {
        threads = std::thread::available_parallelism().map_or(1, |n| n.get().min(32) as u32);
    }
    search::set_threads(threads as usize);
    if pattern.is_none() && (unsafe { libc::isatty(0) } != 1 || unsafe { libc::isatty(1) } != 1) {
        return Err("Pager needs a terminal; use --find for noninteractive searching".into());
    }
    signals::install()?;
    // Validate regex before a potentially expensive decode.
    let current = pattern
        .as_ref()
        .map(|p| Search::new(mode, p, case_mode, simd))
        .transpose()?;
    let mapped = input::open(&path, &compression, temp_dir.as_deref(), threads)?;
    let data = mapped.bytes();
    if let Some(search) = current {
        let mut found = false;
        let mut out = io::BufWriter::new(io::stdout().lock());
        while offset <= data.len() {
            let Some(hit) = search.forward(data, offset, data.len())? else {
                break;
            };
            writeln!(out, "{hit}").map_err(|e| e.to_string())?;
            found = true;
            if !all || hit == data.len() {
                break;
            }
            offset = if mode == Mode::Glob {
                search::next_line(data, hit)
            } else {
                hit + 1
            };
            if mode == Mode::Glob && offset == data.len() {
                break;
            }
        }
        out.flush().map_err(|e| e.to_string())?;
        Ok(if found { 0 } else { 1 })
    } else {
        pager::run(data, offset, mode, case_mode, simd)
    }
}
fn main() {
    let code = match execute() {
        Ok(code) => code,
        Err(message) => {
            eprintln!("{message}");
            if signals::cancelled() {
                signals::code()
            } else {
                2
            }
        }
    };
    std::process::exit(code);
}
