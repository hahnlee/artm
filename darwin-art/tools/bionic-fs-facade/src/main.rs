use bionic_fs_facade::Facade;
use darwin_art_elf_loader::{
    LoadedElf, ResolveError, ResolvedSymbol, SymbolRequest, SymbolResolver,
};
use std::env;
use std::ffi::{c_char, c_int, c_void};
use std::fs::{self, File};
use std::num::NonZeroUsize;
use std::os::fd::AsRawFd;
use std::path::PathBuf;
use std::process::ExitCode;
use std::sync::Arc;

unsafe extern "C" {
    fn __error() -> *mut i32;
    fn darwin_art_bionic_fs_resolve(name: *const c_char) -> Option<unsafe extern "C" fn()>;
    fn darwin_art_bionic_errno_resolve(name: *const c_char) -> Option<unsafe extern "C" fn()>;
    #[link_name = "fpathconf"]
    fn host_fpathconf(fd: c_int, name: c_int) -> i64;
    #[link_name = "fstatvfs"]
    fn host_fstatvfs(fd: c_int, status: *mut HostStatvfs) -> c_int;
}

#[repr(C)]
#[derive(Default)]
struct HostStatvfs {
    f_bsize: u64,
    f_frsize: u64,
    f_blocks: u64,
    f_bfree: u64,
    f_bavail: u64,
    f_files: u64,
    f_ffree: u64,
    f_favail: u64,
    f_fsid: u64,
    f_flag: u64,
    f_namemax: u64,
}

const _: () = assert!(size_of::<HostStatvfs>() == 88);

const DARWIN_PC_2_SYMLINKS: c_int = 15;
const PORTABLE_ST_FLAGS: u64 = 0x0003;
const ANDROID_ST_RDONLY: u64 = 0x0001;

struct ClosedResolver;

impl ClosedResolver {
    fn resolved(address: unsafe extern "C" fn()) -> ResolvedSymbol {
        let address = NonZeroUsize::new(address as usize).expect("function address is non-zero");
        // SAFETY: each provider is manifest-checked against its Android arm64 import ABI
        // and remains linked for the loaded image lifetime.
        unsafe { ResolvedSymbol::new(address) }
    }

    fn lookup(name: &str) -> Option<unsafe extern "C" fn()> {
        let bytes = match name {
            "__errno" => c"__errno",
            "chdir" => c"chdir",
            "close" => c"close",
            "closedir" => c"closedir",
            "fchmod" => c"fchmod",
            "fchmodat" => c"fchmodat",
            "fdopendir" => c"fdopendir",
            "fstat" => c"fstat",
            "ftruncate" => c"ftruncate",
            "getcwd" => c"getcwd",
            "isatty" => c"isatty",
            "link" => c"link",
            "lstat" => c"lstat",
            "mkdir" => c"mkdir",
            "open" => c"open",
            "openat" => c"openat",
            "opendir" => c"opendir",
            "pathconf" => c"pathconf",
            "read" => c"read",
            "readdir" => c"readdir",
            "readlink" => c"readlink",
            "realpath" => c"realpath",
            "remove" => c"remove",
            "rename" => c"rename",
            "stat" => c"stat",
            "statvfs" => c"statvfs",
            "symlink" => c"symlink",
            "truncate" => c"truncate",
            "unlinkat" => c"unlinkat",
            "utimensat" => c"utimensat",
            _ => return None,
        };
        // SAFETY: both closed C resolvers read only the static NUL-terminated name.
        unsafe {
            darwin_art_bionic_fs_resolve(bytes.as_ptr())
                .or_else(|| darwin_art_bionic_errno_resolve(bytes.as_ptr()))
        }
    }
}

impl SymbolResolver for ClosedResolver {
    fn resolve(
        &mut self,
        request: SymbolRequest<'_>,
    ) -> Result<Option<ResolvedSymbol>, ResolveError> {
        if request.version.is_some() {
            return Err(ResolveError::Rejected(
                "filesystem fixture imports must be unversioned".to_owned(),
            ));
        }
        Ok(Self::lookup(request.symbol).map(Self::resolved))
    }
}

fn run() -> Result<(), Box<dyn std::error::Error>> {
    let mut arguments = env::args_os();
    let _program = arguments.next();
    let fixture = arguments
        .next()
        .ok_or("missing Android filesystem fixture")?;
    let root = arguments.next().ok_or("missing mount root directory")?;
    if arguments.next().is_some() {
        return Err("usage: bionic-fs-facade <android-arm64-fixture.so> <mount-root>".into());
    }
    let metadata = fs::metadata(&root)?;
    if !metadata.is_dir() {
        return Err("mount root is not a directory".into());
    }
    let root = PathBuf::from(root);
    let facade = Arc::new(Facade::new(File::open(&root)?, b"/system", b"/system")?);
    let _activation = facade.activate();
    let bytes = fs::read(fixture)?;
    let mut resolver = ClosedResolver;
    let mut image = LoadedElf::load_with_resolver(&bytes, &mut resolver)?;
    image.run_initializers()?;
    let host_file = File::open(root.join("etc/payload.txt"))?;
    // SAFETY: the descriptor is live and Darwin selector 15 is the semantic
    // _PC_2_SYMLINKS reference. Android selector 7 reaches the translated path.
    let host_symlinks = unsafe { host_fpathconf(host_file.as_raw_fd(), DARWIN_PC_2_SYMLINKS) };
    let guest_symlinks = image.call_exported_i32("bionic_fs_fixture_pc_2_symlinks")?;
    if i64::from(guest_symlinks) != host_symlinks {
        return Err(format!(
            "pathconf semantic translation drift: guest={guest_symlinks} host={host_symlinks}"
        )
        .into());
    }
    let mut host_filesystem = HostStatvfs::default();
    // SAFETY: the live descriptor and writable Darwin statvfs record match the
    // local SDK ABI compiled into this native executable.
    if unsafe { host_fstatvfs(host_file.as_raw_fd(), &mut host_filesystem) } != 0 {
        return Err(std::io::Error::last_os_error().into());
    }
    let guest_bsize = image.call_exported_i32("bionic_fs_fixture_statvfs_bsize")?;
    let guest_flags = image.call_exported_i32("bionic_fs_fixture_statvfs_flags")?;
    let expected_flags = (host_filesystem.f_flag & PORTABLE_ST_FLAGS) | ANDROID_ST_RDONLY;
    if u64::try_from(guest_bsize).ok() != Some(host_filesystem.f_bsize)
        || u64::try_from(guest_flags).ok() != Some(expected_flags)
    {
        return Err("statvfs Darwin-to-Android differential drift".into());
    }
    // SAFETY: Darwin __error returns this host pthread's errno cell. It is never
    // exposed to the guest resolver and is used only to audit shim isolation.
    unsafe { *__error() = 33_001 };
    let result = image.call_exported_i32("bionic_fs_fixture_run")?;
    if result != 42 {
        return Err(format!("Android filesystem fixture failed at step {result}").into());
    }
    if facade.has_capability_failure() {
        return Err("facade encountered an untranslatable host capability failure".into());
    }
    // SAFETY: same host-only audit cell set immediately before guest execution.
    if unsafe { *__error() } != 33_001 {
        return Err("filesystem facade changed host errno".into());
    }
    println!(
        "bionic-fs-facade: PASS Android ELF file/path/cwd/DIR+fdopendir mount-root broker errno"
    );
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("bionic-fs-facade: {error}");
            ExitCode::from(2)
        }
    }
}

#[allow(dead_code)]
fn _function_pointer_is_data_pointer_sized() {
    const _: () = assert!(size_of::<unsafe extern "C" fn()>() == size_of::<*mut c_void>());
}
