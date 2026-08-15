extern int DarwinArtGenericGrandchildValue(void);

__attribute__((visibility("default"))) int DarwinArtGenericChildValue(void) {
  return DarwinArtGenericGrandchildValue() + 10;
}
