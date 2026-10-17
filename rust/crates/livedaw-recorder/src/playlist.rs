//! What plays: tracks of clips on the timeline, the loop, and the renderer the
//! reader thread uses to turn them into playback channels.

use crate::wav::WavReader;
use crate::NUM_PLAYBACK_CHANNELS;
use std::collections::HashMap;
use std::f32::consts::{FRAC_PI_2, PI};
use std::path::{Path, PathBuf};

/// Fade curves (`Recorder::FadeShape` order).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum FadeShape {
    Linear,
    #[default]
    EqualPower,
    SCurve,
}

impl FadeShape {
    pub fn from_i32(v: i32) -> Self {
        match v {
            0 => Self::Linear,
            2 => Self::SCurve,
            _ => Self::EqualPower,
        }
    }

    /// Gain of a fade in at `x` (0 = start, 1 = end). A fade out uses the
    /// same curve backwards, so a linear crossfade sums to 1 and an
    /// equal-power one keeps the power.
    pub fn gain(self, x: f32) -> f32 {
        let x = x.clamp(0.0, 1.0);
        match self {
            Self::Linear => x,
            Self::EqualPower => (x * FRAC_PI_2).sin(),
            Self::SCurve => 0.5 - 0.5 * (x * PI).cos(),
        }
    }
}

/// A region of a WAV file placed on the timeline.
#[derive(Clone, Debug, PartialEq)]
pub struct Clip {
    pub file: PathBuf,
    /// First frame of the file that plays.
    pub file_offset: i64,
    /// Timeline position of that frame.
    pub start: i64,
    pub length: i64,
    /// Linear.
    pub gain: f32,
    /// Fade lengths in samples, inside the clip.
    pub fade_in: i64,
    pub fade_out: i64,
    pub fade_in_shape: FadeShape,
    pub fade_out_shape: FadeShape,
}

impl Clip {
    pub fn new(file: impl Into<PathBuf>, start: i64, length: i64) -> Self {
        Self {
            file: file.into(),
            file_offset: 0,
            start,
            length,
            gain: 1.0,
            fade_in: 0,
            fade_out: 0,
            fade_in_shape: FadeShape::default(),
            fade_out_shape: FadeShape::default(),
        }
    }

    pub fn end(&self) -> i64 {
        self.start + self.length.max(0)
    }

    /// Clip gain and fades at clip-relative sample `k` (0 = the clip's first).
    pub fn gain_at(&self, k: i64) -> f32 {
        let mut g = self.gain;
        if self.fade_in > 0 && k < self.fade_in {
            g *= self.fade_in_shape.gain(k as f32 / self.fade_in as f32);
        }
        if self.fade_out > 0 && k >= self.length - self.fade_out {
            g *= self.fade_out_shape.gain((self.length - k) as f32 / self.fade_out as f32);
        }
        g
    }
}

/// Clips playing on one or two playback channels.
#[derive(Clone, Debug, PartialEq)]
pub struct PlaybackTrack {
    /// First playback channel (stereo uses `channel` and `channel + 1`).
    pub channel: usize,
    pub num_channels: usize,
    pub gain: f32,
    pub mute: bool,
    pub clips: Vec<Clip>,
}

impl PlaybackTrack {
    pub fn new(channel: usize, num_channels: usize, clips: Vec<Clip>) -> Self {
        Self { channel, num_channels, gain: 1.0, mute: false, clips }
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Playlist {
    pub tracks: Vec<PlaybackTrack>,
}

/// The loop range: playback wraps from `end` to `start` when it reaches `end`
/// from below (a position at or past `end` plays on).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LoopRange {
    pub start: i64,
    pub end: i64,
    pub enabled: bool,
}

impl LoopRange {
    pub fn new(start: i64, end: i64, enabled: bool) -> Self {
        Self { start: start.max(0), end: end.max(0), enabled }
    }

    pub fn active(&self) -> bool {
        self.enabled && self.end > self.start
    }

    /// How many of the next `n` frames from `pos` play before the wrap.
    pub fn max_run(&self, pos: i64, n: usize) -> usize {
        if self.active() && pos < self.end {
            n.min((self.end - pos) as usize)
        } else {
            n
        }
    }

    /// The position after `n` frames from `pos` (wrapping as often as needed).
    pub fn advance(&self, pos: i64, n: u64) -> i64 {
        if !self.active() || pos >= self.end {
            return pos + n as i64;
        }
        let to_end = (self.end - pos) as u64;
        if n < to_end {
            return pos + n as i64;
        }
        let len = (self.end - self.start) as u64;
        self.start + ((n - to_end) % len) as i64
    }
}

struct PreparedTrack {
    /// Ring lanes of the track's channels (-1 = none).
    lanes: [i16; 2],
    num_channels: usize,
    gain: f32,
    /// Sorted by start; `max_end[i]` is the latest end among `clips[..=i]`.
    clips: Vec<Clip>,
    max_end: Vec<i64>,
}

/// A playlist ready for rendering: clips sorted and the used playback
/// channels mapped to ring lanes (only those are carried through the ring).
pub struct PreparedPlaylist {
    tracks: Vec<PreparedTrack>,
    /// Playback channel of each lane.
    pub lanes: Vec<usize>,
    /// Lane of each playback channel (-1 = silent).
    pub lane_of: [i16; NUM_PLAYBACK_CHANNELS],
}

impl PreparedPlaylist {
    pub fn new(p: &Playlist) -> Self {
        let mut used = [false; NUM_PLAYBACK_CHANNELS];
        for t in p.tracks.iter().filter(|t| !t.mute) {
            for c in t.channel..t.channel + t.num_channels.clamp(1, 2) {
                if let Some(u) = used.get_mut(c) {
                    *u = true;
                }
            }
        }
        let lanes: Vec<usize> = (0..NUM_PLAYBACK_CHANNELS).filter(|&c| used[c]).collect();
        let mut lane_of = [-1i16; NUM_PLAYBACK_CHANNELS];
        for (lane, &c) in lanes.iter().enumerate() {
            lane_of[c] = lane as i16;
        }
        let tracks = p
            .tracks
            .iter()
            .filter(|t| !t.mute && t.channel < NUM_PLAYBACK_CHANNELS)
            .map(|t| {
                let num_channels = t.num_channels.clamp(1, 2);
                let lane = |c: usize| lane_of.get(c).copied().unwrap_or(-1);
                let mut clips: Vec<Clip> = t.clips.iter().filter(|c| c.length > 0).cloned().collect();
                clips.sort_by_key(|c| c.start);
                let mut latest = i64::MIN;
                let max_end = clips
                    .iter()
                    .map(|c| {
                        latest = latest.max(c.end());
                        latest
                    })
                    .collect();
                PreparedTrack {
                    lanes: [lane(t.channel), if num_channels == 2 { lane(t.channel + 1) } else { -1 }],
                    num_channels,
                    gain: t.gain,
                    clips,
                    max_end,
                }
            })
            .collect();
        Self { tracks, lanes, lane_of }
    }

    pub fn empty() -> Self {
        Self::new(&Playlist::default())
    }

    /// The files the playlist reads.
    pub fn files(&self) -> impl Iterator<Item = &Path> {
        self.tracks.iter().flat_map(|t| t.clips.iter().map(|c| c.file.as_path()))
    }
}

/// Renders playlists from their files (reader thread). Files are opened on
/// first use and kept open; a file that can't be read plays silence.
#[derive(Default)]
pub struct Renderer {
    files: HashMap<PathBuf, Option<WavReader>>,
    left: Vec<f32>,
    right: Vec<f32>,
}

impl Renderer {
    pub fn new() -> Self {
        Self::default()
    }

    /// Closes the files none of `playlists` uses.
    pub fn retain(&mut self, playlists: &[&PreparedPlaylist]) {
        self.files.retain(|path, _| playlists.iter().any(|p| p.files().any(|f| f == path)));
    }

    /// Renders the timeline range [pos, pos + n) of `p` into `out` (one buffer
    /// per lane of `p`, each at least `n` long), replacing their contents.
    pub fn render(&mut self, p: &PreparedPlaylist, pos: i64, out: &mut [Vec<f32>], n: usize) {
        for o in out.iter_mut() {
            o[..n].fill(0.0);
        }
        let end = pos + n as i64;
        for t in &p.tracks {
            let hi = t.clips.partition_point(|c| c.start < end);
            for i in (0..hi).rev() {
                if t.max_end[i] <= pos {
                    break; // every earlier clip has ended too
                }
                let clip = &t.clips[i];
                if clip.end() > pos {
                    self.render_clip(t, clip, pos, out, n);
                }
            }
        }
    }

    fn render_clip(&mut self, t: &PreparedTrack, clip: &Clip, pos: i64, out: &mut [Vec<f32>], n: usize) {
        let a = pos.max(clip.start);
        let b = (pos + n as i64).min(clip.end());
        if b <= a {
            return;
        }
        let m = (b - a) as usize;
        let reader = self.files.entry(clip.file.clone()).or_insert_with(|| WavReader::open(&clip.file).ok());
        let Some(reader) = reader.as_mut() else { return };
        let stereo_file = reader.info().channels > 1;
        self.left.resize(m, 0.0);
        self.right.resize(m, 0.0);
        let file_frame = clip.file_offset + (a - clip.start);
        if reader.read(file_frame, &mut [&mut self.left[..m], &mut self.right[..m]]).is_err() {
            return;
        }

        let offset = (a - pos) as usize;
        let k0 = a - clip.start;
        let plain =
            (clip.fade_in == 0 || k0 >= clip.fade_in) && (clip.fade_out == 0 || k0 + (m as i64) <= clip.length - clip.fade_out);
        let gain = |i: usize| t.gain * if plain { clip.gain } else { clip.gain_at(k0 + i as i64) };
        let lanes = out.len();
        let lane = |l: i16| usize::try_from(l).ok().filter(|&l| l < lanes);
        if t.num_channels == 1 {
            let Some(l0) = lane(t.lanes[0]) else { return };
            let dst = &mut out[l0][offset..offset + m];
            for (i, d) in dst.iter_mut().enumerate() {
                let x = if stereo_file { 0.5 * (self.left[i] + self.right[i]) } else { self.left[i] };
                *d += gain(i) * x;
            }
        } else {
            for (c, src) in [&self.left, &self.right].into_iter().enumerate() {
                let Some(l) = lane(t.lanes[c]) else { continue };
                let dst = &mut out[l][offset..offset + m];
                for (i, d) in dst.iter_mut().enumerate() {
                    *d += gain(i) * src[i];
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wav::tests::temp_dir;
    use crate::wav::WavWriter;

    fn write(path: &Path, chans: &[Vec<f32>]) {
        let mut w = WavWriter::create(path, chans.len(), 48000).unwrap();
        let refs: Vec<&[f32]> = chans.iter().map(|c| &c[..]).collect();
        w.write_planar(&refs, chans[0].len()).unwrap();
        w.finalize().unwrap();
    }

    fn render(p: &Playlist, pos: i64, n: usize) -> Vec<Vec<f32>> {
        let prepared = PreparedPlaylist::new(p);
        let mut out = vec![vec![0.0; n]; prepared.lanes.len()];
        Renderer::new().render(&prepared, pos, &mut out, n);
        out
    }

    #[test]
    fn fade_shapes() {
        for shape in [FadeShape::Linear, FadeShape::EqualPower, FadeShape::SCurve] {
            assert_eq!(shape.gain(0.0), 0.0);
            assert!((shape.gain(1.0) - 1.0).abs() < 1e-6);
            assert!(shape.gain(0.25) < shape.gain(0.5) && shape.gain(0.5) < shape.gain(0.75));
        }
        assert!((FadeShape::Linear.gain(0.5) - 0.5).abs() < 1e-6);
        assert!((FadeShape::EqualPower.gain(0.5) - std::f32::consts::FRAC_1_SQRT_2).abs() < 1e-6);
        assert!((FadeShape::SCurve.gain(0.5) - 0.5).abs() < 1e-6);
        assert!(FadeShape::SCurve.gain(0.1) < FadeShape::Linear.gain(0.1), "S-curve starts slower");
        assert_eq!(FadeShape::from_i32(0), FadeShape::Linear);
        assert_eq!(FadeShape::from_i32(2), FadeShape::SCurve);
        assert_eq!(FadeShape::from_i32(1), FadeShape::EqualPower);
    }

    #[test]
    fn clip_gain_fades_and_track_gain() {
        let dir = temp_dir("render-fades");
        let file = dir.join("dc.wav");
        write(&file, &[vec![1.0; 1000]]);

        let mut clip = Clip::new(&file, 100, 400);
        clip.file_offset = 10;
        clip.gain = 0.5;
        clip.fade_in = 100;
        clip.fade_in_shape = FadeShape::Linear;
        clip.fade_out = 50;
        clip.fade_out_shape = FadeShape::SCurve;
        let mut track = PlaybackTrack::new(3, 1, vec![clip.clone()]);
        track.gain = 0.5;
        let p = Playlist { tracks: vec![track] };

        let out = render(&p, 0, 600);
        assert_eq!(out.len(), 1, "one lane: playback channel 3");
        let x = &out[0];
        assert!(x[..100].iter().all(|&v| v == 0.0), "before the clip");
        assert_eq!(x[100], 0.0, "a fade in starts at silence");
        assert!((x[150] - 0.25 * 0.5).abs() < 1e-6, "halfway through the linear fade in");
        assert!((x[300] - 0.25).abs() < 1e-6, "clip gain x track gain");
        let k = 400 - 25; // halfway through the fade out
        assert!((x[100 + k] - 0.25 * FadeShape::SCurve.gain(0.5)).abs() < 1e-6);
        assert!((x[499] - 0.25 * FadeShape::SCurve.gain(1.0 / 50.0)).abs() < 1e-6);
        assert!(x[500..].iter().all(|&v| v == 0.0), "after the clip");

        // Rendering in pieces gives the same samples.
        let prepared = PreparedPlaylist::new(&p);
        let mut renderer = Renderer::new();
        let mut piece = vec![vec![0.0; 37]];
        for start in (0..600).step_by(37) {
            let n = 37.min(600 - start);
            renderer.render(&prepared, start as i64, &mut piece, n);
            assert_eq!(&piece[0][..n], &x[start..start + n]);
        }

        // Muted tracks are silent and carry no lane.
        let mut muted = p.clone();
        muted.tracks[0].mute = true;
        assert!(render(&muted, 0, 600).is_empty());
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn overlapping_clips_are_summed_and_crossfade() {
        let dir = temp_dir("render-overlap");
        let a = dir.join("a.wav");
        let b = dir.join("b.wav");
        write(&a, &[vec![0.25; 2000]]);
        write(&b, &[vec![0.5; 2000], vec![-0.5; 2000]]);

        // Two clips overlapping on a mono track; a linear crossfade of equal
        // levels stays flat.
        let mut first = Clip::new(&a, 0, 1000);
        first.fade_out = 200;
        first.fade_out_shape = FadeShape::Linear;
        let mut second = Clip::new(&a, 800, 1000);
        second.fade_in = 200;
        second.fade_in_shape = FadeShape::Linear;
        let crossfade = Playlist { tracks: vec![PlaybackTrack::new(0, 1, vec![second.clone(), first.clone()])] };
        let out = render(&crossfade, 0, 1800);
        for (i, v) in out[0].iter().enumerate().skip(1) {
            assert!((v - 0.25).abs() < 1e-6, "sample {i}: {v}");
        }

        // Plain overlaps add up; two tracks on the same channel add up.
        let plain = Playlist {
            tracks: vec![
                PlaybackTrack::new(0, 1, vec![Clip::new(&a, 0, 100), Clip::new(&a, 50, 100)]),
                PlaybackTrack::new(0, 1, vec![Clip::new(&a, 75, 10)]),
            ],
        };
        let out = render(&plain, 0, 200);
        assert_eq!(out[0][10], 0.25);
        assert_eq!(out[0][60], 0.5);
        assert_eq!(out[0][80], 0.75);
        assert_eq!(out[0][120], 0.25);
        assert_eq!(out[0][160], 0.0);

        // A stereo file on a stereo track keeps its sides; on a mono track it
        // is averaged; a mono file on a stereo track feeds both sides.
        let stereo = Playlist {
            tracks: vec![
                PlaybackTrack::new(4, 2, vec![Clip::new(&b, 0, 10)]),
                PlaybackTrack::new(6, 1, vec![Clip::new(&b, 0, 10)]),
                PlaybackTrack::new(8, 2, vec![Clip::new(&a, 0, 10)]),
            ],
        };
        let prepared = PreparedPlaylist::new(&stereo);
        assert_eq!(prepared.lanes, vec![4, 5, 6, 8, 9]);
        assert_eq!((prepared.lane_of[5], prepared.lane_of[7]), (1, -1));
        let out = render(&stereo, 0, 10);
        assert_eq!((out[0][3], out[1][3], out[2][3], out[3][3], out[4][3]), (0.5, -0.5, 0.0, 0.25, 0.25));

        // A missing file plays silence.
        let missing = Playlist { tracks: vec![PlaybackTrack::new(0, 1, vec![Clip::new(dir.join("gone.wav"), 0, 10)])] };
        assert!(render(&missing, 0, 10)[0].iter().all(|&v| v == 0.0));
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn loop_range_wraps() {
        let l = LoopRange::new(100, 200, true);
        assert_eq!(l.max_run(150, 512), 50);
        assert_eq!(l.max_run(200, 512), 512, "at the end: plays on");
        assert_eq!(l.max_run(20, 50), 50);
        assert_eq!(l.advance(150, 49), 199);
        assert_eq!(l.advance(150, 50), 100);
        assert_eq!(l.advance(150, 75), 125);
        assert_eq!(l.advance(150, 50 + 100 * 7 + 3), 103);
        assert_eq!(l.advance(0, 250), 150, "enters the loop from before it");
        assert_eq!(l.advance(250, 10), 260, "past the end: no wrap");
        assert_eq!(LoopRange::new(100, 200, false).advance(150, 100), 250);
        assert!(!LoopRange::new(200, 100, true).active(), "an empty range never loops");
    }
}
