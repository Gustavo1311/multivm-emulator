package com.multivm.core;

/** Geometria do framebuffer da VM (formato XRGB8888). */
public final class FramebufferInfo {
    public final int width;
    public final int height;
    /** Bytes por linha. */
    public final int stride;

    FramebufferInfo(int width, int height, int stride) {
        this.width = width;
        this.height = height;
        this.stride = stride;
    }

    @Override
    public String toString() {
        return width + "x" + height;
    }
}
