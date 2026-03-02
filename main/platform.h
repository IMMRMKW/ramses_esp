/********************************************************************
 * ramses_esp
 * platform.h
 *
 * (C) 2025 Peter Price
 *
 * determine identity of RAMSES_ESP platform
 * Use result to decide which modules of software to start
 * 
 */
#ifndef _PLATFORM_H_
#define _PLATFORM_H_
#include <esp_types.h>

#define _PLATFORM_LIST \
  _PLATFORM( PLATFORM_GW, 0x01, "Gateway" ) \
  _PLATFORM( PLATFORM_CS, 0x02, "CylStat" ) \

#define _PLATFORM(_i,_v,_t) ,_i=_v
enum platform{ PLATFORM_NONE=0 _PLATFORM_LIST };
#undef _PLATFORM

extern uint8_t platform( void );

#endif /* _PLATFORM_H_*/

