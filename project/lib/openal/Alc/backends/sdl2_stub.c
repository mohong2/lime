/*
 * Android stub for the OpenAL "sdl2" backend.
 *
 * On Android we define HAVE_SDL2 so openal reuses SDL's JNIEnv (avoids a
 * duplicate JNI_OnLoad with SDL, which would fail linking liblime.so).
 * ALc.c then unconditionally references ALCsdl2BackendFactory_getFactory in
 * its backend list, but the real SDL2 audio backend cannot be compiled
 * against SDL3 and is useless on Android anyway (OpenSL ES / AAudio are used).
 *
 * This stub provides the missing symbol; its init always fails so the
 * backend is skipped during probing (and the "null" backend remains the
 * guaranteed fallback).
 */

#include "config.h"

#include "alMain.h"
#include "backends/base.h"


typedef struct ALCsdl2BackendFactory {
    DERIVE_FROM_TYPE(ALCbackendFactory);
} ALCsdl2BackendFactory;

static ALCboolean ALCsdl2BackendFactory_init(ALCsdl2BackendFactory *self)
{
    (void)self;
    return ALC_FALSE;
}

static void ALCsdl2BackendFactory_deinit(ALCsdl2BackendFactory *self)
{
    (void)self;
}

static ALCboolean ALCsdl2BackendFactory_querySupport(ALCsdl2BackendFactory *self, ALCbackend_Type type)
{
    (void)self;
    (void)type;
    return ALC_FALSE;
}

static void ALCsdl2BackendFactory_probe(ALCsdl2BackendFactory *self, enum DevProbe type)
{
    (void)self;
    (void)type;
}

static ALCbackend* ALCsdl2BackendFactory_createBackend(ALCsdl2BackendFactory *self, ALCdevice *device, ALCbackend_Type type)
{
    (void)self;
    (void)device;
    (void)type;
    return NULL;
}

DEFINE_ALCBACKENDFACTORY_VTABLE(ALCsdl2BackendFactory);

ALCbackendFactory *ALCsdl2BackendFactory_getFactory(void)
{
    static ALCsdl2BackendFactory factory = { { GET_VTABLE2(ALCsdl2BackendFactory, ALCbackendFactory) } };
    return STATIC_CAST(ALCbackendFactory, &factory);
}
