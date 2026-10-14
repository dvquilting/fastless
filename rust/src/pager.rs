use crate::{
    literal::Simd,
    search::{self, Mode, Search},
    signals,
};
use std::{
    io::{self, Write},
    mem,
};
struct Terminal {
    saved: libc::termios,
}
impl Terminal {
    fn enter() -> Result<Self, String> {
        let mut saved: libc::termios = unsafe { mem::zeroed() };
        if unsafe { libc::tcgetattr(0, &mut saved) } != 0 {
            return Err(io::Error::last_os_error().to_string());
        }
        let mut raw = saved;
        // IEXTEN off keeps Ctrl-V and Ctrl-O for the pager on BSD and macOS.
        raw.c_lflag &= !(libc::ICANON | libc::ECHO | libc::IEXTEN);
        raw.c_iflag &= !(libc::IXON | libc::ICRNL);
        raw.c_cc[libc::VMIN] = 1;
        raw.c_cc[libc::VTIME] = 0;
        // Ctrl-Y scrolls instead of suspending.
        #[cfg(any(
            target_vendor = "apple",
            target_os = "freebsd",
            target_os = "dragonfly",
            target_os = "netbsd",
            target_os = "openbsd"
        ))]
        {
            raw.c_cc[libc::VDSUSP] = libc::_POSIX_VDISABLE;
        }
        if unsafe { libc::tcsetattr(0, libc::TCSAFLUSH, &raw) } != 0 {
            return Err(io::Error::last_os_error().to_string());
        }
        let result = Self { saved };
        io::stdout()
            .write_all(b"\x1b[?1049h")
            .map_err(|e| e.to_string())?;
        Ok(result)
    }
}
impl Drop for Terminal {
    fn drop(&mut self) {
        unsafe {
            libc::tcsetattr(0, libc::TCSAFLUSH, &self.saved);
        }
        let mut out = io::stdout();
        let _ = out.write_all(b"\x1b[0m\x1b[?1049l");
        let _ = out.flush();
    }
}
fn dimensions() -> (usize, usize) {
    let mut w: libc::winsize = unsafe { mem::zeroed() };
    unsafe {
        libc::ioctl(1, libc::TIOCGWINSZ, &mut w);
    }
    (
        if w.ws_row >= 3 { w.ws_row as usize } else { 24 },
        if w.ws_col >= 10 {
            w.ws_col as usize
        } else {
            80
        },
    )
}
fn key() -> i32 {
    loop {
        match signals::wait_for_input() {
            Ok(true) => (),
            Ok(false) => return 0,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(_) => return -1,
        }
        let mut byte = 0u8;
        let n = unsafe { libc::read(0, (&mut byte as *mut u8).cast(), 1) };
        if n == 1 {
            return byte as i32;
        }
        if n == 0 || io::Error::last_os_error().kind() != io::ErrorKind::Interrupted {
            return -1;
        }
    }
}
/// Reads one command, translating less's alternative keys, ESC sequences and
/// terminal cursor keys to the pager's own f b j k d u g G q. Unknown escape
/// sequences become 0, which only redraws.
fn command() -> i32 {
    let c = key();
    // Control keys: ^F ^V, ^B, ^E ^N, ^Y ^P ^K, ^D, ^U.
    let alias = match u8::try_from(c) {
        Ok(b'z' | 0x06 | 0x16) => b'f',
        Ok(0x02) => b'b',
        Ok(b'e' | 0x05 | 0x0e) => b'j',
        Ok(b'y' | 0x19 | 0x10 | 0x0b) => b'k',
        Ok(0x04) => b'd',
        Ok(0x15) => b'u',
        Ok(b'<') => b'g',
        Ok(b'>') => b'G',
        Ok(b'Q') => b'q',
        Ok(27) => return escape(),
        _ => return c,
    };
    alias as i32
}
fn escape() -> i32 {
    let mut c = key();
    match u8::try_from(c) {
        Ok(b'<') => return b'g' as i32,
        Ok(b'>') => return b'G' as i32,
        Ok(b' ') => return b'f' as i32,
        Ok(b'v') => return b'b' as i32,
        // SS3 cursor keys: ESC O A
        Ok(b'O') => c = key(),
        // CSI: ESC [ parameters final
        Ok(b'[') => {
            let (mut param, mut first) = (0, true);
            loop {
                c = key();
                match u8::try_from(c) {
                    Ok(b';') => first = false,
                    Ok(d @ b'0'..=b'9') if first && param < 1000 => {
                        param = param * 10 + i32::from(d - b'0')
                    }
                    Ok(0x20..=0x3f) => (),
                    _ => break,
                }
            }
            if c == b'~' as i32 {
                return match param {
                    1 | 7 => b'g' as i32,
                    4 | 8 => b'G' as i32,
                    5 => b'b' as i32,
                    6 => b'f' as i32,
                    _ => 0,
                };
            }
        }
        _ => return c.min(0),
    }
    match u8::try_from(c) {
        Ok(b'A') => b'k' as i32,
        Ok(b'B') => b'j' as i32,
        Ok(b'H') => b'g' as i32,
        Ok(b'F') => b'G' as i32,
        _ => c.min(0),
    }
}
fn draw(
    data: &[u8],
    top: usize,
    current: Option<&Search>,
    mode: Mode,
    message: &str,
    rows: usize,
    cols: usize,
) -> Result<usize, String> {
    let mut out = Vec::with_capacity((rows * cols).min(64 * 1024));
    out.extend_from_slice(b"\x1b[0m\x1b[H");
    let mut p = top;
    for _ in 0..rows - 1 {
        out.extend_from_slice(b"\x1b[2K");
        if p < data.len() {
            let end = search::line_end(data, p);
            let (mut q, mut col) = (p, 0usize);
            while q < end && col < cols - 1 {
                col = if data[q] == b'\t' {
                    (col + 8) & !7
                } else {
                    col + 1
                };
                q += 1;
            }
            let marks = current.map(|s| s.highlight(data, p, end, q));
            q = p;
            col = 0;
            let mut highlighted = false;
            while q < end && col < cols - 1 {
                let matched = marks.as_ref().is_some_and(|v| v[q - p]);
                if matched != highlighted {
                    out.extend_from_slice(if matched { b"\x1b[7m" } else { b"\x1b[0m" });
                    highlighted = matched;
                }
                let c = data[q];
                q += 1;
                if c == b'\t' {
                    let stop = ((col + 8) & !7).min(cols - 1);
                    out.resize(out.len() + stop - col, b' ');
                    col = stop;
                } else {
                    out.push(if (32..=126).contains(&c) { c } else { b'.' });
                    col += 1;
                }
            }
            if highlighted {
                out.extend_from_slice(b"\x1b[0m");
            }
            p = if end < data.len() { end + 1 } else { end };
        } else {
            out.push(b'~');
        }
        out.extend_from_slice(b"\r\n");
    }
    let percent = if data.is_empty() {
        100.0
    } else {
        100.0 * top as f64 / data.len() as f64
    };
    let status = format!(
        "fastless | byte {top}/{} ({percent:.1}%) | {}{} | {message}",
        data.len(),
        mode.name(),
        if current.is_some_and(|s| s.fold) {
            ", ignore case"
        } else {
            ""
        }
    );
    out.extend_from_slice(b"\x1b[7m\x1b[2K");
    out.extend_from_slice(&status.as_bytes()[..status.len().min(cols - 1)]);
    out.extend_from_slice(b"\x1b[0m");
    let mut stdout = io::stdout().lock();
    stdout
        .write_all(&out)
        .and_then(|_| stdout.flush())
        .map_err(|e| e.to_string())?;
    Ok(p)
}
fn prompt(rows: &mut usize, prefix: u8) -> Option<Vec<u8>> {
    let mut text = Vec::new();
    loop {
        let row = *rows;
        let mut out = io::stdout();
        let _ = write!(
            out,
            "\x1b[{row};1H\x1b[2K{}{}",
            prefix as char,
            String::from_utf8_lossy(&text)
        );
        let _ = out.flush();
        let c = key();
        if c == 13 || c == 10 {
            return Some(text);
        }
        if c == 27 || c < 0 || signals::cancelled() || signals::exiting() != 0 {
            return None;
        }
        if signals::resized() {
            signals::clear_resized();
            *rows = dimensions().0;
        }
        if c == 127 || c == 8 {
            text.pop();
        } else if (32..127).contains(&c) && text.len() < 4095 {
            text.push(c as u8);
        }
    }
}
pub fn run(
    data: &[u8],
    offset: usize,
    mut mode: Mode,
    case_mode: u8,
    simd: Simd,
) -> Result<i32, String> {
    let _terminal = Terminal::enter()?;
    let mut top = search::line_start(data, offset);
    let mut current: Option<Search> = None;
    let mut last_hit: Option<usize> = None;
    let mut direction = 1;
    let mut message = "q quit; / search; r/l/w mode".to_string();
    loop {
        if signals::exiting() != 0 {
            break;
        }
        signals::clear();
        let (mut rows, cols) = dimensions();
        let page_end = draw(data, top, current.as_ref(), mode, &message, rows, cols)?;
        let c = command();
        if c < 0 || c == b'q' as i32 || signals::exiting() != 0 {
            break;
        }
        match c as u8 {
            b' ' | b'f' => {
                if page_end < data.len() {
                    top = page_end;
                }
                last_hit = None;
            }
            b'b' => {
                for _ in 0..rows - 1 {
                    top = search::previous_line(data, top);
                }
                last_hit = None;
            }
            b'j' | b'\n' | b'\r' => {
                let p = search::next_line(data, top);
                if p < data.len() {
                    top = p;
                }
                last_hit = None;
            }
            b'k' => {
                top = search::previous_line(data, top);
                last_hit = None;
            }
            b'd' => {
                for _ in 0..(rows - 1) / 2 {
                    let p = search::next_line(data, top);
                    if p < data.len() {
                        top = p;
                    }
                }
                last_hit = None;
            }
            b'u' => {
                for _ in 0..(rows - 1) / 2 {
                    top = search::previous_line(data, top);
                }
                last_hit = None;
            }
            b'g' => {
                top = 0;
                last_hit = None;
            }
            b'G' => {
                top = if data.is_empty() {
                    0
                } else {
                    search::line_start(
                        data,
                        data.len() - usize::from(data[data.len() - 1] == b'\n'),
                    )
                };
                for _ in 1..rows - 1 {
                    top = search::previous_line(data, top);
                }
                last_hit = None;
            }
            b'r' | b'l' | b'w' => {
                let new_mode = match c as u8 {
                    b'r' => Mode::Regex,
                    b'l' => Mode::Literal,
                    _ => Mode::Glob,
                };
                if let Some(s) = &current {
                    match Search::new(new_mode, &s.text, case_mode, simd) {
                        Ok(s) => {
                            current = Some(s);
                            mode = new_mode;
                        }
                        Err(e) => message = e,
                    }
                } else {
                    mode = new_mode;
                }
                last_hit = None;
            }
            b'/' | b'?' | b'n' | b'N' => {
                let fresh = c == b'/' as i32 || c == b'?' as i32;
                if fresh {
                    let Some(input) = prompt(&mut rows, c as u8) else {
                        continue;
                    };
                    if !input.is_empty() {
                        match Search::new(mode, &input, case_mode, simd) {
                            Ok(s) => current = Some(s),
                            Err(e) => {
                                message = e;
                                continue;
                            }
                        }
                    }
                    direction = if c == b'/' as i32 { 1 } else { -1 };
                }
                let Some(s) = &current else {
                    message = "No search pattern".into();
                    continue;
                };
                let dir = if c == b'N' as i32 {
                    -direction
                } else {
                    direction
                };
                let mut start = if fresh { top } else { last_hit.unwrap_or(top) };
                if !fresh && last_hit.is_some() && dir > 0 {
                    if start == data.len() {
                        message = "No further match".into();
                        continue;
                    }
                    start = if mode == Mode::Glob {
                        search::next_line(data, start)
                    } else {
                        start + 1
                    };
                }
                print!("\x1b[{rows};1H\x1b[2KSearching... (Ctrl-C cancels)");
                let _ = io::stdout().flush();
                let result = if dir > 0 {
                    s.forward(data, start, data.len())
                } else {
                    s.backward(data, start)
                };
                match result {
                    Ok(Some(hit)) => {
                        last_hit = Some(hit);
                        top = search::line_start(data, hit);
                        message = format!("Match at byte {hit}");
                    }
                    Ok(None) => message = "Pattern not found (no wrap)".into(),
                    Err(e) => {
                        message = if signals::cancelled() {
                            "Search cancelled".into()
                        } else {
                            e
                        }
                    }
                }
            }
            _ => (),
        }
    }
    Ok(if signals::exiting() != 0 {
        signals::code()
    } else {
        0
    })
}
