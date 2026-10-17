//! Prints which parts of the WING parameter tree map to Live DAW.
//!
//! cargo run -p livedaw-wing --example wing_coverage

fn main() {
    print!("{}", livedaw_wing::map::coverage().report());
}
