use darwin_art_elf_loader::{
    Capability, ClosedElfNamespace, LoadError, NamespaceError, ResolveError, ResolvedSymbol,
    SymbolRequest, SymbolResolver,
};
use std::env;
use std::fs;
use std::num::NonZeroUsize;

const PARENT: &str = "libgraph_parent.so";
const DEP_A: &str = "libgraph_dep_a.so";
const DEP_B: &str = "libgraph_dep_b.so";
const CYCLE_A: &str = "libgraph_cycle_a.so";
const CYCLE_B: &str = "libgraph_cycle_b.so";
const ABSOLUTE: &str = "libgraph_absolute.so";

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let arguments: Vec<_> = env::args_os().collect();
    if arguments.len() != 10 {
        return Err("usage: elf-namespace-gate PARENT DEP_A DEP_A_ALT DEP_A_WRONG DEP_B CYCLE_A CYCLE_B UNKNOWN_PARENT ABSOLUTE".into());
    }
    let parent = fs::read(&arguments[1])?;
    let dep_a = fs::read(&arguments[2])?;
    let dep_a_alt = fs::read(&arguments[3])?;
    let dep_a_wrong = fs::read(&arguments[4])?;
    let dep_b = fs::read(&arguments[5])?;
    let cycle_a = fs::read(&arguments[6])?;
    let cycle_b = fs::read(&arguments[7])?;
    let unknown_parent = fs::read(&arguments[8])?;
    let absolute = fs::read(&arguments[9])?;

    let first = make_graph(&parent, &dep_a, &dep_b)?.load(PARENT)?;
    let first_value = first.call_root_exported_i32("graph_value")?;
    if first.load_order() != [PARENT, DEP_B, DEP_A]
        || first.initialization_order() != [DEP_A, DEP_B, PARENT]
        || first.unload_order() != [PARENT, DEP_B, DEP_A]
        || first_value != 62
    {
        return Err(format!(
            "recursive load/scope/constructor order mismatch: load={:?} init={:?} unload={:?} value={first_value}",
            first.load_order(), first.initialization_order(), first.unload_order()
        ).into());
    }
    let retained = first.clone();
    if first.reference_count() != 2 {
        return Err("graph retain did not increment the reference count".into());
    }
    first.close();
    if retained.reference_count() != 1 || retained.call_root_exported_i32("graph_value")? != 62 {
        return Err("graph close released live mappings too early".into());
    }
    retained.close();

    let isolated = make_graph(&parent, &dep_a_alt, &dep_b)?.load(PARENT)?;
    if isolated.call_root_exported_i32("graph_value")? != 202 {
        return Err("same-SONAME namespaces leaked into each other".into());
    }

    let mut virtualized = ClosedElfNamespace::new();
    virtualized.add_elf(PARENT, parent.as_slice())?;
    virtualized.add_elf(DEP_A, dep_a.as_slice())?;
    virtualized.add_provider(DEP_B)?;
    let mut provider = DepBProvider::default();
    let virtualized = virtualized.load_with_resolver(PARENT, &mut provider)?;
    if virtualized.call_root_exported_i32("graph_value")? != 37 || provider.requests != 2 {
        return Err("explicit virtual DSO provider source mismatch".into());
    }

    match make_graph(&parent, &dep_a_wrong, &dep_b)?.load(PARENT) {
        Err(NamespaceError::Load {
            source:
                darwin_art_elf_loader::LoadError::Resolver {
                    source: ResolveError::VersionMismatch { .. },
                    ..
                },
            ..
        }) => {}
        Err(error) => return Err(format!("wrong version mismatch error: {error}").into()),
        Ok(_) => return Err("mismatched GNU symbol version loaded".into()),
    }

    let mut cycle = ClosedElfNamespace::new();
    cycle.add_elf(CYCLE_A, cycle_a)?;
    cycle.add_elf(CYCLE_B, cycle_b)?;
    let cycle = cycle.load(CYCLE_A)?;
    if cycle.load_order() != [CYCLE_A, CYCLE_B]
        || cycle.initialization_order() != [CYCLE_B, CYCLE_A]
        || cycle.call_root_exported_i32("cycle_a_value")? != 22
    {
        return Err("cycle graph was not accepted deterministically".into());
    }

    let mut unknown = ClosedElfNamespace::new();
    unknown.add_elf(PARENT, unknown_parent)?;
    match unknown.load(PARENT) {
        Err(NamespaceError::UnknownDependency { soname, .. }) if soname == DEP_B => {}
        Err(error) => {
            return Err(format!("unknown dependency returned wrong error: {error}").into());
        }
        Ok(_) => return Err("unknown dependency escaped the closed namespace".into()),
    }

    let mut absolute_namespace = ClosedElfNamespace::new();
    absolute_namespace.add_elf(ABSOLUTE, absolute)?;
    match absolute_namespace.load(ABSOLUTE) {
        Err(NamespaceError::Load {
            source: LoadError::Capability(Capability::AbsoluteSymbolDefinition),
            ..
        }) => {}
        Err(error) => return Err(format!("absolute symbol returned wrong error: {error}").into()),
        Ok(_) => return Err("SHN_ABS definition did not fail closed".into()),
    }

    println!(
        "elf-namespace-gate: recursive=parent+2deps scope=BFS version=GNU weak=zero \
         constructors=dependency-first cycle=accepted isolation=same-SONAME \
         provider=explicit rollback=RAII owner=Arc-clone close=last mapping-unload=reverse \
         protected=supported ABS=reject closed=no-dyld"
    );
    Ok(())
}

fn make_graph(
    parent: &[u8],
    dep_a: &[u8],
    dep_b: &[u8],
) -> Result<ClosedElfNamespace, NamespaceError> {
    let mut namespace = ClosedElfNamespace::new();
    namespace.add_elf(PARENT, parent)?;
    // Deliberately add sources in an order unrelated to DT_NEEDED traversal.
    namespace.add_elf(DEP_A, dep_a)?;
    namespace.add_elf(DEP_B, dep_b)?;
    Ok(namespace)
}

unsafe extern "C" fn provided_dep_b_value() -> i32 {
    7
}

#[derive(Default)]
struct DepBProvider {
    requests: usize,
}

impl SymbolResolver for DepBProvider {
    fn resolve(
        &mut self,
        request: SymbolRequest<'_>,
    ) -> Result<Option<ResolvedSymbol>, ResolveError> {
        self.requests += 1;
        if request.needed_libraries != [DEP_B, DEP_A] {
            return Err(ResolveError::Rejected(
                "virtual provider received the wrong dependency scope".to_owned(),
            ));
        }
        if request.symbol == "optional_graph_value" {
            return Ok(None);
        }
        if request.symbol != "dep_b_value" || request.version.is_some() {
            return Err(ResolveError::Rejected(format!(
                "unexpected virtual provider request: {}",
                request.symbol
            )));
        }
        let address = NonZeroUsize::new(provided_dep_b_value as usize)
            .ok_or_else(|| ResolveError::Rejected("null fixture provider".to_owned()))?;
        // SAFETY: the function has static lifetime and the exact no-argument i32 fixture ABI.
        Ok(Some(unsafe { ResolvedSymbol::new(address) }))
    }
}
