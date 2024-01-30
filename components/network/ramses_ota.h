/********************************************************************
 * ramses_esp
 * ramses_ota.h
 *
 * (C) 2023 Peter Price
 *
 * Over The Air upgrade
 *
 */
#ifndef _RAMSES_OTA_H_
#define _RAMSES_OTA_H_

extern void ota_set_url( const char * url );
extern void ota_set_version( const char * version );
extern void ota_set_filename( const char * filename );
extern void ota_start(void);

extern void ramses_ota_init( BaseType_t coreID );

#endif // _RAMSES_OTA_H_
