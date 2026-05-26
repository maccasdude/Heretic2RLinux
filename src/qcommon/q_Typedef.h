//
// q_Typedef.h
//
// Copyright 1998 Raven Software
//

#pragma once

// MSVC's _inline keyword is just plain inline on GCC.
// We use 'static inline' rather than plain 'inline' because plain inline in
// C99 means "the inline definition serves only this translation unit; an
// external symbol must exist somewhere else", which causes link errors for
// _inline functions defined in headers (like Q_stricmp, GetFreeNode).
#if !(defined(_WIN32) || defined(WIN32))
#  ifndef _inline
#    define _inline static inline
#  endif
#endif

// File-scope forward declarations of struct tags that get used in function
// prototypes scattered across many headers. Without these, GCC follows
// C99 6.7.2.3p6 and treats `struct foo` inside a parameter list as a
// distinct tag scoped only to that prototype - which causes "conflicting
// types" errors when the same struct is later fully defined elsewhere.
// MSVC silently lets this slide; GCC does not.
struct client_entity_s;
struct client_particle_s;
struct edict_s;
struct gclient_s;
struct gitem_s;
struct cplane_s;
struct csurface_s;
struct trace_s;
struct pmove_s;
struct usercmd_s;
struct pmove_state_s;
struct centity_s;
struct Surface_s;
struct cvar_s;
struct entity_s;
struct dlight_s;
struct lightstyle_s;
struct particle_s;
struct refdef_s;
struct refexport_s;
struct refimport_s;
struct sizebuf_s;
struct netadr_s;
struct sfx_s;
struct sfxcache_s;

typedef float vec3_t[3];
typedef float matrix3_t[3][3];

typedef unsigned short ushort;	//mxd. Way shorter than "unsigned short"...
typedef unsigned uint;			//mxd. Shorter than "size_t", way shorter than "unsigned int"...
typedef unsigned long ulong;	//mxd. Way shorter than "unsigned long"...
typedef unsigned char byte;

#ifndef __cplusplus //mxd. Needed, so code in game/ds.cpp could build...
	typedef enum { false, true } qboolean;
#else
	typedef int qboolean;
#endif

typedef struct edict_s edict_t;

typedef struct paletteRGBA_s
{
	union
	{
		struct
		{
			byte r;
			byte g;
			byte b;
			byte a;
		};
		uint c;
		byte c_array[4];
	};
} paletteRGBA_t;