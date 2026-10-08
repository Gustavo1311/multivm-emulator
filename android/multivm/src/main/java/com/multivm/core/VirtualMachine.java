package com.multivm.core;

import android.graphics.Bitmap;
import android.os.ParcelFileDescriptor;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * Uma maquina virtual (x86-64, i386, ARM ou ARM64).
 *
 * <pre>
 * VmConfig cfg = VmConfig.builder(Architecture.ARM64)
 *         .ramMb(512)
 *         .kernel(dir + "/vmlinuz")
 *         .initrd(dir + "/initramfs")
 *         .addDisk(DiskImage.fromPath(dir + "/disk.img", false))
 *         .build();
 * VirtualMachine vm = VirtualMachine.create(cfg);
 * vm.addSerialListener(data -> terminal.append(data));
 * vm.start();
 * ...
 * vm.close();
 * </pre>
 *
 * A emulacao roda em uma thread propria criada por {@link #start()}. Os
 * listeners de serial e de estado sao chamados nessa thread: nao bloqueie
 * dentro deles (repasse para a thread de UI com um Handler).
 */
public final class VirtualMachine implements AutoCloseable {

    public enum State { CREATED, RUNNING, PAUSED, STOPPED, CLOSED }

    public interface SerialListener {
        /** Bytes escritos pelo convidado na serial principal (ttyS0 / ttyAMA0). */
        void onSerialOutput(byte[] data);
    }

    public interface StateListener {
        void onStateChanged(VirtualMachine vm, State state, ExitReason reason);
    }

    /** Botoes do ponteiro para {@link #sendPointer}. */
    public static final int BUTTON_LEFT = 1, BUTTON_RIGHT = 2, BUTTON_MIDDLE = 4;

    private final VmConfig config;
    private final List<SerialListener> serialListeners = new CopyOnWriteArrayList<>();
    private final List<StateListener> stateListeners = new CopyOnWriteArrayList<>();
    private final Object lock = new Object();
    private long handle;
    private Thread thread;
    private volatile State state = State.CREATED;
    private volatile ExitReason lastExit;

    private VirtualMachine(VmConfig config) {
        this.config = config;
    }

    /** Cria a VM (carrega kernel/initrd e prepara a maquina). Lanca IllegalArgumentException em erro. */
    public static VirtualMachine create(VmConfig config) throws IOException {
        VirtualMachine vm = new VirtualMachine(config);
        List<DiskImage> disks = config.disks;
        String[] paths = new String[disks.size()];
        int[] fds = new int[disks.size()];
        boolean[] ro = new boolean[disks.size()];
        int[] types = new int[disks.size()];
        String[] names = new String[disks.size()];
        for (int i = 0; i < disks.size(); i++) {
            DiskImage d = disks.get(i);
            paths[i] = d.path;
            ro[i] = d.readOnly;
            types[i] = d.type.nativeId;
            names[i] = d.name;
            fds[i] = d.pfd != null ? d.pfd.dup().detachFd() : -1; /* a VM fecha o duplicado */
        }
        NativeBridge.Callbacks cb = data -> {
            for (SerialListener l : vm.serialListeners) l.onSerialOutput(data);
        };
        int[] fw = new int[config.forwards.size() * 3];
        for (int i = 0; i < config.forwards.size(); i++) {
            VmConfig.PortForward f = config.forwards.get(i);
            fw[3 * i] = (f.udp ? 1 : 0) | (f.lan ? 2 : 0);
            fw[3 * i + 1] = f.hostPort;
            fw[3 * i + 2] = f.guestPort;
        }
        vm.handle = NativeBridge.nativeCreate(config.arch.nativeId, config.ramMb, config.kernel, config.initrd,
                config.cmdline, config.dtb, config.firmware, config.vgaBios, config.bootOrder, paths, fds, ro,
                types, names, config.fbWidth, config.fbHeight, config.rawLoadAddress,
                config.nic.nativeId, config.mac, config.dns, fw, config.sound.nativeId, config.mic, cb);
        return vm;
    }

    /* ---- midias com a VM ligada ---- */

    /** Lista os drives da VM (CD, disquete e discos) e o que esta em cada um. */
    public List<MediaSlot> getMediaSlots() {
        String[] raw = NativeBridge.nativeMediaList(handle());
        List<MediaSlot> out = new java.util.ArrayList<>(raw.length);
        for (int i = 0; i < raw.length; i++) out.add(MediaSlot.parse(i, raw[i]));
        return out;
    }

    /**
     * Coloca a imagem no drive: ISO num CD, imagem num disquete, ou um disco num slot
     * SATA vazio (hot-plug). A midia anterior e desligada e fechada.
     */
    public void insertMedia(int slot, DiskImage image) throws IOException {
        int fd = image.pfd != null ? image.pfd.dup().detachFd() : -1; /* a VM fecha o duplicado */
        NativeBridge.nativeMediaInsert(handle(), slot, image.path, fd, image.readOnly, image.name);
    }

    /** Ejeta o CD/disquete, ou desconecta o disco SATA (hot-unplug). */
    public void ejectMedia(int slot) throws IOException {
        NativeBridge.nativeMediaEject(handle(), slot);
    }

    /** Conecta um disco rigido novo numa porta SATA livre; devolve o slot. Exige controlador AHCI. */
    public int addHardDisk(DiskImage image) throws IOException {
        int fd = image.pfd != null ? image.pfd.dup().detachFd() : -1;
        return NativeBridge.nativeMediaAddDisk(handle(), image.path, fd, image.readOnly, image.name);
    }

    /* ---- som ---- */

    /** Estado do som: sem placa, tocando pelo AAudio ou placa sem saida (Android 7). */
    public enum AudioState { NONE, ACTIVE, UNSUPPORTED }

    public AudioState getAudioState() {
        int s = NativeBridge.nativeAudioState(handle());
        return s == 1 ? AudioState.ACTIVE : s == 2 ? AudioState.UNSUPPORTED : AudioState.NONE;
    }

    /** Silencia a saida de som (o convidado continua tocando normalmente). */
    public void setAudioMuted(boolean muted) { NativeBridge.nativeAudioMute(handle(), muted); }

    /* ---- VNC ---- */

    private long vnc;

    /**
     * Liga o servidor VNC desta VM. lan = false aceita so conexoes deste aparelho
     * (127.0.0.1); true aceita da rede local. password: ate 8 caracteres, ou null.
     */
    public void startVnc(int port, String password, boolean lan) throws IOException {
        synchronized (lock) {
            if (vnc != 0) return;
            vnc = NativeBridge.nativeVncStart(handle(), lan ? "0.0.0.0" : "127.0.0.1", port, password);
        }
    }

    public void stopVnc() {
        synchronized (lock) {
            if (vnc != 0) NativeBridge.nativeVncStop(vnc);
            vnc = 0;
        }
    }

    public boolean isVncRunning() { return vnc != 0; }

    /** Clientes VNC conectados agora. */
    public int getVncClients() {
        long v = vnc;
        return v != 0 ? NativeBridge.nativeVncClients(v) : 0;
    }

    public VmConfig getConfig() { return config; }
    public State getState() { return state; }
    /** Motivo do ultimo termino de execucao (ou null). */
    public ExitReason getLastExitReason() { return lastExit; }

    public void addSerialListener(SerialListener l) { serialListeners.add(l); }
    public void removeSerialListener(SerialListener l) { serialListeners.remove(l); }
    public void addStateListener(StateListener l) { stateListeners.add(l); }
    public void removeStateListener(StateListener l) { stateListeners.remove(l); }

    private void setState(State s, ExitReason r) {
        state = s;
        for (StateListener l : stateListeners) l.onStateChanged(this, s, r);
    }

    private long handle() {
        long h = handle;
        if (h == 0) throw new IllegalStateException("VM fechada");
        return h;
    }

    /** Inicia (ou retoma apos termino) a execucao em uma thread propria. */
    public void start() {
        synchronized (lock) {
            final long h = handle();
            if (thread != null && thread.isAlive()) throw new IllegalStateException("VM ja em execucao");
            thread = new Thread(() -> {
                setState(State.RUNNING, null);
                ExitReason r = ExitReason.fromNative(NativeBridge.nativeRun(h));
                lastExit = r;
                setState(State.STOPPED, r);
            }, "MultiVM-" + config.arch.id());
            thread.start();
        }
    }

    /** Pede a parada da execucao (assincrono). */
    public void stop() {
        if (handle != 0) NativeBridge.nativeStop(handle);
    }

    /** Aguarda o termino da thread da VM. */
    public boolean awaitStop(long timeoutMs) throws InterruptedException {
        Thread t;
        synchronized (lock) { t = thread; }
        if (t == null) return true;
        t.join(timeoutMs);
        return !t.isAlive();
    }

    /** Reinicia a maquina (equivale ao botao de reset). */
    public void reset() { NativeBridge.nativeReset(handle()); }

    public void pause() {
        NativeBridge.nativePause(handle());
        if (state == State.RUNNING) setState(State.PAUSED, null);
    }

    public void resume() {
        NativeBridge.nativeResume(handle());
        if (state == State.PAUSED) setState(State.RUNNING, null);
    }

    public boolean isPaused() { return NativeBridge.nativeIsPaused(handle()); }

    /** Envia bytes para a serial do convidado. Retorna quantos foram aceitos. */
    public int sendSerial(byte[] data, int off, int len) {
        return NativeBridge.nativeSerialInput(handle(), data, off, len);
    }

    public int sendSerial(String text) {
        byte[] b = text.getBytes(StandardCharsets.UTF_8);
        return sendSerial(b, 0, b.length);
    }

    /** Tecla usando codigos android.view.KeyEvent.KEYCODE_*. Retorna false se nao mapeada. */
    public boolean sendKey(int androidKeyCode, boolean down) {
        int code = KeyMapper.toEvdev(androidKeyCode);
        if (code <= 0) return false;
        NativeBridge.nativeKeyEvent(handle(), code, down);
        return true;
    }

    /** Tecla usando codigos evdev do Linux (KEY_A = 30, ...). */
    public void sendKeyEvdev(int evdevCode, boolean down) {
        NativeBridge.nativeKeyEvent(handle(), evdevCode, down);
    }

    /** Movimento relativo do ponteiro (mouse PS/2 no PC). */
    public void sendPointer(int dx, int dy, int wheel, int buttons) {
        NativeBridge.nativePointerEvent(handle(), dx, dy, wheel, buttons);
    }

    public long getInstructionCount() { return NativeBridge.nativeInstructionCount(handle()); }

    /**
     * Quanto o relogio do convidado ficou para tras do tempo real, em nanossegundos.
     * Quando o aparelho nao da conta (ou o app fica em segundo plano), o tempo do
     * convidado anda mais devagar em vez de perder interrupcoes de timer.
     */
    public long getClockLagNanos() { return NativeBridge.nativeClockLag(handle()); }

    /**
     * Aperta o botao de energia ACPI (x86): o sistema convidado normalmente inicia um
     * desligamento ordenado, e o estado passa a STOPPED quando ele terminar.
     */
    public void pressPowerButton() { NativeBridge.nativePowerButton(handle()); }

    /** Informacoes do framebuffer, ou null se a VM nao tiver um. */
    public FramebufferInfo getFramebufferInfo() {
        int[] v = NativeBridge.nativeFbInfo(handle());
        return v == null ? null : new FramebufferInfo(v[0], v[1], v[2]);
    }

    /** Muda sempre que o convidado escreve no framebuffer (para decidir se redesenha). */
    public int getFramebufferGeneration() { return NativeBridge.nativeFbGeneration(handle()); }

    private static FramebufferInfo fbResult(long r) {
        if (r <= 0) return null;
        int w = (int) (r >>> 32), h = (int) r;
        return new FramebufferInfo(w, h, w * 4);
    }

    /**
     * Copia o framebuffer para int[] (0xAARRGGBB, linhas contiguas).
     * Com BIOS as dimensoes mudam com o modo de video: o retorno informa o
     * tamanho copiado, ou null se o array for pequeno demais (consulte
     * {@link #getFramebufferInfo()} e realoque) ou se nao houver framebuffer.
     */
    public FramebufferInfo copyFramebuffer(int[] argb) {
        return fbResult(NativeBridge.nativeFbCopyInts(handle(), argb));
    }

    /** Copia para um ByteBuffer direto em RGBA (layout do Bitmap ARGB_8888). Mesmo retorno de {@link #copyFramebuffer(int[])}. */
    public FramebufferInfo copyFramebuffer(ByteBuffer directRgba) {
        if (!directRgba.isDirect()) throw new IllegalArgumentException("ByteBuffer precisa ser direto");
        return fbResult(NativeBridge.nativeFbCopyRgba(handle(), directRgba));
    }

    private ByteBuffer bitmapBuf;

    /**
     * Copia o framebuffer para um Bitmap ARGB_8888 com as mesmas dimensoes.
     * Retorna false se as dimensoes nao batem (o modo de video mudou): crie um
     * novo Bitmap com {@link #getFramebufferInfo()} e tente de novo.
     */
    public boolean copyFramebuffer(Bitmap bitmap) {
        return updateBitmap(bitmap) >= 0;
    }

    private Bitmap lastBitmap;
    private final int[] bitmapState = new int[3];
    private boolean noDirectBitmap;

    /**
     * Atualiza um Bitmap ARGB_8888 com o framebuffer, copiando so as linhas que mudaram
     * desde a ultima chamada com o mesmo Bitmap. Retorna 1 se o Bitmap mudou, 0 se nada
     * mudou e -1 se as dimensoes nao batem (crie um novo com {@link #getFramebufferInfo()}).
     */
    public int updateBitmap(Bitmap bitmap) {
        if (bitmap != lastBitmap) {
            lastBitmap = bitmap;
            bitmapState[0] = 0;
        }
        if (!noDirectBitmap && bitmap.getConfig() == Bitmap.Config.ARGB_8888) {
            int r = NativeBridge.nativeFbCopyBitmap(handle(), bitmap, bitmapState);
            if (r != -2) return r;
            noDirectBitmap = true;
        }
        return copyBitmapBuffer(bitmap) ? 1 : -1;
    }

    private boolean copyBitmapBuffer(Bitmap bitmap) {
        FramebufferInfo fi = getFramebufferInfo();
        if (fi == null || bitmap.getWidth() != fi.width || bitmap.getHeight() != fi.height
                || bitmap.getConfig() != Bitmap.Config.ARGB_8888) return false;
        int size = fi.width * fi.height * 4;
        if (bitmapBuf == null || bitmapBuf.capacity() < size)
            bitmapBuf = ByteBuffer.allocateDirect(size).order(ByteOrder.nativeOrder());
        bitmapBuf.clear();
        FramebufferInfo got = fbResult(NativeBridge.nativeFbCopyRgba(handle(), bitmapBuf));
        if (got == null || got.width != fi.width || got.height != fi.height) return false;
        bitmapBuf.rewind();
        bitmapBuf.limit(size);
        bitmap.copyPixelsFromBuffer(bitmapBuf);
        return true;
    }

    /**
     * Conteudo da tela em modo texto VGA (80x25 etc.), com linhas separadas por
     * '\n'; null se a tela estiver em modo grafico ou a maquina nao tiver VGA.
     */
    public String getTextScreen() { return NativeBridge.nativeTextScreen(handle()); }

    /** Para a VM (se estiver rodando), espera a thread e libera os recursos nativos. */
    @Override
    public void close() {
        long h;
        Thread t;
        synchronized (lock) {
            h = handle;
            if (h == 0) return;
            t = thread;
        }
        stopVnc();
        NativeBridge.nativeStop(h);
        if (t != null) {
            boolean interrupted = false;
            while (t.isAlive()) {
                try {
                    t.join();
                } catch (InterruptedException e) {
                    interrupted = true;
                }
            }
            if (interrupted) Thread.currentThread().interrupt();
        }
        synchronized (lock) {
            NativeBridge.nativeDestroy(h);
            handle = 0;
        }
        setState(State.CLOSED, lastExit);
    }
}
