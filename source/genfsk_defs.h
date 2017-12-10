/*
 * genfsk_defs.h
 *
 *  Created on: 24 Nov 2017
 *      Author: Samuel Mott
 */

#ifndef GENFSK_DEFS_H_
#define GENFSK_DEFS_H_

//#define RX
#define RX
#define DEVICEADDRESS 2

#if defined(TX) && defined(RX)
#error "Cannot have device both transmit and receive"
#endif

#if !defined(TX) && !defined(RX)
#error "Must define device to either transmit or receive"
#endif

#endif /* GENFSK_DEFS_H_ */
