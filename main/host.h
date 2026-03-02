/********************************************************************
 * ramses_esp
 * host.h
 *
 * (C) 2023 Peter Price
 *
 * Host Interface public API
 *
 */
#ifndef _HOST_H_
#define _HOST_H_

extern void Host_init(BaseType_t coreID, uint8_t platforms);
extern void call_Host_Task();

#endif // _HOST_H_
