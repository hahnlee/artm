extern int DarwinArtGenericChildValue(void);

static int g_generic_graph_value;

__attribute__((constructor)) static void GenericGraphInitialize(void) {
  g_generic_graph_value = DarwinArtGenericChildValue();
}

__attribute__((visibility("default"))) int JNI_OnLoad(void* vm, void* reserved) {
  return vm != 0 && reserved == 0 && g_generic_graph_value == 20 ? 0x00010006 : -1;
}
