/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

/*****************************************************************************
 * name:		cl_cin_bink.c
 *
 * desc:		Bink (.bik) playback backend for the cinematic system.
 *
 * Star Trek: Voyager -- Elite Force ships its FMV as Bink 1, not RoQ:
 * baseEF/pak0.pk3 holds video/eflogo.bik, video/intro.bik and video/st_*.bik,
 * all BIKi 512x384 @ 15fps with one 44.1kHz stereo RDFT audio track.
 *
 * Decoding is done by the vendored library in code/binkdec (LGPL-2.1+, a port
 * of FFmpeg's Bink decoders).  This file owns everything engine-facing: the
 * frame clock, the YUV->RGBA conversion, the raw-audio feed, and the fades the
 * SP game DLL asks for through cl_VidFadeUp / cl_VidFadeDown.
 *
 * cl_cin.c drives this through CIN_Bink_*.  The backend fills the same
 * cin_cache fields the RoQ decoder does (buf / CIN_WIDTH / CIN_HEIGHT /
 * drawX / drawY / dirty), so CIN_DrawCinematic and CIN_UploadCinematic need no
 * knowledge of the format.
 *****************************************************************************/

#include "client.h"
#include "snd_local.h"
#include "cl_cin_bink.h"

#if !defined( USE_BINK ) || !USE_BINK

// Built without the decoder (USE_BINK=0).  CIN_Bink_Open failing is the same
// path as an unreadable file, so cl_cin.c needs no build-time knowledge of this.

void CIN_Bink_Init( void ) {
}

qboolean CIN_Bink_IsBinkHeader( const byte *header, int len ) {
	return qfalse;
}

void *CIN_Bink_Open( const char *path, qboolean silent, int *width, int *height ) {
	Com_Printf( "CIN_Bink_Open: this build has no Bink support (USE_BINK=0)\n" );
	return NULL;
}

e_status CIN_Bink_Run( void *state, byte **buf, qboolean *dirty, qboolean looping ) {
	return FMV_EOF;
}

void CIN_Bink_DrawFade( void *state, float x, float y, float w, float h ) {
}

void CIN_Bink_Close( void *state ) {
}

#else

#include "bink_api.h"

// same locally-declared externs cl_cin.c uses
extern int	s_soundtime;			// sample PAIRS
extern int	CL_ScaledMilliseconds( void );

// A hitch must not be paid back frame-by-frame: the raw-audio ring holds
// MAX_RAW_SAMPLES pairs, only ~0.37s at 44.1kHz, so a long catch-up burst would
// overflow it.  Decode at most this many frames per CIN_Bink_Run...
#define BINK_MAX_CATCHUP_FRAMES	4
// ...and past this much lag, drop the backlog and resync the clock instead.
#define BINK_RESYNC_MSEC		500

#define BINK_FADE_MSEC			500

// Bink front-loads an audio primer: intro.bik hands over 34560 samples with its
// first frame -- 784ms, where a steady frame carries about 2900.  The raw stream
// only holds MAX_RAW_SAMPLES (371ms at 44.1kHz) and S_RawSamples does not reject
// a surplus, it wraps and overwrites samples that have not played yet, which is
// audible as a click.  So decoded audio is parked here and released as the
// stream drains.  Two seconds of stereo is far more backlog than Bink presents.
#define BINK_PCM_QUEUE_SAMPLES	( 2 * 48000 )
// leave the stream this far short of full, so a late frame has somewhere to land
#define BINK_PCM_RESERVE		2048

typedef struct {
	BinkFile	*bf;

	byte		*rgba;			// width*height*4, RGBA8888
	int			width, height;

	int			fpsNum, fpsDen;
	int			nbFrames;

	int			framesDecoded;	// frames handed to rgba so far
	int			startTime;		// CL_ScaledMilliseconds() when playback began
	int			startSoundtime;	// s_soundtime when playback began
	int			durationMsec;

	qboolean	silent;
	qboolean	hasAudio;
	qboolean	fadeUp, fadeDown;

	short		*pcmQueue;		// interleaved, waiting for room in the raw stream
	int			pcmHead;		// per-channel read cursor
	int			pcmCount;		// per-channel samples held
	int			pcmRate;
	int			pcmChannels;
} cinBink_t;

cvar_t	*cl_VidFadeUp;
cvar_t	*cl_VidFadeDown;

// the vendored decoder logs to stderr by default, which nothing sees in the
// Windows GUI build or on Android
void (*bink_log_hook)( const char *msg );

static void CIN_Bink_Log( const char *msg ) {
	Com_Printf( "Bink: %s", msg );
}

void CIN_Bink_Init( void ) {
	cl_VidFadeUp   = Cvar_Get( "cl_VidFadeUp",   "0", 0 );
	cl_VidFadeDown = Cvar_Get( "cl_VidFadeDown", "0", 0 );
	bink_log_hook  = CIN_Bink_Log;
}

/*
==================
CIN_Bink_IsBinkHeader

Bink 1 files start with "BIK" plus a revision letter; Bink 2 uses "KB2", which
the decoder rejects at demux.  EF is entirely BIKi.
==================
*/
qboolean CIN_Bink_IsBinkHeader( const byte *header, int len ) {
	if ( len < 4 ) {
		return qfalse;
	}
	return (qboolean)( header[0] == 'B' && header[1] == 'I' && header[2] == 'K' );
}

/*
==================
CIN_Bink_Open
==================
*/
void *CIN_Bink_Open( const char *path, qboolean silent, int *width, int *height ) {
	cinBink_t				*cb;
	const BinkStreamInfo	*vi;
	fileHandle_t			f;
	byte					*fileBuf;
	long					fileLen;

	// deliberately not FS_ReadFile: that allocates hunk temp memory, and
	// intro.bik is 29MB.  These reads are large and short-lived, so they are
	// kept off both the hunk and the zone.
	fileLen = FS_FOpenFileRead( path, &f, qtrue );
	if ( fileLen <= 0 || !f ) {
		if ( f ) {
			FS_FCloseFile( f );
		}
		Com_DPrintf( "CIN_Bink_Open: cannot read %s\n", path );
		return NULL;
	}

	fileBuf = malloc( fileLen );
	if ( !fileBuf ) {
		FS_FCloseFile( f );
		Com_Printf( "CIN_Bink_Open: out of memory for %s (%li bytes)\n", path, fileLen );
		return NULL;
	}
	FS_Read( fileBuf, fileLen, f );
	FS_FCloseFile( f );

	cb = Z_Malloc( sizeof( *cb ) );
	Com_Memset( cb, 0, sizeof( *cb ) );

	// the demuxer keeps its own padded copy, so the read buffer goes straight back
	if ( bink_open_memory( &cb->bf, fileBuf, (int)fileLen ) < 0 ) {
		Com_Printf( "CIN_Bink_Open: %s is not a playable Bink file\n", path );
		free( fileBuf );
		Z_Free( cb );
		return NULL;
	}
	free( fileBuf );

	vi = bink_stream( cb->bf, 0 );
	if ( !vi || vi->width <= 0 || vi->height <= 0 || !bink_has_video_decoder( cb->bf ) ) {
		Com_Printf( "CIN_Bink_Open: %s has no usable video stream\n", path );
		bink_close( &cb->bf );
		Z_Free( cb );
		return NULL;
	}

	cb->width    = vi->width;
	cb->height   = vi->height;
	cb->fpsNum   = vi->fps_num > 0 ? vi->fps_num : 15;
	cb->fpsDen   = vi->fps_den > 0 ? vi->fps_den : 1;
	cb->nbFrames = vi->nb_frames;
	cb->silent   = silent;

	cb->durationMsec = (int)( (int64_t)cb->nbFrames * 1000 * cb->fpsDen / cb->fpsNum );

	// snapshot the fade flags: the game DLL sets them immediately before it
	// issues inGameCinematic, and a mid-video change must not take effect
	cb->fadeUp   = (qboolean)( cl_VidFadeUp   && cl_VidFadeUp->integer );
	cb->fadeDown = (qboolean)( cl_VidFadeDown && cl_VidFadeDown->integer );

	cb->rgba = Z_Malloc( cb->width * cb->height * 4 );
	Com_Memset( cb->rgba, 0, cb->width * cb->height * 4 );

	{
		int		si;

		for ( si = 1 ; si < bink_stream_count( cb->bf ) ; si++ ) {
			const BinkStreamInfo	*ai = bink_stream( cb->bf, si );

			if ( ai && ai->type == BINK_STREAM_AUDIO && ai->sample_rate > 0 ) {
				cb->hasAudio = qtrue;
				break;
			}
		}
	}

	// must follow the scan above -- hasAudio decides whether this is needed
	if ( cb->hasAudio && !cb->silent ) {
		cb->pcmQueue = Z_Malloc( BINK_PCM_QUEUE_SAMPLES * 2 * sizeof( short ) );
	}

	cb->framesDecoded  = 0;
	cb->startTime      = CL_ScaledMilliseconds();
	cb->startSoundtime = s_soundtime;

	if ( !cb->silent ) {
		s_rawend[0] = s_soundtime;
	}

	Com_DPrintf( "CIN_Bink_Open: %s %ix%i %i/%i fps %i frames\n",
		path, cb->width, cb->height, cb->fpsNum, cb->fpsDen, cb->nbFrames );

	*width  = cb->width;
	*height = cb->height;
	return cb;
}

/*
==================
CIN_Bink_YUVtoRGBA

YUV420P -> RGBA8888, studio-range BT.601 -- the same matrix the decoder's own
bink_frame_to_rgb uses.  Measured rather than assumed: a luma histogram over the
EF videos puts the black floor as a hard spike at Y=16 with the top rolling off
past Y=234, so the content is limited range.  The full-range matrix would crush
the blacks of every cutscene.

Alpha is written as 255: the renderer uploads this as GL_RGBA and never samples
the alpha, but the source layout must still be 4 bytes per pixel.
==================
*/
static void CIN_Bink_YUVtoRGBA( const BinkFrameResult *fr, byte *dst, int width, int height ) {
	int				row, col;
	const byte		*yPlane = fr->planes[0];
	const byte		*uPlane = fr->planes[1];
	const byte		*vPlane = fr->planes[2];

	for ( row = 0; row < height; row++ ) {
		const byte	*yLine = yPlane + (size_t)row * fr->strides[0];
		const byte	*uLine = uPlane + (size_t)( row >> 1 ) * fr->strides[1];
		const byte	*vLine = vPlane + (size_t)( row >> 1 ) * fr->strides[2];
		byte		*out   = dst + (size_t)row * width * 4;

		for ( col = 0; col < width; col++ ) {
			int	y  = yLine[col] - 16;
			int	cb = uLine[col >> 1] - 128;
			int	cr = vLine[col >> 1] - 128;
			int	r, g, b;

			r = ( 298 * y + 409 * cr + 128 ) >> 8;
			g = ( 298 * y - 100 * cb - 208 * cr + 128 ) >> 8;
			b = ( 298 * y + 516 * cb + 128 ) >> 8;

			out[0] = (byte)( r < 0 ? 0 : ( r > 255 ? 255 : r ) );
			out[1] = (byte)( g < 0 ? 0 : ( g > 255 ? 255 : g ) );
			out[2] = (byte)( b < 0 ? 0 : ( b > 255 ? 255 : b ) );
			out[3] = 255;
			out += 4;
		}
	}
}

/*
==================
CIN_Bink_QueueAudio

Converts a frame's audio (interleaved float32) to 16-bit and parks it.  It is not
handed straight to S_RawSamples because Bink delivers it unevenly -- see the
BINK_PCM_QUEUE_SAMPLES note.
==================
*/
static void CIN_Bink_QueueAudio( cinBink_t *cb, const BinkFrameResult *fr ) {
	const float	*src;
	int			channels, rate, samples, total, i;

	if ( !cb->pcmQueue || fr->nb_audio <= 0 || !fr->audio[0] ) {
		return;
	}

	src      = fr->audio[0];
	channels = fr->audio_channels[0];
	rate     = fr->audio_sample_rate[0];
	samples  = fr->audio_nb_samples[0];		// per channel

	if ( samples <= 0 || channels <= 0 || channels > 2 || rate <= 0 ) {
		return;
	}

	cb->pcmRate     = rate;
	cb->pcmChannels = channels;

	// reclaim what has already been handed over
	if ( cb->pcmHead > 0 ) {
		if ( cb->pcmCount > 0 ) {
			memmove( cb->pcmQueue, cb->pcmQueue + cb->pcmHead * channels,
				cb->pcmCount * channels * sizeof( short ) );
		}
		cb->pcmHead = 0;
	}

	if ( cb->pcmCount + samples > BINK_PCM_QUEUE_SAMPLES ) {
		samples = BINK_PCM_QUEUE_SAMPLES - cb->pcmCount;
		if ( samples <= 0 ) {
			Com_DPrintf( "CIN_Bink: audio queue full, dropping a packet\n" );
			return;
		}
	}

	total = samples * channels;
	for ( i = 0 ; i < total ; i++ ) {
		float	f = src[i] * 32767.0f;

		if ( f > 32767.0f ) {
			f = 32767.0f;
		} else if ( f < -32768.0f ) {
			f = -32768.0f;
		}
		cb->pcmQueue[ ( cb->pcmCount * channels ) + i ] = (short)f;
	}
	cb->pcmCount += samples;
}

/*
==================
CIN_Bink_FlushAudio

Releases as much of the queue as the raw stream has room for.  Called every frame
so the backlog drains even when no new video frame is due.
==================
*/
static void CIN_Bink_FlushAudio( cinBink_t *cb ) {
	int		lead, roomOut, roomIn, feed;

	if ( !cb->pcmQueue || cb->pcmCount <= 0 || cb->pcmChannels <= 0 ) {
		return;
	}

	lead = s_rawend[0] - s_soundtime;
	if ( lead < 0 ) {
		lead = 0;
	}
	roomOut = MAX_RAW_SAMPLES - BINK_PCM_RESERVE - lead;
	if ( roomOut <= 0 ) {
		return;
	}

	// the stream counts samples at the output rate, the queue holds them at the file's
	roomIn = ( dma.speed > 0 && cb->pcmRate > 0 )
		? (int)( (int64_t)roomOut * cb->pcmRate / dma.speed )
		: roomOut;

	feed = cb->pcmCount < roomIn ? cb->pcmCount : roomIn;
	if ( feed <= 0 ) {
		return;
	}

	S_RawSamples( 0, feed, cb->pcmRate, 2, cb->pcmChannels,
		(byte *)( cb->pcmQueue + cb->pcmHead * cb->pcmChannels ), 1.0f, -1 );


	cb->pcmHead  += feed;
	cb->pcmCount -= feed;
}

/*
==================
CIN_Bink_TargetFrame

Which frame playback should have reached by now.

Paced from the audio clock whenever the video has a soundtrack.  Each decoded
frame pushes that frame's audio, so pacing the decode from the wall clock lets
the two drift: the buffered lead grows until it passes MAX_RAW_SAMPLES, at which
point S_RawSamples silently discards the excess and every drop is an audible
click.  Measured on Quest, where the wall clock ran far enough ahead of the sound
device to overflow the ring about once a second.  Following s_soundtime instead
makes the device's own consumption rate the clock, which cannot drift from it,
and keeps the picture locked to the sound rather than merely close to it.

The wall clock remains the fallback: for a silent video, and until the mixer
starts advancing s_soundtime -- otherwise playback would never begin with sound
switched off.
==================
*/
static int CIN_Bink_TargetFrame( cinBink_t *cb ) {
	int		elapsed, target, maxLag;

	if ( !cb->silent && cb->hasAudio && dma.speed > 0 && s_soundtime > cb->startSoundtime ) {
		// s_soundtime counts samples at the output rate, not the file's
		int		outPerFrame = ( dma.speed * cb->fpsDen ) / cb->fpsNum;

		if ( outPerFrame > 0 ) {
			return ( s_soundtime - cb->startSoundtime ) / outPerFrame;
		}
	}

	elapsed = CL_ScaledMilliseconds() - cb->startTime;
	if ( elapsed < 0 ) {
		elapsed = 0;
	}
	target = (int)( (int64_t)elapsed * cb->fpsNum / ( 1000 * cb->fpsDen ) );

	// a long stall (level load, alt-tab) is dropped rather than paid back, so
	// the audio ring is never flooded trying to catch up
	maxLag = ( BINK_RESYNC_MSEC * cb->fpsNum ) / ( 1000 * cb->fpsDen );
	if ( target - cb->framesDecoded > maxLag ) {
		cb->startTime = CL_ScaledMilliseconds()
			- (int)( (int64_t)cb->framesDecoded * 1000 * cb->fpsDen / cb->fpsNum );
		target = cb->framesDecoded;
	}

	return target;
}

/*
==================
CIN_Bink_Run

Advances playback to the current target frame and reports the newest frame.
==================
*/
e_status CIN_Bink_Run( void *state, byte **buf, qboolean *dirty, qboolean looping ) {
	cinBink_t		*cb = (cinBink_t *)state;
	BinkFrameResult	fr;
	int				target, decoded;

	if ( !cb ) {
		return FMV_EOF;
	}

	*dirty = qfalse;
	*buf   = cb->rgba;

	target = CIN_Bink_TargetFrame( cb );

	for ( decoded = 0; cb->framesDecoded <= target && decoded < BINK_MAX_CATCHUP_FRAMES; decoded++ ) {
		int		ret = bink_decode_frame( cb->bf, &fr );

		if ( ret <= 0 ) {
			if ( !looping ) {
				return FMV_EOF;
			}
			if ( bink_seek( cb->bf, 0 ) < 0 ) {
				return FMV_EOF;
			}
			cb->framesDecoded  = 0;
			cb->startTime      = CL_ScaledMilliseconds();
			cb->startSoundtime = s_soundtime;
			cb->pcmHead        = 0;
			cb->pcmCount       = 0;
			if ( !cb->silent ) {
				s_rawend[0] = s_soundtime;
			}
			return FMV_LOOPED;
		}

		if ( !cb->silent ) {
			CIN_Bink_QueueAudio( cb, &fr );
		}

		if ( fr.planes[0] && fr.width == cb->width && fr.height == cb->height ) {
			CIN_Bink_YUVtoRGBA( &fr, cb->rgba, cb->width, cb->height );
			*dirty = qtrue;
		}
		cb->framesDecoded++;
	}

	CIN_Bink_FlushAudio( cb );

	return FMV_PLAY;
}

/*
==================
CIN_Bink_DrawFade

Black overlay for the fades the SP game DLL requests.  Drawn after the frame, in
640x480 virtual coordinates -- SCR_FillRect does its own SCR_AdjustFrom640.
==================
*/
void CIN_Bink_DrawFade( void *state, float x, float y, float w, float h ) {
	cinBink_t	*cb = (cinBink_t *)state;
	float		color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	int			elapsed, remaining;
	float		alpha = 0.0f;

	if ( !cb ) {
		return;
	}

	elapsed = CL_ScaledMilliseconds() - cb->startTime;

	if ( cb->fadeUp && elapsed < BINK_FADE_MSEC ) {
		alpha = 1.0f - (float)elapsed / BINK_FADE_MSEC;
	}

	if ( cb->fadeDown && cb->durationMsec > 0 ) {
		remaining = cb->durationMsec - elapsed;
		if ( remaining < BINK_FADE_MSEC ) {
			float	a = 1.0f - (float)remaining / BINK_FADE_MSEC;
			if ( a > alpha ) {
				alpha = a;
			}
		}
	}

	if ( alpha <= 0.0f ) {
		return;
	}
	color[3] = alpha > 1.0f ? 1.0f : alpha;

	SCR_FillRect( x, y, w, h, color );
}

/*
==================
CIN_Bink_Close
==================
*/
void CIN_Bink_Close( void *state ) {
	cinBink_t	*cb = (cinBink_t *)state;

	if ( !cb ) {
		return;
	}
	if ( cb->bf ) {
		bink_close( &cb->bf );
	}
	if ( cb->rgba ) {
		Z_Free( cb->rgba );
	}
	if ( cb->pcmQueue ) {
		Z_Free( cb->pcmQueue );
	}
	Z_Free( cb );
}

#endif	// USE_BINK
