/********************************************************************
 * ramses_esp
 * radio.h
 *
 * (C) 2023 Peter Price
 *
 * Radion Interface public API
 *
 */
#ifndef _RADIO_H_
#define _RADIO_H_

extern void Radio_init(BaseType_t coreID);
extern void call_Radio_Task();
#endif // _RADIO_H_
