extern int fixture_missing_import(void);

__attribute__((visibility("default"))) int fixture_import(void) {
    return fixture_missing_import();
}

