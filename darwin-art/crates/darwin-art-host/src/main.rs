use darwin_art_host::{RunOptions, run};
use std::env;
use std::error::Error;
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
            "usage: {} [--window-seconds SECONDS] LIBDARWIN_ART CORE_OJ_JAR \
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
    println!(
        "ART Android framework: ProbeActivity().probeValue()={}",
        outcome.process.activity_probe_result
    );
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
