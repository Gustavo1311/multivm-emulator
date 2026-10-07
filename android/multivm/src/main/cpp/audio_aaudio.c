/*
 * Saida e entrada de audio da VM pelo AAudio (Android 8+), em modo de baixa
 * latencia. A libaaudio.so e carregada com dlopen (o build sem NDK nao tem os
 * headers): os prototipos abaixo seguem <aaudio/AAudio.h>.
 *
 * O callback de dados do AAudio puxa os quadros direto do anel do nucleo
 * (mvm_audio_read), sem passar pelo Java. Os streams so ficam ligados enquanto a
 * placa de som do convidado esta tocando/gravando (callbacks start/stop).
 */
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mvm.h"

typedef struct AAudioStreamStruct AAudioStream;
typedef struct AAudioStreamBuilderStruct AAudioStreamBuilder;
typedef int32_t aaudio_result_t;
typedef aaudio_result_t (*data_cb)(AAudioStream *, void *, void *, int32_t);
typedef void (*error_cb)(AAudioStream *, void *, aaudio_result_t);

#define AAUDIO_DIRECTION_OUTPUT 0
#define AAUDIO_DIRECTION_INPUT 1
#define AAUDIO_FORMAT_PCM_I16 1
#define AAUDIO_SHARING_MODE_EXCLUSIVE 0
#define AAUDIO_SHARING_MODE_SHARED 1
#define AAUDIO_PERFORMANCE_MODE_LOW_LATENCY 12
#define AAUDIO_CALLBACK_RESULT_CONTINUE 0
#define AAUDIO_USAGE_GAME 14
#define AAUDIO_INPUT_PRESET_VOICE_RECOGNITION 6

static struct {
    bool loaded, ok;
    aaudio_result_t (*createBuilder)(AAudioStreamBuilder **);
    void (*setDirection)(AAudioStreamBuilder *, int32_t);
    void (*setSampleRate)(AAudioStreamBuilder *, int32_t);
    void (*setChannelCount)(AAudioStreamBuilder *, int32_t);
    void (*setFormat)(AAudioStreamBuilder *, int32_t);
    void (*setSharingMode)(AAudioStreamBuilder *, int32_t);
    void (*setPerformanceMode)(AAudioStreamBuilder *, int32_t);
    void (*setUsage)(AAudioStreamBuilder *, int32_t);
    void (*setInputPreset)(AAudioStreamBuilder *, int32_t);
    void (*setDataCallback)(AAudioStreamBuilder *, data_cb, void *);
    void (*setErrorCallback)(AAudioStreamBuilder *, error_cb, void *);
    aaudio_result_t (*openStream)(AAudioStreamBuilder *, AAudioStream **);
    aaudio_result_t (*deleteBuilder)(AAudioStreamBuilder *);
    aaudio_result_t (*requestStart)(AAudioStream *);
    aaudio_result_t (*requestStop)(AAudioStream *);
    aaudio_result_t (*close)(AAudioStream *);
    int32_t (*getFramesPerBurst)(AAudioStream *);
    aaudio_result_t (*setBufferSize)(AAudioStream *, int32_t);
    int32_t (*getSampleRate)(AAudioStream *);
} aa;

static pthread_mutex_t aa_lock = PTHREAD_MUTEX_INITIALIZER;

static bool aa_load(void)
{
    pthread_mutex_lock(&aa_lock);
    if (!aa.loaded) {
        aa.loaded = true;
        void *h = dlopen("libaaudio.so", RTLD_NOW);
        if (h) {
#define SYM(f, n) (*(void **)&aa.f = dlsym(h, n))
            bool ok = SYM(createBuilder, "AAudio_createStreamBuilder") && SYM(setDirection, "AAudioStreamBuilder_setDirection") &&
                      SYM(setSampleRate, "AAudioStreamBuilder_setSampleRate") &&
                      SYM(setChannelCount, "AAudioStreamBuilder_setChannelCount") &&
                      SYM(setFormat, "AAudioStreamBuilder_setFormat") &&
                      SYM(setSharingMode, "AAudioStreamBuilder_setSharingMode") &&
                      SYM(setPerformanceMode, "AAudioStreamBuilder_setPerformanceMode") &&
                      SYM(setDataCallback, "AAudioStreamBuilder_setDataCallback") &&
                      SYM(setErrorCallback, "AAudioStreamBuilder_setErrorCallback") &&
                      SYM(openStream, "AAudioStreamBuilder_openStream") &&
                      SYM(deleteBuilder, "AAudioStreamBuilder_delete") && SYM(requestStart, "AAudioStream_requestStart") &&
                      SYM(requestStop, "AAudioStream_requestStop") && SYM(close, "AAudioStream_close") &&
                      SYM(getFramesPerBurst, "AAudioStream_getFramesPerBurst") &&
                      SYM(setBufferSize, "AAudioStream_setBufferSizeInFrames") &&
                      SYM(getSampleRate, "AAudioStream_getSampleRate");
            SYM(setUsage, "AAudioStreamBuilder_setUsage");             /* Android 9+ */
            SYM(setInputPreset, "AAudioStreamBuilder_setInputPreset"); /* Android 9+ */
#undef SYM
            aa.ok = ok;
        }
    }
    bool ok = aa.ok;
    pthread_mutex_unlock(&aa_lock);
    return ok;
}

typedef struct aaudio_out {
    mvm_vm *vm;
    pthread_mutex_t lock;
    AAudioStream *st[2]; /* 0 = saida, 1 = entrada */
    bool want[2];        /* o convidado quer o stream ligado */
    bool reopen[2];      /* erro (ex.: fone desconectado): reabrir */
    pthread_t worker;
    bool worker_on, quit;
    pthread_cond_t cond;
} aaudio_out;

static aaudio_result_t on_data(AAudioStream *s, void *user, void *data, int32_t frames)
{
    aaudio_out *a = user;
    if (s == a->st[0])
        mvm_audio_read(a->vm, data, (size_t)frames);
    else
        mvm_audio_write_input(a->vm, data, (size_t)frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void on_error(AAudioStream *s, void *user, aaudio_result_t err)
{
    (void)err;
    aaudio_out *a = user;
    /* nao pode reabrir dentro do callback: avisa a thread auxiliar */
    pthread_mutex_lock(&a->lock);
    for (int i = 0; i < 2; i++)
        if (a->st[i] == s)
            a->reopen[i] = true;
    pthread_cond_signal(&a->cond);
    pthread_mutex_unlock(&a->lock);
}

static AAudioStream *open_stream(aaudio_out *a, bool input, bool exclusive)
{
    AAudioStreamBuilder *b = NULL;
    if (aa.createBuilder(&b) != 0 || !b)
        return NULL;
    aa.setDirection(b, input ? AAUDIO_DIRECTION_INPUT : AAUDIO_DIRECTION_OUTPUT);
    aa.setSampleRate(b, MVM_AUDIO_RATE);
    aa.setChannelCount(b, 2);
    aa.setFormat(b, AAUDIO_FORMAT_PCM_I16);
    aa.setSharingMode(b, exclusive ? AAUDIO_SHARING_MODE_EXCLUSIVE : AAUDIO_SHARING_MODE_SHARED);
    aa.setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    if (!input && aa.setUsage)
        aa.setUsage(b, AAUDIO_USAGE_GAME);
    if (input && aa.setInputPreset)
        aa.setInputPreset(b, AAUDIO_INPUT_PRESET_VOICE_RECOGNITION); /* sem processamento extra */
    aa.setDataCallback(b, on_data, a);
    aa.setErrorCallback(b, on_error, a);
    AAudioStream *s = NULL;
    aaudio_result_t r = aa.openStream(b, &s);
    aa.deleteBuilder(b);
    if (r != 0 || !s)
        return NULL;
    if (aa.getSampleRate(s) != MVM_AUDIO_RATE) { /* exclusivo em outra taxa: o compartilhado reamostra */
        aa.close(s);
        return exclusive ? open_stream(a, input, false) : NULL;
    }
    int32_t burst = aa.getFramesPerBurst(s);
    if (burst > 0)
        aa.setBufferSize(s, burst * 2); /* dois bursts: latencia minima sem falhas */
    return s;
}

/* liga/desliga conforme a->want (com a->lock) */
static void apply(aaudio_out *a, int i)
{
    if (a->want[i] && !a->st[i]) {
        a->st[i] = open_stream(a, i == 1, true);
        if (a->st[i])
            aa.requestStart(a->st[i]);
    } else if (!a->want[i] && a->st[i]) {
        aa.requestStop(a->st[i]);
        aa.close(a->st[i]);
        a->st[i] = NULL;
    }
}

static void *worker(void *opaque)
{
    aaudio_out *a = opaque;
    pthread_mutex_lock(&a->lock);
    while (!a->quit) {
        for (int i = 0; i < 2; i++) {
            if (a->reopen[i]) {
                a->reopen[i] = false;
                if (a->st[i]) {
                    aa.close(a->st[i]);
                    a->st[i] = NULL;
                }
            }
            apply(a, i);
        }
        pthread_cond_wait(&a->cond, &a->lock);
    }
    for (int i = 0; i < 2; i++) {
        a->want[i] = false;
        apply(a, i);
    }
    pthread_mutex_unlock(&a->lock);
    return NULL;
}

/* chamados pela thread da VM: so sinalizam (abrir streams pode demorar) */
static void be_start(void *opaque, bool input)
{
    aaudio_out *a = opaque;
    pthread_mutex_lock(&a->lock);
    a->want[input ? 1 : 0] = true;
    pthread_cond_signal(&a->cond);
    pthread_mutex_unlock(&a->lock);
}

static void be_stop(void *opaque, bool input)
{
    aaudio_out *a = opaque;
    pthread_mutex_lock(&a->lock);
    a->want[input ? 1 : 0] = false;
    pthread_cond_signal(&a->cond);
    pthread_mutex_unlock(&a->lock);
}

static const mvm_audio_backend backend = {be_start, be_stop, NULL};

aaudio_out *aaudio_attach(mvm_vm *vm)
{
    if (!mvm_audio_enabled(vm) || !aa_load())
        return NULL;
    aaudio_out *a = calloc(1, sizeof(*a));
    a->vm = vm;
    pthread_mutex_init(&a->lock, NULL);
    pthread_cond_init(&a->cond, NULL);
    if (pthread_create(&a->worker, NULL, worker, a) != 0) {
        free(a);
        return NULL;
    }
    a->worker_on = true;
    mvm_audio_set_backend(vm, &backend, a);
    return a;
}

void aaudio_detach(aaudio_out *a)
{
    if (!a)
        return;
    mvm_audio_set_backend(a->vm, NULL, NULL);
    pthread_mutex_lock(&a->lock);
    a->quit = true;
    pthread_cond_signal(&a->cond);
    pthread_mutex_unlock(&a->lock);
    if (a->worker_on)
        pthread_join(a->worker, NULL);
    pthread_mutex_destroy(&a->lock);
    pthread_cond_destroy(&a->cond);
    free(a);
}

bool aaudio_available(void) { return aa_load(); }
