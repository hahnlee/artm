package android.os;

/** Signature-only hidden API stub for compiler-side AOSP tests. */
public class Parcel {
  public void writeString(String value) {}
  public String readString() { return null; }
  public void writeStrongBinder(IBinder value) {}
  public IBinder readStrongBinder() { return null; }
  public void writeParcelable(Parcelable value, int flags) {}
  public <T extends Parcelable> T readParcelable(ClassLoader loader) { return null; }
}
