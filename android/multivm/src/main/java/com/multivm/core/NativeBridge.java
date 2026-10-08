package com.multivm.core;

import java.nio.ByteBuffer;

/** Declaracoes das funcoes nativas (libmultivm.so). Uso interno. */
final class NativeBridge {
    static {
        System.loadLibrary("multivm");
    }

    private NativeBridge() {}

    /** Recebe eventos do codigo nativo (chamado na thread da VM). */
    interface Callbacks {
        void onSerial(byte[] data);
    }

    interface LogListener {
        void onLog(int level, String message);
    }

    static native String nativeVersion();
    static native void nativeSetLogLevel(int level);
    static native void nativeSetLogListener(LogListener listener);
    static native String nativeArchName(int arch);

    static native DiskImages.Info nativeImageInfo(String path, int fd) throws java.io.IOException;
    static native void nativeImageCreate(String path, long size, int blockSize) throws java.io.IOException;
    static native void nativeImageConvert(String src, int srcFd, String dst, int format, int flags, int blockSize,
                                          DiskImages.Progress listener) throws java.io.IOException;

    static native long nativeCreate(int arch, int ramMb, String kernel, String initrd, String cmdline,
                                    String dtb, String firmware, String vgaBios, String bootOrder,
                                    String[] diskPaths, int[] diskFds, boolean[] diskReadOnly, int[] diskTypes,
                                    String[] diskNames, int fbWidth, int fbHeight, long rawLoadAddress,
                                    int netModel, String mac, String dns, int[] forwards,
                                    int audioModel, boolean mic, Callbacks callbacks);
    static native void nativeDestroy(long handle);
    static native int nativeRun(long handle);
    static native void nativeStop(long handle);
    static native void nativeReset(long handle);
    static native void nativePause(long handle);
    static native void nativeResume(long handle);
    static native boolean nativeIsPaused(long handle);
    static native int nativeSerialInput(long handle, byte[] data, int off, int len);
    static native void nativeKeyEvent(long handle, int evdevCode, boolean down);
    static native void nativePointerEvent(long handle, int dx, int dy, int dz, int buttons);
    static native long nativeInstructionCount(long handle);
    static native long nativeClockLag(long handle);
    static native void nativePowerButton(long handle);
    static native int[] nativeFbInfo(long handle);
    static native int nativeFbGeneration(long handle);
    /** (largura << 32) | altura; negativo se dst for pequeno; 0 sem framebuffer. */
    static native long nativeFbCopyInts(long handle, int[] dst);
    static native long nativeFbCopyRgba(long handle, ByteBuffer dst);
    /** 1 copiou, 0 sem mudanca, -1 tamanho diferente, -2 sem libjnigraphics. state = {geracao, y0, y1}. */
    static native int nativeFbCopyBitmap(long handle, android.graphics.Bitmap bitmap, int[] state);
    static native String nativeTextScreen(long handle);
    static native void nativeAudioMute(long handle, boolean muted);
    static native String[] nativeMediaList(long handle);
    static native void nativeMediaInsert(long handle, int slot, String path, int fd, boolean readOnly, String name)
            throws java.io.IOException;
    static native void nativeMediaEject(long handle, int slot) throws java.io.IOException;
    static native int nativeMediaAddDisk(long handle, String path, int fd, boolean readOnly, String name)
            throws java.io.IOException;
    static native int nativeAudioState(long handle);
    static native long nativeVncStart(long handle, String bindAddress, int port, String password)
            throws java.io.IOException;
    static native void nativeVncStop(long vnc);
    static native int nativeVncClients(long vnc);
}
