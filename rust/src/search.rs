use crate::{
    literal::{Literal, Simd},
    signals,
};
use std::{
    cell::Cell,
    ffi::{CStr, CString},
    mem,
    sync::{
        atomic::{AtomicUsize, Ordering},
        Mutex,
    },
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
unsafe impl Send for Regex {}
static THREADS: AtomicUsize = AtomicUsize::new(1);
/// Number of threads large searches are divided among.
pub fn set_threads(threads: usize) {
    THREADS.store(threads.max(1), Ordering::Relaxed);
}
// Searches go multi-threaded beyond NEAR bytes, in PIECE units.
#[cfg(not(fastless_tiny_pieces))]
const NEAR: usize = 8 << 20;
#[cfg(not(fastless_tiny_pieces))]
const PIECE: usize = 4 << 20;
#[cfg(fastless_tiny_pieces)]
const NEAR: usize = 40;
#[cfg(fastless_tiny_pieces)]
const PIECE: usize = 24;
/// Longest literal that every match of the regex must contain: a run of
/// ordinary characters outside groups and brackets, none followed by a
/// repetition operator (which may stack, as in "c+?"). Alternation and
/// non-ASCII bytes are not analysed, and anything doubtful simply ends a run.
fn regex_required(text: &[u8]) -> Option<(usize, usize)> {
    if text.contains(&b'|') {
        return None;
    }
    let len = text.len();
    let (mut best, mut run, mut begin, mut depth, mut i) = (0usize, 0usize, 0usize, 0usize, 0usize);
    let mut span = None;
    while i < len {
        let c = text[i];
        let mut next = i + 1;
        let mut plain = c < 128 && !b".[]()*+?{}^$\\\n".contains(&c);
        if c == b'\\' && next < len && b".[]()*+?{}^$\\".contains(&text[next]) {
            plain = true;
            next += 1;
        }
        if plain && depth == 0 && !(next < len && b"*+?{".contains(&text[next])) {
            if run == 0 {
                begin = i;
            }
            run += 1;
            i = next;
            if run > best {
                best = run;
                span = Some((begin, i));
            }
            continue;
        }
        run = 0;
        if plain {
            i = next;
            continue;
        }
        match c {
            b'\\' => next += 1,
            b'(' => depth += 1,
            b')' if depth > 0 => depth -= 1,
            b'{' => {
                while next < len && text[next] != b'}' {
                    next += 1;
                }
            }
            b'[' => {
                if next < len && text[next] == b'^' {
                    next += 1;
                }
                if next < len && text[next] == b']' {
                    next += 1;
                }
                while next < len && text[next] != b']' {
                    if text[next] == b'[' && next + 1 < len && b":.=".contains(&text[next + 1]) {
                        let kind = text[next + 1];
                        next += 2;
                        while next + 1 < len && !(text[next] == kind && text[next + 1] == b']') {
                            next += 1;
                        }
                        next += 2;
                    } else {
                        next += 1;
                    }
                }
                next += 1;
            }
            _ => (),
        }
        i = next;
    }
    span
}
/// Longest run of ordinary characters in a glob: every matching line contains
/// it. The flag is set when the glob is nothing but that run between '*'s.
fn glob_required(text: &[u8]) -> Option<(usize, usize, bool)> {
    let len = text.len();
    let (mut best, mut run, mut runs, mut begin, mut i) = (0usize, 0usize, 0usize, 0usize, 0usize);
    let (mut from, mut to, mut other) = (0, 0, false);
    while i < len {
        let mut next = i + 1;
        let mut c = text[i];
        let wild = c == b'*' || c == b'?';
        if c == b'\\' && next < len {
            c = text[next];
            next += 1;
        }
        if wild || c == b'\n' {
            if c != b'*' {
                other = true;
            }
            run = 0;
            i = next;
            continue;
        }
        if run == 0 {
            begin = i;
            runs += 1;
        }
        run += 1;
        i = next;
        if run > best {
            best = run;
            from = begin;
            to = i;
        }
    }
    (best > 0).then_some((from, to, runs == 1 && !other && from > 0 && to < len))
}
/// Removes backslash escapes from a run found by the functions above.
fn unescape(run: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(run.len());
    let mut i = 0;
    while i < run.len() {
        if run[i] == b'\\' && i + 1 < run.len() {
            i += 1;
        }
        out.push(run[i]);
        i += 1;
    }
    out
}
pub struct Search {
    pub mode: Mode,
    pub text: Vec<u8>,
    pub fold: bool,
    /// The whole pattern in literal mode, otherwise a literal that every
    /// match must contain.
    fast: Option<Literal>,
    regex: Option<Regex>,
    regex_prefix: bool,
    regex_literal: bool,
    glob_contains: bool,
    simd: Simd,
    /// Last line located by `line_at`: (data address, data length, begin, end).
    line: Cell<Option<(usize, usize, usize, usize)>>,
}
impl Search {
    pub fn new(mode: Mode, text: &[u8], case_mode: u8, simd: Simd) -> Result<Self, String> {
        if text.is_empty() {
            return Err("Empty pattern".into());
        }
        let fold =
            case_mode == 2 || (case_mode == 1 && !text.iter().any(|c| c.is_ascii_uppercase()));
        Self::build(mode, text, fold, simd)
    }
    fn build(mode: Mode, text: &[u8], fold: bool, simd: Simd) -> Result<Self, String> {
        let mut result = Self {
            mode,
            text: text.to_vec(),
            fold,
            fast: None,
            regex: None,
            regex_prefix: false,
            regex_literal: false,
            glob_contains: false,
            simd,
            line: Cell::new(None),
        };
        match mode {
            Mode::Literal => result.fast = Some(Literal::new(text, fold, simd)),
            Mode::Regex => {
                result.regex = Some(Regex::new(text, fold)?);
                if let Some((from, to)) = regex_required(text) {
                    result.fast = Some(Literal::new(&unescape(&text[from..to]), fold, simd));
                    result.regex_prefix = from == usize::from(text[0] == b'^');
                    result.regex_literal = from == 0 && to == text.len();
                }
            }
            Mode::Glob => {
                if let Some((from, to, only)) = glob_required(text) {
                    result.fast = Some(Literal::new(&unescape(&text[from..to]), fold, simd));
                    result.glob_contains = only;
                }
            }
        }
        Ok(result)
    }
    /// True when matching is nothing more than finding `fast`.
    fn plain(&self) -> bool {
        self.mode == Mode::Literal || self.regex_literal
    }
    fn glob(&self, data: &[u8]) -> bool {
        if self.glob_contains {
            return self.fast.as_ref().unwrap().find(data).is_some();
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
                if let Some(fast) = &self.fast {
                    let Some(hit) = fast.find(&data[start..end]) else {
                        return Ok(None);
                    };
                    if self.regex_literal {
                        return Ok(Some((start + hit, start + hit + fast.text.len())));
                    }
                    // Only a literal that begins the pattern marks where a
                    // match starts.
                    if self.regex_prefix {
                        start += hit;
                    }
                }
                if end - begin > i32::MAX as usize {
                    return Err("Regex line exceeds 2 GiB; use literal or glob mode".into());
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
    /// Single-threaded search of `[start, limit)`.
    fn forward_scan(
        &self,
        data: &[u8],
        start: usize,
        limit: usize,
    ) -> Result<Option<usize>, String> {
        if start > limit || limit > data.len() {
            return Ok(None);
        }
        // A regex that is a plain literal needs neither line bounds nor regexec.
        if self.plain() {
            let hit = self
                .fast
                .as_ref()
                .unwrap()
                .find(&data[start..limit])
                .map(|h| h + start);
            Self::cancellation()?;
            return Ok(hit);
        }
        let mut at = start;
        while at <= limit {
            Self::cancellation()?;
            // The literal every match must contain (never a newline) is searched
            // for across the whole range, so lines without it are not visited.
            if let Some(fast) = &self.fast {
                match fast.find(&data[at..limit]) {
                    Some(hit) => at += hit,
                    None => break,
                }
            }
            let (begin, end) = self.line_at(data, at);
            if begin >= data.len() && !data.is_empty() {
                break;
            }
            let end = end.min(limit);
            let from = if self.mode == Mode::Glob || !self.regex_prefix {
                start.max(begin)
            } else {
                at
            };
            if let Some((hit, _)) = self.line_span(data, begin, end, from)? {
                return Ok(Some(hit));
            }
            if end >= limit || end >= data.len() {
                break;
            }
            at = end + 1;
        }
        Self::cancellation()?;
        Ok(None)
    }
    /// Bounds of the line containing `pos`. The last answer is kept so that
    /// repeated searches inside one very long line do not rescan it each time.
    fn line_at(&self, data: &[u8], pos: usize) -> (usize, usize) {
        let (address, len) = (data.as_ptr() as usize, data.len());
        if let Some((a, l, begin, end)) = self.line.get() {
            if a == address && l == len && (begin..=end).contains(&pos) {
                return (begin, end);
            }
        }
        let (begin, end) = (line_start(data, pos), line_end(data, pos));
        self.line.set(Some((address, len, begin, end)));
        (begin, end)
    }
    /// Single-threaded search for the last match starting before `before`.
    /// Literal matches must start at or after `floor`; line-based ones must
    /// lie on a line starting at or after it.
    fn backward_scan(
        &self,
        data: &[u8],
        before: usize,
        floor: usize,
    ) -> Result<Option<usize>, String> {
        if before <= floor {
            return Ok(None);
        }
        if self.plain() {
            let hit = self
                .fast
                .as_ref()
                .unwrap()
                .reverse(&data[floor..], before - floor)
                .map(|h| h + floor);
            Self::cancellation()?;
            return Ok(hit);
        }
        let (mut begin, mut end) = self.line_at(data, before - 1);
        while begin >= floor {
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
            if begin <= floor {
                break;
            }
            // With a required literal, skip straight back to the nearest line
            // holding it.
            let earlier = match &self.fast {
                Some(fast) => match fast.reverse(&data[floor..], begin - floor) {
                    Some(hit) => hit + floor,
                    None => break,
                },
                None => begin - 1,
            };
            (begin, end) = self.line_at(data, earlier);
        }
        Self::cancellation()?;
        Ok(None)
    }
    /// A piece `[a, b)` of a larger search owns the literal matches starting
    /// in it or, for line-based modes, the matches on lines starting in it,
    /// so pieces can be searched independently and their results ordered by
    /// position. `first` marks the piece at the search start, which may begin
    /// mid-line.
    fn forward_piece(
        &self,
        data: &[u8],
        mut a: usize,
        mut b: usize,
        mut limit: usize,
        first: bool,
    ) -> Result<Option<usize>, String> {
        if self.plain() {
            let n = self.fast.as_ref().unwrap().text.len();
            if limit - b > n - 1 {
                limit = b + n - 1;
            }
            return self.forward_scan(data, a, limit);
        }
        if !first && a > 0 && data[a - 1] != b'\n' {
            a = next_line(data, a);
        }
        if b < limit {
            b = line_end(data, b - 1).min(limit);
        }
        if a > b {
            Ok(None)
        } else {
            self.forward_scan(data, a, b)
        }
    }
    fn backward_piece(
        &self,
        data: &[u8],
        a: usize,
        mut b: usize,
        before: usize,
    ) -> Result<Option<usize>, String> {
        if !self.plain() {
            b = line_end(data, b - 1) + 1;
        }
        self.backward_scan(data, b.min(before), a)
    }
    /// Searches the pieces of `[low, high)` on all search threads. `bound` is
    /// the limit of a forward search or the `before` of a backward one.
    fn pieces(
        &self,
        data: &[u8],
        forward: bool,
        low: usize,
        high: usize,
        bound: usize,
    ) -> Result<Option<usize>, String> {
        let count = (high - low).div_ceil(PIECE);
        // glibc serializes regexec on one compiled pattern, and the line cache
        // is per search, so every extra thread gets its own copy.
        let extra = THREADS.load(Ordering::Relaxed).min(count).saturating_sub(1);
        let copies: Vec<Search> = (0..extra)
            .filter_map(|_| Self::build(self.mode, &self.text, self.fold, self.simd).ok())
            .collect();
        // Pieces are numbered in search order, so the lowest-numbered result
        // wins and nothing beyond it needs starting.
        let (next, best) = (AtomicUsize::new(0), AtomicUsize::new(usize::MAX));
        let outcome: Mutex<Option<Result<usize, String>>> = Mutex::new(None);
        let work = |search: &Search| loop {
            let i = next.fetch_add(1, Ordering::Relaxed);
            if i >= count || i > best.load(Ordering::Relaxed) || signals::cancelled() {
                break;
            }
            let result = if forward {
                let a = low + i * PIECE;
                search.forward_piece(data, a, (a + PIECE).min(high), bound, false)
            } else {
                let b = high - i * PIECE;
                search.backward_piece(data, b.saturating_sub(PIECE).max(low), b, bound)
            };
            let found = match result {
                Ok(None) => continue,
                Ok(Some(hit)) => Ok(hit),
                Err(_) if signals::cancelled() => continue,
                Err(e) => Err(e),
            };
            let mut slot = outcome.lock().unwrap();
            if i < best.load(Ordering::Relaxed) {
                best.store(i, Ordering::Relaxed);
                *slot = Some(found);
            }
        };
        let work = &work;
        std::thread::scope(|scope| {
            for copy in copies {
                scope.spawn(move || work(&copy));
            }
            work(self);
        });
        Self::cancellation()?;
        outcome.into_inner().unwrap().transpose()
    }
    /// The part nearest the starting point is searched on this thread, so
    /// nearby matches cost no thread start-up; the rest is divided among all
    /// threads.
    pub fn forward(
        &self,
        data: &[u8],
        start: usize,
        limit: usize,
    ) -> Result<Option<usize>, String> {
        if start > limit || limit > data.len() {
            return Ok(None);
        }
        if THREADS.load(Ordering::Relaxed) < 2 || limit - start <= NEAR {
            return self.forward_scan(data, start, limit);
        }
        match self.forward_piece(data, start, start + NEAR, limit, true)? {
            Some(hit) => Ok(Some(hit)),
            None => self.pieces(data, true, start + NEAR, limit, limit),
        }
    }
    pub fn backward(&self, data: &[u8], before: usize) -> Result<Option<usize>, String> {
        let before = before.min(data.len());
        if THREADS.load(Ordering::Relaxed) < 2 || before <= NEAR {
            return self.backward_scan(data, before, 0);
        }
        match self.backward_piece(data, before - NEAR, before, before)? {
            Some(hit) => Ok(Some(hit)),
            None => self.pieces(data, false, 0, before - NEAR, before),
        }
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
        let length = self.fast.as_ref().map_or(0, |fast| fast.text.len());
        if self.mode == Mode::Literal {
            cursor = begin.saturating_sub(self.text.len() - 1);
            limit = visible_end
                .saturating_add(self.text.len() - 1)
                .min(data.len());
        } else if self.regex_literal {
            // Only matches starting in the visible part matter.
            limit = visible_end.saturating_add(length - 1).min(end);
        }
        while cursor < visible_end && !signals::cancelled() {
            let span = if self.mode == Mode::Literal || self.regex_literal {
                self.fast
                    .as_ref()
                    .unwrap()
                    .find(&data[cursor..limit])
                    .map(|hit| (cursor + hit, cursor + hit + length))
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
