/*
 * PTP (Picture Transfer Protocol, ISO 15740 / PIMA 15740) protocol
 * constants and container (de)serialization helpers.
 *
 * This file only knows about the wire format. It does not touch USB.
 * See ptp_transport.h for the USB bulk transport that carries these
 * containers, and camera.h for the high level operations built on top.
 *
 * The Pentax K200D does not use any vendor-specific PTP opcodes (Pentax
 * has no vendor extension opcodes in libgphoto2's ptp2 driver either) so
 * only the standard ISO 15740 operation/response/event codes are needed.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------- Container header --------------------------- */

#define PTP_CONTAINER_HEADER_SIZE   12u
#define PTP_MAX_PARAMS              5u

typedef enum {
    PTP_CONTAINER_TYPE_COMMAND  = 1,
    PTP_CONTAINER_TYPE_DATA     = 2,
    PTP_CONTAINER_TYPE_RESPONSE = 3,
    PTP_CONTAINER_TYPE_EVENT    = 4,
} ptp_container_type_t;

/* -------------------------- Standard operations --------------------------- */

#define PTP_OC_GetDeviceInfo          0x1001
#define PTP_OC_OpenSession            0x1002
#define PTP_OC_CloseSession           0x1003
#define PTP_OC_GetStorageIDs          0x1004
#define PTP_OC_GetStorageInfo         0x1005
#define PTP_OC_GetNumObjects          0x1006
#define PTP_OC_GetObjectHandles       0x1007
#define PTP_OC_GetObjectInfo          0x1008
#define PTP_OC_GetObject              0x1009
#define PTP_OC_GetThumb               0x100A
#define PTP_OC_DeleteObject           0x100B
#define PTP_OC_SendObjectInfo         0x100C
#define PTP_OC_SendObject             0x100D
#define PTP_OC_InitiateCapture        0x100E
#define PTP_OC_FormatStore            0x100F
#define PTP_OC_ResetDevice            0x1010
#define PTP_OC_SelfTest               0x1011
#define PTP_OC_SetObjectProtection    0x1012
#define PTP_OC_PowerDown              0x1013
#define PTP_OC_GetDevicePropDesc      0x1014
#define PTP_OC_GetDevicePropValue     0x1015
#define PTP_OC_SetDevicePropValue     0x1016
#define PTP_OC_ResetDevicePropValue   0x1017
#define PTP_OC_TerminateOpenCapture   0x1018
#define PTP_OC_MoveObject             0x1019
#define PTP_OC_CopyObject             0x101A
#define PTP_OC_GetPartialObject       0x101B
#define PTP_OC_InitiateOpenCapture    0x101C

/* ---------------------------- Response codes ------------------------------ */

#define PTP_RC_OK                          0x2001
#define PTP_RC_GeneralError                0x2002
#define PTP_RC_SessionNotOpen              0x2003
#define PTP_RC_InvalidTransactionID        0x2004
#define PTP_RC_OperationNotSupported       0x2005
#define PTP_RC_ParameterNotSupported       0x2006
#define PTP_RC_IncompleteTransfer          0x2007
#define PTP_RC_InvalidStorageID            0x2008
#define PTP_RC_InvalidObjectHandle         0x2009
#define PTP_RC_DevicePropNotSupported      0x200A
#define PTP_RC_InvalidObjectFormatCode     0x200B
#define PTP_RC_StoreFull                   0x200C
#define PTP_RC_ObjectWriteProtected        0x200D
#define PTP_RC_StoreReadOnly               0x200E
#define PTP_RC_AccessDenied                0x200F
#define PTP_RC_NoThumbnailPresent          0x2010
#define PTP_RC_SelfTestFailed              0x2011
#define PTP_RC_PartialDeletion             0x2012
#define PTP_RC_StoreNotAvailable           0x2013
#define PTP_RC_SpecByFormatUnsupported     0x2014
#define PTP_RC_NoValidObjectInfo           0x2015
#define PTP_RC_InvalidCodeFormat           0x2016
#define PTP_RC_UnknownVendorCode           0x2017
#define PTP_RC_CaptureAlreadyTerminated    0x2018
#define PTP_RC_DeviceBusy                  0x2019
#define PTP_RC_InvalidParentObject         0x201A
#define PTP_RC_InvalidDevicePropFormat     0x201B
#define PTP_RC_InvalidDevicePropValue      0x201C
#define PTP_RC_InvalidParameter            0x201D
#define PTP_RC_SessionAlreadyOpen          0x201E
#define PTP_RC_TransactionCanceled         0x201F
#define PTP_RC_SpecOfDestinationUnsupported 0x2020

/* ------------------------------ Event codes -------------------------------- */

#define PTP_EC_CancelTransaction      0x4001
#define PTP_EC_ObjectAdded            0x4002
#define PTP_EC_ObjectRemoved          0x4003
#define PTP_EC_StoreAdded             0x4004
#define PTP_EC_StoreRemoved           0x4005
#define PTP_EC_DevicePropChanged      0x4006
#define PTP_EC_ObjectInfoChanged      0x4007
#define PTP_EC_DeviceInfoChanged      0x4008
#define PTP_EC_RequestObjectTransfer  0x4009
#define PTP_EC_StoreFull              0x400A
#define PTP_EC_DeviceReset            0x400B
#define PTP_EC_StorageInfoChanged     0x400C
#define PTP_EC_CaptureComplete        0x400D
#define PTP_EC_UnreportedStatus       0x400E

/* --------------------------- Object format codes --------------------------- */

#define PTP_OFC_Undefined       0x3000
#define PTP_OFC_Association     0x3001  /* a folder/directory object */
#define PTP_OFC_EXIF_JPEG       0x3801
#define PTP_OFC_TIFF            0x380D
#define PTP_OFC_DNG             0x3811

/* --------------------------- Device property codes -------------------------- */
/* Standard PIMA15740 property codes. Most DSLRs only implement a subset. */

#define PTP_DPC_BatteryLevel              0x5001
#define PTP_DPC_FunctionalMode            0x5002
#define PTP_DPC_ImageSize                 0x5003
#define PTP_DPC_CompressionSetting        0x5004
#define PTP_DPC_WhiteBalance              0x5005
#define PTP_DPC_RGBGain                   0x5006
#define PTP_DPC_FNumber                   0x5007
#define PTP_DPC_FocalLength               0x5008
#define PTP_DPC_FocusDistance             0x5009
#define PTP_DPC_FocusMode                 0x500A
#define PTP_DPC_ExposureMeteringMode      0x500B
#define PTP_DPC_FlashMode                 0x500C
#define PTP_DPC_ExposureTime              0x500D
#define PTP_DPC_ExposureProgramMode       0x500E
#define PTP_DPC_ExposureIndex             0x500F
#define PTP_DPC_ExposureBiasCompensation  0x5010
#define PTP_DPC_DateTime                  0x5011
#define PTP_DPC_CaptureDelay              0x5012
#define PTP_DPC_StillCaptureMode          0x5013
#define PTP_DPC_Contrast                  0x5014
#define PTP_DPC_Sharpness                 0x5015
#define PTP_DPC_DigitalZoom               0x5016
#define PTP_DPC_EffectMode                0x5017
#define PTP_DPC_BurstNumber               0x5018
#define PTP_DPC_BurstInterval             0x5019

/* ------------------------------- Data types --------------------------------- */

#define PTP_DTC_UNDEF   0x0000
#define PTP_DTC_INT8    0x0001
#define PTP_DTC_UINT8   0x0002
#define PTP_DTC_INT16   0x0003
#define PTP_DTC_UINT16  0x0004
#define PTP_DTC_INT32   0x0005
#define PTP_DTC_UINT32  0x0006
#define PTP_DTC_INT64   0x0007
#define PTP_DTC_UINT64  0x0008
#define PTP_DTC_ARRAY_MASK 0x4000
#define PTP_DTC_STR     0xFFFF

/* ------------------------------ Vendor IDs ---------------------------------- */

#define PTP_VENDOR_PENTAX  0x0000000Du

/* --------------------------- Little-endian helpers -------------------------- */

static inline uint16_t ptp_rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t ptp_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void ptp_wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline void ptp_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* Build a 12 byte container header (+ up to 5 LE uint32 params) into `out`.
 * `out` must have room for PTP_CONTAINER_HEADER_SIZE + nparams*4 bytes.
 * Returns the number of bytes written. */
size_t ptp_build_header(uint8_t *out, uint32_t length, uint16_t type, uint16_t code,
                         uint32_t transaction_id);

size_t ptp_build_command(uint8_t *out, size_t out_cap, uint16_t opcode,
                          uint32_t transaction_id, const uint32_t *params, int nparams);

/* ------------------------- PTP string (UTF-16LE Pascal string) -------------- */

/* Decode a PTP string field (1 byte char count incl. NUL, then UTF16LE chars)
 * found at `buf[*off]` into a UTF-8 `out` buffer of size out_cap.
 * Advances *off past the field. Returns true on success. */
bool ptp_decode_string(const uint8_t *buf, size_t buf_len, size_t *off,
                       char *out, size_t out_cap);

/* Skip over a PTP string field without decoding it. */
bool ptp_skip_string(const uint8_t *buf, size_t buf_len, size_t *off);

#ifdef __cplusplus
}
#endif
