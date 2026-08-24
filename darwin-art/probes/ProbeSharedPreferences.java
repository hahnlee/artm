package dev.darwinart.probe;

import android.content.SharedPreferences;

import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/** Process-owned preferences used until the Darwin ContextImpl storage layer exists. */
public final class ProbeSharedPreferences implements SharedPreferences {
    private static final Object REMOVED = new Object();
    private final Map<String, Object> values = new HashMap<>();
    private final Set<OnSharedPreferenceChangeListener> listeners = new HashSet<>();

    @Override
    public synchronized Map<String, ?> getAll() {
        return Collections.unmodifiableMap(new HashMap<>(values));
    }

    @Override
    public synchronized String getString(String key, String defaultValue) {
        Object value = values.get(key);
        return value instanceof String ? (String) value : defaultValue;
    }

    @Override
    @SuppressWarnings("unchecked")
    public synchronized Set<String> getStringSet(String key, Set<String> defaultValue) {
        Object value = values.get(key);
        return value instanceof Set ? new HashSet<>((Set<String>) value) : defaultValue;
    }

    @Override
    public synchronized int getInt(String key, int defaultValue) {
        Object value = values.get(key);
        return value instanceof Integer ? (Integer) value : defaultValue;
    }

    @Override
    public synchronized long getLong(String key, long defaultValue) {
        Object value = values.get(key);
        return value instanceof Long ? (Long) value : defaultValue;
    }

    @Override
    public synchronized float getFloat(String key, float defaultValue) {
        Object value = values.get(key);
        return value instanceof Float ? (Float) value : defaultValue;
    }

    @Override
    public synchronized boolean getBoolean(String key, boolean defaultValue) {
        Object value = values.get(key);
        return value instanceof Boolean ? (Boolean) value : defaultValue;
    }

    @Override
    public synchronized boolean contains(String key) {
        return values.containsKey(key);
    }

    @Override
    public Editor edit() {
        return new EditorImpl();
    }

    @Override
    public synchronized void registerOnSharedPreferenceChangeListener(
            OnSharedPreferenceChangeListener listener) {
        if (listener != null) listeners.add(listener);
    }

    @Override
    public synchronized void unregisterOnSharedPreferenceChangeListener(
            OnSharedPreferenceChangeListener listener) {
        listeners.remove(listener);
    }

    private final class EditorImpl implements Editor {
        private final Map<String, Object> changes = new HashMap<>();
        private boolean clear;

        @Override
        public Editor putString(String key, String value) {
            changes.put(key, value == null ? REMOVED : value);
            return this;
        }

        @Override
        public Editor putStringSet(String key, Set<String> value) {
            changes.put(key, value == null ? REMOVED : new HashSet<>(value));
            return this;
        }

        @Override
        public Editor putInt(String key, int value) {
            changes.put(key, value);
            return this;
        }

        @Override
        public Editor putLong(String key, long value) {
            changes.put(key, value);
            return this;
        }

        @Override
        public Editor putFloat(String key, float value) {
            changes.put(key, value);
            return this;
        }

        @Override
        public Editor putBoolean(String key, boolean value) {
            changes.put(key, value);
            return this;
        }

        @Override
        public Editor remove(String key) {
            changes.put(key, REMOVED);
            return this;
        }

        @Override
        public Editor clear() {
            clear = true;
            return this;
        }

        @Override
        public boolean commit() {
            applyChanges();
            return true;
        }

        @Override
        public void apply() {
            applyChanges();
        }

        private void applyChanges() {
            Set<String> changed = new HashSet<>();
            Set<OnSharedPreferenceChangeListener> observers;
            synchronized (ProbeSharedPreferences.this) {
                if (clear) {
                    changed.addAll(values.keySet());
                    values.clear();
                }
                for (Map.Entry<String, Object> entry : changes.entrySet()) {
                    changed.add(entry.getKey());
                    if (entry.getValue() == REMOVED) {
                        values.remove(entry.getKey());
                    } else {
                        values.put(entry.getKey(), entry.getValue());
                    }
                }
                observers = new HashSet<>(listeners);
            }
            for (OnSharedPreferenceChangeListener listener : observers) {
                for (String key : changed) {
                    listener.onSharedPreferenceChanged(ProbeSharedPreferences.this, key);
                }
            }
        }
    }
}
