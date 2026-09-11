package dev.darwinart.security;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.Key;
import java.security.KeyStore;
import java.security.KeyStoreException;
import java.security.KeyStoreSpi;
import java.security.InvalidAlgorithmParameterException;
import java.security.InvalidKeyException;
import java.security.SecureRandom;
import java.security.NoSuchAlgorithmException;
import java.security.cert.Certificate;
import java.security.cert.CertificateException;
import java.util.Collections;
import java.util.Date;
import java.util.Enumeration;
import java.util.Map;
import java.util.HashMap;
import javax.crypto.MacSpi;
import javax.crypto.KeyGeneratorSpi;
import javax.crypto.SecretKey;
import javax.crypto.ShortBufferException;
import java.security.spec.AlgorithmParameterSpec;

/** Explicit AndroidKeyStore boundary; no host key material is exposed. */
public final class DarwinAndroidKeyStore extends KeyStoreSpi {
    private static final Map<String, SecretKey> KEYS = new HashMap<>();
    static synchronized SecretKey key(String alias) { return KEYS.get(alias); }
    static synchronized void put(String alias, SecretKey key) { KEYS.put(alias, key); }
    @Override public Key engineGetKey(String alias, char[] password) { return key(alias); }
    @Override public Certificate[] engineGetCertificateChain(String alias) { return null; }
    @Override public Certificate engineGetCertificate(String alias) { return null; }
    @Override public Date engineGetCreationDate(String alias) { return null; }
    @Override public void engineSetKeyEntry(String alias, Key key, char[] password,
            Certificate[] chain) throws KeyStoreException {
        throw new KeyStoreException("AndroidKeyStore key import is unsupported on Darwin");
    }
    @Override public void engineSetKeyEntry(String alias, byte[] key,
            Certificate[] chain) throws KeyStoreException {
        throw new KeyStoreException("AndroidKeyStore key import is unsupported on Darwin");
    }
    @Override public void engineSetCertificateEntry(String alias, Certificate cert)
            throws KeyStoreException {
        throw new KeyStoreException("AndroidKeyStore certificate import is unsupported on Darwin");
    }
    @Override public void engineDeleteEntry(String alias) throws KeyStoreException {
        throw new KeyStoreException("AndroidKeyStore is empty on Darwin");
    }
    @Override public Enumeration<String> engineAliases() {
        return Collections.emptyEnumeration();
    }
    @Override public boolean engineContainsAlias(String alias) { return false; }
    @Override public int engineSize() { return 0; }
    @Override public boolean engineIsKeyEntry(String alias) { return false; }
    @Override public boolean engineIsCertificateEntry(String alias) { return false; }
    @Override public String engineGetCertificateAlias(Certificate cert) { return null; }
    @Override public void engineStore(OutputStream stream, char[] password)
            throws IOException, NoSuchAlgorithmException, CertificateException {
        if (stream != null) throw new IOException("AndroidKeyStore is not exportable");
    }
    @Override public void engineLoad(InputStream stream, char[] password)
            throws IOException, NoSuchAlgorithmException, CertificateException {
        if (stream != null) throw new IOException("AndroidKeyStore is not importable");
    }
    @Override public KeyStore.Entry engineGetEntry(String alias,
            KeyStore.ProtectionParameter protection) { return null; }
    @Override public boolean engineEntryInstanceOf(String alias,
            Class<? extends KeyStore.Entry> entryClass) { return false; }

    public static final class HmacKey implements SecretKey {
        private final byte[] material;
        HmacKey(byte[] material) { this.material = material.clone(); }
        byte[] material() { return material.clone(); }
        @Override public String getAlgorithm() { return "HmacSHA256"; }
        @Override public String getFormat() { return null; }
        @Override public byte[] getEncoded() { return null; }
    }

    public static final class HmacKeyGenerator extends KeyGeneratorSpi {
        private SecureRandom random = new SecureRandom();
        private String alias = "key2";
        @Override protected void engineInit(SecureRandom random) {
            if (random != null) this.random = random;
        }
        @Override protected void engineInit(AlgorithmParameterSpec params, SecureRandom random)
                throws InvalidAlgorithmParameterException {
            if (random != null) this.random = random;
            if (params != null) {
                try {
                    java.lang.reflect.Method getAlias = params.getClass().getMethod("getKeystoreAlias");
                    Object value = getAlias.invoke(params);
                    if (value instanceof String) alias = (String) value;
                } catch (ReflectiveOperationException ignored) { }
            }
        }
        @Override protected void engineInit(int keysize, SecureRandom random) {
            if (random != null) this.random = random;
        }
        @Override protected SecretKey engineGenerateKey() {
            byte[] material = new byte[32];
            random.nextBytes(material);
            HmacKey key = new HmacKey(material);
            put(alias, key);
            return key;
        }
    }

    public static final class HmacMac extends MacSpi {
        private byte[] key;
        private final java.io.ByteArrayOutputStream input = new java.io.ByteArrayOutputStream();
        @Override protected int engineGetMacLength() { return 32; }
        @Override protected void engineInit(Key key, AlgorithmParameterSpec params)
                throws InvalidKeyException {
            if (key instanceof HmacKey) this.key = ((HmacKey) key).material();
            else if (key instanceof SecretKey && key.getEncoded() != null) {
                this.key = key.getEncoded().clone();
            } else throw new InvalidKeyException("not a usable HMAC key");
            engineReset();
        }
        @Override protected void engineUpdate(byte inputByte) { input.write(inputByte); }
        @Override protected void engineUpdate(byte[] bytes, int offset, int len) {
            input.write(bytes, offset, len);
        }
        @Override protected byte[] engineDoFinal() {
            if (key == null) throw new IllegalStateException("HMAC key is not initialized");
            byte[] block = new byte[64];
            byte[] material = key;
            if (material.length > block.length) material = sha256(material);
            System.arraycopy(material, 0, block, 0, material.length);
            byte[] outer = block.clone(), inner = block.clone();
            for (int i = 0; i < block.length; i++) { outer[i] ^= 0x5c; inner[i] ^= 0x36; }
            byte[] message = input.toByteArray();
            byte[] innerHash = sha256(concat(inner, message));
            byte[] result = sha256(concat(outer, innerHash));
            engineReset();
            return result;
        }
        @Override protected void engineReset() { input.reset(); }
        private static byte[] sha256(byte[] bytes) {
            try { return java.security.MessageDigest.getInstance("SHA-256").digest(bytes); }
            catch (java.security.NoSuchAlgorithmException e) { throw new AssertionError(e); }
        }
        private static byte[] concat(byte[] a, byte[] b) {
            byte[] out = new byte[a.length + b.length];
            System.arraycopy(a, 0, out, 0, a.length);
            System.arraycopy(b, 0, out, a.length, b.length);
            return out;
        }
    }
}
