//! Input from the browsers: the binary messages the page sends, decoded, and
//! the queue the host drains on its message thread.
//!
//! Browser -> server messages (little endian):
//! * `1` pointer: u8 action (0 move, 1 down, 2 up, 3 leave), u8 buttons held
//!   after the event (1 left, 2 right, 4 middle), u8 modifiers, f32 x, f32 y;
//! * `2` wheel: u8 modifiers, f32 x, f32 y, f32 dx, f32 dy (JUCE wheel units);
//! * `3` key: u8 down (1) / up (0), u8 modifiers, i32 key (a character, or
//!   one of the negative KEY_ codes);
//! * `4` ack: u32 seq of the frame the page has drawn;
//! * `5` ping: 8 bytes echoed back at once (the page measures the round trip).
//!
//! Positions are fractions of the screen (0..1). Modifiers: 1 shift, 2 ctrl,
//! 4 alt, 8 meta (command).

use std::collections::VecDeque;

pub const MSG_POINTER: u8 = 1;
pub const MSG_WHEEL: u8 = 2;
pub const MSG_KEY: u8 = 3;
pub const MSG_ACK: u8 = 4;
pub const MSG_PING: u8 = 5;

/// Event kinds (`Event::kind`, shared with the C ABI).
pub const KIND_MOVE: i32 = 0;
pub const KIND_DOWN: i32 = 1;
pub const KIND_UP: i32 = 2;
pub const KIND_LEAVE: i32 = 3;
pub const KIND_WHEEL: i32 = 4;
pub const KIND_KEY_DOWN: i32 = 5;
pub const KIND_KEY_UP: i32 = 6;

/// Events waiting for the host before the oldest are dropped.
pub const QUEUE_LIMIT: usize = 512;

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Event {
    pub kind: i32,
    pub x: f32,
    pub y: f32,
    pub dx: f32,
    pub dy: f32,
    /// Pointer: buttons held after the event. Keys: the key.
    pub buttons: i32,
    pub key: i32,
    pub mods: u32,
    /// Which browser (to tell pointers apart).
    pub client: u32,
}

/// What a binary message from the page asks for.
#[derive(Debug, PartialEq)]
pub enum Decoded {
    Input(Event),
    Ack(u32),
    Ping([u8; 8]),
}

fn f32_at(m: &[u8], o: usize) -> Option<f32> {
    let v = f32::from_le_bytes(m.get(o..o + 4)?.try_into().ok()?);
    v.is_finite().then_some(v)
}

pub fn decode(m: &[u8], client: u32) -> Option<Decoded> {
    let unit = |v: f32| v.clamp(0.0, 1.0);
    let ev = Event { client, ..Event::default() };
    Some(match *m.first()? {
        MSG_POINTER if m.len() >= 12 => {
            let kind = match m[1] {
                0 => KIND_MOVE,
                1 => KIND_DOWN,
                2 => KIND_UP,
                3 => KIND_LEAVE,
                _ => return None,
            };
            Decoded::Input(Event {
                kind,
                buttons: i32::from(m[2] & 7),
                mods: u32::from(m[3] & 15),
                x: unit(f32_at(m, 4)?),
                y: unit(f32_at(m, 8)?),
                ..ev
            })
        }
        MSG_WHEEL if m.len() >= 18 => Decoded::Input(Event {
            kind: KIND_WHEEL,
            mods: u32::from(m[1] & 15),
            x: unit(f32_at(m, 2)?),
            y: unit(f32_at(m, 6)?),
            dx: f32_at(m, 10)?.clamp(-50.0, 50.0),
            dy: f32_at(m, 14)?.clamp(-50.0, 50.0),
            ..ev
        }),
        MSG_KEY if m.len() >= 7 => Decoded::Input(Event {
            kind: if m[1] != 0 { KIND_KEY_DOWN } else { KIND_KEY_UP },
            mods: u32::from(m[2] & 15),
            key: i32::from_le_bytes(m[3..7].try_into().ok()?),
            ..ev
        }),
        MSG_ACK if m.len() >= 5 => Decoded::Ack(u32::from_le_bytes(m[1..5].try_into().ok()?)),
        MSG_PING if m.len() >= 9 => Decoded::Ping(m[1..9].try_into().ok()?),
        _ => return None,
    })
}

/// Input waiting for the host. Consecutive moves (same browser, same
/// buttons) collapse into the latest, so a busy message thread catches up.
#[derive(Default)]
pub struct Queue {
    events: VecDeque<Event>,
}

impl Queue {
    pub fn push(&mut self, e: Event) {
        if e.kind == KIND_MOVE {
            if let Some(last) = self.events.back_mut() {
                if last.kind == KIND_MOVE && last.client == e.client && last.buttons == e.buttons {
                    *last = e;
                    return;
                }
            }
        }
        if self.events.len() >= QUEUE_LIMIT {
            self.events.pop_front();
        }
        self.events.push_back(e);
    }

    pub fn pop(&mut self) -> Option<Event> {
        self.events.pop_front()
    }

    pub fn clear(&mut self) {
        self.events.clear();
    }

    pub fn len(&self) -> usize {
        self.events.len()
    }

    pub fn is_empty(&self) -> bool {
        self.events.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pointer(action: u8, buttons: u8, x: f32, y: f32) -> Vec<u8> {
        let mut m = vec![MSG_POINTER, action, buttons, 1];
        m.extend_from_slice(&x.to_le_bytes());
        m.extend_from_slice(&y.to_le_bytes());
        m
    }

    #[test]
    fn decodes_the_page_messages() {
        let Some(Decoded::Input(e)) = decode(&pointer(1, 1, 0.25, 2.0), 3) else { panic!() };
        assert_eq!((e.kind, e.buttons, e.mods, e.x, e.y, e.client), (KIND_DOWN, 1, 1, 0.25, 1.0, 3));

        let mut wheel = vec![MSG_WHEEL, 2];
        for v in [0.5f32, 0.5, 0.0, -0.2] {
            wheel.extend_from_slice(&v.to_le_bytes());
        }
        let Some(Decoded::Input(e)) = decode(&wheel, 0) else { panic!() };
        assert_eq!((e.kind, e.mods, e.dy), (KIND_WHEEL, 2, -0.2));

        let mut key = vec![MSG_KEY, 1, 0];
        key.extend_from_slice(&(-6i32).to_le_bytes());
        let Some(Decoded::Input(e)) = decode(&key, 0) else { panic!() };
        assert_eq!((e.kind, e.key), (KIND_KEY_DOWN, -6));

        assert_eq!(decode(&[MSG_ACK, 7, 0, 0, 0], 0), Some(Decoded::Ack(7)));
        assert_eq!(decode(&[MSG_PING, 1, 2, 3, 4, 5, 6, 7, 8], 0), Some(Decoded::Ping([1, 2, 3, 4, 5, 6, 7, 8])));
    }

    #[test]
    fn refuses_short_or_odd_messages() {
        assert_eq!(decode(&[], 0), None);
        assert_eq!(decode(&[MSG_POINTER, 0, 0], 0), None);
        assert_eq!(decode(&pointer(9, 0, 0.0, 0.0), 0), None);
        assert_eq!(decode(&pointer(0, 0, f32::NAN, 0.0), 0), None);
        assert_eq!(decode(&[99, 0, 0, 0, 0], 0), None);
    }

    #[test]
    fn moves_collapse_but_presses_stay() {
        let mut q = Queue::default();
        let ev = |kind, x, buttons, client| Event { kind, x, buttons, client, ..Event::default() };
        q.push(ev(KIND_MOVE, 0.1, 0, 0));
        q.push(ev(KIND_MOVE, 0.2, 0, 0));
        q.push(ev(KIND_DOWN, 0.2, 1, 0));
        q.push(ev(KIND_MOVE, 0.3, 1, 0));
        q.push(ev(KIND_MOVE, 0.4, 1, 0));
        q.push(ev(KIND_MOVE, 0.5, 1, 1)); // another browser
        q.push(ev(KIND_UP, 0.4, 0, 0));
        let xs: Vec<(i32, f32)> = std::iter::from_fn(|| q.pop()).map(|e| (e.kind, e.x)).collect();
        assert_eq!(xs, vec![(KIND_MOVE, 0.2), (KIND_DOWN, 0.2), (KIND_MOVE, 0.4), (KIND_MOVE, 0.5), (KIND_UP, 0.4)]);

        for i in 0..QUEUE_LIMIT + 10 {
            q.push(ev(KIND_DOWN, i as f32, 1, 0));
        }
        assert_eq!(q.len(), QUEUE_LIMIT);
        assert_eq!(q.pop().unwrap().x, 10.0);
    }
}
