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

// cl_cin_bink.h -- Bink playback backend, driven by cl_cin.c

#ifndef __CL_CIN_BINK_H
#define __CL_CIN_BINK_H

void		CIN_Bink_Init( void );

qboolean	CIN_Bink_IsBinkHeader( const byte *header, int len );

// returns an opaque handle, or NULL if the file will not play
void	   *CIN_Bink_Open( const char *path, qboolean silent, int *width, int *height );

// advances to the wall clock; sets buf to the current RGBA8888 frame and dirty
// when that frame changed.  FMV_PLAY, FMV_LOOPED or FMV_EOF.
e_status	CIN_Bink_Run( void *state, byte **buf, qboolean *dirty, qboolean looping );

// black overlay for cl_VidFadeUp / cl_VidFadeDown, in 640x480 virtual coords
void		CIN_Bink_DrawFade( void *state, float x, float y, float w, float h );

void		CIN_Bink_Close( void *state );

#endif	// __CL_CIN_BINK_H
