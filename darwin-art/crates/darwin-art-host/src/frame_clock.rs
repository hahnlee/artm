//! A display clock independent from the host event-pump loop.
//!
//! Android's DisplayEventReceiver keeps producing vsync edges even while the
//! app main thread is busy.  The old host loop derived vsync from the same
//! `Instant` polling loop that services AppKit and Binder, so a long turn also
//! delayed the next frame edge.  This clock only produces bounded, coalesced
//! edges; Android work still runs on the ART owner thread when the edge is
//! consumed.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver, SyncSender, TryRecvError};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

const DISPLAY_INTERVAL: Duration = Duration::from_nanos(16_666_667);

pub(crate) struct FrameClock {
    ticks: Receiver<Instant>,
    stop: Arc<AtomicBool>,
    worker: Option<JoinHandle<()>>,
}

impl FrameClock {
    pub(crate) fn start() -> Self {
        let (sender, ticks) = mpsc::sync_channel(1);
        let stop = Arc::new(AtomicBool::new(false));
        let worker_stop = Arc::clone(&stop);
        let worker = thread::Builder::new()
            .name("darwin-art-display-clock".to_owned())
            .spawn(move || run_clock(sender, worker_stop))
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
        let mut latest = None;
        loop {
            match self.ticks.try_recv() {
                Ok(tick) => latest = Some(tick),
                Err(TryRecvError::Empty | TryRecvError::Disconnected) => break,
            }
        }
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

fn run_clock(sender: SyncSender<Instant>, stop: Arc<AtomicBool>) {
    let mut next = Instant::now();
    while !stop.load(Ordering::Acquire) {
        let now = Instant::now();
        if now < next {
            thread::sleep(next - now);
            continue;
        }
        // Publish one edge and reset from the actual send time.  If the
        // consumer was blocked, do not replay a burst of missed display
        // periods (or spin trying to catch up); the bounded channel and
        // latest-tick consumer intentionally coalesce those periods.
        let _ = sender.try_send(now);
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
        let clock = FrameClock::start();
        thread::sleep(Duration::from_millis(20));
        assert!(clock.take_latest().is_some());
        thread::sleep(Duration::from_millis(40));
        assert!(clock.take_latest().is_some());
        assert!(clock.take_latest().is_none());
    }
}
