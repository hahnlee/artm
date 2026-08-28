use darwin_art_host::{RunOptions, run, run_service_child};
use std::env;
use std::error::Error;
use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::PathBuf;

fn main() {
    if let Err(error) = main_result() {
        eprintln!("darwin-art-host: {error}");
        std::process::exit(1);
    }
}

fn main_result() -> Result<(), Box<dyn Error>> {
    if let Ok(delay) = env::var("DARWIN_ART_DEBUG_ATTACH_DELAY_MS") {
        std::thread::sleep(std::time::Duration::from_millis(delay.parse()?));
    }
    // The profile daemon owns the shared Android volume. Every application
    // and service process holds its own connection for its entire lifetime.
    let _profile_lease = darwin_art_profile::ProfileLease::connect_from_environment()?;
    let mut arguments = env::args_os();
    let program = arguments.next().unwrap_or_else(|| "darwin-art-host".into());
    let mut values = arguments.collect::<Vec<_>>();
    if values
        .first()
        .is_some_and(|value| value == "--service-child")
    {
        if values.len() != 2 {
            return Err("--service-child requires exactly one control fd".into());
        }
        let control_fd = values[1].to_string_lossy().parse::<i32>()?;
        return run_service_child(control_fd).map_err(Into::into);
    }
    let frame_output = if values.first().is_some_and(|value| value == "--frame-ppm") {
        if values.len() < 2 {
            return Err("--frame-ppm requires a path".into());
        }
        let output = PathBuf::from(&values[1]);
        values.drain(..2);
        Some(output)
    } else {
        None
    };
    let visible_seconds = if values
        .first()
        .is_some_and(|value| value == "--window-seconds")
    {
        if values.len() < 2 {
            return Err("--window-seconds requires a value".into());
        }
        let seconds = values[1].to_string_lossy().parse::<f64>()?;
        values.drain(..2);
        seconds
    } else {
        0.0
    };
    if values.len() != 6 {
        return Err(format!(
            "usage: {} [--frame-ppm PATH] [--window-seconds SECONDS] LIBDARWIN_ART CORE_OJ_JAR \
             CORE_LIBART_JAR FRAMEWORK_JAR CORE_ICU4J_JAR APP_DEX",
            PathBuf::from(program).display()
        )
        .into());
    }
    let options = RunOptions {
        library: PathBuf::from(&values[0]),
        core_oj_jar: PathBuf::from(&values[1]),
        core_libart_jar: PathBuf::from(&values[2]),
        framework_jar: PathBuf::from(&values[3]),
        core_icu4j_jar: PathBuf::from(&values[4]),
        app_dex: PathBuf::from(&values[5]),
        heap_initial_bytes: 64 * 1024 * 1024,
        heap_maximum_bytes: 256 * 1024 * 1024,
        visible_seconds,
        // Android application VMs are process-scoped zygote children. The OS
        // terminates that process instead of calling DestroyJavaVM, which is
        // unsafe for Chromium's still-live native task runners.
        terminate_android_process: env::var_os("DARWIN_ART_APK_APP_PACKAGE").is_some(),
    };
    let outcome = run(&options)?;
    if let Some(path) = frame_output {
        let frame = outcome
            .last_frame
            .as_ref()
            .ok_or("runtime did not produce a frame")?;
        let mut output = BufWriter::new(File::create(path)?);
        write!(output, "P6\n{} {}\n255\n", frame.width, frame.height)?;
        for pixel in &frame.argb_pixels {
            output.write_all(&[
                ((pixel >> 16) & 0xff) as u8,
                ((pixel >> 8) & 0xff) as u8,
                (pixel & 0xff) as u8,
            ])?;
        }
        output.flush()?;
    }
    println!("ART Darwin Runtime::Create: ok");
    println!("ART Darwin app ClassLoader: PathClassLoader");
    println!(
        "ART Darwin DEX interpreter: Hello.answer()={}",
        outcome.process.hello_answer
    );
    println!(
        "ART Darwin JNI: hostPageSize()={} nativeRoundTrip()={}",
        unsafe { getpagesize() },
        outcome.process.native_round_trip
    );
    println!(
        "ART runtime native: System.arraycopy()={}",
        outcome.process.arraycopy_result
    );
    if let (Ok(package), Ok(activity)) = (
        env::var("DARWIN_ART_APK_APP_PACKAGE"),
        env::var("DARWIN_ART_APK_APP_ACTIVITY"),
    ) {
        let render_scale = match env::var("DARWIN_ART_WINDOW_SCALE").as_deref() {
            Ok("2") => 2,
            Ok("1") | Err(_) => 1,
            Ok(_) => return Err("DARWIN_ART_WINDOW_SCALE must be 1 or 2".into()),
        };
        let expected_width = 360 * render_scale;
        let expected_height = 640 * render_scale;
        let widget_expected = env::var("DARWIN_ART_APK_APP_EXPECT_WIDGETS").as_deref() == Ok("1");
        let native_loaded = env::var("DARWIN_ART_APK_APP_NATIVE_PATH")
            .map(|path| !path.is_empty())
            .unwrap_or(false);
        let widget = if widget_expected {
            " widgets=framework-owned"
        } else {
            ""
        };
        if let Some(frame) = outcome.last_frame.as_ref() {
            let all_opaque = frame
                .argb_pixels
                .iter()
                .all(|pixel| pixel & 0xff00_0000 == 0xff00_0000);
            if frame.width != expected_width
                || frame.height != expected_height
                || outcome.frames_presented == 0
                || !all_opaque
            {
                return Err("APK Activity frame did not match its opaque frame contract".into());
            }
            let pixel_count = frame.width as u64 * frame.height as u64;
            println!(
                "ART Android APK: package={package} launcher={activity} classes.dex=APK native={} pixels={pixel_count}/opaque{widget}",
                if native_loaded { 1 } else { 0 }
            );
        } else {
            // The production graphics path renders directly into the
            // CAMetalLayer drawable. It intentionally has no CPU frame
            // callback or readback, so GPU acceptance validates presentation
            // count and the dimensions recorded by the Android frame probe.
            if outcome.frames_presented == 0
                || outcome.process.frame_width != expected_width
                || outcome.process.frame_height != expected_height
            {
                return Err(
                    "APK Activity GPU presentation did not match its frame contract".into(),
                );
            }
            println!(
                "ART Android APK: package={package} launcher={activity} classes.dex=APK native={} gpu=direct drawable={expected_width}x{expected_height}{widget}",
                if native_loaded { 1 } else { 0 }
            );
        }
    } else {
        println!(
            "ART Android framework: ProbeActivity().probeValue()={}",
            outcome.process.activity_probe_result
        );
    }
    println!("ART Android window: Activity.attach()=PhoneWindow+DecorView");
    println!(
        "ART Android view: Activity.setContentView()->DecorView.draw(Canvas)={}x{}",
        outcome.process.frame_width, outcome.process.frame_height
    );
    println!(
        "ART Android lifecycle: Activity.onCreate()={}",
        outcome.process.lifecycle_result
    );
    println!("ART Darwin launcher: main(String[])=ok");
    Ok(())
}

unsafe extern "C" {
    fn getpagesize() -> i32;
}
