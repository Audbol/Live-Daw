//! The USB deck's disk thread: plays the queue into the playback ring and
//! writes the record ring to the files.
//!
//! Who may touch the playback ring: when the audio thread starts a new epoch
//! (play from stop, a clip loaded, a seek, a stop) it stops reading the ring
//! until the disk thread publishes `ready(epoch)`. The disk thread then
//! resets the ring, primes it from the position and hands it over; while
//! the audio thread plays it, the disk thread keeps it full, and pushes a
//! [`Boundary`] where a clip ends and the next begins (or the queue ends).

use super::resample::FileStream;
use super::*;
use crate::codec::open_reader;
use std::time::Instant;

const RENDER_BLOCK: usize = 4096;
const WRITE_BLOCK: usize = 8192;
const MAX_INFOS: usize = 16;

/// The clip being rendered into the playback ring.
struct Stream {
    entry: u64,
    file: FileStream,
    /// Next output frame of the clip to render.
    next: u64,
}

struct Player {
    epoch: u64,
    stream: Option<Stream>,
    scratch: Vec<Vec<f32>>,
}

fn info_of(path: &Path, file: &FileStream, rate: u32) -> PlayFileInfo {
    let i = file.info();
    PlayFileInfo {
        path: path.to_owned(),
        format: i.format,
        kbps: i.kbps,
        channels: i.channels,
        sample_rate: i.sample_rate,
        bits: i.bits,
        float: i.float,
        length: file.length() as i64,
        rate_mismatch: i.sample_rate != rate,
    }
}

fn remember(s: &DeckShared, id: u64, info: PlayFileInfo) {
    let mut infos = lock(&s.infos);
    infos.retain(|(i, _)| *i != id);
    infos.push_back((id, info));
    while infos.len() > MAX_INFOS {
        infos.pop_front();
    }
}

/// Opens clip `id` of the queue for the engine's rate.
fn open(s: &DeckShared, id: u64) -> Result<Stream, ()> {
    let path = s.queue().entry(id).map(|e| e.path.clone()).ok_or(())?;
    let rate = s.sample_rate().round() as u32;
    let file = FileStream::new(open_reader(&path).map_err(|_| ())?, rate);
    remember(s, id, info_of(&path, &file, rate));
    Ok(Stream { entry: id, file, next: 0 })
}

impl Player {
    fn new() -> Self {
        Self { epoch: 0, stream: None, scratch: vec![vec![0.0; RENDER_BLOCK]; USB_CHANNELS] }
    }

    fn ready(&self, s: &DeckShared, status: i32, entry: u64, len: i64, pos: i64) {
        s.ready_status.store(status, Relaxed);
        s.ready_entry.store(entry, Relaxed);
        s.ready_len.store(len, Relaxed);
        s.ready_pos.store(pos, Relaxed);
        s.ready_epoch.store(self.epoch, Release);
    }

    /// A new epoch: reset the ring and prime it (or just stop).
    fn prime(&mut self, s: &DeckShared, ring: &Ring) {
        self.stream = None;
        // SAFETY: the audio thread doesn't read the ring until ready(epoch).
        unsafe { ring.reset() };
        let pos = s.prime_pos.load(Relaxed);
        if pos < 0 {
            return; // stopped
        }
        let mut id = s.prime_entry.load(Relaxed);
        if s.queue().entry(id).is_none() {
            id = s.queue().first().unwrap_or(0);
        }
        if id == 0 {
            self.ready(s, 1, 0, 0, 0);
            return;
        }
        match open(s, id) {
            Ok(mut stream) => {
                let len = stream.file.length() as i64;
                let pos = pos.min(len);
                stream.next = pos as u64;
                self.stream = Some(stream);
                let prime = ((PLAY_PRIME_SECONDS * s.sample_rate()) as usize).min(ring.capacity());
                self.fill(s, ring, prime);
                self.ready(s, 0, id, len, pos);
            }
            Err(()) => self.ready(s, 2, id, 0, 0),
        }
    }

    /// Renders up to `max` frames into the ring, crossing into the next clip
    /// when the current one ends. Returns the frames rendered.
    fn fill(&mut self, s: &DeckShared, ring: &Ring, max: usize) -> usize {
        let mut done = 0;
        let decide_ahead = (DECIDE_AHEAD_SECONDS * s.sample_rate()) as usize;
        while done < max {
            let Some(stream) = self.stream.as_mut() else { break };
            let space = ring.write_space();
            if space == 0 {
                break;
            }
            let left = stream.file.length().saturating_sub(stream.next) as usize;
            if left == 0 {
                // The clip ends: decide what follows once it is nearly played.
                if ring.fill() > decide_ahead {
                    break;
                }
                let after = s.queue().after(stream.entry);
                let at = ring.total_written();
                let boundary = match after {
                    After::Play(next) => match open(s, next) {
                        Ok(next_stream) => {
                            let b = Boundary { epoch: self.epoch, at, next, next_len: next_stream.file.length() as i64, cue: 0 };
                            if !s.boundaries.push(b) {
                                break; // full: retry later
                            }
                            self.stream = Some(next_stream);
                            continue;
                        }
                        // Unreadable: stop there, cueing it (PLAY then reports the error).
                        Err(()) => Boundary { epoch: self.epoch, at, next: 0, next_len: 0, cue: next },
                    },
                    After::Stop(cue) => Boundary { epoch: self.epoch, at, next: 0, next_len: 0, cue: cue.unwrap_or(0) },
                };
                if s.boundaries.push(boundary) {
                    self.stream = None;
                }
                break;
            }
            let n = space.min(left).min(RENDER_BLOCK).min(max - done);
            let channels = stream.file.channels();
            if self.scratch.len() < channels {
                self.scratch.resize(channels, vec![0.0; RENDER_BLOCK]);
            }
            if stream.file.render(stream.next, n, &mut self.scratch[..channels]).is_err() {
                for c in &mut self.scratch[..channels] {
                    c[..n].fill(0.0);
                }
                s.dropouts.fetch_add(1, Relaxed);
            }
            // Mono plays on USB PLAYER 1 and 2; more channels in order (up to 4).
            for lane in 0..USB_CHANNELS {
                let src = if channels == 1 && lane < 2 { Some(0) } else { (lane < channels).then_some(lane) };
                // SAFETY: the disk thread is the ring's producer.
                unsafe {
                    match src {
                        Some(c) => ring.write_lane(lane, 0, &self.scratch[c][..n]),
                        None => ring.zero_lane(lane, 0, n),
                    }
                }
            }
            ring.commit_write(n);
            stream.next += n as u64;
            done += n;
        }
        done
    }

    /// Returns whether it did anything.
    fn step(&mut self, s: &DeckShared) -> bool {
        let Some(ring) = s.ring.get() else { return false };
        let epoch = s.prime_epoch.load(Acquire);
        if epoch != self.epoch {
            self.epoch = epoch;
            self.prime(s, ring);
            return true;
        }
        self.stream.is_some() && self.fill(s, ring, usize::MAX) > 0
    }
}

// ---- recorder ---------------------------------------------------------------------------

struct Job {
    session: Arc<RecSession>,
    file: Option<Box<dyn AudioWriter>>,
    consumed: usize,
    last_header: Instant,
    scratch: Vec<Vec<f32>>,
}

impl Job {
    fn new(session: Arc<RecSession>) -> Self {
        let file = lock(&session.files).pop_front();
        let channels = session.channels;
        Self { session, file, consumed: 0, last_header: Instant::now(), scratch: vec![vec![0.0; WRITE_BLOCK]; channels] }
    }

    fn fail(&self, s: &DeckShared) {
        if !self.session.error.swap(true, Relaxed) {
            s.dropouts.fetch_add(1, Relaxed);
        }
    }

    fn finish_file(&mut self, s: &DeckShared) {
        let Some(f) = self.file.take() else { return };
        if f.frames() == 0 {
            // Nothing was recorded into it (STOP before the first sample).
            let path = f.path().to_owned();
            drop(f);
            let _ = std::fs::remove_file(path);
        } else if f.finalize().is_err() {
            self.fail(s);
        }
    }

    fn write_silence(&mut self, s: &DeckShared, n: usize) {
        if let Some(f) = self.file.as_mut() {
            if f.write_silence(n).is_err() {
                self.fail(s);
            }
        }
    }

    /// Writes what the ring holds, handling NEW FILE and gaps where they
    /// fall. Returns whether anything was done.
    fn drain(&mut self, s: &DeckShared) -> bool {
        let session = self.session.clone();
        let mut did = false;
        loop {
            let available = session.ring.available();
            while let Some(ev) = session.events.peek().filter(|e| e.at <= self.consumed) {
                session.events.pop();
                did = true;
                match ev.kind {
                    RecEventKind::NewFile => {
                        self.finish_file(s);
                        self.file = lock(&session.files).pop_front();
                        self.last_header = Instant::now();
                    }
                    RecEventKind::Gap(n) => self.write_silence(s, n),
                }
            }
            let until_event = session.events.peek().map_or(usize::MAX, |e| e.at - self.consumed);
            let n = available.min(WRITE_BLOCK).min(until_event);
            if n == 0 {
                break;
            }
            for (lane, buf) in self.scratch.iter_mut().enumerate() {
                // SAFETY: the disk thread is the ring's only consumer.
                unsafe { session.ring.read_lane(lane, 0, &mut buf[..n]) };
            }
            session.ring.commit_read(n);
            self.consumed += n;
            if let Some(f) = self.file.as_mut() {
                let channels: Vec<&[f32]> = self.scratch.iter().map(|c| &c[..n]).collect();
                if f.write_planar(&channels, n).is_err() {
                    self.fail(s);
                }
            }
            did = true;
        }
        if self.last_header.elapsed() >= crate::HEADER_INTERVAL {
            self.last_header = Instant::now();
            if let Some(f) = self.file.as_mut() {
                if f.update_header().is_err() {
                    self.fail(s);
                }
            }
        }
        did
    }

    fn done(&self) -> bool {
        self.session.ended.load(Acquire)
            && self.session.ring.available() == 0
            && self.consumed >= self.session.end_at.load(Relaxed)
            && self.session.events.peek().is_none_or(|e| e.at > self.consumed)
    }

    fn finish(mut self, s: &DeckShared) {
        // Events at the very end (a NEW FILE then STOP): the files stay valid.
        while let Some(ev) = self.session.events.pop() {
            if ev.kind == RecEventKind::NewFile {
                self.finish_file(s);
                self.file = lock(&self.session.files).pop_front();
            }
        }
        self.finish_file(s);
        // Files never reached (a NEW FILE the audio thread didn't take).
        for f in lock(&self.session.files).drain(..) {
            let path = f.path().to_owned();
            if f.frames() == 0 {
                drop(f);
                let _ = std::fs::remove_file(path);
            } else {
                let _ = f.finalize();
            }
        }
    }
}

pub(crate) fn run(s: Arc<DeckShared>) {
    let mut player = Player::new();
    let mut jobs: Vec<Job> = Vec::new();
    loop {
        let kick = s.kick.load(Acquire);
        let shutdown = s.shutdown.load(Acquire);
        for session in lock(&s.new_sessions).drain(..) {
            jobs.push(Job::new(session));
        }
        let mut did = player.step(&s);
        for job in jobs.iter_mut() {
            if shutdown && !job.session.ended.load(Acquire) {
                job.session.end(job.session.ring.total_written());
            }
            did |= job.drain(&s);
        }
        let mut i = 0;
        while i < jobs.len() {
            if jobs[i].done() {
                jobs.swap_remove(i).finish(&s);
                did = true;
            } else {
                i += 1;
            }
        }
        s.disk_busy.store(did, Release);
        s.disk_kick.store(kick, Release);
        if shutdown && jobs.is_empty() && lock(&s.new_sessions).is_empty() {
            break;
        }
        if !did {
            let active = player.stream.is_some() || !jobs.is_empty();
            std::thread::park_timeout(Duration::from_millis(if active { 2 } else { 20 }));
        }
    }
}
