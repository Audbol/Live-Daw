//! The disk writer thread: drains the record rings into the take files,
//! computes the peaks for drawing while recording, rewrites the WAV headers
//! every few seconds and finalizes the takes once the audio thread has let
//! go of a recording.

use crate::wav::WavWriter;
use crate::*;

const WRITE_BLOCK: usize = 8192;

#[derive(Clone, Copy)]
struct PeakAcc {
    count: usize,
    min: [f32; 2],
    max: [f32; 2],
}

impl PeakAcc {
    const EMPTY: Self = Self { count: 0, min: [f32::MAX; 2], max: [f32::MIN; 2] };

    /// Adds `n` frames of `nc` channels (None: silence); completed peaks go
    /// to `out` as min/max pairs per channel.
    fn add(&mut self, nc: usize, channels: Option<[&[f32]; 2]>, n: usize, out: &mut Vec<f32>) {
        let mut i = 0;
        while i < n {
            let k = (SAMPLES_PER_PEAK - self.count).min(n - i);
            for c in 0..nc {
                let (lo, hi) = match channels {
                    Some(ch) => ch[c][i..i + k].iter().fold((self.min[c], self.max[c]), |(lo, hi), &x| (lo.min(x), hi.max(x))),
                    None => (self.min[c].min(0.0), self.max[c].max(0.0)),
                };
                self.min[c] = lo;
                self.max[c] = hi;
            }
            self.count += k;
            i += k;
            if self.count == SAMPLES_PER_PEAK {
                for c in 0..nc {
                    out.extend_from_slice(&[self.min[c], self.max[c]]);
                }
                *self = Self::EMPTY;
            }
        }
    }
}

/// One recording on the writer thread.
pub(crate) struct WriterJob {
    session: Arc<RecordSession>,
    files: Vec<WavWriter>,
    failed: Vec<bool>,
    /// Ring frames consumed.
    consumed: usize,
    /// Frames written to each file (ring frames and gaps).
    written: i64,
    peaks: Vec<PeakAcc>,
    new_peaks: Vec<Vec<f32>>,
    last_header: Instant,
    scratch: Vec<Vec<f32>>,
}

impl WriterJob {
    pub(crate) fn new(session: Arc<RecordSession>, files: Vec<WavWriter>) -> Self {
        let tracks = session.tracks.len();
        let streams = session.streams.len();
        Self {
            failed: vec![false; files.len()],
            files,
            consumed: 0,
            written: 0,
            peaks: vec![PeakAcc::EMPTY; tracks],
            new_peaks: vec![Vec::new(); tracks],
            last_header: Instant::now(),
            scratch: vec![vec![0.0; WRITE_BLOCK]; streams],
            session,
        }
    }

    fn drained(&self) -> bool {
        self.session.ring.available() == 0 && self.session.gaps.is_empty()
    }

    fn fail(&mut self, s: &Shared, track: usize) {
        if !self.failed[track] {
            self.failed[track] = true;
            s.dropouts.fetch_add(1, Relaxed);
        }
    }

    fn write_silence(&mut self, s: &Shared, n: usize) {
        for t in 0..self.files.len() {
            if !self.failed[t] && self.files[t].write_silence(n).is_err() {
                self.fail(s, t);
            }
            let nc = self.session.tracks[t].num_channels;
            self.peaks[t].add(nc, None, n, &mut self.new_peaks[t]);
        }
        self.written += n as i64;
    }

    fn write_frames(&mut self, s: &Shared, n: usize) {
        let session = self.session.clone();
        for (t, track) in session.tracks.iter().enumerate() {
            let first = track.first_stream;
            let second = first + track.num_channels.min(2) - 1;
            let channels = [&self.scratch[first][..n], &self.scratch[second][..n]];
            if !self.failed[t] && self.files[t].write_planar(&channels[..track.num_channels], n).is_err() {
                self.failed[t] = true;
                s.dropouts.fetch_add(1, Relaxed);
            }
            self.peaks[t].add(track.num_channels, Some(channels), n, &mut self.new_peaks[t]);
        }
        self.written += n as i64;
    }

    /// Writes what the ring holds. Returns whether anything was written.
    fn drain(&mut self, s: &Shared) -> bool {
        let session = self.session.clone();
        let mut did = false;
        loop {
            // The available frames first: a gap before them is then visible too.
            let available = session.ring.available();
            while let Some(gap) = session.gaps.peek().filter(|g| g.at <= self.consumed) {
                session.gaps.pop();
                self.write_silence(s, gap.len);
                did = true;
            }
            let until_gap = session.gaps.peek().map_or(usize::MAX, |g| g.at - self.consumed);
            let n = available.min(WRITE_BLOCK).min(until_gap);
            if n == 0 {
                break;
            }
            for (lane, buf) in self.scratch.iter_mut().enumerate() {
                // SAFETY: the writer is the ring's only consumer.
                unsafe { session.ring.read_lane(lane, 0, &mut buf[..n]) };
            }
            session.ring.commit_read(n);
            self.consumed += n;
            self.write_frames(s, n);
            did = true;
        }
        for (t, peaks) in self.new_peaks.iter_mut().enumerate() {
            if !peaks.is_empty() {
                lock(&session.peaks[t]).append(peaks);
            }
        }
        if self.last_header.elapsed() >= HEADER_INTERVAL {
            self.last_header = Instant::now();
            for t in 0..self.files.len() {
                if !self.failed[t] && self.files[t].update_header().is_err() {
                    self.fail(s, t);
                }
            }
        }
        did
    }

    /// The audio thread is done and everything is written: close the files
    /// and report the takes.
    fn finish(mut self, s: &Shared) {
        let session = self.session.clone();
        let end = session.end_frames.load(Relaxed);
        if self.written < end {
            self.write_silence(s, (end - self.written) as usize);
        }
        if end <= 0 || !session.started.load(Acquire) {
            // Never rolled: no take.
            for f in self.files.drain(..) {
                let path = f.path().to_owned();
                drop(f);
                let _ = std::fs::remove_file(path);
            }
            return;
        }
        let start = session.start_pos.load(Relaxed);
        let mut takes = Vec::new();
        for (t, f) in self.files.drain(..).enumerate() {
            let track = &session.tracks[t];
            match f.finalize() {
                Ok(frames) if !self.failed[t] => takes.push(Take {
                    track: track.track,
                    file: track.file.clone(),
                    start,
                    length: frames as i64,
                    num_channels: track.num_channels,
                }),
                Ok(_) => {}
                Err(_) => {
                    s.dropouts.fetch_add(1, Relaxed);
                }
            }
        }
        lock(&s.finished).extend(takes);
    }
}

pub(crate) fn run(s: Arc<Shared>) {
    let mut jobs: Vec<WriterJob> = Vec::new();
    loop {
        let kick = s.kick.load(Acquire);
        jobs.append(&mut lock(&s.new_sessions));
        let shutdown = s.shutdown.load(Acquire);
        let mut did = false;
        for job in jobs.iter_mut() {
            if shutdown && !job.session.ended.load(Acquire) {
                // The audio thread is gone: whatever it recorded is the take.
                job.session.end(job.session.frames.load(Acquire));
            }
            did |= job.drain(&s);
        }
        let mut i = 0;
        while i < jobs.len() {
            if jobs[i].session.ended.load(Acquire) && jobs[i].drained() {
                jobs.swap_remove(i).finish(&s);
                did = true;
            } else {
                i += 1;
            }
        }
        let busy = did || jobs.iter().any(|j| j.session.ended.load(Acquire));
        s.writer_busy.store(busy, Release);
        s.writer_kick.store(kick, Release);
        if shutdown && jobs.is_empty() && lock(&s.new_sessions).is_empty() {
            break;
        }
        if !did {
            std::thread::park_timeout(Duration::from_millis(5));
        }
    }
}
