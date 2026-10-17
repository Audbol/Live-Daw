//! The audio-thread side: the transport, the playback buffers and the record
//! capture. The engine calls, per chunk of a callback:
//!
//! 1. [`Recorder::begin_chunk`] - takes the commands of the message thread,
//!    starts rolling once the reader has primed, may shorten the chunk (at the
//!    loop end and at a pending ring switch, so every chunk is contiguous on
//!    the timeline) and fills the playback buffers for it;
//! 2. [`Recorder::write_record_stream`] for each of [`Recorder::record_streams`]
//!    (hardware inputs at the start of the chunk, strip taps after the strips);
//! 3. [`Recorder::end_chunk`] - commits the recorded frames, advances the
//!    position and applies a stop or locate at the end of the chunk.
//!
//! Sample accuracy: playback sample `i` of a chunk is timeline position
//! `p + i`, and recorded sample `k` of a take is `start + k`.

use crate::*;
use std::sync::atomic::fence;

#[derive(Clone, Copy, Debug, PartialEq)]
enum After {
    Nothing,
    /// Stop at the end of this chunk (optionally moving to `pos`).
    Stop {
        pos: Option<i64>,
    },
    /// Re-prime at the end of this chunk from `pos` (None: where the chunk ends).
    Prime {
        pos: Option<i64>,
        record: bool,
    },
}

impl After {
    fn pos(self) -> Option<i64> {
        match self {
            Self::Nothing => None,
            Self::Stop { pos } | Self::Prime { pos, .. } => pos,
        }
    }
}

#[derive(Clone, Copy, Debug)]
struct Pending {
    slot: usize,
    seq: i64,
    token: u64,
}

pub(crate) struct AudioSide {
    max_block: usize,
    state: TransportState,
    position: i64,
    epoch: u64,
    record_on_roll: bool,
    /// Transport frames since rolling began (the reader's alignment unit).
    seq: i64,
    active: Option<usize>,
    loop_range: LoopRange,
    /// Frames the active ring still owes after an underrun (skipped when they arrive).
    deficit: usize,
    fade_from: Option<usize>,
    fade_done: usize,
    pending: Option<Pending>,
    last_token: u64,
    declick_in: usize,
    after: After,
    processed: u64,
    session: *const RecordSession,
    armed: *const RecordSession,
    rec_frames: i64,
    rec_space: usize,
    chunk_rolling: bool,
    chunk_recording: bool,
    playback: Vec<Vec<f32>>,
    ptrs: [*const f32; NUM_PLAYBACK_CHANNELS],
    dirty: u64,
}

impl AudioSide {
    pub(crate) fn new(_sample_rate: f64, max_block: usize) -> Self {
        let mut a = Self {
            max_block,
            state: TransportState::Stopped,
            position: 0,
            epoch: 0,
            record_on_roll: false,
            seq: 0,
            active: None,
            loop_range: LoopRange::default(),
            deficit: 0,
            fade_from: None,
            fade_done: 0,
            pending: None,
            last_token: 0,
            declick_in: 0,
            after: After::Nothing,
            processed: 0,
            session: std::ptr::null(),
            armed: std::ptr::null(),
            rec_frames: 0,
            rec_space: 0,
            chunk_rolling: false,
            chunk_recording: false,
            playback: Vec::new(),
            ptrs: [std::ptr::null(); NUM_PLAYBACK_CHANNELS],
            dirty: 0,
        };
        a.allocate(max_block);
        a
    }

    fn allocate(&mut self, max_block: usize) {
        self.max_block = max_block;
        self.playback = (0..NUM_PLAYBACK_CHANNELS).map(|_| vec![0.0; max_block]).collect();
        for (p, b) in self.ptrs.iter_mut().zip(&self.playback) {
            *p = b.as_ptr();
        }
        self.dirty = 0;
    }

    pub(crate) fn prepare(&mut self, s: &Shared, _sample_rate: f64, max_block: usize) {
        self.allocate(max_block);
        if self.state != TransportState::Stopped
            || self.after != After::Nothing
            || !self.session.is_null()
            || !self.armed.is_null()
        {
            self.stop_now(s);
        }
    }

    pub(crate) fn shutdown(&mut self) {
        self.end_recording();
        self.end_armed();
    }

    // ---- state changes ---------------------------------------------------------

    fn publish(&self, s: &Shared) {
        s.position.store(self.position, Release);
        s.seq.store(self.seq, Release);
        s.state.store(self.state as i32, Release);
        s.commands_processed.store(self.processed, Release);
    }

    fn end_recording(&mut self) {
        // SAFETY: the session stays alive until it is ended (the writer waits).
        if let Some(session) = unsafe { self.session.as_ref() } {
            session.end(self.rec_frames);
        }
        self.session = std::ptr::null();
        self.rec_frames = 0;
    }

    fn end_armed(&mut self) {
        // SAFETY: as above.
        if let Some(session) = unsafe { self.armed.as_ref() } {
            session.end(0);
        }
        self.armed = std::ptr::null();
    }

    /// The audio thread lets go of every ring.
    fn release_slots(&mut self, s: &Shared) {
        self.active = None;
        self.fade_from = None;
        self.pending = None;
        self.deficit = 0;
        for used in &s.slot_in_use {
            used.store(false, Release);
        }
        s.playback_buffered.store(0, Relaxed);
    }

    fn stop_now(&mut self, s: &Shared) {
        self.end_recording();
        self.end_armed();
        self.release_slots(s);
        self.after = After::Nothing;
        self.state = TransportState::Stopped;
        self.seq = 0;
        self.epoch += 1;
        s.epoch.store(self.epoch, Release);
        self.publish(s);
    }

    /// Starting: the reader fills a ring from `pos`, then rolling begins.
    fn begin_prime(&mut self, s: &Shared, pos: i64, record: bool) {
        self.end_recording();
        if !record {
            self.end_armed();
        }
        self.release_slots(s);
        self.after = After::Nothing;
        self.state = TransportState::Starting;
        self.position = pos.max(0);
        self.seq = 0;
        self.record_on_roll = record;
        self.epoch += 1;
        s.prime_pos.store(self.position, Relaxed);
        s.prime_record.store(record, Relaxed);
        s.prime_epoch.store(self.epoch, Release);
        s.epoch.store(self.epoch, Release);
        self.publish(s);
    }

    fn drain_commands(&mut self, s: &Shared) {
        while let Some(cmd) = s.commands.pop() {
            self.processed += 1;
            let rolling = self.state.is_rolling();
            match cmd {
                Command::None => {}
                Command::Play => match self.after {
                    _ if self.state == TransportState::Stopped => self.begin_prime(s, self.position, false),
                    After::Stop { pos } if rolling => self.after = After::Prime { pos, record: false },
                    _ => {}
                },
                Command::Stop => match self.state {
                    TransportState::Stopped => {}
                    TransportState::Starting => self.stop_now(s),
                    _ => self.after = After::Stop { pos: self.after.pos() },
                },
                Command::Locate(p) => match self.state {
                    TransportState::Stopped => self.position = p.max(0),
                    TransportState::Starting => self.begin_prime(s, p, self.record_on_roll),
                    _ => {
                        self.after = match self.after {
                            After::Stop { .. } => After::Stop { pos: Some(p) },
                            After::Prime { record, .. } => After::Prime { pos: Some(p), record },
                            // A locate ends a recording; playback goes on from there.
                            After::Nothing => After::Prime { pos: Some(p), record: false },
                        }
                    }
                },
                Command::Record => {
                    let new = s.pending_session.swap(std::ptr::null_mut(), AcqRel);
                    // SAFETY: alive until ended.
                    let Some(session) = (unsafe { new.as_ref() }) else { continue };
                    if self.state == TransportState::Recording && self.after == After::Nothing {
                        session.end(0); // already recording
                        continue;
                    }
                    self.end_armed();
                    self.armed = new;
                    match self.state {
                        TransportState::Stopped | TransportState::Starting => self.begin_prime(s, self.position, true),
                        _ => self.after = After::Prime { pos: self.after.pos(), record: true },
                    }
                }
            }
        }
    }

    fn activate_if_ready(&mut self, s: &Shared) {
        if self.state != TransportState::Starting || s.ready_epoch.load(Acquire) != self.epoch {
            return;
        }
        let slot = s.ready_slot.load(Relaxed).min(1);
        s.slot_in_use[slot].store(true, Relaxed);
        self.active = Some(slot);
        // SAFETY: the reader handed the slot over with ready_epoch.
        self.loop_range = unsafe { s.slots[slot].get() }.loop_range;
        self.deficit = 0;
        self.seq = 0;
        self.declick_in = DECLICK_SAMPLES;
        self.state = TransportState::Playing;
        if self.record_on_roll && !self.armed.is_null() {
            self.session = std::mem::replace(&mut self.armed, std::ptr::null());
            self.rec_frames = 0;
            // SAFETY: alive until ended.
            let session = unsafe { &*self.session };
            session.start_pos.store(self.position, Relaxed);
            session.started.store(true, Release);
            self.state = TransportState::Recording;
        }
    }

    /// Picks up a switch request the reader published (a seqlock).
    fn poll_pending(&mut self, s: &Shared) {
        let t1 = s.pend_token.load(Acquire);
        if t1 & 1 == 1 || t1 == self.last_token {
            return;
        }
        let slot = s.pend_slot.load(Relaxed);
        let seq = s.pend_seq.load(Relaxed);
        let epoch = s.pend_epoch.load(Relaxed);
        fence(Acquire);
        if s.pend_token.load(Relaxed) != t1 {
            return;
        }
        self.last_token = t1;
        if epoch == self.epoch && self.state.is_rolling() && slot < 2 {
            self.pending = Some(Pending { slot, seq, token: t1 });
        }
    }

    /// Switches rings at the requested transport frame (shortening the chunk
    /// to land on it). Returns the chunk length.
    fn handle_switch(&mut self, s: &Shared, n: usize) -> usize {
        let Some(p) = self.pending else { return n };
        if self.seq < p.seq {
            return n.min((p.seq - self.seq) as usize);
        }
        self.pending = None;
        // SAFETY: the reader handed the slot over with the request.
        let target = unsafe { s.slots[p.slot].get() };
        if self.seq > p.seq || target.ring.available() < n {
            s.switch_missed.store(p.token, Release);
            return n;
        }
        if let Some(old) = self.fade_from.take() {
            s.slot_in_use[old].store(false, Release);
        }
        s.slot_in_use[p.slot].store(true, Relaxed);
        self.fade_from = self.active;
        self.fade_done = 0;
        self.active = Some(p.slot);
        self.loop_range = target.loop_range;
        self.deficit = 0;
        s.switch_done.store(p.token, Release);
        n
    }

    fn clear_playback(&mut self) {
        let mut dirty = self.dirty;
        while dirty != 0 {
            let ch = dirty.trailing_zeros() as usize;
            self.playback[ch].fill(0.0);
            dirty &= dirty - 1;
        }
        self.dirty = 0;
    }

    fn fill_playback(&mut self, s: &Shared, n: usize) {
        self.clear_playback();
        let Some(slot) = self.active else { return };
        // SAFETY: the audio thread owns the active slot's consumer side.
        let d = unsafe { s.slots[slot].get() };
        let ring = &d.ring;
        if self.deficit > 0 {
            let k = self.deficit.min(ring.available());
            ring.commit_read(k);
            self.deficit -= k;
        }
        let got = ring.available().min(n);
        for (lane, &ch) in d.lane_channel.iter().enumerate() {
            // SAFETY: consumer side, within the available frames (zero-filled beyond).
            unsafe { ring.read_lane(lane, 0, &mut self.playback[ch][..n]) };
            self.dirty |= 1 << ch;
        }

        // Crossfade from the previous content after a playlist / loop change.
        if let Some(old) = self.fade_from {
            // SAFETY: still in use (slot_in_use) until released below.
            let od = unsafe { s.slots[old].get() };
            let done = self.fade_done;
            let t = |i: usize| ((done + i) as f32 / CROSSFADE_SAMPLES as f32).min(1.0);
            for &ch in &d.lane_channel {
                for (i, x) in self.playback[ch][..n].iter_mut().enumerate() {
                    *x *= t(i);
                }
            }
            for (lane, &ch) in od.lane_channel.iter().enumerate() {
                // SAFETY: consumer side of the old ring.
                unsafe { od.ring.add_lane(lane, 0, &mut self.playback[ch][..n], |i| 1.0 - t(i)) };
                self.dirty |= 1 << ch;
            }
            od.ring.commit_read(n);
            self.fade_done += n;
            if self.fade_done >= CROSSFADE_SAMPLES {
                s.slot_in_use[old].store(false, Release);
                self.fade_from = None;
            }
        }

        // Declick: fade in when rolling starts, out before a stop or locate.
        let fade_out = self.after != After::Nothing;
        if self.declick_in > 0 || fade_out {
            let into = DECLICK_SAMPLES - self.declick_in;
            let out_len = n.min(DECLICK_SAMPLES) as f32;
            let fading_in = self.declick_in > 0;
            let gain = |i: usize| {
                let mut g = 1.0f32;
                if fading_in {
                    g *= ((into + i) as f32 / DECLICK_SAMPLES as f32).min(1.0);
                }
                if fade_out {
                    g *= ((n - 1 - i) as f32 / out_len).min(1.0);
                }
                g
            };
            let mut dirty = self.dirty;
            while dirty != 0 {
                let ch = dirty.trailing_zeros() as usize;
                for (i, x) in self.playback[ch][..n].iter_mut().enumerate() {
                    *x *= gain(i);
                }
                dirty &= dirty - 1;
            }
            self.declick_in = self.declick_in.saturating_sub(n);
        }

        ring.commit_read(got);
        if got < n {
            self.deficit += n - got;
            s.dropouts.fetch_add(1, Relaxed);
        }
        s.playback_buffered.store(ring.available(), Relaxed);
    }

    pub(crate) fn begin_chunk(&mut self, s: &Shared, n: usize) -> usize {
        let mut n = n.clamp(1, self.max_block);
        self.drain_commands(s);
        self.activate_if_ready(s);
        self.poll_pending(s);
        self.chunk_rolling = self.state.is_rolling();
        self.chunk_recording = false;
        if !self.chunk_rolling {
            self.clear_playback();
            s.disk_load.store(0.0);
            return n;
        }
        if self.after == After::Nothing {
            n = self.handle_switch(s, n);
        }
        n = self.loop_range.max_run(self.position, n);
        self.fill_playback(s, n);
        if self.state == TransportState::Recording {
            // SAFETY: alive until ended; the audio thread is the producer.
            if let Some(session) = unsafe { self.session.as_ref() } {
                self.rec_space = session.ring.write_space();
                self.chunk_recording = true;
            }
        }
        n
    }

    pub(crate) fn record_streams(&self) -> &[RecordStream] {
        match unsafe { self.session.as_ref() } {
            // SAFETY: alive until ended.
            Some(session) if self.chunk_recording => &session.streams,
            _ => &[],
        }
    }

    pub(crate) fn write_record_stream(&self, stream: usize, src: Option<&[f32]>, n: usize) {
        if !self.chunk_recording {
            return;
        }
        // SAFETY: alive until ended; the audio thread is the producer.
        let session = unsafe { &*self.session };
        let m = n.min(self.rec_space);
        match src {
            Some(x) => {
                let n = n.min(x.len());
                unsafe { session.ring.write_lane(stream, 0, &x[..m.min(n)]) };
                if let Some(level) = session.levels.get(stream) {
                    level.fetch_max(x[..n].iter().fold(0.0f32, |p, v| p.max(v.abs())));
                }
            }
            None => unsafe { session.ring.zero_lane(stream, 0, m) },
        }
    }

    pub(crate) fn end_chunk(&mut self, s: &Shared, n: usize) {
        let mut record_fill = 0.0f32;
        if self.chunk_recording {
            // SAFETY: alive until ended.
            let session = unsafe { &*self.session };
            let m = n.min(self.rec_space);
            session.ring.commit_write(m);
            if m < n {
                session.gaps.push(Gap { at: session.ring.total_written(), len: n - m });
                s.dropouts.fetch_add(1, Relaxed);
            }
            self.rec_frames += n as i64;
            session.frames.store(self.rec_frames, Release);
            record_fill = session.ring.fill() as f32 / session.ring.capacity() as f32;
        }
        if self.chunk_rolling {
            self.seq += n as i64;
            self.position = self.loop_range.advance(self.position, n as u64);
            let play_fill = match self.active {
                // SAFETY: consumer side of the active slot.
                Some(slot) => {
                    let ring = unsafe { &s.slots[slot].get().ring };
                    ring.available() as f32 / ring.capacity() as f32
                }
                None => 1.0,
            };
            s.disk_load.store(record_fill.max(1.0 - play_fill).clamp(0.0, 1.0));
        }
        self.chunk_rolling = false;
        self.chunk_recording = false;
        match std::mem::replace(&mut self.after, After::Nothing) {
            After::Nothing => {}
            After::Stop { pos } => {
                if let Some(p) = pos {
                    self.position = p.max(0);
                }
                self.stop_now(s);
            }
            After::Prime { pos, record } => self.begin_prime(s, pos.unwrap_or(self.position), record),
        }
        self.publish(s);
    }
}

impl Recorder {
    /// Audio thread, at the start of each chunk: see the module docs. Returns
    /// how many of the `n` samples this chunk covers (at least 1).
    ///
    /// # Safety
    /// Audio thread only, never concurrently with itself or `prepare`.
    pub unsafe fn begin_chunk(&self, n: usize) -> usize {
        (*self.audio.get()).begin_chunk(&self.shared, n)
    }

    /// The transport state and the timeline position of the current chunk
    /// (between `begin_chunk` and `end_chunk`).
    ///
    /// # Safety
    /// Audio thread only.
    pub unsafe fn chunk_state(&self) -> (TransportState, i64) {
        let a = &*self.audio.get();
        (a.state, a.position)
    }

    /// The 64 playback buffers (valid for the current chunk's length).
    ///
    /// # Safety
    /// Audio thread only.
    pub unsafe fn playback_ptrs(&self) -> &[*const f32] {
        &(*self.audio.get()).ptrs
    }

    /// The mono streams recorded in this chunk (empty when not recording).
    ///
    /// # Safety
    /// Audio thread only, between `begin_chunk` and `end_chunk`.
    pub unsafe fn record_streams(&self) -> &[RecordStream] {
        (*self.audio.get()).record_streams()
    }

    /// Records the chunk's `n` samples of `stream` (None: silence).
    ///
    /// # Safety
    /// Audio thread only, between `begin_chunk` and `end_chunk`.
    pub unsafe fn write_record_stream(&self, stream: usize, src: Option<&[f32]>, n: usize) {
        (*self.audio.get()).write_record_stream(stream, src, n)
    }

    /// Audio thread, at the end of each chunk.
    ///
    /// # Safety
    /// Audio thread only, after `begin_chunk` returned `n`.
    pub unsafe fn end_chunk(&self, n: usize) {
        (*self.audio.get()).end_chunk(&self.shared, n)
    }
}
