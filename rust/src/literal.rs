use crate::signals;

#[derive(Clone, Copy)]
pub struct Simd {
    pub enabled: bool,
    pub force: bool,
}
impl Simd {
    pub fn configure(mode: &str) -> Result<Self, String> {
        #[cfg(target_arch = "x86_64")]
        let available = std::is_x86_feature_detected!("avx2");
        #[cfg(not(target_arch = "x86_64"))]
        let available = false;
        match mode {
            "auto" => Ok(Self {
                enabled: available,
                force: false,
            }),
            "off" => Ok(Self {
                enabled: false,
                force: false,
            }),
            "avx2" if available => Ok(Self {
                enabled: true,
                force: true,
            }),
            _ => Err("Invalid SIMD mode or AVX2 unavailable; use auto, off, or avx2".into()),
        }
    }
}
pub struct Literal {
    pub text: Vec<u8>,
    pub fold: bool,
    skip: [usize; 256],
    reverse_skip: [usize; 256],
    anchor: usize,
    simd: Simd,
}
/// Candidates given to the skip-table/libc scan after a dense AVX2 block,
/// before AVX2 is tried again.
#[cfg(target_arch = "x86_64")]
const DENSE_WINDOW: usize = 64 * 1024;
#[inline]
pub fn lower(c: u8) -> u8 {
    if c.is_ascii_uppercase() {
        c + 32
    } else {
        c
    }
}
impl Literal {
    pub fn new(text: &[u8], fold: bool, simd: Simd) -> Self {
        assert!(!text.is_empty());
        let n = text.len();
        let mut result = Self {
            text: text.to_vec(),
            fold,
            skip: [n; 256],
            reverse_skip: [n; 256],
            anchor: n - 1,
            simd,
        };
        for (i, &c) in text.iter().enumerate().take(n - 1) {
            result.skip[if fold { lower(c) } else { c } as usize] = n - 1 - i;
        }
        for i in (1..n).rev() {
            let c = if fold { lower(text[i]) } else { text[i] };
            result.reverse_skip[c as usize] = i;
        }
        let first = if fold { lower(text[0]) } else { text[0] };
        for i in (1..n).rev() {
            let c = if fold { lower(text[i]) } else { text[i] };
            if c != first {
                result.anchor = i;
                break;
            }
        }
        result
    }
    #[inline]
    fn equal(&self, data: &[u8]) -> bool {
        if !self.fold {
            data == self.text.as_slice()
        } else {
            data.iter()
                .zip(&self.text)
                .all(|(&a, &b)| lower(a) == lower(b))
        }
    }
    pub fn find(&self, data: &[u8]) -> Option<usize> {
        let n = self.text.len();
        if n > data.len() || signals::cancelled() {
            return None;
        }
        #[cfg(target_arch = "x86_64")]
        if self.simd.enabled
            && (self.fold || data.len() >= 4096 || self.simd.force)
            && n <= 256
            && data.len() - n >= 31
        {
            // CPU feature checked above; kernel proves every load is in range.
            return unsafe { self.find_avx2(data) };
        }
        self.find_portable(data)
    }
    fn find_portable(&self, data: &[u8]) -> Option<usize> {
        let n = self.text.len();
        let last = data.len() - n;
        let mut pos = 0;
        if !self.fold {
            while pos <= last {
                if signals::cancelled() {
                    return None;
                }
                let candidates = (last - pos + 1).min(8 * 1024 * 1024);
                // Bounded raw byte search; libc never owns either pointer.
                let hit = unsafe {
                    libc::memmem(
                        data.as_ptr().add(pos).cast(),
                        candidates + n - 1,
                        self.text.as_ptr().cast(),
                        n,
                    )
                };
                if !hit.is_null() {
                    return Some(hit as usize - data.as_ptr() as usize);
                }
                pos += candidates;
            }
        } else {
            let mut ticks = 0usize;
            while pos <= last {
                if ticks & 65535 == 0 && signals::cancelled() {
                    return None;
                }
                ticks += 1;
                let mut j = n;
                while j > 0 && lower(data[pos + j - 1]) == lower(self.text[j - 1]) {
                    j -= 1;
                }
                if j == 0 {
                    return Some(pos);
                }
                let step = self.skip[lower(data[pos + n - 1]) as usize];
                if step > last - pos {
                    break;
                }
                pos += step;
            }
        }
        None
    }
    pub fn reverse(&self, data: &[u8], before: usize) -> Option<usize> {
        let n = self.text.len();
        if n > data.len() || before == 0 || signals::cancelled() {
            return None;
        }
        let candidates = (data.len() - n + 1).min(before);
        #[cfg(target_arch = "x86_64")]
        if self.simd.enabled && n <= 256 && candidates >= 32 {
            return unsafe { self.reverse_avx2(data, candidates) };
        }
        self.reverse_portable(data, candidates)
    }
    fn reverse_portable(&self, data: &[u8], candidates: usize) -> Option<usize> {
        let mut pos = candidates - 1;
        let mut ticks = 0usize;
        loop {
            if ticks & 65535 == 0 && signals::cancelled() {
                return None;
            }
            ticks += 1;
            if self.equal(&data[pos..pos + self.text.len()]) {
                return Some(pos);
            }
            let c = if self.fold {
                lower(data[pos])
            } else {
                data[pos]
            };
            let step = self.reverse_skip[c as usize];
            if step > pos {
                break;
            }
            pos -= step;
        }
        None
    }
    #[inline]
    fn verify_mask(
        &self,
        data: &[u8],
        pos: usize,
        mut mask: u32,
        failed: &mut usize,
    ) -> Option<usize> {
        while mask != 0 {
            let bit = mask.trailing_zeros() as usize;
            let start = pos + bit;
            if self.equal(&data[start..start + self.text.len()]) {
                return Some(start);
            }
            *failed += 1;
            mask &= mask - 1;
        }
        None
    }
}

#[cfg(target_arch = "x86_64")]
mod avx2 {
    use super::*;
    use std::arch::x86_64::*;
    #[target_feature(enable = "avx2")]
    unsafe fn anchor_matches(ptr: *const u8, needle: __m256i, fold: bool) -> __m256i {
        let mut bytes = _mm256_loadu_si256(ptr.cast());
        if fold {
            bytes = _mm256_or_si256(bytes, _mm256_set1_epi8(0x20));
        }
        _mm256_cmpeq_epi8(bytes, needle)
    }
    #[target_feature(enable = "avx2")]
    unsafe fn candidates(
        p: &Literal,
        ptr: *const u8,
        a: __m256i,
        b: __m256i,
        fold_a: bool,
        fold_b: bool,
    ) -> __m256i {
        let mut matches = anchor_matches(ptr, a, fold_a);
        if p.anchor != 0 {
            matches = _mm256_and_si256(matches, anchor_matches(ptr.add(p.anchor), b, fold_b));
        }
        matches
    }
    impl Literal {
        #[target_feature(enable = "avx2")]
        pub(super) unsafe fn find_avx2(&self, data: &[u8]) -> Option<usize> {
            let count = data.len() - self.text.len() + 1;
            let (mut pos, mut ticks) = (0usize, 0usize);
            let a = if self.fold {
                lower(self.text[0])
            } else {
                self.text[0]
            };
            let b = if self.fold {
                lower(self.text[self.anchor])
            } else {
                self.text[self.anchor]
            };
            let (fold_a, fold_b) = (
                self.fold && a.is_ascii_lowercase(),
                self.fold && b.is_ascii_lowercase(),
            );
            let (first, second) = (_mm256_set1_epi8(a as i8), _mm256_set1_epi8(b as i8));
            // count-pos proves even the last anchor load ends before data.len().
            while count - pos >= 128 {
                if ticks & 4095 == 0 && signals::cancelled() {
                    return None;
                }
                ticks += 1;
                let ptr = data.as_ptr().add(pos);
                let m0 = candidates(self, ptr, first, second, fold_a, fold_b);
                let m1 = candidates(self, ptr.add(32), first, second, fold_a, fold_b);
                let m2 = candidates(self, ptr.add(64), first, second, fold_a, fold_b);
                let m3 = candidates(self, ptr.add(96), first, second, fold_a, fold_b);
                let any = _mm256_or_si256(_mm256_or_si256(m0, m1), _mm256_or_si256(m2, m3));
                if _mm256_movemask_epi8(any) != 0 {
                    let mut failed = 0;
                    for (offset, m) in [(0, m0), (32, m1), (64, m2), (96, m3)] {
                        if let Some(hit) = self.verify_mask(
                            data,
                            pos + offset,
                            _mm256_movemask_epi8(m) as u32,
                            &mut failed,
                        ) {
                            return Some(hit);
                        }
                    }
                    // Dense false positives favor the skip-table/libc algorithm,
                    // but only locally: the data may be sparse again further on.
                    if failed >= 16 {
                        pos += 128;
                        let window = (count - pos).min(DENSE_WINDOW);
                        if window == 0 {
                            return None;
                        }
                        let range = &data[pos..pos + window + self.text.len() - 1];
                        if let Some(hit) = self.find_portable(range) {
                            return Some(hit + pos);
                        }
                        if signals::cancelled() {
                            return None;
                        }
                        pos += window;
                        continue;
                    }
                }
                pos += 128;
            }
            while count - pos >= 32 {
                if signals::cancelled() {
                    return None;
                }
                let m = candidates(self, data.as_ptr().add(pos), first, second, fold_a, fold_b);
                if let Some(hit) =
                    self.verify_mask(data, pos, _mm256_movemask_epi8(m) as u32, &mut 0)
                {
                    return Some(hit);
                }
                pos += 32;
            }
            while pos < count {
                if self.equal(&data[pos..pos + self.text.len()]) {
                    return Some(pos);
                }
                pos += 1;
            }
            None
        }
        #[target_feature(enable = "avx2")]
        pub(super) unsafe fn reverse_avx2(&self, data: &[u8], mut count: usize) -> Option<usize> {
            let mut ticks = 0usize;
            let a = if self.fold {
                lower(self.text[0])
            } else {
                self.text[0]
            };
            let b = if self.fold {
                lower(self.text[self.anchor])
            } else {
                self.text[self.anchor]
            };
            let (fold_a, fold_b) = (
                self.fold && a.is_ascii_lowercase(),
                self.fold && b.is_ascii_lowercase(),
            );
            let (first, second) = (_mm256_set1_epi8(a as i8), _mm256_set1_epi8(b as i8));
            while count >= 32 {
                if ticks & 4095 == 0 && signals::cancelled() {
                    return None;
                }
                ticks += 1;
                let pos = count - 32;
                let m = candidates(self, data.as_ptr().add(pos), first, second, fold_a, fold_b);
                let mut mask = _mm256_movemask_epi8(m) as u32;
                let mut failed = 0;
                while mask != 0 {
                    let bit = 31 - mask.leading_zeros() as usize;
                    if self.equal(&data[pos + bit..pos + bit + self.text.len()]) {
                        return Some(pos + bit);
                    }
                    failed += 1;
                    mask &= !(1u32 << bit);
                }
                count = pos;
                if failed >= 8 && count > 0 {
                    let window = count.min(DENSE_WINDOW);
                    let low = count - window;
                    if let Some(hit) = self.reverse_portable(&data[low..], window) {
                        return Some(hit + low);
                    }
                    if signals::cancelled() {
                        return None;
                    }
                    count = low;
                }
            }
            while count > 0 {
                count -= 1;
                if self.equal(&data[count..count + self.text.len()]) {
                    return Some(count);
                }
            }
            None
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn next(state: &mut u64) -> usize {
        *state ^= *state << 13;
        *state ^= *state >> 7;
        *state ^= *state << 17;
        *state as u32 as usize
    }
    fn verify(data: &[u8], text: &[u8], fold: bool, before: usize) {
        let reference = |window: &[u8]| {
            window
                .iter()
                .zip(text)
                .all(|(&a, &b)| if fold { lower(a) == lower(b) } else { a == b })
        };
        let first = if text.len() <= data.len() {
            data.windows(text.len()).position(reference)
        } else {
            None
        };
        let last = if text.len() <= data.len() {
            data.windows(text.len())
                .enumerate()
                .take(before)
                .filter(|(_, w)| reference(w))
                .map(|(i, _)| i)
                .next_back()
        } else {
            None
        };
        let mut literal = Literal::new(text, fold, Simd::configure("off").unwrap());
        for mode in ["off", "auto", "avx2"] {
            if let Ok(simd) = Simd::configure(mode) {
                literal.simd = simd;
                assert_eq!(
                    literal.find(data),
                    first,
                    "{mode} forward n={} len={} fold={fold}",
                    text.len(),
                    data.len()
                );
                assert_eq!(
                    literal.reverse(data, before),
                    last,
                    "{mode} reverse n={} len={} fold={fold}",
                    text.len(),
                    data.len()
                );
            }
        }
    }
    #[test]
    fn randomized_binary_matches() {
        let mut state = 0x391abe17u64;
        let alphabet = b"aAbB@[`{!?xX";
        for trial in 0..6000 {
            let len = next(&mut state) % 8192;
            let n = 1 + next(&mut state) % 513;
            let fold = trial & 1 != 0;
            let mut hay: Vec<u8> = (0..len)
                .map(|_| {
                    if trial % 3 != 0 {
                        alphabet[next(&mut state) % alphabet.len()]
                    } else {
                        next(&mut state) as u8
                    }
                })
                .collect();
            let text: Vec<u8> = (0..n)
                .map(|_| {
                    if trial % 3 != 0 {
                        alphabet[next(&mut state) % alphabet.len()]
                    } else {
                        next(&mut state) as u8
                    }
                })
                .collect();
            if n <= len && trial % 4 != 0 {
                let start = next(&mut state) % (len - n + 1);
                hay[start..start + n].copy_from_slice(&text);
                if fold {
                    for c in &mut hay[start..start + n] {
                        *c = c.to_ascii_uppercase();
                    }
                }
            }
            verify(&hay, &text, fold, next(&mut state) % (len + 2));
        }
    }
    #[test]
    fn guard_pages_and_dense_candidates() {
        let page = unsafe { libc::sysconf(libc::_SC_PAGESIZE) } as usize;
        let base = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                page * 3,
                libc::PROT_NONE,
                libc::MAP_PRIVATE | libc::MAP_ANONYMOUS,
                -1,
                0,
            )
        };
        assert_ne!(base, libc::MAP_FAILED);
        assert_eq!(
            unsafe {
                libc::mprotect(
                    (base as *mut u8).add(page).cast(),
                    page,
                    libc::PROT_READ | libc::PROT_WRITE,
                )
            },
            0
        );
        for edge in 0..2 {
            for len in 0..=640 {
                let ptr =
                    unsafe { (base as *mut u8).add(page + if edge == 0 { 0 } else { page - len }) };
                let hay = unsafe { std::slice::from_raw_parts_mut(ptr, len) };
                for n in [
                    1, 2, 3, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 256, 257, 511,
                ] {
                    let mut text = vec![b'a'; n];
                    text[n / 2] = b'B';
                    hay.fill(b'A');
                    if n <= len {
                        hay[len - n..].copy_from_slice(&text);
                    }
                    for fold in [false, true] {
                        verify(hay, &text, fold, len);
                    }
                }
            }
        }
        unsafe {
            libc::munmap(base, page * 3);
        }
        for n in 3..=256 {
            let mut hay: Vec<_> = (0..8192)
                .map(|i| if i & 1 == 0 { b'a' } else { b'b' })
                .collect();
            let mut text = hay[..n].to_vec();
            text[n / 2] = b'Z';
            hay[4001..4001 + n].copy_from_slice(&text);
            for fold in [false, true] {
                verify(&hay, &text, fold, hay.len());
            }
        }
    }
    #[test]
    fn every_byte_case_folding() {
        for a in 0..=255u8 {
            let hay = [a; 128];
            for b in 0..=255u8 {
                verify(&hay, &[b], true, 80);
            }
        }
    }
}
