/*
 * Ponte JNI entre a API Java (com.multivm.core) e o nucleo em C (mvm.h).
 *
 * Modelo de threads: VirtualMachine.start() cria uma thread Java que chama
 * nativeRun(); todo o codigo da VM (e os callbacks de serial) roda nela.
 * As demais funcoes podem ser chamadas de qualquer thread.
 */
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mvm.h"

/* audio_aaudio.c */
typedef struct aaudio_out aaudio_out;
aaudio_out *aaudio_attach(mvm_vm *vm);
void aaudio_detach(aaudio_out *a);

#define JNI_FN(name) Java_com_multivm_core_NativeBridge_##name

typedef struct {
    mvm_vm *vm;
    JavaVM *jvm;
    jobject listener;        /* ref global: com.multivm.core.NativeBridge$Callbacks */
    jmethodID on_serial;     /* void onSerial(byte[]) */
    JNIEnv *run_env;         /* valido apenas durante nativeRun */
    aaudio_out *audio;       /* saida/microfone pelo AAudio (NULL = sem som) */
} vm_ctx;

static JavaVM *g_jvm;
static jobject g_log_listener;
static jmethodID g_on_log;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

static vm_ctx *ctx_of(jlong h) { return (vm_ctx *)(intptr_t)h; }

static void throw_ex(JNIEnv *env, const char *cls, const char *msg)
{
    jclass c = (*env)->FindClass(env, cls);
    if (c)
        (*env)->ThrowNew(env, c, msg);
}

/* ------------------------------------------------------------ log */

static void log_to_java(void *opaque, mvm_log_level level, const char *msg)
{
    (void)opaque;
    JNIEnv *env = NULL;
    bool attached = false;
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, (void *)&env, NULL) != JNI_OK)
            return;
        attached = true;
    }
    pthread_mutex_lock(&g_log_lock);
    jobject l = g_log_listener ? (*env)->NewLocalRef(env, g_log_listener) : NULL;
    pthread_mutex_unlock(&g_log_lock);
    if (l) {
        jstring s = (*env)->NewStringUTF(env, msg);
        (*env)->CallVoidMethod(env, l, g_on_log, (jint)level, s);
        if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, s);
        (*env)->DeleteLocalRef(env, l);
    }
    if (attached)
        (*g_jvm)->DetachCurrentThread(g_jvm);
}

JNIEXPORT void JNICALL JNI_FN(nativeSetLogListener)(JNIEnv *env, jclass cls, jobject listener)
{
    (void)cls;
    pthread_mutex_lock(&g_log_lock);
    if (g_log_listener) {
        (*env)->DeleteGlobalRef(env, g_log_listener);
        g_log_listener = NULL;
    }
    if (listener) {
        g_log_listener = (*env)->NewGlobalRef(env, listener);
        jclass lc = (*env)->GetObjectClass(env, listener);
        g_on_log = (*env)->GetMethodID(env, lc, "onLog", "(ILjava/lang/String;)V");
    }
    pthread_mutex_unlock(&g_log_lock);
    mvm_set_log_callback(listener ? log_to_java : NULL, NULL);
}

JNIEXPORT void JNICALL JNI_FN(nativeSetLogLevel)(JNIEnv *env, jclass cls, jint level)
{
    (void)env; (void)cls;
    mvm_set_log_level((mvm_log_level)level);
}

JNIEXPORT jstring JNICALL JNI_FN(nativeVersion)(JNIEnv *env, jclass cls)
{
    (void)cls;
    return (*env)->NewStringUTF(env, MVM_VERSION);
}

/* ------------------------------------------------------------ criacao */

static char *dup_jstring(JNIEnv *env, jstring s)
{
    if (!s)
        return NULL;
    const char *c = (*env)->GetStringUTFChars(env, s, NULL);
    char *r = c ? strdup(c) : NULL;
    if (c)
        (*env)->ReleaseStringUTFChars(env, s, c);
    return r;
}

JNIEXPORT jlong JNICALL JNI_FN(nativeCreate)(JNIEnv *env, jclass cls, jint arch, jint ram_mb, jstring kernel,
                                            jstring initrd, jstring cmdline, jstring dtb, jstring firmware,
                                            jstring vga_bios, jstring boot_order,
                                            jobjectArray disk_paths, jintArray disk_fds, jbooleanArray disk_ro,
                                            jintArray disk_types, jobjectArray disk_names, jint fb_w, jint fb_h,
                                            jlong raw_load_addr,
                                            jint net_model, jstring mac, jstring dns, jintArray forwards,
                                            jint audio_model, jboolean mic, jobject callbacks)
{
    (void)cls;
    mvm_config cfg;
    mvm_config_init(&cfg, (mvm_arch)arch);
    cfg.ram_mb = (uint32_t)ram_mb;
    char *s_kernel = dup_jstring(env, kernel), *s_initrd = dup_jstring(env, initrd);
    char *s_cmdline = dup_jstring(env, cmdline), *s_dtb = dup_jstring(env, dtb);
    char *s_fw = dup_jstring(env, firmware);
    char *s_vga = dup_jstring(env, vga_bios), *s_boot = dup_jstring(env, boot_order);
    cfg.vga_bios = s_vga;
    cfg.boot_order = s_boot;
    cfg.kernel = s_kernel;
    cfg.initrd = s_initrd;
    cfg.cmdline = s_cmdline;
    cfg.dtb = s_dtb;
    cfg.firmware = s_fw;
    cfg.fb_width = (uint32_t)fb_w;
    cfg.fb_height = (uint32_t)fb_h;
    cfg.raw_load_addr = (uint64_t)raw_load_addr;

    /* rede: forwards = [flags, porta do host, porta do convidado]...; flags: 1 = UDP, 2 = rede local */
    cfg.net.model = (mvm_nic_model)net_model;
    char *s_mac = dup_jstring(env, mac), *s_dns = dup_jstring(env, dns);
    unsigned m[6];
    if (s_mac && sscanf(s_mac, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
        for (int i = 0; i < 6; i++)
            cfg.net.mac[i] = (uint8_t)m[i];
        cfg.net.has_mac = true;
    }
    cfg.net.dns = s_dns;
    jsize nfw = forwards ? (*env)->GetArrayLength(env, forwards) / 3 : 0;
    if (nfw > MVM_MAX_FORWARDS)
        nfw = MVM_MAX_FORWARDS;
    jint *fw = forwards ? (*env)->GetIntArrayElements(env, forwards, NULL) : NULL;
    for (jsize i = 0; i < nfw && fw; i++) {
        cfg.net.forwards[i].udp = fw[3 * i] & 1;
        cfg.net.forwards[i].lan = (fw[3 * i] & 2) != 0;
        cfg.net.forwards[i].host_port = (uint16_t)fw[3 * i + 1];
        cfg.net.forwards[i].guest_port = (uint16_t)fw[3 * i + 2];
    }
    cfg.net.nforwards = (int)nfw;
    if (fw)
        (*env)->ReleaseIntArrayElements(env, forwards, fw, JNI_ABORT);
    cfg.audio.model = (mvm_snd_model)audio_model;
    cfg.audio.mic = mic;

    char *s_disks[MVM_MAX_DISKS] = {0}, *s_names[MVM_MAX_DISKS] = {0};
    jsize nd = disk_paths ? (*env)->GetArrayLength(env, disk_paths) : 0;
    if (nd > MVM_MAX_DISKS)
        nd = MVM_MAX_DISKS;
    jint *fds = disk_fds ? (*env)->GetIntArrayElements(env, disk_fds, NULL) : NULL;
    jboolean *ro = disk_ro ? (*env)->GetBooleanArrayElements(env, disk_ro, NULL) : NULL;
    jint *types = disk_types ? (*env)->GetIntArrayElements(env, disk_types, NULL) : NULL;
    for (jsize i = 0; i < nd; i++) {
        jstring p = (jstring)(*env)->GetObjectArrayElement(env, disk_paths, i);
        s_disks[i] = dup_jstring(env, p);
        if (p)
            (*env)->DeleteLocalRef(env, p);
        cfg.disks[i].path = s_disks[i];
        cfg.disks[i].readonly = ro ? ro[i] : false;
        cfg.disks[i].type = types ? (mvm_disk_type)types[i] : MVM_DISK_AUTO;
        if (fds && fds[i] >= 0) {
            cfg.disks[i].fd = fds[i];
            cfg.disks[i].has_fd = true;
        }
        /* CD/disquete sem arquivo: drive vazio (aceita midia com a VM ligada) */
        if (!s_disks[i] && !cfg.disks[i].has_fd)
            cfg.disks[i].empty = true;
        jstring nm = disk_names ? (jstring)(*env)->GetObjectArrayElement(env, disk_names, i) : NULL;
        s_names[i] = dup_jstring(env, nm);
        if (nm)
            (*env)->DeleteLocalRef(env, nm);
        cfg.disks[i].name = s_names[i];
    }
    if (fds)
        (*env)->ReleaseIntArrayElements(env, disk_fds, fds, JNI_ABORT);
    if (ro)
        (*env)->ReleaseBooleanArrayElements(env, disk_ro, ro, JNI_ABORT);
    if (types)
        (*env)->ReleaseIntArrayElements(env, disk_types, types, JNI_ABORT);

    char err[512] = {0};
    mvm_vm *vm = mvm_create(&cfg, err, sizeof(err));
    free(s_kernel);
    free(s_initrd);
    free(s_cmdline);
    free(s_dtb);
    free(s_fw);
    free(s_vga);
    free(s_boot);
    for (int i = 0; i < MVM_MAX_DISKS; i++) {
        free(s_disks[i]);
        free(s_names[i]);
    }
    free(s_mac);
    free(s_dns);
    if (!vm) {
        throw_ex(env, "java/lang/IllegalArgumentException", err[0] ? err : "falha ao criar a VM");
        return 0;
    }
    vm_ctx *c = calloc(1, sizeof(*c));
    c->vm = vm;
    c->jvm = g_jvm;
    c->audio = aaudio_attach(vm);
    if (callbacks) {
        c->listener = (*env)->NewGlobalRef(env, callbacks);
        jclass lc = (*env)->GetObjectClass(env, callbacks);
        c->on_serial = (*env)->GetMethodID(env, lc, "onSerial", "([B)V");
    }
    return (jlong)(intptr_t)c;
}

JNIEXPORT void JNICALL JNI_FN(nativeDestroy)(JNIEnv *env, jclass cls, jlong h)
{
    (void)cls;
    vm_ctx *c = ctx_of(h);
    if (!c)
        return;
    aaudio_detach(c->audio);
    mvm_destroy(c->vm);
    if (c->listener)
        (*env)->DeleteGlobalRef(env, c->listener);
    free(c);
}

/* ------------------------------------------------------------ execucao */

static void serial_to_java(void *opaque, const uint8_t *data, size_t len)
{
    vm_ctx *c = opaque;
    JNIEnv *env = c->run_env;
    if (!env || !c->listener || !c->on_serial)
        return;
    jbyteArray arr = (*env)->NewByteArray(env, (jsize)len);
    if (!arr)
        return;
    (*env)->SetByteArrayRegion(env, arr, 0, (jsize)len, (const jbyte *)data);
    (*env)->CallVoidMethod(env, c->listener, c->on_serial, arr);
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, arr);
}

JNIEXPORT jint JNICALL JNI_FN(nativeRun)(JNIEnv *env, jclass cls, jlong h)
{
    (void)cls;
    vm_ctx *c = ctx_of(h);
    c->run_env = env;
    mvm_set_serial_output(c->vm, serial_to_java, c);
    int r = mvm_run(c->vm);
    mvm_set_serial_output(c->vm, NULL, NULL);
    c->run_env = NULL;
    return r;
}

JNIEXPORT void JNICALL JNI_FN(nativeStop)(JNIEnv *env, jclass cls, jlong h) { (void)env; (void)cls; mvm_request_stop(ctx_of(h)->vm); }
JNIEXPORT void JNICALL JNI_FN(nativeReset)(JNIEnv *env, jclass cls, jlong h) { (void)env; (void)cls; mvm_request_reset(ctx_of(h)->vm); }
JNIEXPORT void JNICALL JNI_FN(nativePause)(JNIEnv *env, jclass cls, jlong h) { (void)env; (void)cls; mvm_pause(ctx_of(h)->vm); }
JNIEXPORT void JNICALL JNI_FN(nativeResume)(JNIEnv *env, jclass cls, jlong h) { (void)env; (void)cls; mvm_resume(ctx_of(h)->vm); }
JNIEXPORT jboolean JNICALL JNI_FN(nativeIsPaused)(JNIEnv *env, jclass cls, jlong h)
{
    (void)env; (void)cls;
    return mvm_is_paused(ctx_of(h)->vm) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL JNI_FN(nativeSerialInput)(JNIEnv *env, jclass cls, jlong h, jbyteArray data, jint off, jint len)
{
    (void)cls;
    jbyte *b = (*env)->GetByteArrayElements(env, data, NULL);
    if (!b)
        return 0;
    size_t n = mvm_serial_input(ctx_of(h)->vm, (const uint8_t *)b + off, (size_t)len);
    (*env)->ReleaseByteArrayElements(env, data, b, JNI_ABORT);
    return (jint)n;
}

JNIEXPORT void JNICALL JNI_FN(nativeKeyEvent)(JNIEnv *env, jclass cls, jlong h, jint code, jboolean down)
{
    (void)env; (void)cls;
    mvm_key_event(ctx_of(h)->vm, (uint32_t)code, down);
}

JNIEXPORT void JNICALL JNI_FN(nativePointerEvent)(JNIEnv *env, jclass cls, jlong h, jint dx, jint dy, jint dz, jint buttons)
{
    (void)env; (void)cls;
    mvm_pointer_event(ctx_of(h)->vm, dx, dy, dz, (uint32_t)buttons);
}

JNIEXPORT jlong JNICALL JNI_FN(nativeInstructionCount)(JNIEnv *env, jclass cls, jlong h)
{
    (void)env; (void)cls;
    return (jlong)mvm_instruction_count(ctx_of(h)->vm);
}

JNIEXPORT void JNICALL JNI_FN(nativePowerButton)(JNIEnv *env, jclass cls, jlong h)
{
    (void)env; (void)cls;
    mvm_power_button(ctx_of(h)->vm);
}

JNIEXPORT jlong JNICALL JNI_FN(nativeClockLag)(JNIEnv *env, jclass cls, jlong h)
{
    (void)env; (void)cls;
    return (jlong)mvm_clock_lag_ns(ctx_of(h)->vm);
}

/* ------------------------------------------------------------ midias com a VM ligada */

/* cada drive como "tipo|barramento|unidade|presente|somente-leitura|trocavel|nome" */
JNIEXPORT jobjectArray JNICALL JNI_FN(nativeMediaList)(JNIEnv *env, jclass cls, jlong h)
{
    (void)cls;
    mvm_media_info mi[MVM_MAX_MEDIA];
    int n = mvm_media_list(ctx_of(h)->vm, mi, MVM_MAX_MEDIA);
    jobjectArray arr = (*env)->NewObjectArray(env, n, (*env)->FindClass(env, "java/lang/String"), NULL);
    for (int i = 0; i < n; i++) {
        char buf[160];
        /* o nome pode ter caracteres fora do UTF-8 modificado: troca por '?' */
        char name[64];
        snprintf(name, sizeof(name), "%s", mi[i].name);
        for (char *p = name; *p; p++)
            if ((unsigned char)*p < 0x20 || (unsigned char)*p >= 0x80 || *p == '|')
                *p = '?';
        snprintf(buf, sizeof(buf), "%d|%d|%d|%d|%d|%d|%s", mi[i].kind, mi[i].bus, mi[i].unit, mi[i].present,
                 mi[i].readonly, mi[i].changeable, name);
        jstring s = (*env)->NewStringUTF(env, buf);
        (*env)->SetObjectArrayElement(env, arr, i, s);
        (*env)->DeleteLocalRef(env, s);
    }
    return arr;
}

JNIEXPORT void JNICALL JNI_FN(nativeMediaInsert)(JNIEnv *env, jclass cls, jlong h, jint slot, jstring path, jint fd,
                                                 jboolean ro, jstring name)
{
    (void)cls;
    char *p = dup_jstring(env, path), *nm = dup_jstring(env, name);
    char err[256] = {0};
    int r = mvm_media_insert(ctx_of(h)->vm, slot, p, fd, ro, nm, err, sizeof(err));
    free(p);
    free(nm);
    if (r < 0)
        throw_ex(env, "java/io/IOException", err[0] ? err : "falha ao inserir a midia");
}

JNIEXPORT void JNICALL JNI_FN(nativeMediaEject)(JNIEnv *env, jclass cls, jlong h, jint slot)
{
    (void)cls;
    char err[256] = {0};
    if (mvm_media_eject(ctx_of(h)->vm, slot, err, sizeof(err)) < 0)
        throw_ex(env, "java/io/IOException", err[0] ? err : "falha ao ejetar");
}

JNIEXPORT jint JNICALL JNI_FN(nativeMediaAddDisk)(JNIEnv *env, jclass cls, jlong h, jstring path, jint fd, jboolean ro,
                                                  jstring name)
{
    (void)cls;
    char *p = dup_jstring(env, path), *nm = dup_jstring(env, name);
    char err[256] = {0};
    int r = mvm_media_add_disk(ctx_of(h)->vm, p, fd, ro, nm, err, sizeof(err));
    free(p);
    free(nm);
    if (r < 0)
        throw_ex(env, "java/io/IOException", err[0] ? err : "falha ao conectar o disco");
    return r;
}

/* ------------------------------------------------------------ som e VNC */

JNIEXPORT void JNICALL JNI_FN(nativeAudioMute)(JNIEnv *env, jclass cls, jlong h, jboolean muted)
{
    (void)env; (void)cls;
    mvm_audio_set_muted(ctx_of(h)->vm, muted);
}

/* 0 = sem placa de som, 1 = com som, 2 = placa presente mas sem AAudio (Android < 8) */
JNIEXPORT jint JNICALL JNI_FN(nativeAudioState)(JNIEnv *env, jclass cls, jlong h)
{
    (void)env; (void)cls;
    vm_ctx *c = ctx_of(h);
    if (!mvm_audio_enabled(c->vm))
        return 0;
    return c->audio ? 1 : 2;
}

JNIEXPORT jlong JNICALL JNI_FN(nativeVncStart)(JNIEnv *env, jclass cls, jlong h, jstring bind, jint port, jstring password)
{
    (void)cls;
    char *b = dup_jstring(env, bind), *pw = dup_jstring(env, password);
    char err[256] = {0};
    mvm_vnc *v = mvm_vnc_start(ctx_of(h)->vm, b, port, pw && pw[0] ? pw : NULL, err, sizeof(err));
    free(b);
    free(pw);
    if (!v) {
        throw_ex(env, "java/io/IOException", err[0] ? err : "falha ao iniciar o VNC");
        return 0;
    }
    return (jlong)(intptr_t)v;
}

JNIEXPORT void JNICALL JNI_FN(nativeVncStop)(JNIEnv *env, jclass cls, jlong v)
{
    (void)env; (void)cls;
    mvm_vnc_stop((mvm_vnc *)(intptr_t)v);
}

JNIEXPORT jint JNICALL JNI_FN(nativeVncClients)(JNIEnv *env, jclass cls, jlong v)
{
    (void)env; (void)cls;
    return mvm_vnc_clients((mvm_vnc *)(intptr_t)v);
}

/* ------------------------------------------------------------ framebuffer */

JNIEXPORT jintArray JNICALL JNI_FN(nativeFbInfo)(JNIEnv *env, jclass cls, jlong h)
{
    (void)cls;
    mvm_fb_info fi;
    if (!mvm_fb_info_get(ctx_of(h)->vm, &fi))
        return NULL;
    jint v[4] = {(jint)fi.width, (jint)fi.height, (jint)fi.stride, (jint)fi.format};
    jintArray a = (*env)->NewIntArray(env, 4);
    (*env)->SetIntArrayRegion(env, a, 0, 4, v);
    return a;
}

JNIEXPORT jint JNICALL JNI_FN(nativeFbGeneration)(JNIEnv *env, jclass cls, jlong h)
{
    (void)env; (void)cls;
    return (jint)mvm_fb_generation(ctx_of(h)->vm);
}

/* Resultado das copias: (largura << 32) | altura em caso de sucesso, o
 * negativo disso se o destino for pequeno demais e 0 se nao houver framebuffer.
 * Com BIOS as dimensoes mudam conforme o modo de video do convidado. */
static jlong fb_result(bool ok, const mvm_fb_info *fi)
{
    jlong v = ((jlong)fi->width << 32) | (jlong)fi->height;
    return ok ? v : -v;
}

/* Copia para int[] no formato de android.graphics.Color (0xAARRGGBB). */
JNIEXPORT jlong JNICALL JNI_FN(nativeFbCopyInts)(JNIEnv *env, jclass cls, jlong h, jintArray dst)
{
    (void)cls;
    mvm_vm *vm = ctx_of(h)->vm;
    mvm_fb_info fi = {0};
    if (!mvm_fb_info_get(vm, &fi))
        return 0;
    size_t cap = (size_t)(*env)->GetArrayLength(env, dst) * 4;
    jint *p = (*env)->GetPrimitiveArrayCritical(env, dst, NULL);
    if (!p)
        return 0;
    bool ok = mvm_fb_copy(vm, p, cap, &fi);
    if (ok)
        for (uint32_t i = 0; i < fi.width * fi.height; i++)
            p[i] |= (jint)0xff000000;
    (*env)->ReleasePrimitiveArrayCritical(env, dst, p, 0);
    return fb_result(ok, &fi);
}

/* Copia para ByteBuffer direto em RGBA (layout de Bitmap.Config.ARGB_8888). */
JNIEXPORT jlong JNICALL JNI_FN(nativeFbCopyRgba)(JNIEnv *env, jclass cls, jlong h, jobject buf)
{
    (void)cls;
    mvm_vm *vm = ctx_of(h)->vm;
    mvm_fb_info fi = {0};
    if (!mvm_fb_info_get(vm, &fi))
        return 0;
    uint8_t *p = (*env)->GetDirectBufferAddress(env, buf);
    jlong cap = (*env)->GetDirectBufferCapacity(env, buf);
    if (!p || cap <= 0)
        return 0;
    bool ok = mvm_fb_copy(vm, p, (size_t)cap, &fi);
    if (ok)
        for (uint32_t i = 0; i < fi.width * fi.height; i++) {
            uint8_t b = p[4 * i], r = p[4 * i + 2];
            p[4 * i] = r;
            p[4 * i + 2] = b;
            p[4 * i + 3] = 0xff;
        }
    return fb_result(ok, &fi);
}

/* Tela em modo texto VGA (BIOS, boot loaders, console do DOS/Linux), ou null. */
JNIEXPORT jstring JNICALL JNI_FN(nativeTextScreen)(JNIEnv *env, jclass cls, jlong h)
{
    (void)cls;
    static const size_t cap = 16384;
    char *buf = malloc(cap);
    if (!buf)
        return NULL;
    size_t n = mvm_text_screen(ctx_of(h)->vm, buf, cap - 1);
    jstring r = NULL;
    if (n) {
        buf[n] = 0;
        for (size_t i = 0; i < n; i++) /* NewStringUTF exige UTF-8 modificado valido */
            if ((unsigned char)buf[i] >= 0x80 || (buf[i] < 0x20 && buf[i] != '\n'))
                buf[i] = ' ';
        r = (*env)->NewStringUTF(env, buf);
    }
    free(buf);
    return r;
}

JNIEXPORT jstring JNICALL JNI_FN(nativeArchName)(JNIEnv *env, jclass cls, jint arch)
{
    (void)cls;
    return (*env)->NewStringUTF(env, mvm_arch_name((mvm_arch)arch));
}

/* ------------------------------------------------------------ imagens de disco */

static const char *IOEX = "java/io/IOException";

/* com.multivm.core.DiskImages$Info(String, long, long, boolean) */
JNIEXPORT jobject JNICALL JNI_FN(nativeImageInfo)(JNIEnv *env, jclass cls, jstring path, jint fd)
{
    (void)cls;
    const char *p = path ? (*env)->GetStringUTFChars(env, path, NULL) : NULL;
    mvm_image_info info;
    char err[512];
    int r = mvm_image_info_get(p, fd, &info, err, sizeof(err));
    if (p)
        (*env)->ReleaseStringUTFChars(env, path, p);
    if (r < 0) {
        throw_ex(env, IOEX, err);
        return NULL;
    }
    jclass ic = (*env)->FindClass(env, "com/multivm/core/DiskImages$Info");
    if (!ic)
        return NULL;
    jmethodID ctor = (*env)->GetMethodID(env, ic, "<init>", "(Ljava/lang/String;JJZ)V");
    if (!ctor)
        return NULL;
    jstring fmt = (*env)->NewStringUTF(env, info.format);
    return (*env)->NewObject(env, ic, ctor, fmt, (jlong)info.virtual_size, (jlong)info.file_size,
                             (jboolean)info.writable);
}

JNIEXPORT void JNICALL JNI_FN(nativeImageCreate)(JNIEnv *env, jclass cls, jstring path, jlong size, jint block_size)
{
    (void)cls;
    const char *p = (*env)->GetStringUTFChars(env, path, NULL);
    char err[512];
    int r = mvm_image_create(p, (uint64_t)size, (uint32_t)block_size, NULL, err, sizeof(err));
    (*env)->ReleaseStringUTFChars(env, path, p);
    if (r < 0)
        throw_ex(env, IOEX, err);
}

typedef struct {
    JNIEnv *env;
    jobject listener;
    jmethodID on_progress;
} progress_ctx;

static bool progress_to_java(void *opaque, uint64_t done, uint64_t total)
{
    progress_ctx *pc = opaque;
    if (!pc->listener)
        return true;
    jboolean go = (*pc->env)->CallBooleanMethod(pc->env, pc->listener, pc->on_progress, (jlong)done, (jlong)total);
    if ((*pc->env)->ExceptionCheck(pc->env))
        return false; /* a excecao segue para o Java depois do retorno */
    return go;
}

JNIEXPORT void JNICALL JNI_FN(nativeImageConvert)(JNIEnv *env, jclass cls, jstring src, jint src_fd, jstring dst,
                                                  jint fmt, jint flags, jint block_size, jobject listener)
{
    (void)cls;
    progress_ctx pc = {env, listener, NULL};
    if (listener) {
        jclass lc = (*env)->GetObjectClass(env, listener);
        pc.on_progress = (*env)->GetMethodID(env, lc, "onProgress", "(JJ)Z");
        if (!pc.on_progress)
            return;
    }
    const char *s = src ? (*env)->GetStringUTFChars(env, src, NULL) : NULL;
    const char *d = (*env)->GetStringUTFChars(env, dst, NULL);
    char err[512];
    int r = mvm_image_convert(s, src_fd, d, fmt, (unsigned)flags, (uint32_t)block_size, progress_to_java, &pc, err,
                              sizeof(err));
    if (s)
        (*env)->ReleaseStringUTFChars(env, src, s);
    (*env)->ReleaseStringUTFChars(env, dst, d);
    if (r < 0 && !(*env)->ExceptionCheck(env))
        throw_ex(env, IOEX, err);
}
