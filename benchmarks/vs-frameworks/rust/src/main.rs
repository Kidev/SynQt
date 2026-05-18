// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

//! Rust's column of the live-path comparison. Tokio plus tokio-tungstenite, no framework.
//! The same shape as node/live-bare.mjs and go/live.go, so the three files can
//! be read side by side and the only difference is the runtime.
//!
//! No framework, because this is a floor column. It exists so that "SynQt is fast for a Qt
//! thing" and "SynQt is fast" can be told apart, and a framework here would be measuring the
//! framework.
//!
//! Publisher and subscribers share one process, as they do in every other column, so this
//! times an interval on one monotonic clock rather than across two. See COLUMN-CONTRACT.md.
//!
//! Build it `--release` or the number is meaningless. A debug build measures the absence of
//! the optimiser.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::time::{Duration, Instant};

use futures_util::{SinkExt, StreamExt};
use serde::Serialize;
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::{Mutex, Notify};
use tokio_tungstenite::WebSocketStream;
use tokio_tungstenite::tungstenite::Message;

/// The process's monotonic origin. Every stamp is microseconds since this, on both ends, so
/// what is reported is an interval.
///
/// `Instant` is the monotonic clock. `SystemTime` is not, and using it here would be the
/// same defect as reading a wall clock in any other column. A clock step during a run would
/// land in the distribution as a propagation time.
struct Clock {
    origin: Instant,
}

impl Clock {
    fn new() -> Self {
        Self { origin: Instant::now() }
    }

    fn micros(&self) -> u64 {
        self.origin.elapsed().as_micros() as u64
    }
}

/// Resident set size as the OS reports it, which is what every other column reports. A
/// runtime's own allocator accounting is a different measurement and does not go in this
/// field (COLUMN-CONTRACT.md, "The output").
fn resident_bytes() -> u64 {
    let status = match std::fs::read_to_string("/proc/self/status") {
        Ok(text) => text,
        Err(_) => return 0,
    };
    for line in status.lines() {
        if let Some(rest) = line.strip_prefix("VmRSS:") {
            let kib: u64 = rest
                .split_whitespace()
                .next()
                .and_then(|value| value.parse().ok())
                .unwrap_or(0);
            return kib * 1024;
        }
    }
    0
}

/// Process CPU in milliseconds, user plus system. The same figure every other column
/// reports, and the one a host is sized on.
fn cpu_milliseconds() -> f64 {
    let mut usage: libc::rusage = unsafe { std::mem::zeroed() };
    // SAFETY: `usage` is a valid, fully initialised rusage and getrusage only writes into
    // it. This is the one call in the file that leaves safe Rust, and it is here because the
    // standard library exposes no process CPU time.
    if unsafe { libc::getrusage(libc::RUSAGE_SELF, &mut usage) } != 0 {
        return 0.0;
    }
    let micros = |t: libc::timeval| t.tv_sec as f64 * 1e6 + t.tv_usec as f64;
    (micros(usage.ru_utime) + micros(usage.ru_stime)) / 1000.0
}

/// The wall-clock moment this run was recorded, as RFC 3339.
///
/// Wall clock, and the only wall clock in the file. It dates the baseline and is
/// never subtracted from anything. The propagation numbers come off `Instant`.
///
/// Hand-rolled rather than pulling in a date crate for one string. The days-to-civil
/// conversion is Howard Hinnant's, which is the one every implementation uses.
fn recorded_now() -> String {
    let epoch = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0);
    let (days, rest) = (epoch.div_euclid(86_400), epoch.rem_euclid(86_400));
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + if month <= 2 { 1 } else { 0 };
    format!(
        "{year:04}-{month:02}-{day:02}T{:02}:{:02}:{:02}Z",
        rest / 3600,
        (rest % 3600) / 60,
        rest % 60
    )
}

#[derive(Serialize)]
struct Distribution {
    unit: &'static str,
    samples: usize,
    min: f64,
    p50: f64,
    p95: f64,
    p99: f64,
    max: f64,
    mean: f64,
}

/// Linear interpolation between ranks, exactly as measure.mjs does it. A nearest-rank
/// percentile would disagree with every other column at small sample counts, which is the
/// kind of difference that reads as a fact about the runtime.
fn summarize(samples: &[f64]) -> Distribution {
    let mut sorted = samples.to_vec();
    sorted.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let at = |fraction: f64| -> f64 {
        if sorted.is_empty() {
            return 0.0;
        }
        let rank = fraction * (sorted.len() - 1) as f64;
        let low = rank.floor() as usize;
        let high = rank.ceil() as usize;
        if low == high {
            return sorted[low];
        }
        sorted[low] + (rank - low as f64) * (sorted[high] - sorted[low])
    };
    let total: f64 = sorted.iter().sum();
    Distribution {
        unit: "ms",
        samples: sorted.len(),
        min: sorted.first().copied().unwrap_or(0.0),
        p50: at(0.5),
        p95: at(0.95),
        p99: at(0.99),
        max: sorted.last().copied().unwrap_or(0.0),
        mean: if sorted.is_empty() { 0.0 } else { total / sorted.len() as f64 },
    }
}

#[derive(Serialize)]
struct Entry {
    subscribers: usize,
    propagation: Distribution,
    throughput_msgs_per_sec: f64,
    cpu_ms_per_1k: f64,
    rss_bytes_per_conn: f64,
    rss_total_bytes: u64,
    delivered: usize,
    expected: usize,
}

#[derive(Serialize)]
struct Document {
    benchmark: &'static str,
    stack: &'static str,
    path: &'static str,
    rust_version: String,
    host: String,
    arch: &'static str,
    recorded: String,
    rss_available: bool,
    hz: u32,
    saturated: bool,
    seconds: u64,
    payload_bytes: usize,
    sweep: Vec<Entry>,
}

/// The sinks of every accepted subscriber. A broadcast is built once and handed to each of
/// them in turn, as every other column does. Writing a freshly framed message per subscriber
/// would measure the framing N times over.
type Sinks = Arc<Mutex<Vec<futures_util::stream::SplitSink<WebSocketStream<TcpStream>, Message>>>>;

struct Args {
    subscribers: String,
    seconds: u64,
    hz: u32,
    payload: usize,
    saturate: bool,
    out: String,
}

/// `--flag value` and bare `--flag`, the same shape measure.mjs parses, so one set of
/// arguments from run-bench.sh means the same thing to every column.
fn parse_args() -> Args {
    let mut args = Args {
        subscribers: "10,50,100,250".into(),
        seconds: 5,
        hz: 30,
        payload: 256,
        saturate: false,
        out: String::new(),
    };
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let mut index = 0;
    while index < argv.len() {
        let flag = argv[index].clone();
        index += 1;
        let name = match flag.strip_prefix("--") {
            Some(name) => name,
            None => continue,
        };
        let value = argv.get(index).filter(|next| !next.starts_with("--")).cloned();
        if value.is_some() {
            index += 1;
        }
        match (name, value) {
            ("subscribers", Some(value)) => args.subscribers = value,
            ("seconds", Some(value)) => args.seconds = value.parse().unwrap_or(5).max(1),
            ("hz", Some(value)) => args.hz = value.parse().unwrap_or(30).max(1),
            ("payload", Some(value)) => args.payload = value.parse().unwrap_or(256),
            ("out", Some(value)) => args.out = value,
            ("saturate", value) => {
                args.saturate = value.map(|v| v != "false").unwrap_or(true);
            }
            _ => {}
        }
    }
    args
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args = parse_args();
    let clock = Arc::new(Clock::new());
    let sizes: Vec<usize> = args
        .subscribers
        .split(',')
        .filter_map(|part| part.trim().parse::<usize>().ok())
        .filter(|size| *size > 0)
        .collect();

    let rate = if args.saturate {
        "saturation".to_string()
    } else {
        format!("{} Hz", args.hz)
    };
    println!(
        "Rust (bare) live path: {}s at {}, {} byte payload, subscribers {}",
        args.seconds, rate, args.payload, args.subscribers
    );

    let payload = vec![b'x'; args.payload];
    let baseline_rss = resident_bytes();
    let mut sweep = Vec::new();

    for subscriber_count in sizes {
        let listener = TcpListener::bind("127.0.0.1:0").await?;
        let port = listener.local_addr()?.port();
        let sinks: Sinks = Arc::new(Mutex::new(Vec::new()));
        let accepted = Arc::new(AtomicUsize::new(0));

        let accept_sinks = Arc::clone(&sinks);
        let accept_count = Arc::clone(&accepted);
        let acceptor = tokio::spawn(async move {
            while let Ok((stream, _)) = listener.accept().await {
                let socket = match tokio_tungstenite::accept_async(stream).await {
                    Ok(socket) => socket,
                    Err(_) => continue,
                };
                // The subscribers in this workload never send, so the read half of an
                // accepted socket is dropped rather than driven. Nothing arrives on it, and
                // a reader task per subscriber on the server side would be work no other
                // column does.
                let (sink, _reader) = socket.split();
                accept_sinks.lock().await.push(sink);
                accept_count.fetch_add(1, Ordering::Relaxed);
            }
        });

        let measuring = Arc::new(AtomicBool::new(false));
        let delivered_this_frame = Arc::new(AtomicUsize::new(0));
        let fleet = Arc::new(Notify::new());
        // One sample buffer per subscriber rather than one shared behind a lock: N tasks
        // contending on one mutex inside the receive path would be measuring the mutex, and
        // the mutex is the harness rather than the runtime.
        let mut buffers = Vec::with_capacity(subscriber_count);
        let mut readers = Vec::with_capacity(subscriber_count);

        for _ in 0..subscriber_count {
            let (socket, _) =
                tokio_tungstenite::connect_async(format!("ws://127.0.0.1:{port}/")).await?;
            let (_sink, mut stream) = socket.split();
            let samples = Arc::new(Mutex::new(Vec::<f64>::new()));
            buffers.push(Arc::clone(&samples));
            let reader_clock = Arc::clone(&clock);
            let reader_measuring = Arc::clone(&measuring);
            let reader_frame = Arc::clone(&delivered_this_frame);
            let reader_fleet = Arc::clone(&fleet);
            readers.push(tokio::spawn(async move {
                while let Some(Ok(message)) = stream.next().await {
                    let frame = match message {
                        Message::Binary(bytes) if bytes.len() >= 8 => bytes,
                        _ => continue,
                    };
                    if !reader_measuring.load(Ordering::Relaxed) {
                        continue;
                    }
                    let stamp = u64::from_le_bytes(frame[..8].try_into().unwrap());
                    samples
                        .lock()
                        .await
                        .push((reader_clock.micros().saturating_sub(stamp)) as f64 / 1000.0);
                    if reader_frame.fetch_add(1, Ordering::Relaxed) + 1 >= subscriber_count {
                        reader_fleet.notify_one();
                    }
                }
            }));
        }

        let deadline = Instant::now() + Duration::from_secs(30);
        while accepted.load(Ordering::Relaxed) < subscriber_count && Instant::now() < deadline {
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
        if accepted.load(Ordering::Relaxed) < subscriber_count {
            eprintln!("subscribers did not come up at N={subscriber_count}");
            std::process::exit(1);
        }
        let connected_rss = resident_bytes();

        let publish = |clock: &Clock, payload: &[u8]| -> Vec<u8> {
            let mut frame = Vec::with_capacity(8 + payload.len());
            frame.extend_from_slice(&clock.micros().to_le_bytes());
            frame.extend_from_slice(payload);
            frame
        };
        let broadcast = |frame: Vec<u8>, sinks: Sinks| async move {
            let mut held = sinks.lock().await;
            for sink in held.iter_mut() {
                let _ = sink.send(Message::Binary(frame.clone().into())).await;
            }
        };

        // Warm up before measuring, for the same reason every other column does: the first
        // frames pay for lazily grown buffers on both ends and would otherwise be the whole
        // tail. `measuring` is still false, so nothing here reaches the statistics.
        let warmup_ticks = args.hz.min(30);
        for _ in 0..warmup_ticks {
            broadcast(publish(&clock, &payload), Arc::clone(&sinks)).await;
            tokio::time::sleep(Duration::from_micros(1_000_000 / args.hz as u64)).await;
        }
        measuring.store(true, Ordering::Relaxed);

        let mut ticks: usize = 0;
        let cpu_before = cpu_milliseconds();
        let started_at = Instant::now();
        if args.saturate {
            // Closed loop: publish, wait for the whole fleet to have it, publish again.
            // Open-looping at "maximum rate" would measure the send buffer rather than the
            // capacity, and the SynQt column cannot open-loop at all, so this is the mode
            // every column shares.
            while started_at.elapsed() < Duration::from_secs(args.seconds) {
                delivered_this_frame.store(0, Ordering::Relaxed);
                let landed = fleet.notified();
                tokio::pin!(landed);
                broadcast(publish(&clock, &payload), Arc::clone(&sinks)).await;
                ticks += 1;
                if delivered_this_frame.load(Ordering::Relaxed) < subscriber_count {
                    let waited =
                        tokio::time::timeout(Duration::from_secs(5), &mut landed).await;
                    if waited.is_err() {
                        eprintln!(
                            "a frame never reached every subscriber at N={subscriber_count}"
                        );
                        std::process::exit(1);
                    }
                }
            }
        } else {
            ticks = args.seconds as usize * args.hz as usize;
            for tick in 0..ticks {
                broadcast(publish(&clock, &payload), Arc::clone(&sinks)).await;
                let due = Duration::from_micros(
                    (tick as u64 + 1) * 1_000_000 / args.hz as u64,
                );
                let elapsed = started_at.elapsed();
                if due > elapsed {
                    tokio::time::sleep(due - elapsed).await;
                }
            }
            // Let what is in flight land, or the tail of every run reads as loss that is
            // the harness stopping first.
            tokio::time::sleep(Duration::from_millis(500)).await;
        }
        let elapsed_seconds = started_at.elapsed().as_secs_f64();
        let cpu_ms = cpu_milliseconds() - cpu_before;
        measuring.store(false, Ordering::Relaxed);

        let mut propagation = Vec::new();
        for buffer in &buffers {
            propagation.extend_from_slice(&buffer.lock().await);
        }
        let delivered = propagation.len();
        let expected = ticks * subscriber_count;
        let rss_per_conn = if subscriber_count > 0 && connected_rss > baseline_rss {
            (connected_rss - baseline_rss) as f64 / subscriber_count as f64
        } else {
            0.0
        };
        let entry = Entry {
            subscribers: subscriber_count,
            propagation: summarize(&propagation),
            throughput_msgs_per_sec: delivered as f64 / elapsed_seconds.max(0.001),
            cpu_ms_per_1k: if delivered > 0 {
                cpu_ms * 1000.0 / delivered as f64
            } else {
                0.0
            },
            rss_bytes_per_conn: rss_per_conn,
            rss_total_bytes: connected_rss,
            delivered,
            expected,
        };
        println!(
            "  N={}  p50 {:.3} ms  p99 {:.3} ms  {:.0} msg/s  delivered {}/{}",
            entry.subscribers,
            entry.propagation.p50,
            entry.propagation.p99,
            entry.throughput_msgs_per_sec,
            entry.delivered,
            entry.expected
        );
        sweep.push(entry);

        for reader in readers {
            reader.abort();
        }
        {
            let mut held = sinks.lock().await;
            for sink in held.iter_mut() {
                let _ = sink.close().await;
            }
            held.clear();
        }
        acceptor.abort();
        tokio::time::sleep(Duration::from_millis(100)).await;
    }

    let document = Document {
        benchmark: "vs-frameworks-live",
        stack: "rust-bare",
        path: "tokio + tokio-tungstenite",
        // Stamped by build.rs from the rustc that compiled this, not from
        // Cargo.toml's `rust-version`, which is the oldest allowed and not the one
        // that produced the number.
        rust_version: env!("SYNQT_RUSTC_VERSION").to_string(),
        host: format!(
            "linux {}",
            std::fs::read_to_string("/proc/sys/kernel/osrelease")
                .unwrap_or_default()
                .trim()
        ),
        arch: std::env::consts::ARCH,
        recorded: recorded_now(),
        rss_available: true,
        hz: if args.saturate { 0 } else { args.hz },
        saturated: args.saturate,
        seconds: args.seconds,
        payload_bytes: args.payload,
        sweep,
    };
    let encoded = serde_json::to_string_pretty(&document)?;
    if !args.out.is_empty() {
        std::fs::write(&args.out, format!("{encoded}\n"))?;
        println!("\nwrote {}", args.out);
    }
    Ok(())
}
