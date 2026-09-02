#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Barrier, Condvar};
    use std::thread;
    use std::time::Duration;

    static PROCESS_TEST_LOCK: Mutex<()> = Mutex::new(());

    unsafe extern "C" {
        fn __error() -> *mut i32;
        fn darwin_art_bionic_errno_load() -> i32;
    }

    #[test]
    fn proc_executable_readlink_has_android_identity_and_linux_truncation() {
        let facade = Facade::new(File::open("/").unwrap(), b"/system", b"/system").unwrap();
        let mut complete = [0_u8; 64];
        assert_eq!(
            facade.readlink(
                b"/proc/self/exe",
                complete.as_mut_ptr().cast(),
                complete.len(),
            ),
            25
        );
        assert_eq!(&complete[..25], b"/system/bin/app_process64");
        assert_eq!(complete[25], 0);

        let mut truncated = [0_u8; 7];
        assert_eq!(
            facade.readlink(
                b"/proc/thread-self/exe",
                truncated.as_mut_ptr().cast(),
                truncated.len(),
            ),
            7
        );
        assert_eq!(&truncated, b"/system");
        assert_eq!(facade.readlink(b"/proc/self/exe", ptr::null_mut(), 0), -1);
    }

    #[test]
    fn descriptor_allocator_reserves_central_broker_token_range() {
        let mut table = DescriptorTable {
            next: CENTRAL_BROKER_TOKEN_MARKER - 1,
            ..DescriptorTable::default()
        };
        let boundary = table
            .insert(Descriptor::Random(RandomDeviceKind::Random))
            .unwrap();
        let wrapped = table
            .insert(Descriptor::Random(RandomDeviceKind::Urandom))
            .unwrap();
        assert_eq!(boundary, CENTRAL_BROKER_TOKEN_MARKER - 1);
        assert_eq!(wrapped, 10_000);
        assert_eq!(boundary & CENTRAL_BROKER_TOKEN_MARKER, 0);
        assert_eq!(wrapped & CENTRAL_BROKER_TOKEN_MARKER, 0);
    }

    #[test]
    fn unknown_darwin_errno_publishes_eio_and_marks_capability_failure() {
        let facade = Facade::new(File::open("/").unwrap(), b"/system", b"/system").unwrap();
        Facade::set_android_errno(2);
        // SAFETY: Darwin returns the current pthread's host errno cell.
        unsafe { *__error() = 33_002 };

        assert_eq!(
            facade.fail_io(&std::io::Error::from_raw_os_error(123_456)),
            -1
        );
        assert!(facade.has_capability_failure());
        // SAFETY: the standalone errno provider is linked for this test binary.
        assert_eq!(unsafe { darwin_art_bionic_errno_load() }, ANDROID_EIO);
        // SAFETY: same pthread-local host errno cell set above.
        assert_eq!(unsafe { *__error() }, 33_002);
    }

    #[test]
    fn repeated_opendir_closedir_reclaims_side_table_records() {
        let facade = Facade::new(File::open("/").unwrap(), b"/system", b"/system").unwrap();
        for _ in 0..512 {
            let directory = facade.opendir(b"/system");
            assert!(!directory.is_null());
            assert_eq!(facade.closedir(directory), 0);
        }
        assert!(facade.directories.lock().unwrap().streams.is_empty());
    }

    struct BlockingEntropy {
        state: Mutex<(bool, bool)>,
        condition: Condvar,
    }

    impl BlockingEntropy {
        fn new() -> Self {
            Self {
                state: Mutex::new((false, false)),
                condition: Condvar::new(),
            }
        }

        fn wait_until_entered(&self) {
            let state = self.state.lock().unwrap();
            drop(
                self.condition
                    .wait_while(state, |(entered, _)| !*entered)
                    .unwrap(),
            );
        }

        fn release(&self) {
            let mut state = self.state.lock().unwrap();
            state.1 = true;
            self.condition.notify_all();
        }
    }

    impl EntropyBackend for BlockingEntropy {
        fn fill(&self, bytes: &mut [u8]) -> Result<(), ()> {
            let mut state = self.state.lock().unwrap();
            state.0 = true;
            self.condition.notify_all();
            state = self
                .condition
                .wait_while(state, |(_, release)| !*release)
                .unwrap();
            drop(state);
            bytes.fill(0xa5);
            Ok(())
        }
    }

    #[test]
    fn random_read_serializes_close_and_reuses_descriptor_without_stale_kind() {
        let entropy = Arc::new(BlockingEntropy::new());
        let facade = Arc::new(
            Facade::new_with_entropy(
                File::open("/").unwrap(),
                b"/system",
                b"/system",
                entropy.clone(),
            )
            .unwrap(),
        );
        let fd = facade.open(b"/dev/random", O_RDONLY);
        assert!(fd >= 10_000);

        let reader_facade = facade.clone();
        let reader = thread::spawn(move || {
            let mut bytes = [0_u8; 32];
            // SAFETY: the local output remains writable for the whole call.
            let result = unsafe { reader_facade.read(fd, bytes.as_mut_ptr().cast(), bytes.len()) };
            (result, bytes)
        });
        entropy.wait_until_entered();

        let close_done = Arc::new(AtomicBool::new(false));
        let close_facade = facade.clone();
        let close_done_thread = close_done.clone();
        let closer = thread::spawn(move || {
            let result = close_facade.close(fd);
            close_done_thread.store(true, Ordering::Release);
            result
        });
        thread::sleep(Duration::from_millis(20));
        assert!(!close_done.load(Ordering::Acquire));
        entropy.release();
        let (read, bytes) = reader.join().unwrap();
        assert_eq!(read, 32);
        assert_eq!(bytes, [0xa5; 32]);
        assert_eq!(closer.join().unwrap(), 0);

        let reused = facade.open(b"/dev/urandom", O_RDONLY);
        assert_eq!(reused, fd);
        assert_eq!(facade.close(reused), 0);
    }

    #[test]
    fn random_devices_accept_android_descriptor_flags() {
        let facade = Facade::new(File::open("/").unwrap(), b"/system", b"/system").unwrap();
        let fd = facade.open(b"/dev/urandom", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        assert!(fd >= 10_000);
        assert_eq!(facade.close(fd), 0);
    }

    #[test]
    fn private_file_fcntl_translates_android_descriptor_and_status_flags() {
        let private_root = std::env::temp_dir().join(format!(
            "darwin-art-fcntl-{}-{:?}",
            std::process::id(),
            thread::current().id()
        ));
        fs::create_dir_all(&private_root).unwrap();
        let mut facade = Facade::new(File::open("/").unwrap(), b"/", b"/").unwrap();
        facade.private_root = Some(private_root.clone());
        assert_eq!(facade.mkdir(b"/data/fcntl", 0o755), 0);
        let fd = facade.open(b"/data/fcntl/file", O_RDWR | O_CREAT | O_TRUNC);
        assert!(fd >= 10_000);

        assert_eq!(facade.fcntl(fd, 1, 0), 1);
        assert_eq!(facade.fcntl(fd, 2, 1), 0);
        assert_eq!(facade.fcntl(fd, 3, 0) & O_ACCMODE, O_RDWR);
        assert_eq!(facade.fcntl(fd, 4, (O_APPEND | O_NONBLOCK) as isize), 0);
        let status = facade.fcntl(fd, 3, 0);
        assert_eq!(status & O_ACCMODE, O_RDWR);
        assert_ne!(status & O_APPEND, 0);
        assert_ne!(status & O_NONBLOCK, 0);
        assert_eq!(facade.fcntl(fd, 4, O_CREAT as isize), -1);
        assert_eq!(facade.close(fd), 0);
        fs::remove_dir_all(private_root).unwrap();
    }

    #[test]
    fn proc_maps_accepts_self_thread_self_and_virtual_pid_aliases() {
        assert!(Facade::proc_self_maps(b"/proc/self/maps"));
        assert!(Facade::proc_self_maps(b"/proc/thread-self/maps"));
        let current = format!("/proc/{}/maps", std::process::id());
        assert!(Facade::proc_self_maps(current.as_bytes()));
        assert!(Facade::proc_self_maps(b"/proc/999999999/maps"));
        assert!(!Facade::proc_self_maps(b"/proc/0/maps"));
        assert!(!Facade::proc_self_maps(b"/proc/not-a-pid/maps"));
        assert!(!Facade::proc_self_maps(b"/proc/self/status"));
    }

    #[test]
    fn regular_file_host_duplicate_is_owned_and_independent() {
        let project = File::open(concat!(env!("CARGO_MANIFEST_DIR"), "/../..")).unwrap();
        let facade = Arc::new(Facade::new(project, b"/", b"/").unwrap());
        let _activation = facade.activate();
        let fd = facade.open(b"/Cargo.toml", O_RDONLY);
        assert!(fd >= 10_000);

        let mut host_fd = -1;
        // SAFETY: host_fd is writable through this synchronous call.
        assert_eq!(
            unsafe { darwin_art_bionic_fs_dup_host_fd_core(fd, &mut host_fd) },
            1
        );
        assert!(host_fd >= 0);
        // SAFETY: a status of 1 transfers ownership of the returned raw fd.
        let mut duplicate = unsafe { File::from_raw_fd(host_fd) };
        let mut bytes = [0_u8; 5];
        duplicate.read_exact(&mut bytes).unwrap();
        assert_eq!(&bytes, b"[work");
        drop(duplicate);

        let mut original = [0_u8; 5];
        // SAFETY: original is writable for its complete length.
        assert_eq!(
            unsafe { facade.read(fd, original.as_mut_ptr().cast(), original.len()) },
            5
        );
        // POSIX dup shares the open-file description and therefore its offset;
        // the guest descriptor nevertheless remains live after closing the
        // duplicate.
        assert_eq!(&original, b"space");
        assert_eq!(facade.close(fd), 0);
    }

    #[test]
    fn ioctl_kind_lookup_is_atomic_with_close() {
        let facade =
            Arc::new(Facade::new(File::open("/").unwrap(), b"/system", b"/system").unwrap());
        let fd = facade.open(b"/dev/random", O_RDONLY);
        let barrier = Arc::new(Barrier::new(9));
        let mut workers = Vec::new();
        for _ in 0..8 {
            let worker_facade = facade.clone();
            let worker_barrier = barrier.clone();
            workers.push(thread::spawn(move || {
                let _activation = worker_facade.activate();
                worker_barrier.wait();
                for _ in 0..2_000 {
                    let mut info = IoctlFdInfo::default();
                    // SAFETY: info remains writable through this synchronous callback.
                    let status = unsafe {
                        darwin_art_bionic_fs_ioctl_fd_lookup(ptr::null_mut(), fd, &mut info)
                    };
                    assert!(status == IOCTL_FD_FOUND || status == IOCTL_FD_BAD);
                    if status == IOCTL_FD_FOUND {
                        assert_eq!(info.kind, IOCTL_FD_RANDOM_DEVICE);
                    }
                }
            }));
        }
        barrier.wait();
        assert_eq!(facade.close(fd), 0);
        for worker in workers {
            worker.join().unwrap();
        }
    }

    #[test]
    fn private_data_overlay_persists_and_sendfile_preserves_offset_rules() {
        let project = File::open(concat!(env!("CARGO_MANIFEST_DIR"), "/../..")).unwrap();
        let facade = Facade::new(project, b"/", b"/").unwrap();
        assert_eq!(facade.mkdir(b"/data/cache", 0o755), 0);
        let input = facade.open(b"/Cargo.toml", O_RDONLY);
        let output = facade.open(b"/data/cache/copy", O_WRONLY | O_CREAT | O_TRUNC);
        assert!(input >= 10_000 && output >= 10_000);

        let mut result = SendfileResult::default();
        let request = SendfileRequest {
            abi_version: SENDFILE_ABI_VERSION,
            output_fd: output,
            input_fd: input,
            has_explicit_offset: 0,
            offset: 0,
            count: 5,
        };
        assert_eq!(
            facade.sendfile_transfer(&request, &mut result),
            SENDFILE_TRANSFER_OK
        );
        assert_eq!(result.transferred, 5);
        assert_eq!(facade.close(output), 0);

        let persisted = facade.open(b"/data/cache/copy", O_RDONLY);
        let mut first = [0_u8; 5];
        assert_eq!(
            unsafe { facade.read(persisted, first.as_mut_ptr().cast(), 5) },
            5
        );
        assert_eq!(&first, b"[work");
        assert_eq!(facade.close(persisted), 0);
        assert_eq!(facade.chmod(b"/data/cache/copy", 0o640), 0);
        let mut chmod_status = AndroidStat::default();
        assert_eq!(
            unsafe { facade.stat(b"/data/cache/copy", &mut chmod_status, false) },
            0
        );
        assert_eq!(chmod_status.st_mode & 0o7777, 0o640);

        let offset_output = facade.open(b"/data/offset", O_WRONLY | O_CREAT | O_TRUNC);
        let explicit = SendfileRequest {
            output_fd: offset_output,
            has_explicit_offset: 1,
            offset: 1,
            count: 3,
            ..request
        };
        assert_eq!(
            facade.sendfile_transfer(&explicit, &mut result),
            SENDFILE_TRANSFER_OK
        );
        assert_eq!((result.transferred, result.next_offset), (3, 4));
        let current = SendfileRequest {
            output_fd: offset_output,
            count: 1,
            ..request
        };
        assert_eq!(
            facade.sendfile_transfer(&current, &mut result),
            SENDFILE_TRANSFER_OK
        );
        assert_eq!(result.transferred, 1);
        assert_eq!(facade.close(offset_output), 0);
        let offset_copy = facade.open(b"/data/offset", O_RDONLY);
        let mut bytes = [0_u8; 4];
        assert_eq!(
            unsafe { facade.read(offset_copy, bytes.as_mut_ptr().cast(), 4) },
            4
        );
        assert_eq!(&bytes, b"wors");
        assert_eq!(facade.close(offset_copy), 0);

        let positioned = facade.open(b"/data/positioned", O_WRONLY | O_CREAT | O_TRUNC);
        assert_eq!(
            unsafe { facade.pwrite(positioned, b"chrome".as_ptr().cast(), 6, 3) },
            6
        );
        assert_eq!(facade.lseek(positioned, 0, 1), 0);
        assert_eq!(facade.close(positioned), 0);
        let positioned = facade.open(b"/data/positioned", O_RDONLY);
        let mut positioned_bytes = [0xff_u8; 9];
        assert_eq!(
            unsafe { facade.read(positioned, positioned_bytes.as_mut_ptr().cast(), 9) },
            9
        );
        assert_eq!(&positioned_bytes, b"\0\0\0chrome");
        assert_eq!(facade.close(positioned), 0);

        let truncate = facade.open(b"/data/cache/copy", O_WRONLY | O_TRUNC);
        assert_eq!(facade.ftruncate(truncate, 2), 0);
        let mut status = AndroidStat::default();
        assert_eq!(unsafe { facade.fstat(truncate, &mut status) }, 0);
        assert_eq!(status.st_size, 2);
        assert_eq!(facade.close(truncate), 0);
        assert_eq!(facade.open(b"/Cargo.toml", O_WRONLY | O_TRUNC), -1);
    }

    #[test]
    fn private_data_seed_creates_only_authorized_overlay_hierarchy() {
        let facade = Facade::new(File::open("/").unwrap(), b"/", b"/").unwrap();
        assert_eq!(
            facade.seed_private_directory(b"/data/user/0/org.chromium.chrome"),
            0
        );
        assert_eq!(
            facade.mkdir(b"/data/user/0/org.chromium.chrome/files", 0o700),
            0
        );
        assert_eq!(
            facade.mkdir(b"/data/user/0/org.chromium.chrome/files/splitcompat", 0o700),
            0
        );
        assert_eq!(facade.seed_private_directory(b"/system/not-private"), -1);
    }

    #[test]
    fn process_owner_cross_thread_rollback_duplicate_and_quiescent_uninstall() {
        let _serial = PROCESS_TEST_LOCK.lock().unwrap();
        assert_eq!(
            darwin_art_bionic_fs_process_uninstall(),
            PROCESS_OWNER_NOT_INSTALLED
        );
        let root = File::open("/").unwrap();
        // SAFETY: all byte slices and the borrowed directory fd remain live
        // through each synchronous installation attempt.
        assert_eq!(
            unsafe {
                darwin_art_bionic_fs_process_install(
                    root.as_raw_fd(),
                    b"/".as_ptr(),
                    1,
                    ptr::null(),
                    0,
                )
            },
            PROCESS_OWNER_INVALID_ARGUMENT
        );
        assert_eq!(
            unsafe {
                darwin_art_bionic_fs_process_install(
                    root.as_raw_fd(),
                    b"/".as_ptr(),
                    1,
                    b"/darwin-art-missing-process-owner-cwd".as_ptr(),
                    b"/darwin-art-missing-process-owner-cwd".len(),
                )
            },
            PROCESS_OWNER_CREATE_FAILED
        );
        assert_eq!(
            unsafe {
                darwin_art_bionic_fs_process_install(
                    root.as_raw_fd(),
                    b"/".as_ptr(),
                    1,
                    b"/".as_ptr(),
                    1,
                )
            },
            PROCESS_OWNER_OK
        );
        assert_eq!(
            unsafe {
                darwin_art_bionic_fs_process_install(
                    root.as_raw_fd(),
                    b"/".as_ptr(),
                    1,
                    b"/".as_ptr(),
                    1,
                )
            },
            PROCESS_OWNER_ALREADY_INSTALLED
        );
        let worker = thread::spawn(|| {
            // SAFETY: static paths and local output records remain live for
            // each call. This pthread has no TLS Activation guard.
            let fd =
                unsafe { darwin_art_bionic_fs_open_core(c"/dev/random".as_ptr(), O_RDONLY, 0) };
            assert!(fd >= 10_000);
            let mut bytes = [0_u8; 16];
            assert_eq!(
                unsafe {
                    darwin_art_bionic_fs_read_core(fd, bytes.as_mut_ptr().cast(), bytes.len())
                },
                bytes.len() as isize
            );
            let mut info = IoctlFdInfo::default();
            assert_eq!(
                unsafe { darwin_art_bionic_fs_ioctl_fd_lookup(ptr::null_mut(), fd, &mut info) },
                IOCTL_FD_FOUND
            );
            assert_eq!(info.kind, IOCTL_FD_RANDOM_DEVICE);
            assert_eq!(darwin_art_bionic_fs_close_core(fd), 0);
        });
        worker.join().unwrap();
        assert_eq!(darwin_art_bionic_fs_process_has_capability_failure(), 0);
        assert_eq!(darwin_art_bionic_fs_process_uninstall(), PROCESS_OWNER_OK);

        let entropy = Arc::new(BlockingEntropy::new());
        let facade = Arc::new(
            Facade::new_with_entropy(File::open("/").unwrap(), b"/", b"/", entropy.clone())
                .unwrap(),
        );
        assert_eq!(publish_process_facade(facade), PROCESS_OWNER_OK);
        // SAFETY: static path remains live for the call.
        let fd = unsafe { darwin_art_bionic_fs_open_core(c"/dev/random".as_ptr(), O_RDONLY, 0) };
        let reader = thread::spawn(move || {
            let mut bytes = [0_u8; 32];
            // SAFETY: local output remains writable through the call.
            unsafe { darwin_art_bionic_fs_read_core(fd, bytes.as_mut_ptr().cast(), bytes.len()) }
        });
        entropy.wait_until_entered();
        let uninstall_done = Arc::new(AtomicBool::new(false));
        let uninstall_done_thread = uninstall_done.clone();
        let uninstaller = thread::spawn(move || {
            let status = darwin_art_bionic_fs_process_uninstall();
            uninstall_done_thread.store(true, Ordering::Release);
            status
        });
        {
            let state = process_owner_state();
            drop(
                PROCESS_OWNER
                    .quiescent
                    .wait_while(state, |state| !state.draining)
                    .unwrap_or_else(std::sync::PoisonError::into_inner),
            );
        }
        assert!(!uninstall_done.load(Ordering::Acquire));
        assert_eq!(darwin_art_bionic_fs_process_uninstall(), PROCESS_OWNER_BUSY);
        // New calls fail closed once draining begins; they cannot extend the
        // lifetime being awaited by uninstall.
        assert_eq!(
            unsafe { darwin_art_bionic_fs_open_core(c"/dev/random".as_ptr(), O_RDONLY, 0) },
            -1
        );
        entropy.release();
        assert_eq!(reader.join().unwrap(), 32);
        assert_eq!(uninstaller.join().unwrap(), PROCESS_OWNER_OK);
        assert_eq!(darwin_art_bionic_fs_process_has_capability_failure(), -1);
        let mut info = IoctlFdInfo::default();
        assert_eq!(
            unsafe { darwin_art_bionic_fs_ioctl_fd_lookup(ptr::null_mut(), fd, &mut info) },
            IOCTL_FD_CAPABILITY_UNAVAILABLE
        );
        assert_eq!(
            darwin_art_bionic_fs_process_uninstall(),
            PROCESS_OWNER_NOT_INSTALLED
        );
    }

    #[test]
    fn synthetic_device_procfs_is_bounded_and_consistent() {
        let cpuinfo = Facade::synthetic_proc_contents(b"/proc/cpuinfo").unwrap();
        let cpuinfo = String::from_utf8(cpuinfo).unwrap();
        assert_eq!(cpuinfo.matches("processor\t:").count(), 8);
        let meminfo = String::from_utf8(
            Facade::synthetic_proc_contents(b"/proc/meminfo").unwrap(),
        )
        .unwrap();
        assert!(meminfo.contains("MemTotal:        8388608 kB"));
        assert_eq!(
            Facade::synthetic_proc_contents(b"/proc/4242/status"),
            Facade::synthetic_proc_contents(b"/proc/self/status")
        );
        assert_eq!(
            Facade::synthetic_proc_contents(
                b"/sys/devices/system/cpu/cpu7/cpufreq/cpuinfo_max_freq"
            ),
            Some(b"2400000\n".to_vec())
        );
        assert_eq!(
            Facade::synthetic_proc_contents(b"/sys/devices/system/cpu/cpu0/cpu_capacity"),
            Some(b"512\n".to_vec())
        );
        assert_eq!(
            Facade::synthetic_proc_contents(
                b"/sys/devices/system/cpu/cpu3/cpufreq/cpuinfo_max_freq"
            ),
            Some(b"1800000\n".to_vec())
        );
        assert!(Facade::synthetic_proc_contents(
            b"/sys/devices/system/cpu/cpu8/cpu_capacity"
        )
        .is_none());
    }
}
