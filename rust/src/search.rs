use crate::{
    literal::{Literal, Simd},
    signals,
};
use std::{
    ffi::{CStr, CString},
    mem,
};
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    Regex,
    Literal,
    Glob,
}
impl Mode {
    pub fn name(self) -> &'static str {
        match self {
            Self::Regex => "regex",
            Self::Literal => "literal",
            Self::Glob => "glob",
        }
    }
}
pub fn line_end(data: &[u8], pos: usize) -> usize {
    if pos >= data.len() {
        return data.len();
    }
    let hit = unsafe {
        libc::memchr(
            data.as_ptr().add(pos).cast(),
            b'\n' as i32,
            data.len() - pos,
        )
    };
    if hit.is_null() {
        data.len()
    } else {
        hit as usize - data.as_ptr() as usize
    }
}
pub fn line_start(data: &[u8], pos: usize) -> usize {
    let pos = pos.min(data.len());
    if pos == 0 {
        return 0;
    }
    #[cfg(target_os = "linux")]
    {
        let hit = unsafe { libc::memrchr(data.as_ptr().cast(), b'\n' as i32, pos) };
        if hit.is_null() {
            0
        } else {
            hit as usize - data.as_ptr() as usize + 1
        }
    }
    #[cfg(not(target_os = "linux"))]
    {
        data[..pos]
            .iter()
            .rposition(|&c| c == b'\n')
            .map_or(0, |i| i + 1)
    }
}
pub fn next_line(data: &[u8], pos: usize) -> usize {
    let end = line_end(data, pos);
    if end < data.len() {
        end + 1
    } else {
        end
    }
}
pub fn previous_line(data: &[u8], pos: usize) -> usize {
    if pos == 0 {
        0
    } else {
        line_start(data, pos - 1)
    }
}
struct Regex {
    raw: Box<libc::regex_t>,
}
impl Regex {
    fn error(raw: &libc::regex_t, code: i32) -> String {
        let len = unsafe { libc::regerror(code, raw, std::ptr::null_mut(), 0) };
        let mut text = vec![0u8; len.max(1)];
        unsafe {
            libc::regerror(code, raw, text.as_mut_ptr().cast(), text.len());
        }
        CStr::from_bytes_until_nul(&text).map_or_else(
            |_| "Regex error".into(),
            |s| s.to_string_lossy().into_owned(),
        )
    }
    fn new(text: &[u8], fold: bool) -> Result<Self, String> {
        let pattern = CString::new(text).map_err(|_| "Pattern contains NUL")?;
        let mut raw: Box<libc::regex_t> = Box::new(unsafe { mem::zeroed() });
        let code = unsafe {
            libc::regcomp(
                &mut *raw,
                pattern.as_ptr(),
                libc::REG_EXTENDED | if fold { libc::REG_ICASE } else { 0 },
            )
        };
        if code != 0 {
            return Err(Self::error(&raw, code));
        }
        Ok(Self { raw })
    }
    fn find(&self, data: &[u8], start: usize) -> Result<Option<(usize, usize)>, String> {
        if data.len() > i32::MAX as usize {
            return Err("Regex line exceeds 2 GiB; use literal or glob mode".into());
        }
        let mut matched = libc::regmatch_t {
            rm_so: start as _,
            rm_eo: data.len() as _,
        };
        static ZERO: [u8; 1] = [0];
        let ptr = if data.is_empty() {
            ZERO.as_ptr()
        } else {
            data.as_ptr()
        };
        // REG_STARTEND limits reads to this line; no NUL-terminated copy needed.
        let rc = unsafe {
            libc::regexec(
                &*self.raw,
                ptr.cast(),
                1,
                &mut matched,
                libc::REG_STARTEND | if start > 0 { libc::REG_NOTBOL } else { 0 },
            )
        };
        if rc == 0 {
            Ok(Some((matched.rm_so as usize, matched.rm_eo as usize)))
        } else if rc == libc::REG_NOMATCH {
            Ok(None)
        } else {
            Err(Self::error(&self.raw, rc))
        }
    }
}
impl Drop for Regex {
    fn drop(&mut self) {
        unsafe {
            libc::regfree(&mut *self.raw);
        }
    }
}
pub struct Search {
    pub mode: Mode,
    pub text: Vec<u8>,
    pub fold: bool,
    fast: Option<Literal>,
    regex: Option<Regex>,
    regex_literal: bool,
}
impl Search {
    pub fn new(mode: Mode, text: &[u8], case_mode: u8, simd: Simd) -> Result<Self, String> {
        if text.is_empty() {
            return Err("Empty pattern".into());
        }
        let fold =
            case_mode == 2 || (case_mode == 1 && !text.iter().any(|c| c.is_ascii_uppercase()));
        let mut result = Self {
            mode,
            text: text.to_vec(),
            fold,
            fast: None,
            regex: None,
            regex_literal: false,
        };
        match mode {
            Mode::Literal => result.fast = Some(Literal::new(text, fold, simd)),
            Mode::Regex => {
                result.regex = Some(Regex::new(text, fold)?);
                if !text.contains(&b'|') {
                    let start = usize::from(text[0] == b'^');
                    let mut end = start;
                    while end < text.len()
                        && text[end] < 128
                        && !b".[\\]()*+?{}^$".contains(&text[end])
                    {
                        end += 1;
                    }
                    if end > start && (end == text.len() || !b"*+?{".contains(&text[end])) {
                        result.fast = Some(Literal::new(&text[start..end], fold, simd));
                        result.regex_literal = start == 0 && end == text.len();
                    }
                }
            }
            Mode::Glob => {
                if text.len() > 2
                    && text[0] == b'*'
                    && text[text.len() - 1] == b'*'
                    && !text[1..text.len() - 1].iter().any(|c| b"*?\\".contains(c))
                {
                    result.fast = Some(Literal::new(&text[1..text.len() - 1], fold, simd));
                }
            }
        }
        Ok(result)
    }
    fn glob(&self, data: &[u8]) -> bool {
        if let Some(fast) = &self.fast {
            return fast.find(data).is_some();
        }
        let (mut p, mut t, mut star, mut retry, mut ticks) = (0usize, 0usize, None, 0usize, 0usize);
        while t < data.len() {
            ticks += 1;
            if ticks & 65535 == 0 && signals::cancelled() {
                return false;
            }
            if p < self.text.len() && self.text[p] == b'*' {
                p += 1;
                star = Some(p);
                retry = t;
                continue;
            }
            let mut c = self.text.get(p).copied().unwrap_or(0);
            let mut any = c == b'?';
            let mut step = 1;
            if c == b'\\' && p + 1 < self.text.len() {
                c = self.text[p + 1];
                step = 2;
                any = false;
            }
            let mut d = data[t];
            if self.fold {
                c = c.to_ascii_lowercase();
                d = d.to_ascii_lowercase();
            }
            if p < self.text.len() && (any || c == d) {
                p += step;
                t += 1;
            } else if let Some(next) = star {
                p = next;
                retry += 1;
                t = retry;
            } else {
                return false;
            }
        }
        while p < self.text.len() && self.text[p] == b'*' {
            p += 1;
        }
        p == self.text.len()
    }
    fn line_span(
        &self,
        data: &[u8],
        begin: usize,
        end: usize,
        mut start: usize,
    ) -> Result<Option<(usize, usize)>, String> {
        match self.mode {
            Mode::Glob => Ok(if start == begin && self.glob(&data[begin..end]) {
                Some((begin, end))
            } else {
                None
            }),
            Mode::Regex => {
                if end - begin > i32::MAX as usize {
                    return Err("Regex line exceeds 2 GiB; use literal or glob mode".into());
                }
                if let Some(fast) = &self.fast {
                    match fast.find(&data[start..end]) {
                        Some(hit) => start += hit,
                        None => return Ok(None),
                    }
                    if self.regex_literal {
                        return Ok(Some((start, start + fast.text.len())));
                    }
                }
                self.regex
                    .as_ref()
                    .unwrap()
                    .find(&data[begin..end], start - begin)
                    .map(|span| span.map(|(a, b)| (begin + a, begin + b)))
            }
            Mode::Literal => unreachable!(),
        }
    }
    fn cancellation() -> Result<(), String> {
        if signals::cancelled() {
            Err("Search interrupted".into())
        } else {
            Ok(())
        }
    }
    pub fn forward(
        &self,
        data: &[u8],
        start: usize,
        limit: usize,
    ) -> Result<Option<usize>, String> {
        if start > limit || limit > data.len() {
            return Ok(None);
        }
        if self.mode == Mode::Literal {
            let hit = self
                .fast
                .as_ref()
                .unwrap()
                .find(&data[start..limit])
                .map(|h| h + start);
            Self::cancellation()?;
            return Ok(hit);
        }
        let mut begin = line_start(data, start);
        while begin <= limit && (begin < data.len() || data.is_empty()) {
            Self::cancellation()?;
            let end = line_end(data, begin).min(limit);
            if let Some((hit, _)) = self.line_span(data, begin, end, start.max(begin))? {
                return Ok(Some(hit));
            }
            if end >= limit || end >= data.len() {
                break;
            }
            begin = end + 1;
        }
        Self::cancellation()?;
        Ok(None)
    }
    pub fn backward(&self, data: &[u8], before: usize) -> Result<Option<usize>, String> {
        if before == 0 {
            return Ok(None);
        }
        if self.mode == Mode::Literal {
            let hit = self.fast.as_ref().unwrap().reverse(data, before);
            Self::cancellation()?;
            return Ok(hit);
        }
        let mut begin = line_start(data, before - 1);
        loop {
            let end = line_end(data, begin);
            let (mut cursor, mut best) = (begin, None);
            while cursor <= end && cursor < before {
                Self::cancellation()?;
                match self.line_span(data, begin, end, cursor)? {
                    Some((hit, _)) if hit < before => {
                        best = Some(hit);
                        if self.mode == Mode::Glob || hit == end {
                            break;
                        }
                        cursor = hit + 1;
                    }
                    _ => break,
                }
            }
            if best.is_some() {
                return Ok(best);
            }
            if begin == 0 {
                break;
            }
            begin = previous_line(data, begin);
        }
        Self::cancellation()?;
        Ok(None)
    }
    pub fn highlight(
        &self,
        data: &[u8],
        begin: usize,
        end: usize,
        visible_end: usize,
    ) -> Vec<bool> {
        let mut marks = vec![false; visible_end - begin];
        if marks.is_empty() || signals::cancelled() {
            return marks;
        }
        if self.mode == Mode::Glob {
            if self.glob(&data[begin..end]) {
                marks.fill(true);
            }
            return marks;
        }
        let mut cursor = begin;
        let mut limit = end;
        if self.mode == Mode::Literal {
            cursor = begin.saturating_sub(self.text.len() - 1);
            limit = visible_end
                .saturating_add(self.text.len() - 1)
                .min(data.len());
        }
        while cursor < visible_end && !signals::cancelled() {
            let span = if self.mode == Mode::Literal {
                self.fast
                    .as_ref()
                    .unwrap()
                    .find(&data[cursor..limit])
                    .map(|hit| (cursor + hit, cursor + hit + self.text.len()))
            } else {
                self.line_span(data, begin, end, cursor).ok().flatten()
            };
            let Some((a, b)) = span else { break };
            if a >= visible_end {
                break;
            }
            let lo = a.max(begin);
            let hi = b.min(visible_end);
            if hi > lo {
                marks[lo - begin..hi - begin].fill(true);
            }
            cursor = a + 1;
        }
        marks
    }
}
