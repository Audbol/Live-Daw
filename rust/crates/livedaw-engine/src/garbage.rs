//! Deferred deletion for objects the audio thread might still be using.
//!
//! The message thread "retires" an object after unpublishing it. It is dropped
//! (on the message thread, in `collect`) only once the audio callback has
//! completed a full cycle since, so no in-flight callback can still see it. The
//! audio thread never allocates or frees anything.

use std::any::Any;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering::SeqCst};
use std::sync::Mutex;

#[derive(Default)]
pub struct Garbage {
    pending: Mutex<Vec<(u64, Box<dyn Any + Send>)>>,
    callback_count: AtomicU64,
    in_callback: AtomicBool,
    device_running: AtomicBool,
}

impl Garbage {
    #[inline]
    pub fn callback_started(&self) {
        self.in_callback.store(true, SeqCst);
    }

    #[inline]
    pub fn callback_finished(&self) {
        self.callback_count.fetch_add(1, SeqCst);
        self.in_callback.store(false, SeqCst);
    }

    pub fn set_device_running(&self, running: bool) {
        self.device_running.store(running, SeqCst);
    }

    pub fn retire<T: Any + Send>(&self, object: T) {
        let epoch = self.callback_count.load(SeqCst);
        self.pending.lock().unwrap_or_else(|e| e.into_inner()).push((epoch, Box::new(object)));
    }

    /// Message thread: drops everything that is provably unused.
    pub fn collect(&self) {
        let to_free: Vec<Box<dyn Any + Send>> = {
            let mut pending = self.pending.lock().unwrap_or_else(|e| e.into_inner());
            let count = self.callback_count.load(SeqCst);
            let idle = !self.device_running.load(SeqCst) && !self.in_callback.load(SeqCst);
            let (free, keep): (Vec<_>, Vec<_>) = pending.drain(..).partition(|(epoch, _)| idle || count > *epoch);
            *pending = keep;
            free.into_iter().map(|(_, o)| o).collect()
        };
        drop(to_free); // outside the lock
    }

    /// Drops everything now (the caller guarantees the audio thread is stopped).
    pub fn collect_all_now(&self) {
        let all: Vec<_> = self.pending.lock().unwrap_or_else(|e| e.into_inner()).drain(..).collect();
        drop(all);
    }

    pub fn num_pending(&self) -> usize {
        self.pending.lock().unwrap_or_else(|e| e.into_inner()).len()
    }
}
