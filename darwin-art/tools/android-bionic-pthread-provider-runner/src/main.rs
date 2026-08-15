use darwin_art_elf_loader::{
    LoadedElf, ResolveError, ResolvedSymbol, SymbolRequest, SymbolResolver,
};
use std::collections::BTreeSet;
use std::env;
use std::ffi::{c_char, c_int, c_void, CString};
use std::fs;
use std::num::NonZeroUsize;
use std::process::ExitCode;

const EXPECTED: [&str; 11] = [
    "pthread_getspecific",
    "pthread_key_create",
    "pthread_key_delete",
    "pthread_mutex_destroy",
    "pthread_mutex_init",
    "pthread_mutex_lock",
    "pthread_mutex_trylock",
    "pthread_mutex_unlock",
    "pthread_once",
    "pthread_self",
    "pthread_setspecific",
];

unsafe extern "C" {
    fn darwin_art_bionic_pthread_resolve(
        soname: *const c_char,
        symbol: *const c_char,
        version: *const c_char,
    ) -> *mut c_void;
    fn darwin_art_bionic_pthread_provider_reset() -> c_int;
    fn darwin_art_bionic_pthread_capability(capability: *const c_char) -> c_int;
    fn darwin_art_bionic_pthread_key_create(
        key: *mut i32,
        destructor: Option<unsafe extern "C" fn(*mut c_void)>,
    ) -> c_int;
    fn darwin_art_bionic_pthread_key_delete(key: i32) -> c_int;
    fn darwin_art_bionic_pthread_setspecific(key: i32, value: *const c_void) -> c_int;
    fn darwin_art_bionic_pthread_getspecific(key: i32) -> *mut c_void;
}

#[derive(Default)]
struct ClosedResolver {
    requested: BTreeSet<String>,
    addresses: BTreeSet<usize>,
}

impl SymbolResolver for ClosedResolver {
    fn resolve(
        &mut self,
        request: SymbolRequest<'_>,
    ) -> Result<Option<ResolvedSymbol>, ResolveError> {
        if request.needed_libraries != ["libc.so"] {
            return Err(ResolveError::UnknownSoname(
                request.needed_libraries.join(","),
            ));
        }
        let version = request.version.ok_or_else(|| ResolveError::VersionMismatch {
            soname: "libc.so".to_owned(),
            symbol: request.symbol.to_owned(),
            requested: "<unversioned>".to_owned(),
        })?;
        if version.soname != "libc.so"
            || version.name != "LIBC"
            || version.hidden
            || version.flags != 0
        {
            return Err(ResolveError::VersionMismatch {
                soname: version.soname.to_owned(),
                symbol: request.symbol.to_owned(),
                requested: version.name.to_owned(),
            });
        }
        if !EXPECTED.contains(&request.symbol) {
            return Err(ResolveError::Rejected(request.symbol.to_owned()));
        }
        let soname = CString::new("libc.so").unwrap();
        let symbol = CString::new(request.symbol).unwrap();
        let required_version = CString::new("LIBC").unwrap();
        let address = unsafe {
            darwin_art_bionic_pthread_resolve(
                soname.as_ptr(),
                symbol.as_ptr(),
                required_version.as_ptr(),
            )
        } as usize;
        let address = NonZeroUsize::new(address)
            .ok_or_else(|| ResolveError::Rejected(request.symbol.to_owned()))?;
        self.requested.insert(request.symbol.to_owned());
        self.addresses.insert(address.get());
        Ok(Some(unsafe { ResolvedSymbol::new(address) }))
    }
}

fn capability(name: &str) -> bool {
    let name = CString::new(name).unwrap();
    unsafe { darwin_art_bionic_pthread_capability(name.as_ptr()) != 0 }
}

fn expect_closed_resolver_negatives() -> Result<(), Box<dyn std::error::Error>> {
    let libc = CString::new("libc.so")?;
    let symbol = CString::new("pthread_self")?;
    let version = CString::new("LIBC")?;
    let wrong_soname = CString::new("libSystem.B.dylib")?;
    let wrong_version = CString::new("DARWIN")?;
    let unknown = CString::new("pthread_create")?;
    unsafe {
        if !darwin_art_bionic_pthread_resolve(
            wrong_soname.as_ptr(),
            symbol.as_ptr(),
            version.as_ptr(),
        )
        .is_null()
            || !darwin_art_bionic_pthread_resolve(
                libc.as_ptr(),
                symbol.as_ptr(),
                wrong_version.as_ptr(),
            )
            .is_null()
            || !darwin_art_bionic_pthread_resolve(
                libc.as_ptr(),
                unknown.as_ptr(),
                version.as_ptr(),
            )
            .is_null()
        {
            return Err("closed resolver accepted a fallback".into());
        }
    }
    Ok(())
}

fn run() -> Result<(), Box<dyn std::error::Error>> {
    let mut arguments = env::args_os();
    let _program = arguments.next();
    let fixture_path = arguments.next().ok_or("missing Android pthread fixture")?;
    if arguments.next().is_some() {
        return Err("usage: android-bionic-pthread-provider-runner FIXTURE.so".into());
    }
    expect_closed_resolver_negatives()?;
    let bytes = fs::read(fixture_path)?;
    let mut resolver = ClosedResolver::default();
    let mut image = LoadedElf::load_with_resolver(&bytes, &mut resolver)?;
    if image.needed_libraries() != ["libc.so"] {
        return Err("fixture dependency namespace is not exactly libc.so".into());
    }
    image.run_initializers()?;
    if image.call_exported_i32("pthread_fixture_setup")? != 0 {
        return Err("Android fixture setup failed".into());
    }
    if resolver.requested != EXPECTED.into_iter().map(str::to_owned).collect() {
        return Err(format!("resolver import set mismatch: {:?}", resolver.requested).into());
    }
    if resolver.addresses.len() != EXPECTED.len() {
        return Err("provider resolver returned aliased or zero addresses".into());
    }

    let worker_address = image.lookup_exported("pthread_fixture_worker")?;
    let mut threads = Vec::new();
    for _ in 0..8 {
        threads.push(std::thread::spawn(move || {
            let worker: unsafe extern "C" fn() -> i32 =
                unsafe { std::mem::transmute(worker_address) };
            unsafe { worker() }
        }));
    }
    for thread in threads {
        let result = thread.join().map_err(|_| "worker panicked")?;
        if result != 0 {
            return Err(format!("Android fixture worker failed: {result}").into());
        }
    }
    let lookup_race = image.lookup_exported("pthread_fixture_mutex_lookup_race")?;
    let destroy_race = image.lookup_exported("pthread_fixture_mutex_destroy_race")?;
    let mut race_threads = Vec::new();
    for _ in 0..8 {
        race_threads.push(std::thread::spawn(move || {
            let function: unsafe extern "C" fn() -> i32 =
                unsafe { std::mem::transmute(lookup_race) };
            unsafe { function() }
        }));
    }
    race_threads.push(std::thread::spawn(move || {
        let function: unsafe extern "C" fn() -> i32 =
            unsafe { std::mem::transmute(destroy_race) };
        unsafe { function() }
    }));
    for thread in race_threads {
        let result = thread.join().map_err(|_| "destroy/lookup race worker panicked")?;
        if result != 0 {
            return Err(format!("destroy/lookup race failed: {result}").into());
        }
    }
    let finish = image.call_exported_i32("pthread_fixture_finish")?;
    if finish != 0 {
        return Err(format!("Android fixture finish failed: {finish}").into());
    }
    drop(image);
    if unsafe { darwin_art_bionic_pthread_provider_reset() } != 0 {
        return Err("provider retained a live key or mutex".into());
    }
    // Bionic exposes only valid-bit|slot. Its generation is internal key-data
    // state, so use of a deleted integer key is POSIX-undefined and aliases a
    // newly allocated key when that slot is reused. Prove that exact ABI rather
    // than inventing guest-visible generation bits.
    let mut stale_key = 0;
    let mut reused_key = 0;
    unsafe {
        if darwin_art_bionic_pthread_key_create(&mut stale_key, None) != 0
            || darwin_art_bionic_pthread_key_delete(stale_key) != 0
            || darwin_art_bionic_pthread_key_create(&mut reused_key, None) != 0
            || stale_key != reused_key
            || darwin_art_bionic_pthread_setspecific(
                stale_key,
                0x1234usize as *const c_void,
            ) != 0
            || darwin_art_bionic_pthread_getspecific(reused_key) as usize != 0x1234
            || darwin_art_bionic_pthread_setspecific(reused_key, std::ptr::null()) != 0
            || darwin_art_bionic_pthread_key_delete(reused_key) != 0
            || darwin_art_bionic_pthread_provider_reset() != 0
        {
            return Err("Bionic stale-key slot-reuse semantics mismatch".into());
        }
    }
    for supported in [
        "thread-identity-token",
        "tls-key-destructor",
        "once-private",
        "mutex-normal-private",
    ] {
        if !capability(supported) {
            return Err(format!("missing capability {supported}").into());
        }
    }
    for unsupported in [
        "fork",
        "robust",
        "pshared",
        "priority-inheritance",
        "recursive",
        "errorcheck",
        "cancellation",
    ] {
        if capability(unsupported) {
            return Err(format!("unsupported capability escaped: {unsupported}").into());
        }
    }
    println!(
        "android-bionic-pthread-provider: ELF=executed resolver=libc.so@LIBC imports=11"
    );
    println!(
        "pthread-self=stable+unique tls-destructor=2-pass once=1 mutex=8x2000-contention destroy-lookup=race-safe"
    );
    println!(
        "opaque-layout=side-table no-Darwin-reinterpret stale-key=reuse-alias(Bionic-undefined) unsupported=fork+robust+pshared+PI+recursive+errorcheck"
    );
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("android-bionic-pthread-provider: {error}");
            ExitCode::from(2)
        }
    }
}
