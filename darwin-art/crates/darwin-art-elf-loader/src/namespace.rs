use super::{
    DsoLifecycle, ExportedSymbol, LoadError, LoadedElf, RejectAllResolver, ResolveError,
    ResolvedSymbol, STB_GLOBAL, StagedElf, SymbolRequest, SymbolResolver, VersionRequirement,
};
use std::collections::{HashMap, HashSet, VecDeque};
use std::error::Error as StdError;
use std::fmt;
use std::num::NonZeroUsize;
use std::sync::Arc;

/// An explicitly populated, closed ELF namespace.
///
/// Every ELF object is supplied under its Android logical SONAME. An embedded
/// `DT_SONAME`, when present, must match; Android path-loaded DSOs may omit it.
/// No path search, dyld lookup, `dlopen`, or process-global symbol fallback is performed.
#[derive(Default)]
pub struct ClosedElfNamespace {
    sources: HashMap<String, Vec<u8>>,
    providers: HashSet<String>,
}

#[derive(Debug)]
pub enum NamespaceError {
    DuplicateSoname(String),
    UnknownDependency {
        requested_by: String,
        soname: String,
    },
    SonameMismatch {
        supplied: String,
        embedded: Option<String>,
    },
    Load {
        soname: String,
        source: LoadError,
    },
    Lifecycle {
        soname: String,
        message: String,
    },
}

impl fmt::Display for NamespaceError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::DuplicateSoname(soname) => {
                write!(formatter, "duplicate namespace SONAME: {soname}")
            }
            Self::UnknownDependency {
                requested_by,
                soname,
            } => {
                write!(
                    formatter,
                    "{requested_by} requires unknown dependency {soname}"
                )
            }
            Self::SonameMismatch { supplied, embedded } => write!(
                formatter,
                "namespace key {supplied} does not match embedded DT_SONAME {:?}",
                embedded
            ),
            Self::Load { soname, source } => write!(formatter, "failed to load {soname}: {source}"),
            Self::Lifecycle { soname, message } => {
                write!(
                    formatter,
                    "failed to publish lifecycle for {soname}: {message}"
                )
            }
        }
    }
}

impl StdError for NamespaceError {
    fn source(&self) -> Option<&(dyn StdError + 'static)> {
        match self {
            Self::Load { source, .. } => Some(source),
            _ => None,
        }
    }
}

impl ClosedElfNamespace {
    pub fn new() -> Self {
        Self::default()
    }

    /// Adds one immutable ELF byte source. An embedded `DT_SONAME` is checked at load time.
    pub fn add_elf(
        &mut self,
        soname: impl Into<String>,
        bytes: impl Into<Vec<u8>>,
    ) -> Result<(), NamespaceError> {
        let soname = soname.into();
        if soname.is_empty() || self.sources.contains_key(&soname) {
            return Err(NamespaceError::DuplicateSoname(soname));
        }
        if self.providers.contains(&soname) {
            return Err(NamespaceError::DuplicateSoname(soname));
        }
        self.sources.insert(soname, bytes.into());
        Ok(())
    }

    /// Registers a virtual DSO SONAME whose imports are served by `load_with_resolver`.
    pub fn add_provider(&mut self, soname: impl Into<String>) -> Result<(), NamespaceError> {
        let soname = soname.into();
        if soname.is_empty()
            || self.sources.contains_key(&soname)
            || !self.providers.insert(soname.clone())
        {
            return Err(NamespaceError::DuplicateSoname(soname));
        }
        Ok(())
    }

    pub fn load(&self, root: &str) -> Result<LoadedElfGraph, NamespaceError> {
        self.load_with_resolver(root, &mut RejectAllResolver)
    }

    /// Loads a root and its complete recursive `DT_NEEDED` closure.
    ///
    /// The optional resolver is the namespace's explicit non-ELF provider source (for example,
    /// a Bionic facade). It is consulted only after lookup in the requester's ELF dependency
    /// scope and is never retained after eager relocation.
    pub fn load_with_resolver(
        &self,
        root: &str,
        external: &mut dyn SymbolResolver,
    ) -> Result<LoadedElfGraph, NamespaceError> {
        self.load_with_resolver_and_lifecycle(root, external, None)
    }

    pub fn load_with_resolver_and_lifecycle(
        &self,
        root: &str,
        external: &mut dyn SymbolResolver,
        lifecycle: Option<Arc<dyn DsoLifecycle>>,
    ) -> Result<LoadedElfGraph, NamespaceError> {
        let mut builder = GraphBuilder::default();
        if self.providers.contains(root) {
            return Err(NamespaceError::SonameMismatch {
                supplied: root.to_owned(),
                embedded: None,
            });
        }
        builder.stage_recursive(root, root, &self.sources, &self.providers)?;

        let scopes = builder.compute_scopes();
        let catalog = builder.export_catalog()?;
        let object_sonames: Vec<_> = builder
            .objects
            .iter()
            .map(|object| object.soname.clone())
            .collect();
        for &index in &builder.dependency_order {
            let mut resolver = GraphResolver {
                requester: index,
                object_sonames: &object_sonames,
                scopes: &scopes,
                catalog: &catalog,
                provider_sonames: &self.providers,
                external,
            };
            let staged = &mut builder.objects[index].staged;
            staged
                .image
                .finish_load(&mut resolver, staged.page_size, &staged.page_protections)
                .map_err(|source| NamespaceError::Load {
                    soname: builder.objects[index].soname.clone(),
                    source,
                })?;
        }

        // Publish every live reservation only after the full graph has relocated, but before
        // the first constructor can register an image-local `__dso_handle`. Attached lifecycle
        // owners are dropped transactionally if a later publication or constructor fails.
        if let Some(lifecycle) = lifecycle {
            for &index in &builder.dependency_order {
                builder.objects[index]
                    .staged
                    .image
                    .publish_dso_lifecycle(Arc::clone(&lifecycle))
                    .map_err(|message| NamespaceError::Lifecycle {
                        soname: builder.objects[index].soname.clone(),
                        message,
                    })?;
            }
        }

        if let Ok(specification) = std::env::var("DARWIN_ART_ELF_PREFLIGHT_I32") {
            if let Some((soname, symbol)) = specification.split_once(':') {
                if let Some(object) = builder
                    .objects
                    .iter()
                    .find(|object| object.soname == soname)
                {
                    if soname == "libcrypto.so" {
                        let base =
                            object
                                .staged
                                .image
                                .debug_mapped_pointer(0)
                                .map_err(|source| NamespaceError::Load {
                                    soname: object.soname.clone(),
                                    source,
                                })?;
                        for slot in [
                            0x17ab70, 0x17abd0, 0x17abe0, 0x17ac38, 0x17ad90, 0x17adf8, 0x17ae00,
                            0x17ae08, 0x17ae10,
                        ] {
                            let value = object.staged.image.debug_read_mapped_u64(slot).map_err(
                                |source| NamespaceError::Load {
                                    soname: object.soname.clone(),
                                    source,
                                },
                            )?;
                            eprintln!(
                                "DARWIN ELF preflight slot: base={base:#x} slot={slot:#x} value={value:#x} offset={:#x}",
                                value.wrapping_sub(base as u64)
                            );
                        }
                        let instructions =
                            object.staged.image.debug_read_mapped_u64(0xd35dc).map_err(
                                |source| NamespaceError::Load {
                                    soname: object.soname.clone(),
                                    source,
                                },
                            )?;
                        let (guard_address, guard_value) = object.staged.image.debug_stack_guard();
                        let entry = object.staged.image.debug_read_mapped_u64(0xd35b4).map_err(
                            |source| NamespaceError::Load {
                                soname: object.soname.clone(),
                                source,
                            },
                        )?;
                        let diagnostic_patch =
                            object.staged.image.debug_read_mapped_u64(0xd36ec).map_err(
                                |source| NamespaceError::Load {
                                    soname: object.soname.clone(),
                                    source,
                                },
                            )?;
                        let epilogue = object.staged.image.debug_read_mapped_u64(0xd3940).map_err(
                            |source| NamespaceError::Load {
                                soname: object.soname.clone(),
                                source,
                            },
                        )?;
                        eprintln!(
                            "DARWIN ELF preflight guard: address={guard_address:#x} value={guard_value:#x} entry={entry:#018x} instructions={instructions:#018x} diagnostic={diagnostic_patch:#018x} epilogue={epilogue:#018x}"
                        );
                    }
                    let result = object
                        .staged
                        .image
                        .call_exported_i32_before_initializers(symbol)
                        .map_err(|source| NamespaceError::Load {
                            soname: object.soname.clone(),
                            source,
                        })?;
                    eprintln!(
                        "DARWIN ELF preflight: soname={soname} symbol={symbol} result={result}"
                    );
                }
            }
        }

        for &index in &builder.dependency_order {
            builder.objects[index]
                .staged
                .image
                .run_initializers_for_graph()
                .map_err(|source| NamespaceError::Load {
                    soname: builder.objects[index].soname.clone(),
                    source,
                })?;
        }
        // Finalization is graph-transactional: no object is armed until every constructor in
        // dependency order has returned. A structural failure in a later initializer therefore
        // drops even the already initialized prefix without running a partial finalizer set.
        for &index in &builder.dependency_order {
            builder.objects[index].staged.image.arm_finalizers();
        }

        let root_index = *builder
            .indices
            .get(root)
            .expect("staged root must have an index");
        let load_order = builder
            .discovery_order
            .iter()
            .map(|&index| builder.objects[index].soname.clone())
            .collect();
        let initialization_order = builder
            .dependency_order
            .iter()
            .map(|&index| builder.objects[index].soname.clone())
            .collect();
        let unload_order = builder
            .dependency_order
            .iter()
            .rev()
            .map(|&index| builder.objects[index].soname.clone())
            .collect();
        let objects = builder
            .objects
            .into_iter()
            .map(|object| Some(object.staged.image))
            .collect();
        Ok(LoadedElfGraph {
            inner: Arc::new(GraphInner {
                objects,
                root_index,
                load_order,
                initialization_order,
                unload_order,
                drop_order: builder.dependency_order,
            }),
        })
    }
}

/// One loaded graph owner. Clones share its mappings until the final clone is closed.
///
/// Separate calls to `ClosedElfNamespace::load` create independent graphs; this is deliberately
/// not a namespace-global `dlopen` handle cache.
#[derive(Clone)]
pub struct LoadedElfGraph {
    inner: Arc<GraphInner>,
}

impl LoadedElfGraph {
    pub fn load_order(&self) -> &[String] {
        &self.inner.load_order
    }

    pub fn initialization_order(&self) -> &[String] {
        &self.inner.initialization_order
    }

    /// Returns the deterministic reverse-constructor finalization and mapping teardown order.
    pub fn unload_order(&self) -> &[String] {
        &self.inner.unload_order
    }

    pub fn reference_count(&self) -> usize {
        Arc::strong_count(&self.inner)
    }

    pub fn call_root_exported_i32(&self, name: &str) -> Result<i32, LoadError> {
        self.inner.objects[self.inner.root_index]
            .as_ref()
            .expect("live graph object")
            .call_exported_i32(name)
    }

    pub fn lookup_root_exported(&self, name: &str) -> Result<usize, LoadError> {
        self.inner.objects[self.inner.root_index]
            .as_ref()
            .expect("live graph object")
            .lookup_exported(name)
    }

    pub fn close(self) {}
}

struct GraphInner {
    objects: Vec<Option<LoadedElf>>,
    root_index: usize,
    load_order: Vec<String>,
    initialization_order: Vec<String>,
    unload_order: Vec<String>,
    drop_order: Vec<usize>,
}

// SAFETY: LoadedElf is Send and graph contents are immutable after eager relocation and init.
unsafe impl Send for GraphInner {}
unsafe impl Sync for GraphInner {}

impl Drop for GraphInner {
    fn drop(&mut self) {
        for &index in self.drop_order.iter().rev() {
            drop(self.objects[index].take());
        }
    }
}

struct GraphObject {
    soname: String,
    staged: StagedElf,
    dependencies: Vec<usize>,
}

#[derive(Default)]
struct GraphBuilder {
    objects: Vec<GraphObject>,
    indices: HashMap<String, usize>,
    visiting: HashSet<String>,
    visited: HashSet<String>,
    discovery_order: Vec<usize>,
    dependency_order: Vec<usize>,
}

impl GraphBuilder {
    fn stage_recursive(
        &mut self,
        requested_by: &str,
        soname: &str,
        sources: &HashMap<String, Vec<u8>>,
        providers: &HashSet<String>,
    ) -> Result<usize, NamespaceError> {
        if let Some(&index) = self.indices.get(soname) {
            return Ok(index);
        }
        let bytes = sources
            .get(soname)
            .ok_or_else(|| NamespaceError::UnknownDependency {
                requested_by: requested_by.to_owned(),
                soname: soname.to_owned(),
            })?;
        let staged = LoadedElf::stage(bytes).map_err(|source| NamespaceError::Load {
            soname: soname.to_owned(),
            source,
        })?;
        if staged
            .image
            .soname()
            .is_some_and(|embedded| embedded != soname)
        {
            return Err(NamespaceError::SonameMismatch {
                supplied: soname.to_owned(),
                embedded: staged.image.soname().map(ToOwned::to_owned),
            });
        }
        let needed = staged.image.needed_libraries().to_vec();
        let index = self.objects.len();
        self.indices.insert(soname.to_owned(), index);
        self.discovery_order.push(index);
        self.visiting.insert(soname.to_owned());
        self.objects.push(GraphObject {
            soname: soname.to_owned(),
            staged,
            dependencies: Vec::new(),
        });
        for dependency in needed {
            if providers.contains(&dependency) {
                continue;
            }
            let dependency_index = self.stage_recursive(soname, &dependency, sources, providers)?;
            self.objects[index].dependencies.push(dependency_index);
        }
        self.visiting.remove(soname);
        if self.visited.insert(soname.to_owned()) {
            self.dependency_order.push(index);
        }
        Ok(index)
    }

    fn compute_scopes(&self) -> Vec<Vec<usize>> {
        (0..self.objects.len())
            .map(|root| {
                let mut scope = Vec::new();
                let mut seen = HashSet::new();
                let mut queue = VecDeque::from([root]);
                while let Some(index) = queue.pop_front() {
                    if !seen.insert(index) {
                        continue;
                    }
                    scope.push(index);
                    queue.extend(self.objects[index].dependencies.iter().copied());
                }
                scope
            })
            .collect()
    }

    fn export_catalog(&self) -> Result<Vec<Vec<ExportedSymbol>>, NamespaceError> {
        self.objects
            .iter()
            .map(|object| {
                object
                    .staged
                    .image
                    .exported_symbols()
                    .map_err(|source| NamespaceError::Load {
                        soname: object.soname.clone(),
                        source,
                    })
            })
            .collect()
    }
}

struct GraphResolver<'a> {
    requester: usize,
    object_sonames: &'a [String],
    scopes: &'a [Vec<usize>],
    catalog: &'a [Vec<ExportedSymbol>],
    provider_sonames: &'a HashSet<String>,
    external: &'a mut dyn SymbolResolver,
}

impl GraphResolver<'_> {
    fn graph_lookup(
        &self,
        symbol: &str,
        version: Option<VersionRequirement<'_>>,
    ) -> Result<Option<usize>, ResolveError> {
        let scope = &self.scopes[self.requester];
        let candidate_objects: Vec<usize> = match version {
            Some(requirement) => {
                let Some(index) = scope
                    .iter()
                    .copied()
                    .find(|&index| self.object_sonames[index] == requirement.soname)
                else {
                    if self.provider_sonames.contains(requirement.soname) {
                        return Ok(None);
                    }
                    return Err(ResolveError::UnknownSoname(requirement.soname.to_owned()));
                };
                vec![index]
            }
            None => scope.clone(),
        };
        let mut weak = None;
        for index in candidate_objects {
            for export in &self.catalog[index] {
                if export.name != symbol {
                    continue;
                }
                let version_matches = match version {
                    Some(requirement) => {
                        export.version.as_deref() == Some(requirement.name)
                            && export.version_hidden == requirement.hidden
                    }
                    None => !export.version_hidden,
                };
                if !version_matches {
                    continue;
                }
                if export.binding == STB_GLOBAL {
                    return Ok(Some(export.address));
                }
                weak.get_or_insert(export.address);
            }
        }
        if let Some(requirement) = version {
            let provider_has_symbol = scope.iter().copied().any(|index| {
                self.object_sonames[index] == requirement.soname
                    && self.catalog[index]
                        .iter()
                        .any(|export| export.name == symbol)
            });
            if provider_has_symbol {
                return Err(ResolveError::VersionMismatch {
                    soname: requirement.soname.to_owned(),
                    symbol: symbol.to_owned(),
                    requested: requirement.name.to_owned(),
                });
            }
        }
        Ok(weak)
    }
}

impl SymbolResolver for GraphResolver<'_> {
    fn resolve(
        &mut self,
        request: SymbolRequest<'_>,
    ) -> Result<Option<ResolvedSymbol>, ResolveError> {
        if let Some(address) = self.graph_lookup(request.symbol, request.version)? {
            let address = NonZeroUsize::new(address)
                .ok_or_else(|| ResolveError::Rejected("ELF export has null address".to_owned()))?;
            // SAFETY: the graph owns the provider mapping until every graph handle is dropped.
            return Ok(Some(unsafe { ResolvedSymbol::new(address) }));
        }
        let provider_is_explicit = request
            .version
            .is_some_and(|requirement| self.provider_sonames.contains(requirement.soname))
            || request
                .needed_libraries
                .iter()
                .any(|soname| self.provider_sonames.contains(soname));
        if provider_is_explicit {
            self.external.resolve(request)
        } else {
            Ok(None)
        }
    }
}
