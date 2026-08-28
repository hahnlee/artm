use darwin_art_abi::{AbiHeader, StatusCode};
use darwin_art_engine_sys::{HostServices, ServiceSpawnRequest, ServiceSpawnResult};
use std::collections::HashMap;
use std::env;
use std::ffi::{CStr, OsString, c_void};
use std::os::fd::{FromRawFd, IntoRawFd, RawFd};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::net::UnixStream;
use std::os::unix::process::CommandExt;
use std::process::{Child, Command};
use std::sync::Mutex;

use crate::RunOptions;

const CHILD_CONTROL_FD: RawFd = 3;
unsafe extern "C" {
    fn dup2(source: i32, destination: i32) -> i32;
    fn close(fd: i32) -> i32;
}

struct ManagedChild {
    child: Child,
}

pub(crate) struct ServiceProcessManager {
    executable: OsString,
    options: RunOptions,
    children: Mutex<HashMap<i32, ManagedChild>>,
}

impl ServiceProcessManager {
    pub(crate) fn new(options: RunOptions) -> Result<Self, String> {
        let executable = env::current_exe()
            .map_err(|error| format!("could not locate host executable: {error}"))?
            .into_os_string();
        Ok(Self {
            executable,
            options,
            children: Mutex::new(HashMap::new()),
        })
    }

    pub(crate) fn native_services(&mut self) -> HostServices {
        HostServices {
            header: AbiHeader::new(core::mem::size_of::<HostServices>()),
            context: std::ptr::from_mut(self).cast::<c_void>(),
            spawn_service: Some(spawn_service),
            release_service: Some(release_service),
        }
    }

    fn spawn(&self, request: &ServiceRequest<'_>) -> Result<(i32, RawFd), String> {
        let (browser_stream, child_stream) =
            UnixStream::pair().map_err(|error| format!("service socketpair failed: {error}"))?;
        let inherited_fd = child_stream.into_raw_fd();
        let mut command = Command::new(&self.executable);
        command
            .arg("--service-child")
            .arg(CHILD_CONTROL_FD.to_string())
            .env(
                "DARWIN_ART_APK_SERVICE_COMPONENT",
                std::ffi::OsStr::from_bytes(request.component.to_bytes()),
            )
            .env(
                "DARWIN_ART_APK_SERVICE_INSTANCE",
                std::ffi::OsStr::from_bytes(request.instance_name.to_bytes()),
            )
            .env(
                "DARWIN_ART_APK_PROCESS_NAME",
                std::ffi::OsStr::from_bytes(request.process_name.to_bytes()),
            )
            .env(
                "DARWIN_ART_APK_ISOLATED_PROCESS",
                if request.isolated { "1" } else { "0" },
            )
            .env("DARWIN_ART_CHILD_LIBRARY", &self.options.library)
            .env("DARWIN_ART_CHILD_CORE_OJ", &self.options.core_oj_jar)
            .env(
                "DARWIN_ART_CHILD_CORE_LIBART",
                &self.options.core_libart_jar,
            )
            .env("DARWIN_ART_CHILD_FRAMEWORK", &self.options.framework_jar)
            .env("DARWIN_ART_CHILD_CORE_ICU4J", &self.options.core_icu4j_jar)
            .env("DARWIN_ART_CHILD_APP_DEX", &self.options.app_dex);
        // SAFETY: dup2 and close are async-signal-safe and the closure only
        // manipulates the one pre-created descriptor before exec.
        unsafe {
            command.pre_exec(move || {
                if dup2(inherited_fd, CHILD_CONTROL_FD) < 0 {
                    return Err(std::io::Error::last_os_error());
                }
                if inherited_fd != CHILD_CONTROL_FD {
                    close(inherited_fd);
                }
                Ok(())
            });
        }
        let child = command
            .spawn()
            .map_err(|error| format!("could not spawn Android service: {error}"));
        // into_raw_fd transferred ownership out of Rust; the parent must
        // close its copy regardless of whether spawn succeeded.
        unsafe { close(inherited_fd) };
        let child = child?;
        let pid = i32::try_from(child.id()).map_err(|_| "child PID overflow".to_owned())?;
        self.children
            .lock()
            .map_err(|_| "service child table poisoned".to_owned())?
            .insert(pid, ManagedChild { child });
        Ok((pid, browser_stream.into_raw_fd()))
    }

    fn release(&self, pid: i32) -> Result<(), String> {
        let mut child = self
            .children
            .lock()
            .map_err(|_| "service child table poisoned".to_owned())?
            .remove(&pid)
            .ok_or_else(|| format!("unknown service child PID {pid}"))?
            .child;
        if child
            .try_wait()
            .map_err(|error| error.to_string())?
            .is_none()
        {
            child.kill().map_err(|error| error.to_string())?;
        }
        child.wait().map_err(|error| error.to_string())?;
        Ok(())
    }

    pub(crate) fn shutdown_all(&mut self) -> Result<(), String> {
        let children = self
            .children
            .get_mut()
            .map_err(|_| "service child table poisoned".to_owned())?;
        let mut first_error = None;
        for (_, mut managed) in children.drain() {
            match managed.child.try_wait() {
                Ok(Some(_)) => continue,
                Ok(None) => {
                    if let Err(error) = managed.child.kill()
                        && first_error.is_none()
                    {
                        first_error = Some(error.to_string());
                    }
                }
                Err(error) if first_error.is_none() => {
                    first_error = Some(error.to_string());
                }
                Err(_) => {}
            }
            if let Err(error) = managed.child.wait()
                && first_error.is_none()
            {
                first_error = Some(error.to_string());
            }
        }
        first_error.map_or(Ok(()), Err)
    }

    /// Signal every Android service process immediately before the browser
    /// process exits. Waiting for each child here gives Chromium time to
    /// observe renderer death and run callbacks against a process that is
    /// already being torn down. Android instead removes the whole process
    /// group as one lifetime boundary.
    pub(crate) fn terminate_for_process_exit(&mut self) -> Result<(), String> {
        let children = self
            .children
            .get_mut()
            .map_err(|_| "service child table poisoned".to_owned())?;
        let mut first_error = None;
        for managed in children.values_mut() {
            match managed.child.try_wait() {
                Ok(Some(_)) => {}
                Ok(None) => {
                    if let Err(error) = managed.child.kill()
                        && first_error.is_none()
                    {
                        first_error = Some(error.to_string());
                    }
                }
                Err(error) if first_error.is_none() => {
                    first_error = Some(error.to_string());
                }
                Err(_) => {}
            }
        }
        first_error.map_or(Ok(()), Err)
    }
}

impl Drop for ServiceProcessManager {
    fn drop(&mut self) {
        let _ = self.shutdown_all();
    }
}

struct ServiceRequest<'a> {
    component: &'a CStr,
    instance_name: &'a CStr,
    process_name: &'a CStr,
    isolated: bool,
}

unsafe fn checked_request<'a>(
    request: *const ServiceSpawnRequest,
) -> Result<ServiceRequest<'a>, StatusCode> {
    // SAFETY: caller supplies the versioned ABI pointer for this callback.
    let request = unsafe { request.as_ref() }.ok_or(StatusCode::InvalidArgument)?;
    if !request
        .header
        .accepts(core::mem::size_of::<ServiceSpawnRequest>())
        || request.component.is_null()
        || request.instance_name.is_null()
        || request.process_name.is_null()
        || !matches!(request.isolated, 0 | 1)
    {
        return Err(StatusCode::InvalidArgument);
    }
    // SAFETY: callback contract requires NUL-terminated strings borrowed for
    // this call. Command copies them before the callback returns.
    let component = unsafe { CStr::from_ptr(request.component) };
    let instance_name = unsafe { CStr::from_ptr(request.instance_name) };
    let process_name = unsafe { CStr::from_ptr(request.process_name) };
    if component.to_bytes().is_empty() || process_name.to_bytes().is_empty() {
        return Err(StatusCode::InvalidArgument);
    }
    Ok(ServiceRequest {
        component,
        instance_name,
        process_name,
        isolated: request.isolated == 1,
    })
}

unsafe extern "C" fn spawn_service(
    context: *mut c_void,
    request: *const ServiceSpawnRequest,
    result: *mut ServiceSpawnResult,
) -> i32 {
    // SAFETY: HostServices keeps the manager alive for this callback.
    let Some(manager) = (unsafe { context.cast::<ServiceProcessManager>().as_ref() }) else {
        return StatusCode::InvalidArgument as i32;
    };
    let Some(result) = (unsafe { result.as_mut() }) else {
        return StatusCode::InvalidArgument as i32;
    };
    if !result
        .header
        .accepts(core::mem::size_of::<ServiceSpawnResult>())
    {
        return StatusCode::InvalidArgument as i32;
    }
    let request = match unsafe { checked_request(request) } {
        Ok(request) => request,
        Err(status) => return status as i32,
    };
    match manager.spawn(&request) {
        Ok((pid, control_fd)) => {
            result.host_pid = pid;
            result.control_fd = control_fd;
            StatusCode::Ok as i32
        }
        Err(error) => {
            eprintln!("darwin-art-host: {error}");
            StatusCode::Internal as i32
        }
    }
}

unsafe extern "C" fn release_service(context: *mut c_void, host_pid: i32) -> i32 {
    // SAFETY: HostServices owns this context through the synchronous run.
    let Some(manager) = (unsafe { context.cast::<ServiceProcessManager>().as_ref() }) else {
        return StatusCode::InvalidArgument as i32;
    };
    match manager.release(host_pid) {
        Ok(()) => StatusCode::Ok as i32,
        Err(error) => {
            eprintln!("darwin-art-host: {error}");
            StatusCode::NotFound as i32
        }
    }
}

pub fn run_service_child(control_fd: RawFd) -> Result<(), String> {
    if control_fd < 0 {
        return Err("invalid service control fd".to_owned());
    }
    for required in [
        "DARWIN_ART_APK_SERVICE_COMPONENT",
        "DARWIN_ART_APK_PROCESS_NAME",
        "DARWIN_ART_CHILD_LIBRARY",
        "DARWIN_ART_CHILD_CORE_OJ",
        "DARWIN_ART_CHILD_CORE_LIBART",
        "DARWIN_ART_CHILD_FRAMEWORK",
        "DARWIN_ART_CHILD_CORE_ICU4J",
        "DARWIN_ART_CHILD_APP_DEX",
    ] {
        if env::var_os(required).is_none() {
            return Err(format!("service child missing {required}"));
        }
    }
    // Keep Rust ownership of the inherited endpoint while ART's Service
    // owner thread uses the same descriptor for Binder transactions.
    // SAFETY: --service-child receives the single inherited, owned endpoint.
    let _control_stream = unsafe { UnixStream::from_raw_fd(control_fd) };
    unsafe { env::set_var("DARWIN_ART_SERVICE_CONTROL_FD", control_fd.to_string()) };
    let required_path = |name: &str| {
        env::var_os(name)
            .map(std::path::PathBuf::from)
            .ok_or_else(|| format!("service child missing {name}"))
    };
    let options = RunOptions {
        library: required_path("DARWIN_ART_CHILD_LIBRARY")?,
        core_oj_jar: required_path("DARWIN_ART_CHILD_CORE_OJ")?,
        core_libart_jar: required_path("DARWIN_ART_CHILD_CORE_LIBART")?,
        framework_jar: required_path("DARWIN_ART_CHILD_FRAMEWORK")?,
        core_icu4j_jar: required_path("DARWIN_ART_CHILD_CORE_ICU4J")?,
        app_dex: required_path("DARWIN_ART_CHILD_APP_DEX")?,
        heap_initial_bytes: 64 * 1024 * 1024,
        heap_maximum_bytes: 256 * 1024 * 1024,
        visible_seconds: 0.0,
        terminate_android_process: true,
    };
    crate::run(&options)
        .map(|_| ())
        .map_err(|error| format!("Android service process failed: {error}"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;

    #[test]
    fn request_validation_rejects_non_boolean_isolated_flag() {
        let component = CString::new("pkg/Service").unwrap();
        let empty = CString::new("").unwrap();
        let process = CString::new("pkg:child").unwrap();
        let request = ServiceSpawnRequest {
            header: AbiHeader::new(core::mem::size_of::<ServiceSpawnRequest>()),
            component: component.as_ptr(),
            instance_name: empty.as_ptr(),
            process_name: process.as_ptr(),
            isolated: 2,
        };
        // SAFETY: all pointers remain valid through validation.
        assert!(matches!(
            unsafe { checked_request(&request) },
            Err(StatusCode::InvalidArgument)
        ));
    }
}
