extern int provider_value(void);
extern int provider_data;

static int (*provider_slot)(void) = provider_value;

__attribute__((visibility("default"))) int imported_value(void) {
    return provider_value() + provider_slot() + provider_data;
}
