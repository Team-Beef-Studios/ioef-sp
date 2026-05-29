# ============================================================================
# libefxr.so -- the ioEF engine (qcommon + client + server + botlib + GL1
# renderer + OpenXR/Android VR layer), built as one shared library.
#
# Mirrors the desktop Makefile Q3OBJ set, but:
#   * renderergl1 + renderercommon are compiled IN (no USE_RENDERER_DLOPEN)
#   * SDL windowing/input/sound are dropped; EGL/GLES come from the VR layer
#     and audio from a minimal OpenSLES backend
#   * the GL1 fixed-function calls run on GLES3 via gl4es (include path first)
#   * sys_unix.c / sys_main.c (POSIX) are reused; only main() is guarded out and
#     a small JNI + VR_main glue file (EFXR_AndroidGlue.c) drives the frame loop
# Variables (IOEF_ROOT, GL4ES_PATH, OPENXR_SDK, ...) come from Application.mk.
# ============================================================================

LOCAL_PATH := $(IOEF_ROOT)

include $(CLEAR_VARS)

LOCAL_MODULE := efxr

LOCAL_CFLAGS := \
    -DELITEFORCE \
    -DBUILD_VR \
    -DXR_USE_PLATFORM_ANDROID \
    -DXR_USE_GRAPHICS_API_OPENGL_ES \
    -DHAVE_GLES \
    -DEFXR_CLIENT \
    -DARCH_STRING=\"aarch64\" \
    -DBOTLIB \
    -DUSE_LOCAL_HEADERS=1 \
    -DNO_VM_COMPILED \
    -DUSE_CODEC_MP3=1 \
    -fvisibility=hidden -fno-strict-aliasing -Wno-write-strings \
    -fcommon
# -fcommon: the static-linked renderer and the engine each have a tentative
# definition of some globals (e.g. com_altivec in tr_init.c + common.c); -fcommon
# (pre-clang-15 default) merges them instead of erroring on duplicate symbols.
# NOTE: do NOT define the "off" features (USE_OPENAL/CURL/MUMBLE/VOIP/CODEC_*).
# Much of the engine guards them with #ifdef, so -DUSE_X=0 would ENABLE them
# (defined, value 0) and pull in absent headers (e.g. client.h's <opus.h> under
# #ifdef USE_VOIP).  Leaving them undefined disables both #ifdef and #if checks.

# gl4es include FIRST so <GL/gl.h> / <GL/glext.h> resolve to the translation
# layer rather than any system GL.
LOCAL_C_INCLUDES := \
    $(GL4ES_PATH)/include \
    $(LOCAL_PATH) \
    $(LOCAL_PATH)/qcommon \
    $(LOCAL_PATH)/client \
    $(LOCAL_PATH)/server \
    $(LOCAL_PATH)/renderercommon \
    $(LOCAL_PATH)/renderergl1 \
    $(LOCAL_PATH)/jpeg-8c \
    $(LOCAL_PATH)/vr \
    $(LOCAL_PATH)/vr/android \
    $(LOCAL_PATH)/vr/openxr \
    $(SUPPORT_LIBS)/libmad

LOCAL_LDLIBS := -lGLESv3 -lEGL -landroid -llog -lOpenSLES -lz -ldl
# (NDK r26 uses lld; the old JKXR -fuse-ld=bfd workaround is invalid here.)

LOCAL_SHARED_LIBRARIES := openxr_loader gl4es
# libmad (MP3 decode) -- static; powers snd_codec_mp3.c (intro VO/music + the
# cinematic scroll timing, which is driven off the audio clock).
LOCAL_STATIC_LIBRARIES := mad

# ---- source selection: glob each dir, then drop platform/arch/codec files ---
ALL_QCOMMON  := $(wildcard $(LOCAL_PATH)/qcommon/*.c)
# Keep the portable interpreter VM; drop the compiled-VM and non-target backends.
QCOMMON_SKIP := vm_x86.c vm_powerpc.c vm_powerpc_asm.c vm_sparc.c vm_none.c
QCOMMON_SRC  := $(filter-out $(addprefix $(LOCAL_PATH)/qcommon/,$(QCOMMON_SKIP)),$(ALL_QCOMMON))

ALL_CLIENT   := $(wildcard $(LOCAL_PATH)/client/*.c)
# Drop OpenAL, cURL, mumble, and the opus/ogg codec backends; keep wav + mp3
# (mp3 is built against the vendored libmad static lib -- see LOCAL_STATIC_LIBRARIES).
CLIENT_SKIP  := snd_openal.c qal.c cl_curl.c libmumblelink.c \
                snd_codec_opus.c snd_codec_ogg.c
CLIENT_SRC   := $(filter-out $(addprefix $(LOCAL_PATH)/client/,$(CLIENT_SKIP)),$(ALL_CLIENT))

SERVER_SRC   := $(wildcard $(LOCAL_PATH)/server/*.c)
# sv_rankings.c (GameRanger online rankings) needs the proprietary grapi.h SDK
# and isn't in the desktop build either -- drop it.
SERVER_SRC   := $(filter-out $(LOCAL_PATH)/server/sv_rankings.c,$(SERVER_SRC))
BOTLIB_SRC   := $(wildcard $(LOCAL_PATH)/botlib/*.c)
RENDC_SRC    := $(wildcard $(LOCAL_PATH)/renderercommon/*.c)
JPEG_SRC     := $(wildcard $(LOCAL_PATH)/jpeg-8c/*.c)

# GL1 renderer minus the SDL glimp/gamma (replaced by the Android VR glimp).
ALL_RGL1     := $(wildcard $(LOCAL_PATH)/renderergl1/*.c)
# tr_subs.c provides Com_Printf/Com_Error ri-wrappers for the standalone renderer
# DLL; static-linked here the engine's own (common.c) are used instead.
RGL1_SKIP    := sdl_glimp.c sdl_gamma.c tr_subs.c
RGL1_SRC     := $(filter-out $(addprefix $(LOCAL_PATH)/renderergl1/,$(RGL1_SKIP)),$(ALL_RGL1))

# POSIX system layer: reuse sys_unix.c + a passive (non-tty) console + logfile.
SYS_SRC      := $(LOCAL_PATH)/sys/sys_main.c \
                $(LOCAL_PATH)/sys/sys_unix.c \
                $(LOCAL_PATH)/sys/con_passive.c \
                $(LOCAL_PATH)/sys/con_log.c

# Shared VR layer + Android OpenXR/EGL implementation (code/vr + code/vr/android).
VR_SRC       := $(LOCAL_PATH)/vr/VrInputCommon.c \
                $(LOCAL_PATH)/vr/VrCvars.c \
                $(LOCAL_PATH)/vr/android/TBXR_Common.c \
                $(LOCAL_PATH)/vr/android/OpenXrInput.c \
                $(LOCAL_PATH)/vr/android/EFXR_SurfaceView.c \
                $(LOCAL_PATH)/vr/android/EFXR_AndroidGlue.c \
                $(LOCAL_PATH)/vr/android/android_glimp.c \
                $(LOCAL_PATH)/vr/android/android_crash.c \
                $(LOCAL_PATH)/vr/android/snd_opensles.c

ENGINE_SRC := $(QCOMMON_SRC) $(CLIENT_SRC) $(SERVER_SRC) $(BOTLIB_SRC) \
              $(RENDC_SRC) $(RGL1_SRC) $(JPEG_SRC) $(SYS_SRC) $(VR_SRC)

# ndk-build wants LOCAL_SRC_FILES relative to LOCAL_PATH.
LOCAL_SRC_FILES := $(ENGINE_SRC:$(LOCAL_PATH)/%=%)

include $(BUILD_SHARED_LIBRARY)

$(call import-module,AndroidPrebuilt/jni)
