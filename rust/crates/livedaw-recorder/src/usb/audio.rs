//! The audio-thread side of the USB deck. The engine calls, per chunk:
//!
//! 1. [`UsbDeck::begin_chunk`] - takes the commands, fills the four USB
//!    PLAYER buffers ([`UsbDeck::player_ptrs`]) and gets the recorder ready;
//! 2. [`UsbDeck::write_rec`] for each RECORDER port ([`UsbDeck::rec_tap`])
//!    once the strips have run;
//! 3. [`UsbDeck::end_chunk`] - commits the recorded frames and applies a
//!    pause, stop or seek at the end of the chunk (after its fade-out).

use super::*;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum PState {
    Stopped,
    /// The disk thread fills the ring for `epoch`; then Playing or Paused.
    Priming,
    Playing,
    Paused,
}

/// What happens at the end of a chunk the player fades out.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum PAfter {
    Nothing,
    Pause,
    Stop,
    Prime { entry: u64, pos: i64, play: bool },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum RState {
    Stopped,
    Recording,
    Paused,
}

pub(crate) struct DeckAudio {
    max_block: usize,
    // ---- player
    state: PState,
    want_play: bool,
    error: bool,
    epoch: u64,
    entry: u64,
    pos: i64,
    len: i64,
    after: PAfter,
    declick_in: usize,
    bufs: Vec<Vec<f32>>,
    ptrs: [*const f32; USB_CHANNELS],
    dirty: bool,
    // ---- recorder
    rstate: RState,
    session: *const RecSession,
    rec_frames: i64,
    rec_space: usize,
    chunk_recording: bool,
    /// An event the queue couldn't take yet (retried each chunk).
    held_event: Option<RecEvent>,
    play_processed: u64,
    rec_processed: u64,
}

impl DeckAudio {
    pub(crate) fn new(max_block: usize) -> Self {
        let mut a = Self {
            max_block,
            state: PState::Stopped,
            want_play: false,
            error: false,
            epoch: 0,
            entry: 0,
            pos: 0,
            len: 0,
            after: PAfter::Nothing,
            declick_in: 0,
            bufs: Vec::new(),
            ptrs: [std::ptr::null(); USB_CHANNELS],
            dirty: false,
            rstate: RState::Stopped,
            session: std::ptr::null(),
            rec_frames: 0,
            rec_space: 0,
            chunk_recording: false,
            held_event: None,
            play_processed: 0,
            rec_processed: 0,
        };
        a.allocate(max_block);
        a
    }

    fn allocate(&mut self, max_block: usize) {
        self.max_block = max_block;
        self.bufs = (0..USB_CHANNELS).map(|_| vec![0.0; max_block]).collect();
        for (p, b) in self.ptrs.iter_mut().zip(&self.bufs) {
            *p = b.as_ptr();
        }
        self.dirty = false;
    }

    pub(crate) fn prepare(&mut self, s: &DeckShared, max_block: usize) {
        self.allocate(max_block);
        // The ring's content was rendered for the old rate: start over.
        if self.state != PState::Stopped {
            self.stop_player(s, true);
        }
        if self.rstate != RState::Stopped {
            self.stop_recording();
        }
        self.publish_player(s);
        self.publish_rec(s);
    }

    pub(crate) fn shutdown(&mut self) {
        // SAFETY: a session stays alive until it is ended.
        if let Some(session) = unsafe { self.session.as_ref() } {
            session.end(session.ring.total_written());
        }
        self.session = std::ptr::null();
    }

    // ---- player ----------------------------------------------------------------------

    fn publish_player(&self, s: &DeckShared) {
        let state = match self.state {
            PState::Stopped if self.error => PlayerState::Error,
            PState::Stopped => PlayerState::Stopped,
            PState::Priming if self.want_play => PlayerState::Playing,
            PState::Priming | PState::Paused => PlayerState::Paused,
            PState::Playing => PlayerState::Playing,
        };
        s.play_entry.store(self.entry, Release);
        s.play_pos.store(self.pos, Release);
        s.play_len.store(self.len, Release);
        s.play_state.store(state as i32, Release);
        s.play_processed.store(self.play_processed, Release);
    }

    fn begin_prime(&mut self, s: &DeckShared, entry: u64, pos: i64, play: bool) {
        self.epoch += 1;
        self.state = PState::Priming;
        self.want_play = play;
        self.error = false;
        self.after = PAfter::Nothing;
        self.entry = entry;
        self.pos = pos.max(0);
        s.prime_entry.store(entry, Relaxed);
        s.prime_pos.store(self.pos, Relaxed);
        // Release: the reads of the old ring happen before the disk thread resets it.
        s.prime_epoch.store(self.epoch, Release);
    }

    /// Stops; `rewind` goes back to the start of the clip.
    fn stop_player(&mut self, s: &DeckShared, rewind: bool) {
        self.state = PState::Stopped;
        self.after = PAfter::Nothing;
        self.want_play = false;
        if rewind {
            self.pos = 0;
        }
        // The disk thread stops filling: a new epoch nobody primes.
        self.epoch += 1;
        s.prime_entry.store(0, Relaxed);
        s.prime_pos.store(-1, Relaxed);
        s.prime_epoch.store(self.epoch, Release);
    }

    fn drain_play_commands(&mut self, s: &DeckShared) {
        while let Some(cmd) = s.play_cmds.pop() {
            self.play_processed += 1;
            let playing = self.state == PState::Playing;
            match cmd {
                PlayCmd::None => {}
                PlayCmd::Play => match self.state {
                    PState::Stopped => self.begin_prime(s, self.entry, self.pos, true),
                    PState::Priming => self.want_play = true,
                    PState::Paused => {
                        self.state = PState::Playing;
                        self.declick_in = PLAY_DECLICK_SAMPLES;
                    }
                    PState::Playing => {
                        if self.after != PAfter::Nothing {
                            // A pause or stop not yet done: carry on instead.
                            if let PAfter::Prime { entry, pos, .. } = self.after {
                                self.after = PAfter::Prime { entry, pos, play: true };
                            } else {
                                self.after = PAfter::Nothing;
                            }
                        }
                    }
                },
                PlayCmd::Pause => match self.state {
                    PState::Playing => {
                        self.after = match self.after {
                            PAfter::Prime { entry, pos, .. } => PAfter::Prime { entry, pos, play: false },
                            PAfter::Stop => PAfter::Stop,
                            _ => PAfter::Pause,
                        }
                    }
                    PState::Priming => self.want_play = false,
                    _ => {}
                },
                PlayCmd::Stop => {
                    if playing {
                        self.after = PAfter::Stop;
                    } else {
                        self.stop_player(s, true);
                    }
                }
                PlayCmd::Load { entry, pos, play } => {
                    if playing {
                        self.after = PAfter::Prime { entry, pos, play };
                    } else {
                        self.begin_prime(s, entry, pos, play);
                    }
                }
                PlayCmd::Seek(pos) => match self.state {
                    PState::Stopped => {
                        self.pos = pos.max(0).min(if self.len > 0 { self.len } else { i64::MAX });
                        self.error = false;
                    }
                    PState::Playing => {
                        let play = !matches!(self.after, PAfter::Pause | PAfter::Stop);
                        self.after = PAfter::Prime { entry: self.entry, pos, play };
                    }
                    PState::Priming | PState::Paused => {
                        let play = self.state == PState::Priming && self.want_play;
                        self.begin_prime(s, self.entry, pos, play);
                    }
                },
            }
        }
    }

    fn activate_if_ready(&mut self, s: &DeckShared) {
        if self.state != PState::Priming || s.ready_epoch.load(Acquire) != self.epoch {
            return;
        }
        let status = s.ready_status.load(Relaxed);
        self.entry = s.ready_entry.load(Relaxed);
        self.len = s.ready_len.load(Relaxed);
        self.pos = s.ready_pos.load(Relaxed);
        match status {
            0 => {
                self.state = if self.want_play { PState::Playing } else { PState::Paused };
                self.declick_in = PLAY_DECLICK_SAMPLES;
            }
            _ => {
                // Nothing to play (1) or the clip can't be read (2).
                self.stop_player(s, false);
                self.error = status == 2;
            }
        }
    }

    fn clear_buffers(&mut self) {
        if self.dirty {
            for b in &mut self.bufs {
                b.fill(0.0);
            }
            self.dirty = false;
        }
    }

    /// Reads `n` frames of the ring into the player buffers, crossing clip
    /// boundaries; returns the clip to cue if the queue ended in this chunk.
    fn read_ring(&mut self, s: &DeckShared, ring: &Ring, n: usize) -> Option<u64> {
        let mut done = 0;
        let mut ended = None;
        while done < n {
            // Boundaries reached.
            let mut limit = usize::MAX;
            while let Some(b) = s.boundaries.peek() {
                if b.epoch != self.epoch {
                    s.boundaries.pop();
                    continue;
                }
                let read = ring.total_read();
                if b.at > read {
                    limit = b.at - read;
                    break;
                }
                s.boundaries.pop();
                if b.next == 0 {
                    ended = Some(b.cue);
                    break;
                }
                self.entry = b.next;
                self.len = b.next_len;
                self.pos = 0;
            }
            if ended.is_some() {
                break;
            }
            let k = (n - done).min(ring.available()).min(limit);
            if k == 0 {
                // Underrun: silence (the position waits for the disk).
                s.dropouts.fetch_add(1, Relaxed);
                break;
            }
            for (lane, b) in self.bufs.iter_mut().enumerate() {
                // SAFETY: the audio thread is the ring's consumer while Playing.
                unsafe { ring.read_lane(lane, 0, &mut b[done..done + k]) };
            }
            ring.commit_read(k);
            self.pos += k as i64;
            done += k;
        }
        for b in &mut self.bufs {
            b[done..n].fill(0.0);
        }
        ended
    }

    fn fill_player(&mut self, s: &DeckShared, n: usize) {
        self.clear_buffers();
        // Stale boundaries of earlier epochs.
        while s.boundaries.peek().is_some_and(|b| b.epoch < self.epoch) {
            s.boundaries.pop();
        }
        if self.state != PState::Playing {
            return;
        }
        let Some(ring) = s.ring.get() else { return };
        let ended = self.read_ring(s, ring, n);
        self.dirty = true;

        // Declick: fade in after start / resume, out before a pause, stop or seek.
        let fade_out = self.after != PAfter::Nothing && ended.is_none();
        if self.declick_in > 0 || fade_out {
            let into = PLAY_DECLICK_SAMPLES - self.declick_in;
            let out_len = n.min(PLAY_DECLICK_SAMPLES) as f32;
            let fading_in = self.declick_in > 0;
            for b in &mut self.bufs {
                for (i, x) in b[..n].iter_mut().enumerate() {
                    let mut g = 1.0f32;
                    if fading_in {
                        g *= ((into + i) as f32 / PLAY_DECLICK_SAMPLES as f32).min(1.0);
                    }
                    if fade_out {
                        g *= ((n - 1 - i) as f32 / out_len).min(1.0);
                    }
                    *x *= g;
                }
            }
            self.declick_in = self.declick_in.saturating_sub(n);
        }
        for (b, l) in self.bufs.iter().zip(&s.play_levels) {
            l.fetch_max(b[..n].iter().fold(0.0f32, |p, v| p.max(v.abs())));
        }
        if let Some(cue) = ended {
            // The queue ended (the clip's own end needs no fade).
            let len = self.len;
            self.stop_player(s, true);
            if cue != 0 && cue != self.entry {
                self.entry = cue;
                self.len = 0;
            } else {
                self.len = len;
            }
        }
    }

    fn end_player_chunk(&mut self, s: &DeckShared) {
        match std::mem::replace(&mut self.after, PAfter::Nothing) {
            PAfter::Nothing => {}
            PAfter::Pause => self.state = PState::Paused,
            PAfter::Stop => self.stop_player(s, true),
            PAfter::Prime { entry, pos, play } => self.begin_prime(s, entry, pos, play),
        }
        self.publish_player(s);
    }

    // ---- recorder ----------------------------------------------------------------------

    fn publish_rec(&self, s: &DeckShared) {
        let state = match self.rstate {
            RState::Stopped => RecState::Stopped,
            RState::Recording => RecState::Recording,
            RState::Paused => RecState::Paused,
        };
        s.rec_frames.store(self.rec_frames, Release);
        s.rec_state.store(state as i32, Release);
        s.rec_processed.store(self.rec_processed, Release);
    }

    fn stop_recording(&mut self) {
        // SAFETY: alive until ended.
        if let Some(session) = unsafe { self.session.as_ref() } {
            session.end(session.ring.total_written());
        }
        self.session = std::ptr::null();
        self.held_event = None;
        self.rstate = RState::Stopped;
    }

    fn push_event(&mut self, ev: RecEvent) {
        // SAFETY: alive until ended; the audio thread is the event producer.
        let Some(session) = (unsafe { self.session.as_ref() }) else { return };
        if let Some(held) = self.held_event {
            if session.events.push(held) {
                self.held_event = None;
            }
        }
        if (self.held_event.is_some() || !session.events.push(ev))
            && (self.held_event.is_none() || ev.kind == RecEventKind::NewFile)
        {
            // Full (the disk thread is far behind): keep it for the next
            // chunk (a NEW FILE wins over a gap).
            self.held_event = Some(ev);
        }
    }

    fn drain_rec_commands(&mut self, s: &DeckShared) {
        while let Some(cmd) = s.rec_cmds.pop() {
            self.rec_processed += 1;
            match cmd {
                RecCmd::None => {}
                RecCmd::Rec => match self.rstate {
                    RState::Paused => self.rstate = RState::Recording,
                    RState::Recording => {}
                    RState::Stopped => {
                        let new = s.pending_session.swap(std::ptr::null_mut(), AcqRel);
                        if !new.is_null() {
                            self.session = new;
                            self.rstate = RState::Recording;
                            self.rec_frames = 0;
                            s.rec_file_start.store(0, Release);
                        }
                    }
                },
                RecCmd::Pause => {
                    if self.rstate == RState::Recording {
                        self.rstate = RState::Paused;
                    }
                }
                RecCmd::Stop => {
                    if self.rstate != RState::Stopped {
                        self.stop_recording();
                    }
                }
                RecCmd::NewFile => {
                    // SAFETY: alive until ended.
                    if let Some(session) = unsafe { self.session.as_ref() } {
                        let at = session.ring.total_written();
                        self.push_event(RecEvent { at, kind: RecEventKind::NewFile });
                        s.rec_file_start.store(self.rec_frames, Release);
                    }
                }
            }
        }
    }

    fn begin_rec_chunk(&mut self, s: &DeckShared) {
        self.drain_rec_commands(s);
        if let Some(held) = self.held_event {
            self.held_event = None;
            self.push_event(held);
        }
        self.chunk_recording = false;
        if self.rstate == RState::Recording {
            // SAFETY: alive until ended; the audio thread is the producer.
            if let Some(session) = unsafe { self.session.as_ref() } {
                self.rec_space = session.ring.write_space();
                self.chunk_recording = true;
            }
        }
    }

    pub(crate) fn write_rec(&self, s: &DeckShared, ch: usize, src: Option<&[f32]>, n: usize) {
        if let (Some(level), Some(x)) = (s.rec_levels.get(ch), src) {
            level.fetch_max(x[..n.min(x.len())].iter().fold(0.0f32, |p, v| p.max(v.abs())));
        }
        if !self.chunk_recording {
            return;
        }
        // SAFETY: alive until ended; the audio thread is the producer.
        let session = unsafe { &*self.session };
        if ch >= session.channels {
            return;
        }
        let m = n.min(self.rec_space);
        match src {
            Some(x) => unsafe { session.ring.write_lane(ch, 0, &x[..m.min(x.len())]) },
            None => unsafe { session.ring.zero_lane(ch, 0, m) },
        }
    }

    fn end_rec_chunk(&mut self, s: &DeckShared, n: usize) {
        if self.chunk_recording {
            // SAFETY: alive until ended.
            let session = unsafe { &*self.session };
            let m = n.min(self.rec_space);
            session.ring.commit_write(m);
            if m < n {
                self.push_event(RecEvent { at: session.ring.total_written(), kind: RecEventKind::Gap(n - m) });
                s.dropouts.fetch_add(1, Relaxed);
            }
            self.rec_frames += n as i64;
        }
        self.chunk_recording = false;
        self.publish_rec(s);
    }

    // ---- per chunk -----------------------------------------------------------------------

    pub(crate) fn begin_chunk(&mut self, s: &DeckShared, n: usize) {
        let n = n.clamp(1, self.max_block);
        self.drain_play_commands(s);
        self.activate_if_ready(s);
        self.fill_player(s, n);
        self.begin_rec_chunk(s);
    }

    pub(crate) fn end_chunk(&mut self, s: &DeckShared, n: usize) {
        let n = n.clamp(1, self.max_block);
        self.end_player_chunk(s);
        self.end_rec_chunk(s, n);
    }
}

impl UsbDeck {
    /// Audio thread, at the start of each chunk (after the multitrack
    /// recorder's, so `n` is the chunk's final length).
    ///
    /// # Safety
    /// Audio thread only, never concurrently with itself or `prepare`.
    pub unsafe fn begin_chunk(&self, n: usize) {
        (*self.audio.get()).begin_chunk(&self.shared, n)
    }

    /// The four USB PLAYER buffers (valid for the current chunk).
    ///
    /// # Safety
    /// Audio thread only.
    pub unsafe fn player_ptrs(&self) -> &[*const f32; USB_CHANNELS] {
        &(*self.audio.get()).ptrs
    }

    /// What RECORDER port `port` records (read once per chunk by the engine).
    pub fn rec_tap(&self, port: usize) -> RecTap {
        self.tap(port)
    }

    /// Records the chunk's `n` samples of RECORDER port `ch` (None: silence)
    /// and meters it.
    ///
    /// # Safety
    /// Audio thread only, between `begin_chunk` and `end_chunk`.
    pub unsafe fn write_rec(&self, ch: usize, src: Option<&[f32]>, n: usize) {
        (*self.audio.get()).write_rec(&self.shared, ch, src, n)
    }

    /// Audio thread, at the end of each chunk.
    ///
    /// # Safety
    /// Audio thread only, after `begin_chunk(n)`.
    pub unsafe fn end_chunk(&self, n: usize) {
        (*self.audio.get()).end_chunk(&self.shared, n)
    }
}
