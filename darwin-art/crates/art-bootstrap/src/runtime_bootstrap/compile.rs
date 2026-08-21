use super::*;
use crate::native_build::{
    FileHashCache, PendingNativeCompile, compile_pending_native, compile_with_dependency_cache,
    record_cache_result,
};

pub(crate) struct RuntimeBootstrapCompiled {
    pub(crate) objects: Vec<PathBuf>,
    pub(crate) compiled_objects: usize,
    pub(crate) cached_objects: usize,
}

pub(crate) fn compile(
    staged: &RuntimeBootstrapStaging,
    real_graphics: bool,
) -> Result<RuntimeBootstrapCompiled> {
    let includes = staged
        .includes
        .iter()
        .map(PathBuf::as_path)
        .collect::<Vec<_>>();
    let runtime_includes = staged
        .runtime_includes
        .iter()
        .map(PathBuf::as_path)
        .collect::<Vec<_>>();
    let compiler_identity = format!(
        "{}macOS {} ({})",
        command_output(Command::new("clang++").arg("--version"))?,
        command_output(Command::new("sw_vers").arg("-productVersion"))?.trim(),
        command_output(Command::new("sw_vers").arg("-buildVersion"))?.trim()
    );
    let file_hash_cache_path = staged.build_dir.join("file-hashes.cache");
    let mut file_hash_cache = FileHashCache::load(&file_hash_cache_path)?;
    let mut objects = Vec::new();
    let mut compiled_objects = 0usize;
    let mut cached_objects = 0usize;
    let operator_object = staged.object_dir.join("generated_operator_out.cc.o");
    let mut operator_command = runtime_bootstrap_cpp_command(&runtime_includes);
    operator_command
        .arg("-idirafter")
        .arg(&staged.ndk_arch_include)
        .arg("-idirafter")
        .arg(&staged.ndk_include)
        .arg("-Wno-macro-redefined")
        .arg("-c")
        .arg(&staged.operator_source)
        .arg("-o")
        .arg(&operator_object);
    record_cache_result(
        compile_with_dependency_cache(
            &mut operator_command,
            &operator_object,
            &compiler_identity,
            &mut file_hash_cache,
        )?,
        &mut compiled_objects,
        &mut cached_objects,
    );
    objects.push(operator_object);

    for profile_source in [
        "profile/profile_boot_info.cc",
        "profile/profile_compilation_info.cc",
    ] {
        let profile_object = staged
            .object_dir
            .join(format!("libprofile_{}.o", profile_source.replace('/', "_")));
        let mut profile_command = runtime_bootstrap_cpp_command(&runtime_includes);
        profile_command
            .arg("-idirafter")
            .arg(&staged.ndk_arch_include)
            .arg("-idirafter")
            .arg(&staged.ndk_include)
            .arg("-Wno-macro-redefined")
            .arg("-c")
            .arg(staged.libprofile.join(profile_source))
            .arg("-o")
            .arg(&profile_object);
        record_cache_result(
            compile_with_dependency_cache(
                &mut profile_command,
                &profile_object,
                &compiler_identity,
                &mut file_hash_cache,
            )?,
            &mut compiled_objects,
            &mut cached_objects,
        );
        objects.push(profile_object);
    }
    if real_graphics {
        let os_linux_object = staged.object_dir.join("artbase_os_linux_aosp_fmt.cc.o");
        let mut os_linux_command = runtime_cpp_command(&includes);
        os_linux_command
            .arg("-c")
            .arg(staged.artbase.join("base/os_linux.cc"))
            .arg("-o")
            .arg(&os_linux_object);
        record_cache_result(
            compile_with_dependency_cache(
                &mut os_linux_command,
                &os_linux_object,
                &compiler_identity,
                &mut file_hash_cache,
            )?,
            &mut compiled_objects,
            &mut cached_objects,
        );
    }

    let openjdk_math_object = staged.object_dir.join("libcore_openjdk_Math.c.o");
    let mut openjdk_math_command = Command::new("clang");
    openjdk_math_command
        .args([
            "-std=gnu11",
            "-O2",
            "-DNDEBUG",
            "-ftrivial-auto-var-init=zero",
            "-ffunction-sections",
            "-fdata-sections",
        ])
        .arg(format!("-I{}", staged.android_jni_include.display()))
        .arg(format!("-I{}", staged.nativehelper_full_include.display()))
        .arg(format!(
            "-I{}",
            staged.nativehelper_platform_headers.display()
        ))
        .arg(format!("-I{}", staged.liblog_include.display()))
        .arg("-c")
        .arg(
            staged
                .root
                .join("_aosp/libcore/ojluni/src/main/native/Math.c"),
        )
        .arg("-o")
        .arg(&openjdk_math_object);
    record_cache_result(
        compile_with_dependency_cache(
            &mut openjdk_math_command,
            &openjdk_math_object,
            &compiler_identity,
            &mut file_hash_cache,
        )?,
        &mut compiled_objects,
        &mut cached_objects,
    );
    objects.push(openjdk_math_object);

    let jni_proxy_object = staged.object_dir.join("darwin_art_jni_proxy.c.o");
    let mut jni_proxy_command = Command::new("clang");
    jni_proxy_command
        .args([
            "-std=c17",
            "-O2",
            "-DNDEBUG",
            "-fvisibility=hidden",
            "-ffunction-sections",
            "-fdata-sections",
            "-Wall",
            "-Wextra",
            "-Werror",
        ])
        .arg(format!("-I{}", staged.jni_proxy_include.display()))
        .arg(format!("-I{}", staged.jni_proxy_generated.display()))
        .arg("-c")
        .arg(staged.root.join("tools/android-jni-proxy/src/proxy.c"))
        .arg("-o")
        .arg(&jni_proxy_object);
    record_cache_result(
        compile_with_dependency_cache(
            &mut jni_proxy_command,
            &jni_proxy_object,
            &compiler_identity,
            &mut file_hash_cache,
        )?,
        &mut compiled_objects,
        &mut cached_objects,
    );
    objects.push(jni_proxy_object);

    let adapter_jobs = adapter_jobs(staged, real_graphics, &includes);
    let (adapter_objects, adapter_compiled, adapter_cached) =
        compile_pending_native(adapter_jobs, &compiler_identity)?;
    compiled_objects += adapter_compiled;
    cached_objects += adapter_cached;
    objects.extend(adapter_objects);

    let runtime_jobs = runtime_jobs(staged, &runtime_includes);
    let (runtime_objects, runtime_compiled, runtime_cached) =
        compile_pending_native(runtime_jobs, &compiler_identity)?;
    compiled_objects += runtime_compiled;
    cached_objects += runtime_cached;
    for object in runtime_objects {
        let kind = command_output(Command::new("file").arg(&object))?;
        if !kind.contains("Mach-O 64-bit object arm64") {
            return Err(format!("unexpected Runtime object format: {kind}").into());
        }
        objects.push(object);
    }
    let runtime_object = staged.runtime_core_object_dir.join("runtime.cc.o");
    let symbols = command_output(Command::new("nm").args(["-gU"]).arg(&runtime_object))?;
    if !symbols.contains("_ZN3art7Runtime6Create") {
        return Err("compiled Runtime object does not export Runtime::Create".into());
    }
    file_hash_cache.save(&file_hash_cache_path)?;
    Ok(RuntimeBootstrapCompiled {
        objects,
        compiled_objects,
        cached_objects,
    })
}

fn adapter_jobs(
    staged: &RuntimeBootstrapStaging,
    real_graphics: bool,
    includes: &[&Path],
) -> Vec<PendingNativeCompile> {
    let mut jobs = Vec::new();
    for adapter_source in [
        "darwin_art_abi_layout.cc",
        "darwin_android_jni_trampoline.cc",
        "darwin_android_elf_image_registry.cc",
        "darwin_provider_owners.cc",
        "darwin_framework_natives.cc",
        "darwin_framework_animation_natives.cc",
        "darwin_icu_natives.cc",
        "darwin_icu_jni_bridge.cc",
        "darwin_libcore_natives.cc",
        "darwin_runtime_adapters.cc",
        "darwin_runtime_platform_stubs.cc",
        "darwin_native_bridge_stubs.cc",
        "darwin_jni_shorty.cc",
        "darwin_jni_proxy_lookup.cc",
        "darwin_jni_proxy_registration.cc",
        "darwin_runtime_elf_lifecycle.cc",
        "darwin_runtime_elf_resolver.cc",
        "darwin_runtime_native_loader.cc",
        "darwin_runtime_jni_registration.cc",
        "darwin_sigchain.cc",
        "fault_handler_arm64_darwin.cc",
    ] {
        if (real_graphics && adapter_source == "darwin_icu_natives.cc")
            || (!real_graphics && adapter_source == "darwin_icu_jni_bridge.cc")
        {
            continue;
        }
        let adapter_object = staged.object_dir.join(format!("{adapter_source}.o"));
        let mut adapter_command = if real_graphics && adapter_source == "darwin_libcore_natives.cc"
        {
            let mut libcore_includes = includes.to_vec();
            libcore_includes
                .retain(|path| *path != Path::new("/opt/homebrew/opt/icu4c@78/include"));
            libcore_includes.insert(0, staged.android_icu_i18n.as_path());
            libcore_includes.insert(0, staged.android_icu_common.as_path());
            runtime_bootstrap_cpp_command(&libcore_includes)
        } else {
            runtime_bootstrap_cpp_command(includes)
        };
        if real_graphics && adapter_source == "darwin_framework_natives.cc" {
            adapter_command
                .arg("-DDARWIN_ART_REAL_GRAPHICS")
                .arg("-I")
                .arg(&staged.libcutils_include);
        }
        if real_graphics && adapter_source == "darwin_icu_jni_bridge.cc" {
            adapter_command.arg("-I").arg(staged.root.join("include"));
        }
        if real_graphics && adapter_source == "darwin_libcore_natives.cc" {
            adapter_command.arg("-DDARWIN_ART_FULL_LIBCORE_LINUX");
        }
        if matches!(
            adapter_source,
            "darwin_runtime_adapters.cc"
                | "darwin_runtime_elf_lifecycle.cc"
                | "darwin_runtime_elf_resolver.cc"
                | "darwin_runtime_native_loader.cc"
                | "darwin_runtime_jni_registration.cc"
                | "darwin_provider_owners.cc"
                | "darwin_jni_proxy_lookup.cc"
                | "darwin_jni_proxy_registration.cc"
        ) {
            for include in [
                "tools/bionic-provider-namespace/include",
                "tools/bionic-dso-lifecycle-facade/include",
                "tools/bionic-fs-facade/include",
                "tools/bionic-dns-facade/include",
                "tools/bionic-socket-broker-adapter/include",
                "tools/bionic-sendfile-facade/include",
                "tools/bionic-stdio-facade/include",
                "tools/bionic-ioctl-facade/include",
                "tools/bionic-strftime-facade/include",
            ] {
                adapter_command.arg("-I").arg(staged.root.join(include));
            }
        }
        if adapter_source == "darwin_android_elf_image_registry.cc" {
            adapter_command.arg("-I").arg(
                staged
                    .root
                    .join("tools/android-dl-iterate-phdr-provider/include"),
            );
        }
        adapter_command
            .arg("-idirafter")
            .arg(&staged.ndk_arch_include)
            .arg("-idirafter")
            .arg(&staged.ndk_include)
            .arg("-Wno-macro-redefined")
            .arg("-c")
            .arg(staged.root.join("compat").join(adapter_source))
            .arg("-o")
            .arg(&adapter_object);
        jobs.push(PendingNativeCompile {
            command: adapter_command,
            object: adapter_object,
        });
    }
    jobs
}

fn runtime_jobs(staged: &RuntimeBootstrapStaging, includes: &[&Path]) -> Vec<PendingNativeCompile> {
    let sources = [
        "fault_handler.cc",
        "interpreter/mterp/nterp.cc",
        "dex_register_location.cc",
        "handle.cc",
        "java_frame_root_info.cc",
        "jit/jit_memory_region.cc",
        "jit/profile_saver.cc",
        "jit/small_pattern_matcher.cc",
        "offsets.cc",
        "reflective_value_visitor.cc",
        "jit/jit.cc",
        "jit/jit_code_cache.cc",
        "jit/jit_options.cc",
        "jit/profiling_info.cc",
        "mirror/emulated_stack_frame.cc",
        "mirror/executable.cc",
        "monitor_objects_stack_visitor.cc",
        "signal_catcher.cc",
        "debug_print.cc",
        "debugger.cc",
        "dex/dex_file_annotations.cc",
        "exec_utils.cc",
        "hidden_api.cc",
        "jni/check_jni.cc",
        "jni/jni_internal.cc",
        "method_handles.cc",
        "mirror/class_ext.cc",
        "mirror/field.cc",
        "mirror/method.cc",
        "mirror/method_handle_impl.cc",
        "mirror/method_handles_lookup.cc",
        "mirror/method_type.cc",
        "mirror/var_handle.cc",
        "native_bridge_art_interface.cc",
        "native_stack_dump.cc",
        "oat/elf_file.cc",
        "oat/index_bss_mapping.cc",
        "oat/jni_stub_hash_map.cc",
        "plugin.cc",
        "scoped_thread_state_change.cc",
        "startup_completed_task.cc",
        "string_builder_append.cc",
        "ti/agent.cc",
        "trace.cc",
        "trace_profile.cc",
        "var_handles.cc",
        "verifier/class_verifier.cc",
        "verifier/instruction_flags.cc",
        "verifier/method_verifier.cc",
        "verifier/reg_type.cc",
        "verifier/reg_type_cache.cc",
        "verifier/register_line.cc",
        "verifier/verifier_deps.cc",
        "backtrace_helper.cc",
        "jit/debugger_interface.cc",
        "metrics/reporter.cc",
        "monitor_pool.cc",
        "non_debuggable_classes.cc",
        "nterp_helpers.cc",
        "oat/image.cc",
        "oat/oat.cc",
        "oat/oat_file.cc",
        "oat/oat_file_assistant.cc",
        "oat/oat_file_assistant_context.cc",
        "oat/oat_quick_method_header.cc",
        "oat/stack_map.cc",
        "oat/sdc_file.cc",
        "object_lock.cc",
        "quick_exception_handler.cc",
        "reference_table.cc",
        "reflection.cc",
        "reflective_handle_scope.cc",
        "stack.cc",
        "thread_pool.cc",
        "vdex_file.cc",
        "entrypoints/entrypoint_utils.cc",
        "entrypoints/jni/jni_entrypoints.cc",
        "entrypoints/math_entrypoints.cc",
        "entrypoints/quick/quick_alloc_entrypoints.cc",
        "entrypoints/quick/quick_cast_entrypoints.cc",
        "entrypoints/quick/quick_deoptimization_entrypoints.cc",
        "entrypoints/quick/quick_dexcache_entrypoints.cc",
        "entrypoints/quick/quick_entrypoints_enum.cc",
        "entrypoints/quick/quick_field_entrypoints.cc",
        "entrypoints/quick/quick_fillarray_entrypoints.cc",
        "entrypoints/quick/quick_jni_entrypoints.cc",
        "entrypoints/quick/quick_lock_entrypoints.cc",
        "entrypoints/quick/quick_math_entrypoints.cc",
        "entrypoints/quick/quick_string_builder_append_entrypoints.cc",
        "entrypoints/quick/quick_thread_entrypoints.cc",
        "entrypoints/quick/quick_throw_entrypoints.cc",
        "entrypoints/quick/quick_trampoline_entrypoints.cc",
        "runtime.cc",
        "class_linker.cc",
        "thread.cc",
        "thread_list.cc",
        "gc/heap.cc",
        "intern_table.cc",
        "instrumentation.cc",
        "runtime_callbacks.cc",
        "oat/oat_file_manager.cc",
        "jni/java_vm_ext.cc",
        "runtime_options.cc",
        "base/locks.cc",
        "base/gc_visited_arena_pool.cc",
        "base/mem_map_arena_pool.cc",
        "base/quasi_atomic.cc",
        "base/timing_logger.cc",
        "gc/accounting/bitmap.cc",
        "gc/accounting/card_table.cc",
        "gc/accounting/heap_bitmap.cc",
        "gc/accounting/mod_union_table.cc",
        "gc/accounting/remembered_set.cc",
        "gc/accounting/space_bitmap.cc",
        "gc/allocation_record.cc",
        "gc/allocator/art-dlmalloc.cc",
        "gc/allocator/rosalloc.cc",
        "gc/collector/concurrent_copying.cc",
        "gc/collector/garbage_collector.cc",
        "gc/collector/immune_region.cc",
        "gc/collector/immune_spaces.cc",
        "gc/collector/mark_compact.cc",
        "gc/collector/mark_sweep.cc",
        "gc/collector/partial_mark_sweep.cc",
        "gc/collector/semi_space.cc",
        "gc/collector/sticky_mark_sweep.cc",
        "gc/gc_cause.cc",
        "gc/reference_processor.cc",
        "gc/reference_queue.cc",
        "gc/scoped_gc_critical_section.cc",
        "gc/space/bump_pointer_space.cc",
        "gc/space/dlmalloc_space.cc",
        "gc/space/image_space.cc",
        "gc/space/large_object_space.cc",
        "gc/space/malloc_space.cc",
        "gc/space/region_space.cc",
        "gc/space/rosalloc_space.cc",
        "gc/space/space.cc",
        "gc/space/zygote_space.cc",
        "gc/task_processor.cc",
        "gc/verification.cc",
        "javaheapprof/javaheapsampler.cc",
        "app_info.cc",
        "art_field.cc",
        "art_method.cc",
        "barrier.cc",
        "cha.cc",
        "class_loader_context.cc",
        "class_root.cc",
        "class_table.cc",
        "common_throws.cc",
        "compat_framework.cc",
        "indirect_reference_table.cc",
        "jni/jni_env_ext.cc",
        "jni/local_reference_table.cc",
        "jni/jni_id_manager.cc",
        "mirror/array.cc",
        "mirror/class.cc",
        "mirror/dex_cache.cc",
        "mirror/object.cc",
        "mirror/stack_frame_info.cc",
        "mirror/stack_trace_element.cc",
        "mirror/string.cc",
        "mirror/throwable.cc",
        "native/dalvik_system_BaseDexClassLoader.cc",
        "native/dalvik_system_DexFile.cc",
        "native/dalvik_system_VMDebug.cc",
        "native/dalvik_system_VMRuntime.cc",
        "native/dalvik_system_VMStack.cc",
        "native/dalvik_system_ZygoteHooks.cc",
        "native/java_lang_Class.cc",
        "native/java_lang_Object.cc",
        "native/java_lang_StackStreamFactory.cc",
        "native/java_lang_String.cc",
        "native/java_lang_StringFactory.cc",
        "native/java_lang_System.cc",
        "native/java_lang_Thread.cc",
        "native/java_lang_Throwable.cc",
        "native/java_lang_VMClassLoader.cc",
        "native/java_lang_invoke_MethodHandle.cc",
        "native/java_lang_invoke_MethodHandleImpl.cc",
        "native/java_lang_ref_FinalizerReference.cc",
        "native/java_lang_ref_Reference.cc",
        "native/java_lang_reflect_Array.cc",
        "native/java_lang_reflect_Constructor.cc",
        "native/java_lang_reflect_Executable.cc",
        "native/java_lang_reflect_Field.cc",
        "native/java_lang_reflect_Method.cc",
        "native/java_lang_reflect_Parameter.cc",
        "native/java_lang_reflect_Proxy.cc",
        "native/java_util_concurrent_atomic_AtomicLong.cc",
        "native/jdk_internal_misc_Unsafe.cc",
        "native/libcore_io_Memory.cc",
        "native/libcore_util_CharsetUtils.cc",
        "native/org_apache_harmony_dalvik_ddmc_DdmServer.cc",
        "native/org_apache_harmony_dalvik_ddmc_DdmVmInternal.cc",
        "native/sun_misc_Unsafe.cc",
        "runtime_common.cc",
        "runtime_intrinsics.cc",
        "well_known_classes.cc",
    ];
    let patched_sources = [
        "runtime.cc",
        "class_linker.cc",
        "thread.cc",
        "thread_list.cc",
        "gc/heap.cc",
        "gc/collector/garbage_collector.cc",
        "gc/collector/mark_compact.cc",
        "gc/space/malloc_space.cc",
        "gc/space/space.cc",
        "entrypoints/quick/quick_alloc_entrypoints.cc",
        "entrypoints/quick/quick_trampoline_entrypoints.cc",
        "runtime_common.cc",
        "oat/oat_file.cc",
        "exec_utils.cc",
        "signal_catcher.cc",
        "nterp_helpers.cc",
        "interpreter/mterp/nterp.cc",
    ];
    sources
        .into_iter()
        .map(|source| {
            let object = staged
                .runtime_core_object_dir
                .join(format!("{}.o", source.replace('/', "_")));
            let source_path = if patched_sources.contains(&source) {
                staged.patched_runtime.join(source)
            } else {
                staged.runtime.join(source)
            };
            let mut command = runtime_bootstrap_cpp_command(includes);
            if matches!(
                source,
                "entrypoints/jni/jni_entrypoints.cc" | "oat/jni_stub_hash_map.cc"
            ) {
                command.args(["-include", "arch/arm64/jni_frame_arm64.h"]);
            }
            command
                .arg("-idirafter")
                .arg(&staged.ndk_arch_include)
                .arg("-idirafter")
                .arg(&staged.ndk_include)
                .arg("-Wno-macro-redefined")
                .arg("-c")
                .arg(source_path)
                .arg("-o")
                .arg(&object);
            PendingNativeCompile { command, object }
        })
        .collect()
}
