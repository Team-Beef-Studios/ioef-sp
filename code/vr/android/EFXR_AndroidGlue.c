/*
===========================================================================
EFXR_AndroidGlue.c -- JNI lifecycle bridge + engine entry for the standalone
Android/Quest build.

The Java activity (GLES3JNIActivity) calls these JNI functions.  onCreate spawns
a dedicated render thread (AppThreadFunction) which runs the normal engine
frame loop (VR_main = Com_Init + Com_Frame loop) -- there is no process main()
on Android (sys_main.c's main() is #ifdef'd out).

VR is brought up the SAME way as the PCVR/Windows build: the engine's
CL_InitRenderer calls VR_PreRendererInit (which, on Android, also creates the
EGL/GLES context via TBXR_InitialiseOpenXR) and then VR_InitOnce (session +
swapchains).  So this glue does NOT itself call the TBXR_* init sequence -- it
only wires up the JVM/activity (needed by xrInitializeLoaderKHR) and the thread.

Modeled on JKXR's android/JKXR_SurfaceView.cpp (JNI half).
===========================================================================
*/

#include "../VrCommon.h"

#include <jni.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <android/log.h>

// Engine entry points (qcommon / client).
void Com_Init( char *commandLine );
void Com_Frame( void );
void NET_Init( void );
void CON_Init( void );
void Sys_SetDefaultInstallPath( const char *path );

// Set by TBXR_Common.c; flipped true on onDestroy so the frame loop exits.
extern bool destroyed;

// gl4es one-time init (sets up the process-global glstate + hardware caps).
// Called from onCreate on the JNI thread before the render thread starts, which
// is exactly where RTCWQuest (also GL1-over-gl4es) initialises it.
extern void initialize_gl4es( void );
extern void EFXR_InstallCrashHandler( void );   // android_crash.c

// The shared VR thread/JVM state (declared in android/TBXR_Common.h, defined here).
ovrAppThread gAppThread;

static char  s_commandLine[1024] = "";
static pthread_t s_thread;

// ---------------------------------------------------------------------------
// Input layer: SDL input is dropped on Android.  All in-game input is fed from
// OpenXR controllers (VR_GetController*) inside cl_input.c CL_FinishMove, so the
// engine's IN_* hooks are no-ops here.
// ---------------------------------------------------------------------------
void IN_Init( void *windowData ) { (void)windowData; }
void IN_Frame( void ) {}
void IN_Shutdown( void ) {}
void IN_Restart( void ) {}

// ---------------------------------------------------------------------------
// Engine main loop (replaces sys_main.c main() on Android).
// ---------------------------------------------------------------------------
int VR_main( void )
{
	Sys_SetDefaultInstallPath( "/sdcard/EFXR" );

	Com_Init( s_commandLine );
	NET_Init();
	CON_Init();

	while ( !destroyed )
	{
		IN_Frame();
		Com_Frame();
	}

	return 0;
}

// The engine logs via Com_Printf -> Sys_Print -> CON_Print -> fputs(stderr).
// Android drops stdout/stderr, so pipe them into logcat (tag EFXR-engine) so we
// can actually see the engine's diagnostics (and Com_Error messages).
static int s_logPipe[2];
static void * EFXR_LogcatThread( void *arg )
{
	char buf[1024];
	ssize_t n;
	(void)arg;
	while ( ( n = read( s_logPipe[0], buf, sizeof(buf) - 1 ) ) > 0 ) {
		if ( buf[n - 1] == '\n' ) n--;
		buf[n] = '\0';
		__android_log_write( ANDROID_LOG_INFO, "EFXR-engine", buf );
	}
	return NULL;
}
static void EFXR_RedirectStdioToLogcat( void )
{
	pthread_t t;
	setvbuf( stdout, NULL, _IOLBF, 0 );
	setvbuf( stderr, NULL, _IONBF, 0 );
	if ( pipe( s_logPipe ) != 0 ) return;
	dup2( s_logPipe[1], STDOUT_FILENO );
	dup2( s_logPipe[1], STDERR_FILENO );
	pthread_create( &t, NULL, EFXR_LogcatThread, NULL );
	pthread_detach( t );
}

void * AppThreadFunction( void * parm )
{
	JNIEnv *env = NULL;
	(void)parm;

	(*gAppThread.JavaVm)->AttachCurrentThread( gAppThread.JavaVm, &env, NULL );
	prctl( PR_SET_NAME, (long)"EFXR::main", 0, 0, 0 );

	EFXR_RedirectStdioToLogcat();
	EFXR_InstallCrashHandler();   // backtrace -> /sdcard/EFXR/efxr_crash.log + logcat

	VR_main();

	(*gAppThread.JavaVm)->DetachCurrentThread( gAppThread.JavaVm );
	return NULL;
}

// ---------------------------------------------------------------------------
// JNI
// ---------------------------------------------------------------------------
jint JNI_OnLoad( JavaVM* vm, void* reserved )
{
	(void)reserved;
	gAppThread.JavaVm = vm;
	return JNI_VERSION_1_6;
}

JNIEXPORT jlong JNICALL
Java_com_teambeefvr_efxr_GLES3JNILib_onCreate( JNIEnv * env, jclass clazz, jobject activity, jstring commandLineParams )
{
	(void)clazz;

	// Capture the command line for Com_Init.
	const char *cmd = (*env)->GetStringUTFChars( env, commandLineParams, NULL );
	if ( cmd )
	{
		Q_strncpyz( s_commandLine, cmd, sizeof( s_commandLine ) );
		(*env)->ReleaseStringUTFChars( env, commandLineParams, cmd );
	}

	// JVM + a global ref to the activity (xrInitializeLoaderKHR /
	// XrInstanceCreateInfoAndroidKHR need these).
	(*env)->GetJavaVM( env, &gAppThread.JavaVm );
	gAppThread.ActivityObject = (*env)->NewGlobalRef( env, activity );
	gAppThread.Resumed = false;
	gAppThread.NativeWindow = NULL;
	destroyed = qfalse;

	initialize_gl4es();   // once, before the render thread (RTCWQuest placement)

	pthread_attr_t attr;
	pthread_attr_init( &attr );
	pthread_create( &s_thread, &attr, AppThreadFunction, NULL );
	pthread_attr_destroy( &attr );

	return (jlong)1;
}

JNIEXPORT void JNICALL
Java_com_teambeefvr_efxr_GLES3JNILib_onStart( JNIEnv * env, jclass clazz, jlong handle, jobject obj )
{
	(void)env; (void)clazz; (void)handle; (void)obj;
}

JNIEXPORT void JNICALL
Java_com_teambeefvr_efxr_GLES3JNILib_onResume( JNIEnv * env, jclass clazz, jlong handle )
{
	(void)env; (void)clazz; (void)handle;
	gAppThread.Resumed = true;
}

JNIEXPORT void JNICALL
Java_com_teambeefvr_efxr_GLES3JNILib_onPause( JNIEnv * env, jclass clazz, jlong handle )
{
	(void)env; (void)clazz; (void)handle;
	gAppThread.Resumed = false;
}

JNIEXPORT void JNICALL
Java_com_teambeefvr_efxr_GLES3JNILib_onStop( JNIEnv * env, jclass clazz, jlong handle )
{
	(void)env; (void)clazz; (void)handle;
}

JNIEXPORT void JNICALL
Java_com_teambeefvr_efxr_GLES3JNILib_onDestroy( JNIEnv * env, jclass clazz, jlong handle )
{
	(void)env; (void)clazz; (void)handle;
	destroyed = qtrue;
}

JNIEXPORT void JNICALL
Java_com_teambeefvr_efxr_GLES3JNILib_onSurfaceCreated( JNIEnv * env, jclass clazz, jlong handle, jobject surface )
{
	(void)clazz; (void)handle;
	gAppThread.NativeWindow = ANativeWindow_fromSurface( env, surface );
}

JNIEXPORT void JNICALL
Java_com_teambeefvr_efxr_GLES3JNILib_onSurfaceChanged( JNIEnv * env, jclass clazz, jlong handle, jobject surface )
{
	(void)clazz; (void)handle;
	if ( gAppThread.NativeWindow )
	{
		ANativeWindow_release( gAppThread.NativeWindow );
		gAppThread.NativeWindow = NULL;
	}
	gAppThread.NativeWindow = ANativeWindow_fromSurface( env, surface );
}

JNIEXPORT void JNICALL
Java_com_teambeefvr_efxr_GLES3JNILib_onSurfaceDestroyed( JNIEnv * env, jclass clazz, jlong handle )
{
	(void)env; (void)clazz; (void)handle;
	if ( gAppThread.NativeWindow )
	{
		ANativeWindow_release( gAppThread.NativeWindow );
		gAppThread.NativeWindow = NULL;
	}
}
