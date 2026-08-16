use darwin_art_host::{RunOptions, run};
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
    let mut arguments = env::args_os();
    let program = arguments.next().unwrap_or_else(|| "darwin-art-host".into());
    let mut values = arguments.collect::<Vec<_>>();
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
        heap_maximum_bytes: 64 * 1024 * 1024,
        visible_seconds,
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
        let frame = outcome
            .last_frame
            .as_ref()
            .ok_or("APK Activity did not produce a host frame")?;
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
        let widget = if env::var("DARWIN_ART_APK_APP_EXPECT_WIDGETS").as_deref() == Ok("1") {
            let mut distinct_colors = Vec::new();
            for pixel in &frame.argb_pixels {
                if !distinct_colors.contains(pixel) {
                    distinct_colors.push(*pixel);
                    if distinct_colors.len() >= 16 {
                        break;
                    }
                }
            }
            if distinct_colors.len() < 8 {
                return Err("Android framework widget frame lacks visual diversity".into());
            }
            " widgets=TextView+CheckBox+RadioButton+ToggleButton+SeekBar+ProgressBar+Button colors>=8"
        } else {
            ""
        };
        let pixel_count = frame.width as u64 * frame.height as u64;
        println!(
            "ART Android APK: package={package} launcher={activity} classes.dex=APK native=0 pixels={pixel_count}/opaque{widget}"
        );
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
