/*---------------------------------------------------------------------------/
/  Configurations of FatFs Module (R0.15 w/patch1) for PHOTON
/---------------------------------------------------------------------------*/
// Vendored from pico-sdk/lib/tinyusb/lib/fatfs (ChaN, BSD-style licence, see
// 00readme.txt). Only this file is PHOTON-specific: one SD volume, 8.3 +
// long names, FAT16/FAT32/exFAT (a 64 GB card comes exFAT out of the box),
// no RTC (timestamps are fixed; the board has no clock), and mkfs only for
// the host tests, which format a RAM disk.

#define FFCONF_DEF	80286	/* Revision ID */

/*---------------------------------------------------------------------------/
/ Function Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_READONLY	0
#define FF_FS_MINIMIZE	0
#define FF_USE_FIND		0
#ifdef PHOTON_HOST_TEST
#define FF_USE_MKFS		1
#else
#define FF_USE_MKFS		0
#endif
#define FF_USE_FASTSEEK	0
#define FF_USE_EXPAND	0
#define FF_USE_CHMOD	0
#define FF_USE_LABEL	0
#define FF_USE_FORWARD	0
#define FF_USE_STRFUNC	0
#define FF_PRINT_LLI	0
#define FF_PRINT_FLOAT	0
#define FF_STRF_ENCODE	0

/*---------------------------------------------------------------------------/
/ Locale and Namespace Configurations
/---------------------------------------------------------------------------*/

#define FF_CODE_PAGE	437
#define FF_USE_LFN		1	/* static LFN working buffer; exFAT requires LFN */
#define FF_MAX_LFN		255
#define FF_LFN_UNICODE	0	/* ANSI/OEM (code page 437) API strings */
#define FF_LFN_BUF		255
#define FF_SFN_BUF		12
#define FF_FS_RPATH		0

/*---------------------------------------------------------------------------/
/ Drive/Volume Configurations
/---------------------------------------------------------------------------*/

#define FF_VOLUMES		1
#define FF_STR_VOLUME_ID	0
#define FF_VOLUME_STRS		"SD"
#define FF_MULTI_PARTITION	0
#define FF_MIN_SS		512
#define FF_MAX_SS		512
#define FF_LBA64		0
#define FF_MIN_GPT		0x10000000
#define FF_USE_TRIM		0

/*---------------------------------------------------------------------------/
/ System Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_TINY		0
#define FF_FS_EXFAT		1
#define FF_FS_NORTC		1	/* no clock on the board: every file is stamped */
#define FF_NORTC_MON	1	/* 2026-01-01 00:00 */
#define FF_NORTC_MDAY	1
#define FF_NORTC_YEAR	2026
#define FF_FS_NOFSINFO	0
#define FF_FS_LOCK		0
#define FF_FS_REENTRANT	0
#define FF_FS_TIMEOUT	1000
