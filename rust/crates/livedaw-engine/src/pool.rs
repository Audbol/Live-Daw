//! A fork/join pool for the audio callback.
//!
//! The audio thread calls `parallel_for`: it publishes a job, wakes the workers,
//! takes part in the work itself and returns once every index has run. Nothing
//! allocates or locks on the audio thread.
//!
//! Worker threads are provided by the host (the JUCE app runs them as realtime
//! threads, so priority/MMCSS is handled per platform): each calls
//! `run_worker(index)`, which returns after `shutdown()`.

use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicPtr, AtomicU32, AtomicU64, AtomicUsize, Ordering::*};
use std::sync::OnceLock;
use std::thread::Thread;
use std::time::{Duration, Instant};

type TaskFn = unsafe fn(*const (), usize);

const MAX_TASKS_PER_JOB: usize = 0xffff;

/// Idle workers spin this long before parking, until a block duration is known.
const DEFAULT_SPIN_NS: u64 = 100_000;

pub struct WorkerPool {
    workers: Box<[OnceLock<Thread>]>,
    registered: AtomicUsize,

    job_task: AtomicUsize, // TaskFn as usize (0 = none)
    job_context: AtomicPtr<()>,
    /// Bits 32-63: generation, 16-31: task count, 0-15: next index to claim.
    /// Keeping the count in the same word as the claim index means a worker can
    /// never claim an index of one job against the count of another.
    work: AtomicU64,
    remaining: AtomicUsize,
    wake_signal: AtomicU32,
    generation: AtomicU32, // only touched by the audio thread
    should_exit: AtomicBool,
    /// How long an idle worker spins before it parks (nanoseconds).
    spin_ns: AtomicU64,
    /// Tasks that panicked (their work was skipped).
    pub panicked_tasks: AtomicI32,
}

impl WorkerPool {
    pub fn new(num_workers: usize) -> Self {
        Self {
            workers: (0..num_workers).map(|_| OnceLock::new()).collect(),
            registered: AtomicUsize::new(0),
            job_task: AtomicUsize::new(0),
            job_context: AtomicPtr::new(std::ptr::null_mut()),
            work: AtomicU64::new(0),
            remaining: AtomicUsize::new(0),
            wake_signal: AtomicU32::new(0),
            generation: AtomicU32::new(0),
            should_exit: AtomicBool::new(false),
            spin_ns: AtomicU64::new(DEFAULT_SPIN_NS),
            panicked_tasks: AtomicI32::new(0),
        }
    }

    pub fn num_workers(&self) -> usize {
        self.workers.len()
    }

    pub fn num_running_workers(&self) -> usize {
        self.registered.load(Acquire)
    }

    /// Sizes the spin budget to the block duration before workers sleep.
    ///
    /// Workers spin for a quarter of a block (20-300 us): long enough to stay
    /// hot between the phases of one block, short enough to park between
    /// blocks. Spinning longer than a block would keep these realtime threads
    /// busy all the time and starve the rest of the app on small machines. A
    /// worker that wakes late never stalls a block: the audio thread takes part
    /// in every job.
    pub fn set_block_duration_hint(&self, sample_rate: f64, block_size: usize) {
        if sample_rate > 0.0 && block_size > 0 {
            let block_ns = 1.0e9 * block_size as f64 / sample_rate;
            self.spin_ns.store((block_ns / 4.0).clamp(20_000.0, 300_000.0) as u64, Relaxed);
        }
    }

    /// Worker thread body. Returns when `shutdown()` is called.
    pub fn run_worker(&self, index: usize) {
        let Some(slot) = self.workers.get(index) else { return };
        if slot.set(std::thread::current()).is_err() {
            return; // index already taken
        }
        self.registered.fetch_add(1, AcqRel);

        let _ftz = crate::denormals::ScopedNoDenormals::new();
        let mut seen = self.wake_signal.load(Acquire);

        while !self.should_exit.load(Acquire) {
            let budget = Duration::from_nanos(self.spin_ns.load(Relaxed));
            let spin_start = Instant::now();
            while self.wake_signal.load(Acquire) == seen && !self.should_exit.load(Acquire) && spin_start.elapsed() < budget {
                std::hint::spin_loop();
                std::thread::yield_now();
            }
            while self.wake_signal.load(Acquire) == seen && !self.should_exit.load(Acquire) {
                std::thread::park(); // unparked by parallel_for (spurious wakeups just loop)
            }
            seen = self.wake_signal.load(Acquire);
            if self.should_exit.load(Acquire) {
                break;
            }
            self.run_tasks();
        }
    }

    /// Asks all workers to return from `run_worker`.
    pub fn shutdown(&self) {
        self.should_exit.store(true, Release);
        self.wake_signal.fetch_add(1, AcqRel);
        for w in self.workers.iter() {
            if let Some(t) = w.get() {
                t.unpark();
            }
        }
    }

    fn run_tasks(&self) {
        loop {
            let w = self.work.load(Acquire);
            let index = (w & 0xffff) as usize;
            let count = ((w >> 16) & 0xffff) as usize;
            if index >= count {
                return;
            }
            // If the claim below succeeds, this generation still had unclaimed work
            // when these were loaded, so the caller cannot have replaced them yet.
            let task = self.job_task.load(Acquire);
            let ctx = self.job_context.load(Acquire);
            if task == 0 {
                return;
            }
            if self.work.compare_exchange_weak(w, w + 1, AcqRel, Acquire).is_err() {
                continue;
            }
            // SAFETY: `task` was stored from a `TaskFn` in parallel_for and `ctx` is
            // kept alive by the caller until `remaining` reaches zero.
            let task: TaskFn = unsafe { std::mem::transmute::<usize, TaskFn>(task) };
            if catch_unwind(AssertUnwindSafe(|| unsafe { task(ctx as *const (), index) })).is_err() {
                self.panicked_tasks.fetch_add(1, Relaxed);
            }
            // Always count the task as done, even if it panicked: the audio
            // thread must never wait forever.
            self.remaining.fetch_sub(1, AcqRel);
        }
    }

    /// Runs `f(i)` for every i in 0..count, in parallel. Audio thread only; not re-entrant.
    pub fn parallel_for<F: Fn(usize) + Sync>(&self, count: usize, f: &F) {
        if count == 0 {
            return;
        }
        if self.registered.load(Acquire) == 0 || count == 1 {
            for i in 0..count {
                if catch_unwind(AssertUnwindSafe(|| f(i))).is_err() {
                    self.panicked_tasks.fetch_add(1, Relaxed);
                }
            }
            return;
        }
        // Jobs hold at most MAX_TASKS_PER_JOB tasks (16 bits in the work word).
        let mut base = 0;
        while base < count {
            let n = MAX_TASKS_PER_JOB.min(count - base);
            if base == 0 && n == count {
                self.run_job(n, f);
            } else {
                self.run_job(n, &|i| f(base + i));
            }
            base += n;
        }
    }

    fn run_job<F: Fn(usize) + Sync>(&self, count: usize, f: &F) {
        unsafe fn trampoline<F: Fn(usize)>(ctx: *const (), i: usize) {
            (*(ctx as *const F))(i)
        }

        let generation = self.generation.load(Relaxed).wrapping_add(1);
        self.generation.store(generation, Relaxed);
        self.job_task.store(trampoline::<F> as TaskFn as usize, Relaxed);
        self.job_context.store(f as *const F as *mut (), Relaxed);
        self.remaining.store(count, Relaxed);
        self.work.store(((generation as u64) << 32) | ((count as u64) << 16), Release);

        self.wake_signal.fetch_add(1, AcqRel);
        for w in self.workers.iter() {
            if let Some(t) = w.get() {
                t.unpark();
            }
        }

        self.run_tasks();

        while self.remaining.load(Acquire) > 0 {
            std::hint::spin_loop();
            std::thread::yield_now();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;

    #[test]
    fn every_index_runs_exactly_once() {
        for workers in [0usize, 1, 3] {
            let pool = Arc::new(WorkerPool::new(workers));
            let threads: Vec<_> = (0..workers)
                .map(|i| {
                    let p = pool.clone();
                    std::thread::spawn(move || p.run_worker(i))
                })
                .collect();
            while pool.num_running_workers() < workers {
                std::thread::yield_now();
            }

            let hits: Vec<AtomicUsize> = (0..257).map(|_| AtomicUsize::new(0)).collect();
            let mut expected = vec![0usize; 257];
            for round in 0..2000 {
                let count = 1 + (round * 7) % 257;
                pool.parallel_for(count, &|i| {
                    hits[i].fetch_add(1, Relaxed);
                });
                for e in expected.iter_mut().take(count) {
                    *e += 1;
                }
            }
            for i in 0..257 {
                assert_eq!(hits[i].load(Relaxed), expected[i], "index {i} with {workers} workers");
            }

            pool.shutdown();
            threads.into_iter().for_each(|t| t.join().unwrap());
        }
    }

    #[test]
    fn a_panicking_task_does_not_hang_the_caller() {
        let pool = Arc::new(WorkerPool::new(2));
        let threads: Vec<_> = (0..2)
            .map(|i| {
                let p = pool.clone();
                std::thread::spawn(move || p.run_worker(i))
            })
            .collect();
        while pool.num_running_workers() < 2 {
            std::thread::yield_now();
        }

        let prev = std::panic::take_hook();
        std::panic::set_hook(Box::new(|_| {}));
        let done = AtomicUsize::new(0);
        pool.parallel_for(64, &|i| {
            if i == 13 {
                panic!("bug in a task");
            }
            done.fetch_add(1, Relaxed);
        });
        std::panic::set_hook(prev);

        assert_eq!(done.load(Relaxed), 63);
        assert_eq!(pool.panicked_tasks.load(Relaxed), 1);
        pool.shutdown();
        threads.into_iter().for_each(|t| t.join().unwrap());
    }
}
