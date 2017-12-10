/*!
* Copyright 2016-2017 NXP
*
* \file
*
* This is a source file for the main application.
*
* Redistribution and use in source and binary forms, with or without modification,
* are permitted provided that the following conditions are met:
*
* o Redistributions of source code must retain the above copyright notice, this list
*   of conditions and the following disclaimer.
*
* o Redistributions in binary form must reproduce the above copyright notice, this
*   list of conditions and the following disclaimer in the documentation and/or
*   other materials provided with the distribution.
*
* o Neither the name of Freescale Semiconductor, Inc. nor the names of its
*   contributors may be used to endorse or promote products derived from this
*   software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
* ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
* ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#ifndef __GEN_FSK_TESTS_H__
#define __GEN_FSK_TESTS_H__

#include "stdint.h"

/*! *********************************************************************************
*************************************************************************************
* Public type definitions
*************************************************************************************
********************************************************************************** */
typedef enum ct_event_tag
{
  gCtEvtRxDone_c       = 0x00000001U,
  gCtEvtTxDone_c       = 0x00000002U,
  gCtEvtSeqTimeout_c   = 0x00000004U,
  gCtEvtRxFailed_c     = 0x00000008U,
  
  gCtEvtTimerExpired_c = 0x00000010U,
  gCtEvtUart_c         = 0x00000020U,
  gCtEvtKBD_c          = 0x00000040U,
  gCtEvtSelfEvent_c    = 0x00000080U,
  
  gCtEvtWakeUp_c       = 0x00000100U,
  
  gCtEvtMaxEvent_c     = 0x00000200U,
  gCtEvtEventsAll_c    = 0x000003FFU
}ct_event_t;

typedef enum ct_param_type_tag{
    gParamTypeNumber_c = 0,
    gParamTypeString_c,
    gParamTypeBool_c,
    gParamTypeMaxType_c
}ct_param_type_t;

typedef struct ct_config_params_tag
{
    ct_param_type_t paramType;
    uint8_t paramName[20];
    union
    {
        uint32_t decValue;
        uint8_t stringValue[4];
        bool_t  boolValue;
    }
    paramValue;
}ct_config_params_t;

typedef struct ct_rx_indication_tag
{
    uint64_t timestamp;
    uint8_t *pBuffer;
    uint16_t bufferLength; 
    uint8_t rssi;
    uint8_t crcValid;
}ct_rx_indication_t;

typedef void (* pHookAppNotification) ( void );
typedef void (* pTmrHookNotification) (void*);
/*! *********************************************************************************
*************************************************************************************
* Public macros
*************************************************************************************
********************************************************************************** */
#define gModeRx_c (1)
#define gModeTx_c (2)
#define gDefaultMode_c gModeRx_c

/*tx power*/
#define gGenFskMaxTxPowerLevel_c     (0x20)
#define gGenFskMinTxPowerLevel_c     (0x00)
#define gGenFskDefaultTxPowerLevel_c (0x08)

/*channel*/
#define gGenFskMaxChannel_c     (0x7F)
#define gGenFskMinChannel_c     (0x00)
#define gGenFskDefaultChannel_c (0x2A)
                                        
/*network address*/
#define gGenFskDefaultSyncAddress_c  (0x8E89BED6)
#define gGenFskDefaultSyncAddrSize_c (0x03) /*bytes = size + 1*/

/*the following field sizes must be multiple of 8 bit*/
#define gGenFskDefaultH0FieldSize_c     (8)
#define gGenFskDefaultLengthFieldSize_c (6)
#define gGenFskDefaultH1FieldSize_c     (2)
#define gGenFskDefaultHeaderSizeBytes_c ((gGenFskDefaultH0FieldSize_c + \
                                         gGenFskDefaultLengthFieldSize_c + \
                                             gGenFskDefaultH1FieldSize_c) >> 3)                             
#if gGenFskDefaultLengthFieldSize_c < 3
#error "For this application the length field size should not be less than 3"
#endif

/*payload length*/
#define gGenFskMaxPayloadLen_c ((1 << gGenFskDefaultLengthFieldSize_c) - 1)

/*test opcode + 2byte packet index + 2byte number of packets for Radio*/
#define gGenFskMinPayloadLen_c (6) 
#define gGenFskDefaultPayloadLen_c (gGenFskMinPayloadLen_c)

#define gGenFskDefaultMaxBufferSize_c (gGenFskDefaultSyncAddrSize_c + 1 + \
                                       gGenFskDefaultHeaderSizeBytes_c  + \
                                           gGenFskMaxPayloadLen_c)

/*H0 and H1 config*/
#define gGenFskDefaultH0Value_c        (0x0000)
#define gGenFskDefaultH0Mask_c         ((1 << gGenFskDefaultH0FieldSize_c) - 1)

#define gGenFskDefaultH1Value_c        (0x0000)
#define gGenFskDefaultH1Mask_c         ((1 << gGenFskDefaultH1FieldSize_c) - 1)
/*! *********************************************************************************
*************************************************************************************
* Public memory declarations
*************************************************************************************
********************************************************************************** */
extern ct_config_params_t gaConfigParams[];
extern uint8_t mAppSerId;
extern uint8_t mAppTmrId;
/*! *********************************************************************************
*************************************************************************************
* Public prototypes
*************************************************************************************
********************************************************************************** */
extern void GenFskInit(pHookAppNotification pFunc, pTmrHookNotification pTmrFunc);

/* Genfsk RX handler */
extern bool_t Genfsk_Receive(ct_event_t evType, void* pAssociatedValue);
/* Genfsk TX handler */
extern bool_t Genfsk_Send(ct_event_t evType, void* pAssociatedValue, uint8_t ledstate, uint8_t address);
#endif
