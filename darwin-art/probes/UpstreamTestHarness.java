package dev.darwinart.probe;

import java.io.OutputStream;
import java.io.PrintStream;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.HashSet;
import java.util.Set;

/** Process-local output capture for unmodified pinned AOSP ART run tests. */
public final class UpstreamTestHarness {
    private UpstreamTestHarness() {}

    private static PrintStream originalOut;
    private static PrintStream originalErr;
    private static Thread shutdownHook;
    private static boolean outputPrepared;

    public static Class<?> load(String className) throws ClassNotFoundException {
        return Class.forName(className, false, Thread.currentThread().getContextClassLoader());
    }

    private static native void writeOutputByte(int channel, int value);
    private static native void writeOutputChunk(int channel, byte[] bytes, int offset, int length);

    private static void flushOutput() {
        System.out.flush();
        System.err.flush();
    }

    private static final class NativeOutputStream extends OutputStream {
        private final int channel;

        NativeOutputStream(int channel) { this.channel = channel; }

        @Override public void write(int value) {
            writeOutputByte(channel, value);
        }

        @Override public void write(byte[] bytes, int offset, int length) {
            if (bytes == null) throw new NullPointerException("bytes");
            if ((offset | length) < 0 || length > bytes.length - offset) {
                throw new IndexOutOfBoundsException();
            }
            if (length != 0) writeOutputChunk(channel, bytes, offset, length);
        }
    }

    private static final class OutputShutdownHook extends Thread {
        @Override public void run() { flushOutput(); }
    }

    public static void prepareOutput() {
        if (outputPrepared) return;
        originalOut = System.out;
        originalErr = System.err;
        shutdownHook = new OutputShutdownHook();
        System.setOut(new PrintStream(new NativeOutputStream(0), true));
        System.setErr(new PrintStream(new NativeOutputStream(1), true));
        Runtime.getRuntime().addShutdownHook(shutdownHook);
        outputPrepared = true;
    }

    private static final class TestMainThread extends Thread {
        private final String className;
        private volatile Throwable failure;

        TestMainThread(String className) {
            super("ART run-test main");
            this.className = className;
        }

        @Override public void run() {
            try {
                Class<?> mainClass = load(className);
                Method main = mainClass.getDeclaredMethod("main", String[].class);
                main.setAccessible(true);
                main.invoke(null, (Object) new String[0]);
            } catch (InvocationTargetException exception) {
                failure = exception.getCause();
            } catch (Throwable throwable) {
                failure = throwable;
            }
        }
    }

    private static void awaitNewNonDaemonThreads(Set<Thread> baseline) throws InterruptedException {
        while (true) {
            Set<Thread> running = Thread.getAllStackTraces().keySet();
            Thread pending = null;
            for (Thread thread : running) {
                if (!baseline.contains(thread) && thread != Thread.currentThread()
                        && thread.isAlive() && !thread.isDaemon()) {
                    pending = thread;
                    break;
                }
            }
            if (pending == null) return;
            pending.join();
        }
    }

    public static Set<Thread> beginRun() {
        prepareOutput();
        return new HashSet<>(Thread.getAllStackTraces().keySet());
    }

    public static boolean dispatchExplicitUncaughtException(Throwable failure) {
        Thread thread = Thread.currentThread();
        Thread.UncaughtExceptionHandler handler = thread.getUncaughtExceptionHandler();
        // ThreadGroup is Java's implicit fallback. A dalvikvm-style launcher
        // reports an unhandled Main exception as failure. ThreadGroup must
        // still run when an application installed the process-wide default
        // handler because its uncaughtException() delegates to that handler.
        if (handler == null || (handler == thread.getThreadGroup()
                && Thread.getDefaultUncaughtExceptionHandler() == null)) return false;
        handler.uncaughtException(thread, failure);
        return true;
    }

    public static Throwable finishRun(Set<Thread> baseline, Throwable failure) {
        // AOSP keeps a process alive while application-created non-daemon
        // threads are running. The Darwin host cannot call DestroyJavaVM for
        // an Android process (the process lifetime is the shutdown boundary),
        // so perform the same join before publishing captured output and
        // returning to the host's _exit path. This is observable Android
        // process semantics, not a test-specific delay.
        try {
            awaitNewNonDaemonThreads(baseline);
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            if (failure == null) failure = interrupted;
        }
        if (failure != null) {
            // Match dalvikvm's owner-thread uncaught-exception presentation.
            // The native launcher uses the returned Throwable only to select
            // its exit status and must not print it a second time.
            System.err.print("Exception in thread \"main\" ");
            failure.printStackTrace(System.err);
        }
        flushOutput();
        // Keep the descriptor bridge installed through owner-thread detach and
        // VM destruction. JVMTI tests intentionally observe that final phase.
        return failure;
    }

    public static Throwable run(String className) {
        Throwable failure = null;
        try {
            prepareOutput();
            Set<Thread> baseline = new HashSet<>(Thread.getAllStackTraces().keySet());
            TestMainThread mainThread = new TestMainThread(className);
            mainThread.start();
            mainThread.join();
            awaitNewNonDaemonThreads(baseline);
            failure = mainThread.failure;
        } catch (Throwable throwable) {
            failure = throwable;
        } finally {
            if (failure != null) failure.printStackTrace(System.err);
            flushOutput();
            if (outputPrepared) {
                System.setOut(originalOut);
                System.setErr(originalErr);
                Runtime.getRuntime().removeShutdownHook(shutdownHook);
                outputPrepared = false;
            }
        }
        return failure;
    }
}
