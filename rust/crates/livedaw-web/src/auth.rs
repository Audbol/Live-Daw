//! The PIN: checked on the server before a browser gets any frame, with wrong
//! guesses slowed down per address and overall.

use std::collections::HashMap;
use std::net::IpAddr;
use std::time::{Duration, Instant};

/// Wrong PINs an address may enter before it has to wait.
pub const FREE_TRIES: u32 = 3;
/// The first wait; it doubles with every further wrong PIN, up to MAX_LOCK.
pub const FIRST_LOCK: Duration = Duration::from_secs(5);
pub const MAX_LOCK: Duration = Duration::from_secs(300);
/// Wrong PINs from all addresses together in GLOBAL_WINDOW before everybody
/// waits (against guessing from many addresses).
pub const GLOBAL_TRIES: usize = 20;
pub const GLOBAL_WINDOW: Duration = Duration::from_secs(60);

/// A PIN is 4 to 8 digits; empty means none.
pub fn valid_pin(pin: &str) -> bool {
    pin.is_empty() || ((4..=8).contains(&pin.len()) && pin.bytes().all(|b| b.is_ascii_digit()))
}

#[derive(Debug, PartialEq, Eq)]
pub enum Verdict {
    Accepted,
    Wrong,
    /// Locked out: try again after this long.
    Wait(Duration),
}

#[derive(Default)]
struct Failures {
    count: u32,
    locked_until: Option<Instant>,
}

/// Checks PINs and remembers the wrong ones.
pub struct PinGuard {
    pin: String,
    by_addr: HashMap<IpAddr, Failures>,
    recent: Vec<Instant>,
}

impl PinGuard {
    pub fn new(pin: &str) -> Self {
        Self { pin: pin.to_string(), by_addr: HashMap::new(), recent: Vec::new() }
    }

    pub fn required(&self) -> bool {
        !self.pin.is_empty()
    }

    /// How long `addr` must still wait before it may try (None: it may).
    pub fn wait(&mut self, addr: IpAddr, now: Instant) -> Option<Duration> {
        self.recent.retain(|t| now.duration_since(*t) < GLOBAL_WINDOW);
        let global = (self.recent.len() >= GLOBAL_TRIES).then(|| GLOBAL_WINDOW - now.duration_since(self.recent[0]));
        let own = self
            .by_addr
            .get(&addr)
            .and_then(|f| f.locked_until)
            .and_then(|t| t.checked_duration_since(now))
            .filter(|d| !d.is_zero());
        match (global, own) {
            (Some(a), Some(b)) => Some(a.max(b)),
            (a, b) => a.or(b),
        }
    }

    pub fn check(&mut self, addr: IpAddr, attempt: &str, now: Instant) -> Verdict {
        if !self.required() {
            return Verdict::Accepted;
        }
        if let Some(w) = self.wait(addr, now) {
            return Verdict::Wait(w);
        }
        // Compare without an early exit (timing).
        let same =
            attempt.len() == self.pin.len() && attempt.bytes().zip(self.pin.bytes()).fold(0u8, |d, (a, b)| d | (a ^ b)) == 0;
        if same {
            self.by_addr.remove(&addr);
            return Verdict::Accepted;
        }
        self.recent.push(now);
        let f = self.by_addr.entry(addr).or_default();
        f.count += 1;
        if f.count >= FREE_TRIES {
            let doublings = (f.count - FREE_TRIES).min(10);
            f.locked_until = Some(now + (FIRST_LOCK * (1u32 << doublings)).min(MAX_LOCK));
        }
        Verdict::Wrong
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pins_are_four_to_eight_digits() {
        for ok in ["", "1234", "00000000"] {
            assert!(valid_pin(ok), "{ok}");
        }
        for bad in ["123", "123456789", "12a4", " 1234"] {
            assert!(!valid_pin(bad), "{bad}");
        }
    }

    #[test]
    fn no_pin_lets_everybody_in() {
        let mut g = PinGuard::new("");
        assert!(!g.required());
        assert_eq!(g.check("10.0.0.1".parse().unwrap(), "", Instant::now()), Verdict::Accepted);
    }

    #[test]
    fn wrong_pins_lock_the_address_out_longer_each_time() {
        let mut g = PinGuard::new("4711");
        let a: IpAddr = "10.0.0.1".parse().unwrap();
        let b: IpAddr = "10.0.0.2".parse().unwrap();
        let t0 = Instant::now();
        assert_eq!(g.check(a, "0000", t0), Verdict::Wrong);
        assert_eq!(g.check(a, "1111", t0), Verdict::Wrong);
        assert_eq!(g.check(a, "2222", t0), Verdict::Wrong);
        // Locked: even the right PIN waits.
        assert_eq!(g.check(a, "4711", t0 + Duration::from_secs(1)), Verdict::Wait(Duration::from_secs(4)));
        // Other addresses are not affected.
        assert_eq!(g.check(b, "4711", t0), Verdict::Accepted);
        let t1 = t0 + FIRST_LOCK;
        assert_eq!(g.check(a, "3333", t1), Verdict::Wrong);
        assert_eq!(g.wait(a, t1), Some(FIRST_LOCK * 2));
        let t2 = t1 + FIRST_LOCK * 2;
        assert_eq!(g.check(a, "4711", t2), Verdict::Accepted);
        // Success forgets the failures.
        assert_eq!(g.check(a, "5555", t2), Verdict::Wrong);
        assert_eq!(g.wait(a, t2), None);
    }

    #[test]
    fn many_wrong_pins_from_anywhere_lock_everybody_out() {
        let mut g = PinGuard::new("4711");
        let t0 = Instant::now();
        for i in 0..GLOBAL_TRIES {
            let addr = IpAddr::from([10, 0, 1, i as u8]);
            assert_eq!(g.check(addr, "0000", t0), Verdict::Wrong);
        }
        let fresh: IpAddr = "10.0.2.1".parse().unwrap();
        assert_eq!(g.check(fresh, "4711", t0), Verdict::Wait(GLOBAL_WINDOW));
        assert_eq!(g.check(fresh, "4711", t0 + GLOBAL_WINDOW), Verdict::Accepted);
    }
}
