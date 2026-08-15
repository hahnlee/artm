# Contributing

## Commits

Use Conventional Commits for every commit:

```text
<type>(<optional-scope>): <imperative summary>
```

Common types are `feat`, `fix`, `perf`, `refactor`, `docs`, `test`, `build`,
`ci`, and `chore`. Keep each commit focused and buildable when practical.

Examples:

```text
feat(art): execute app dex through PathClassLoader
fix(darwin): preserve compressed references above PAGEZERO
perf(gpu): avoid framebuffer copies during presentation
docs(architecture): document the host graphics boundary
```

Generated outputs, downloaded AOSP trees, local Android system inputs, and
tool caches must stay out of Git. Revision locks and reproducible source/build
orchestration belong in Git.
