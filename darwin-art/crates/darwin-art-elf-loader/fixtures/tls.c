static __thread int fixture_tls_state = 7;

__attribute__((visibility("default"))) int fixture_tls(void) {
    return fixture_tls_state;
}

