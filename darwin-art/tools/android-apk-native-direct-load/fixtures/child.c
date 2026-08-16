extern int DarwinArtApkDirectGrandchildValue(void);

__attribute__((visibility("default"))) int DarwinArtApkDirectChildValue(void) {
  return DarwinArtApkDirectGrandchildValue() + 20;
}
