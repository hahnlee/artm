//! A display clock independent from the host event-pump loop.
//!
//! Android's DisplayEventReceiver keeps producing vsync edges even while the
//! app main thread is busy.  The old host loop derived vsync from the same
//! `Instant` polling loop that services AppKit and Binder, so a long turn also
//! delayed the next frame edge.  This clock only produces bounded, coalesced
//! edges; Android work still runs on the ART owner thread when the edge is
//! consumed.

use darwin_art_engine::LooperWakeToken;
use darwin_art_engine::ScanoutToken;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

const DISPLAY_INTERVAL: Duration = Duration::from_nanos(16_666_667);

pub(crate) struct FrameClock {
    ticks: Arc<Mutex<TickState>>,
    stop: Arc<AtomicBool>,
    worker: Option<JoinHandle<()>>,
}

struct TickState {
    latest: Option<Instant>,
    pending: bool,
}

impl FrameClock {
    pub(crate) fn start(wake: Option<LooperWakeToken>, scanout: Option<ScanoutToken>) -> Self {
        let ticks = Arc::new(Mutex::new(TickState {
            latest: None,
            pending: false,
        }));
        let stop = Arc::new(AtomicBool::new(false));
        let worker_ticks = Arc::clone(&ticks);
        let worker_stop = Arc::clone(&stop);
        let worker = thread::Builder::new()
            .name("darwin-art-display-clock".to_owned())
            .spawn(move || run_clock(worker_ticks, worker_stop, wake, scanout))
            .expect("display clock thread must start");
        Self {
            ticks,
            stop,
            worker: Some(worker),
        }
    }

    /// Consume all pending edges and return only the newest one.
    ///
    /// A blocked UI thread must not receive a burst of stale frames after it
    /// becomes available; dropping intermediate edges matches Android's
    /// latest-vsync scheduling and keeps the queue bounded.
    pub(crate) fn take_latest(&self) -> Option<Instant> {
        let mut state = self.ticks.lock().expect("display clock state poisoned");
        let latest = state.latest.take();
        state.pending = false;
        latest
    }
}

impl Drop for FrameClock {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Release);
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

fn run_clock(
    ticks: Arc<Mutex<TickState>>,
    stop: Arc<AtomicBool>,
    wake: Option<LooperWakeToken>,
    scanout: Option<ScanoutToken>,
) {
    let mut next = Instant::now();
    while !stop.load(Ordering::Acquire) {
        let now = Instant::now();
        if now < next {
            thread::sleep(next - now);
            continue;
        }
        // Publish the newest edge. A single pending bit makes the wake token
        // edge-triggered: a blocked owner consumes one latest timestamp,
        // while intermediate vsyncs replace the stale value without filling
        // a queue or generating a wake storm.
        let should_wake = {
            let mut state = ticks.lock().expect("display clock state poisoned");
            state.latest = Some(now);
            if state.pending {
                false
            } else {
                state.pending = true;
                true
            }
        };
        if should_wake {
            if let Some(token) = wake {
                token.wake();
            }
        }
        // Scanout is a separate consumer from the ART owner pulse. It only
        // appends a bounded latest-wins AppKit command; Android UI/JNI work
        // stays on the owner while the main actor performs the actual blit.
        if should_wake {
            if let Some(token) = scanout {
                let _ = token.present();
            }
        }
        next = now + DISPLAY_INTERVAL;
    }
}

#[cfg(test)]
mod tests {
    use super::FrameClock;
    use std::thread;
    use std::time::Duration;

    #[test]
    fn clock_is_bounded_and_produces_edges() {
        let clock = FrameClock::start(None, None);
        thread::sleep(Duration::from_millis(20));
        assert!(clock.take_latest().is_some());
        thread::sleep(Duration::from_millis(40));
        assert!(clock.take_latest().is_some());
        assert!(clock.take_latest().is_none());
    }
}
