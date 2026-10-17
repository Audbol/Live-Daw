//! The disk reader thread: renders the playlist ahead of the play position
//! into the two playback rings ("slots").
//!
//! Who may touch a slot:
//! * When the audio thread starts a new epoch (play, locate, record) it is in
//!   Starting and uses no slot until the reader publishes `ready(epoch)`: the
//!   reader resets slot 0, primes it from the position and hands it over.
//! * While rolling, the audio thread consumes the active slot (and, during a
//!   crossfade, the previous one; `slot_in_use`). The reader keeps the active
//!   slot full. When the playlist or the loop changes, it sets up the other
//!   slot (once it is no longer in use), renders the new content into it from
//!   a transport frame `S` a little ahead of the audio thread and publishes a
//!   switch request. The audio thread splits its chunk to land on `S`, then
//!   crossfades to the new slot - or reports the request missed if it was
//!   already past `S` (the reader retries further ahead). Until the answer
//!   the reader leaves that slot alone.
//!
//! Transport frames ("seq") count the frames rolled since the epoch began;
//! frame `k` of a ring rendered for frame `seq0` onwards is `seq0 + k`.

use crate::playlist::Renderer;
use crate::*;
use std::sync::atomic::fence;

const RENDER_BLOCK: usize = 4096;

struct Ctx {
    slot: usize,
    playlist: Arc<Prepared>,
    loop_range: LoopRange,
    /// Timeline position of the next frame to render.
    next_pos: i64,
    /// Transport frame and position of the ring's first frame.
    seq0: i64,
    pos0: i64,
}

struct Switch {
    ctx: Ctx,
    seq: i64,
    /// 0 until published.
    token: u64,
}

struct Reader {
    s: Arc<Shared>,
    renderer: Renderer,
    scratch: Vec<Vec<f32>>,
    epoch: u64,
    record: bool,
    active: Option<Ctx>,
    switch: Option<Switch>,
    margin: i64,
}

pub(crate) fn run(s: Arc<Shared>) {
    let mut r = Reader {
        s,
        renderer: Renderer::new(),
        scratch: Vec::new(),
        epoch: 0,
        record: false,
        active: None,
        switch: None,
        margin: 0,
    };
    while !r.s.shutdown.load(Acquire) {
        let busy = r.step();
        if !busy {
            let rolling = r.active.is_some() || r.s.state() != TransportState::Stopped;
            std::thread::park_timeout(Duration::from_millis(if rolling { 2 } else { 20 }));
        }
    }
}

/// Renders up to `max` frames of `ctx` into its slot's ring (loop wraps
/// included). Returns the frames rendered.
fn fill(s: &Shared, renderer: &mut Renderer, scratch: &mut Vec<Vec<f32>>, ctx: &mut Ctx, max: usize) -> usize {
    // SAFETY: the reader is the slot's producer; the slot isn't being reset.
    let d = unsafe { s.slots[ctx.slot].get() };
    let n = d.ring.write_space().min(RENDER_BLOCK).min(max);
    if n == 0 {
        return 0;
    }
    let lanes = d.lane_channel.len();
    scratch.resize_with(lanes, Vec::new);
    for b in scratch.iter_mut() {
        b.resize(RENDER_BLOCK, 0.0);
    }
    let mut done = 0;
    while done < n {
        let run = ctx.loop_range.max_run(ctx.next_pos, n - done);
        renderer.render(&ctx.playlist, ctx.next_pos, &mut scratch[..lanes], run);
        for (lane, b) in scratch[..lanes].iter().enumerate() {
            // SAFETY: producer side, within the free space.
            unsafe { d.ring.write_lane(lane, done, &b[..run]) };
        }
        ctx.next_pos = ctx.loop_range.advance(ctx.next_pos, run as u64);
        done += run;
    }
    d.ring.commit_write(n);
    n
}

impl Reader {
    fn capacity(&self) -> usize {
        let max_block = self.s.max_block.load(Relaxed);
        ((PLAYBACK_BUFFER_SECONDS * self.s.sample_rate()) as usize).max(8 * max_block)
    }

    fn initial_margin(&self) -> i64 {
        let max_block = self.s.max_block.load(Relaxed);
        ((0.05 * self.s.sample_rate()) as usize).max(4 * max_block) as i64
    }

    /// Resets a slot for `playlist` (reallocating the ring if its shape changed).
    ///
    /// # Safety
    /// The audio thread must not be able to use the slot.
    unsafe fn setup_slot(&self, slot: usize, playlist: &Prepared, loop_range: LoopRange) {
        let d = self.s.slots[slot].get_mut();
        let lanes = playlist.lanes.len();
        let capacity = self.capacity();
        if d.ring.lanes() != lanes || d.ring.capacity() != capacity {
            d.ring = Ring::new(lanes, capacity);
        } else {
            d.ring.reset();
        }
        d.lane_channel.clone_from(&playlist.lanes);
        d.loop_range = loop_range;
    }

    fn step(&mut self) -> bool {
        let s = self.s.clone();
        let kick = s.kick.load(Acquire);
        let version = s.content_version.load(Acquire);
        let epoch = s.epoch.load(Acquire);
        let mut busy = false;
        if epoch != self.epoch {
            self.epoch = epoch;
            self.active = None;
            self.switch = None;
            if s.prime_epoch.load(Acquire) == epoch {
                busy = true;
                self.prime(epoch);
            }
        }
        if self.active.is_some() && s.epoch.load(Acquire) == self.epoch {
            busy |= self.roll();
        }
        s.reader_busy.store(busy, Release);
        s.reader_epoch.store(self.epoch, Release);
        s.reader_seen.store(version, Release);
        s.reader_kick.store(kick, Release);
        busy
    }

    fn prime(&mut self, epoch: u64) {
        let s = self.s.clone();
        let pos = s.prime_pos.load(Relaxed);
        let record = s.prime_record.load(Relaxed);
        let content = s.content().clone();
        // Recording never loops.
        let loop_range = if record { LoopRange::default() } else { content.loop_range };
        self.record = record;
        // SAFETY: Starting - the audio thread uses no slot until ready(epoch).
        unsafe { self.setup_slot(0, &content.playlist, loop_range) };
        let mut ctx = Ctx { slot: 0, playlist: content.playlist, loop_range, next_pos: pos, seq0: 0, pos0: pos };
        let target = ((PRIME_SECONDS * s.sample_rate()) as usize).max(4 * s.max_block.load(Relaxed)).min(self.capacity());
        // SAFETY: shared view of the slot just set up.
        let ring = unsafe { &s.slots[0].get().ring };
        while ring.available() < target {
            if s.epoch.load(Acquire) != epoch || s.shutdown.load(Acquire) {
                return; // a newer request: start over
            }
            fill(&s, &mut self.renderer, &mut self.scratch, &mut ctx, target - ring.available());
        }
        s.ready_slot.store(0, Relaxed);
        s.ready_epoch.store(epoch, Release);
        self.renderer.retain(&[&ctx.playlist]);
        self.active = Some(ctx);
        self.margin = self.initial_margin();
    }

    fn publish_switch(&self, sw: &mut Switch) {
        let s = &self.s;
        let writing = s.pend_token.load(Relaxed) | 1;
        s.pend_token.store(writing, Relaxed);
        fence(Release);
        s.pend_slot.store(sw.ctx.slot, Relaxed);
        s.pend_seq.store(sw.seq, Relaxed);
        s.pend_epoch.store(self.epoch, Relaxed);
        s.pend_token.store(writing + 1, Release);
        sw.token = writing + 1;
    }

    fn roll(&mut self) -> bool {
        let s = self.s.clone();
        let mut busy = false;

        // The audio thread's answer to a published switch.
        if let Some(token) = self.switch.as_ref().map(|sw| sw.token).filter(|&t| t != 0) {
            if s.switch_done.load(Acquire) == token {
                let sw = self.switch.take().expect("switch");
                self.active = Some(sw.ctx); // the old slot fades out, then is released
                self.margin = self.initial_margin();
                if let Some(a) = &self.active {
                    self.renderer.retain(&[&a.playlist]);
                }
                busy = true;
            } else if s.switch_missed.load(Acquire) == token {
                self.switch = None;
                self.margin = (self.margin * 2).min(self.capacity() as i64 / 2);
                busy = true;
            }
        }

        // New content: render it into the other slot from a little ahead.
        if self.switch.is_none() {
            let content = s.content().clone();
            let want_loop = if self.record { LoopRange::default() } else { content.loop_range };
            let active = self.active.as_ref().expect("active");
            let other = 1 - active.slot;
            let changed = !Arc::ptr_eq(&active.playlist, &content.playlist) || active.loop_range != want_loop;
            if changed && !s.slot_in_use[other].load(Acquire) {
                let at = s.seq.load(Acquire) + self.margin;
                let pos = active.loop_range.advance(active.pos0, (at - active.seq0).max(0) as u64);
                // SAFETY: not in use by the audio thread and no request refers to it.
                unsafe { self.setup_slot(other, &content.playlist, want_loop) };
                self.switch = Some(Switch {
                    ctx: Ctx {
                        slot: other,
                        playlist: content.playlist,
                        loop_range: want_loop,
                        next_pos: pos,
                        seq0: at,
                        pos0: pos,
                    },
                    seq: at,
                    token: 0,
                });
                busy = true;
            }
        }

        // Keep the playing ring full first, then the next one.
        if let Some(active) = self.active.as_mut() {
            busy |= fill(&s, &mut self.renderer, &mut self.scratch, active, RENDER_BLOCK) > 0;
        }
        let ready = (2 * s.max_block.load(Relaxed)).max(256).min(self.capacity());
        if let Some(mut sw) = self.switch.take() {
            busy |= fill(&s, &mut self.renderer, &mut self.scratch, &mut sw.ctx, RENDER_BLOCK) > 0;
            // SAFETY: shared view; the reader is the producer.
            let filled = unsafe { s.slots[sw.ctx.slot].get() }.ring.available();
            let mut keep = true;
            if sw.token == 0 && filled >= ready {
                if s.seq.load(Acquire) >= sw.seq {
                    // Too late for this point: try further ahead.
                    keep = false;
                    self.margin = (self.margin * 2).min(self.capacity() as i64 / 2);
                } else {
                    self.publish_switch(&mut sw);
                }
                busy = true;
            }
            if keep {
                self.switch = Some(sw);
            }
        }
        busy
    }
}
