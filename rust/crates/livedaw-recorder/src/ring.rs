//! Lock-free single-producer / single-consumer rings.
//!
//! [`Ring`] moves audio between the audio thread and the disk threads: frames of
//! several lanes (mono streams) stored planar, with one write and one read
//! index shared by all lanes, so a frame is committed for every lane at once.
//! [`SpscQueue`] carries small `Copy` messages (transport commands, record gaps).
//!
//! Both are wait-free: the producer only stores the write index (Release) after
//! filling the frames, the consumer only stores the read index (Release) after
//! reading them. Indices count frames since the last reset and never wrap in
//! practice (2^64 frames).

use std::cell::UnsafeCell;
use std::sync::atomic::{AtomicUsize, Ordering::*};

/// A planar ring of `lanes` × `capacity` samples.
///
/// The producer writes lanes at an offset from the write position, then
/// [`commit_write`](Ring::commit_write)s the frames; the consumer reads lanes at
/// an offset from the read position, then [`commit_read`](Ring::commit_read)s.
pub struct Ring {
    lanes: usize,
    capacity: usize,
    data: Box<[UnsafeCell<f32>]>,
    write: AtomicUsize,
    read: AtomicUsize,
}

// SAFETY: the producer only writes frames outside [read, write), the consumer
// only reads frames inside it; the index stores/loads order those accesses.
unsafe impl Sync for Ring {}
unsafe impl Send for Ring {}

impl Ring {
    pub fn new(lanes: usize, capacity: usize) -> Self {
        let capacity = capacity.max(1);
        Self {
            lanes,
            capacity,
            data: (0..lanes * capacity).map(|_| UnsafeCell::new(0.0)).collect(),
            write: AtomicUsize::new(0),
            read: AtomicUsize::new(0),
        }
    }

    pub fn lanes(&self) -> usize {
        self.lanes
    }

    pub fn capacity(&self) -> usize {
        self.capacity
    }

    /// Producer: frames that can be written.
    pub fn write_space(&self) -> usize {
        self.capacity - self.write.load(Relaxed).wrapping_sub(self.read.load(Acquire))
    }

    /// Consumer: frames that can be read.
    pub fn available(&self) -> usize {
        self.write.load(Acquire).wrapping_sub(self.read.load(Relaxed))
    }

    /// Any thread: frames in the ring (a snapshot, for meters).
    pub fn fill(&self) -> usize {
        self.write.load(Relaxed).wrapping_sub(self.read.load(Relaxed)).min(self.capacity)
    }

    /// Total frames committed by the producer since the last reset.
    pub fn total_written(&self) -> usize {
        self.write.load(Acquire)
    }

    /// Total frames consumed since the last reset.
    pub fn total_read(&self) -> usize {
        self.read.load(Acquire)
    }

    fn base(&self) -> *mut f32 {
        // UnsafeCell<f32> has the layout of f32.
        self.data.as_ptr() as *mut f32
    }

    /// The two contiguous parts of `n` samples of `lane` starting at ring index `at`.
    fn parts(&self, lane: usize, at: usize, n: usize) -> [(usize, usize); 2] {
        let start = at % self.capacity;
        let first = n.min(self.capacity - start);
        let lane_base = lane * self.capacity;
        [(lane_base + start, first), (lane_base, n - first)]
    }

    /// Producer: writes `src` into `lane`, `offset` frames after the write
    /// position. Frames beyond the free space are dropped.
    ///
    /// # Safety
    /// Only the producer may call this.
    pub unsafe fn write_lane(&self, lane: usize, offset: usize, src: &[f32]) {
        if lane >= self.lanes {
            return;
        }
        let n = src.len().min(self.write_space().saturating_sub(offset));
        let mut done = 0;
        for (at, len) in self.parts(lane, self.write.load(Relaxed).wrapping_add(offset), n) {
            std::ptr::copy_nonoverlapping(src.as_ptr().add(done), self.base().add(at), len);
            done += len;
        }
    }

    /// Producer: writes `n` zeros into `lane` (like [`write_lane`](Ring::write_lane)).
    ///
    /// # Safety
    /// Only the producer may call this.
    pub unsafe fn zero_lane(&self, lane: usize, offset: usize, n: usize) {
        if lane >= self.lanes {
            return;
        }
        let n = n.min(self.write_space().saturating_sub(offset));
        for (at, len) in self.parts(lane, self.write.load(Relaxed).wrapping_add(offset), n) {
            std::ptr::write_bytes(self.base().add(at), 0, len);
        }
    }

    /// Producer: publishes `n` written frames (clamped to the free space).
    pub fn commit_write(&self, n: usize) {
        let n = n.min(self.write_space());
        self.write.store(self.write.load(Relaxed).wrapping_add(n), Release);
    }

    /// Consumer: copies `dst.len()` samples of `lane`, `offset` frames after
    /// the read position. Samples beyond what is available are zeroed.
    ///
    /// # Safety
    /// Only the consumer may call this.
    pub unsafe fn read_lane(&self, lane: usize, offset: usize, dst: &mut [f32]) {
        let n = if lane < self.lanes { dst.len().min(self.available().saturating_sub(offset)) } else { 0 };
        let mut done = 0;
        if n > 0 {
            for (at, len) in self.parts(lane, self.read.load(Relaxed).wrapping_add(offset), n) {
                std::ptr::copy_nonoverlapping(self.base().add(at), dst.as_mut_ptr().add(done), len);
                done += len;
            }
        }
        dst[done..].fill(0.0);
    }

    /// Consumer: adds `gain(i) * sample` of `lane` into `dst` (see [`read_lane`](Ring::read_lane)).
    ///
    /// # Safety
    /// Only the consumer may call this.
    pub unsafe fn add_lane(&self, lane: usize, offset: usize, dst: &mut [f32], gain: impl Fn(usize) -> f32) {
        let n = if lane < self.lanes { dst.len().min(self.available().saturating_sub(offset)) } else { 0 };
        let mut done = 0;
        if n > 0 {
            for (at, len) in self.parts(lane, self.read.load(Relaxed).wrapping_add(offset), n) {
                let src = std::slice::from_raw_parts(self.base().add(at), len);
                for (j, s) in src.iter().enumerate() {
                    dst[done + j] += gain(done + j) * s;
                }
                done += len;
            }
        }
    }

    /// Consumer: releases `n` frames (clamped to what is available).
    pub fn commit_read(&self, n: usize) {
        let n = n.min(self.available());
        self.read.store(self.read.load(Relaxed).wrapping_add(n), Release);
    }

    /// Empties the ring.
    ///
    /// # Safety
    /// Neither the producer nor the consumer may be using the ring.
    pub unsafe fn reset(&self) {
        self.write.store(0, Release);
        self.read.store(0, Release);
    }
}

/// A bounded single-producer / single-consumer queue of small `Copy` values.
pub struct SpscQueue<T: Copy + Default> {
    slots: Box<[UnsafeCell<T>]>,
    head: AtomicUsize, // next to pop
    tail: AtomicUsize, // next to push
}

// SAFETY: as for Ring; a slot is only written while outside [head, tail).
unsafe impl<T: Copy + Default + Send> Sync for SpscQueue<T> {}
unsafe impl<T: Copy + Default + Send> Send for SpscQueue<T> {}

impl<T: Copy + Default> SpscQueue<T> {
    pub fn new(capacity: usize) -> Self {
        Self {
            slots: (0..capacity.max(1)).map(|_| UnsafeCell::new(T::default())).collect(),
            head: AtomicUsize::new(0),
            tail: AtomicUsize::new(0),
        }
    }

    pub fn capacity(&self) -> usize {
        self.slots.len()
    }

    pub fn len(&self) -> usize {
        self.tail.load(Acquire).wrapping_sub(self.head.load(Acquire))
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Producer: false (and nothing queued) when full.
    pub fn push(&self, value: T) -> bool {
        let tail = self.tail.load(Relaxed);
        if tail.wrapping_sub(self.head.load(Acquire)) >= self.slots.len() {
            return false;
        }
        // SAFETY: the slot is outside [head, tail): the consumer doesn't read it.
        unsafe { *self.slots[tail % self.slots.len()].get() = value };
        self.tail.store(tail.wrapping_add(1), Release);
        true
    }

    /// Consumer: the oldest value, without removing it.
    pub fn peek(&self) -> Option<T> {
        let head = self.head.load(Relaxed);
        if head == self.tail.load(Acquire) {
            return None;
        }
        // SAFETY: the slot is inside [head, tail): the producer doesn't write it.
        Some(unsafe { *self.slots[head % self.slots.len()].get() })
    }

    /// Consumer: removes and returns the oldest value.
    pub fn pop(&self) -> Option<T> {
        let value = self.peek()?;
        self.head.store(self.head.load(Relaxed).wrapping_add(1), Release);
        Some(value)
    }

    /// Consumer: drops everything queued.
    pub fn clear(&self) {
        while self.pop().is_some() {}
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;

    #[test]
    fn ring_wraps_and_respects_capacity() {
        let ring = Ring::new(2, 10);
        assert_eq!((ring.write_space(), ring.available()), (10, 0));
        unsafe {
            ring.write_lane(0, 0, &[1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0]);
            ring.write_lane(1, 0, &[-1.0; 7]);
        }
        ring.commit_write(7);
        assert_eq!((ring.write_space(), ring.available()), (3, 7));

        let mut out = [0.0; 5];
        unsafe { ring.read_lane(0, 2, &mut out) };
        assert_eq!(out, [3.0, 4.0, 5.0, 6.0, 7.0]);
        ring.commit_read(6);

        // Wrap around the end of the storage.
        unsafe {
            ring.write_lane(0, 0, &[8.0, 9.0, 10.0, 11.0, 12.0, 13.0, 14.0, 15.0, 16.0, 17.0, 18.0]);
        }
        ring.commit_write(100); // clamped to the free space (9)
        assert_eq!(ring.available(), 10);
        let mut all = [0.0; 12];
        unsafe { ring.read_lane(0, 0, &mut all) };
        assert_eq!(all, [7.0, 8.0, 9.0, 10.0, 11.0, 12.0, 13.0, 14.0, 15.0, 16.0, 0.0, 0.0], "beyond the data reads zeros");

        let mut sum = [1.0; 3];
        unsafe { ring.add_lane(0, 1, &mut sum, |i| i as f32) };
        assert_eq!(sum, [1.0, 1.0 + 9.0, 1.0 + 20.0]);

        ring.commit_read(10);
        assert_eq!((ring.available(), ring.total_read(), ring.total_written()), (0, 16, 16));
        unsafe { ring.reset() };
        assert_eq!(ring.write_space(), 10);
    }

    #[test]
    fn ring_moves_a_stream_between_threads_in_order() {
        const TOTAL: usize = 200_000;
        let ring = Arc::new(Ring::new(3, 257));
        let producer = {
            let ring = ring.clone();
            std::thread::spawn(move || {
                let mut next = 0usize;
                let mut chunk = [0.0f32; 61];
                while next < TOTAL {
                    let n = ring.write_space().min(chunk.len()).min(TOTAL - next);
                    if n == 0 {
                        std::thread::yield_now();
                        continue;
                    }
                    for lane in 0..3 {
                        for (i, x) in chunk[..n].iter_mut().enumerate() {
                            *x = ((next + i) * (lane + 1)) as f32;
                        }
                        unsafe { ring.write_lane(lane, 0, &chunk[..n]) };
                    }
                    ring.commit_write(n);
                    next += n;
                }
            })
        };
        let mut seen = 0usize;
        let mut buf = [0.0f32; 37];
        while seen < TOTAL {
            let n = ring.available().min(buf.len());
            if n == 0 {
                std::thread::yield_now();
                continue;
            }
            for lane in 0..3 {
                unsafe { ring.read_lane(lane, 0, &mut buf[..n]) };
                for (i, x) in buf[..n].iter().enumerate() {
                    assert_eq!(*x, ((seen + i) * (lane + 1)) as f32);
                }
            }
            ring.commit_read(n);
            seen += n;
        }
        producer.join().unwrap();
        assert_eq!(ring.available(), 0);
    }

    #[test]
    fn queue_is_fifo_and_bounded() {
        let q = SpscQueue::<u32>::new(3);
        assert!(q.push(1) && q.push(2) && q.push(3));
        assert!(!q.push(4), "full");
        assert_eq!((q.peek(), q.pop(), q.len()), (Some(1), Some(1), 2));
        assert!(q.push(4));
        assert_eq!([q.pop(), q.pop(), q.pop(), q.pop()], [Some(2), Some(3), Some(4), None]);

        let q = Arc::new(SpscQueue::<u64>::new(16));
        let p = {
            let q = q.clone();
            std::thread::spawn(move || {
                for i in 0..50_000u64 {
                    while !q.push(i) {
                        std::thread::yield_now();
                    }
                }
            })
        };
        let mut expected = 0;
        while expected < 50_000 {
            if let Some(v) = q.pop() {
                assert_eq!(v, expected);
                expected += 1;
            }
        }
        p.join().unwrap();
    }
}
