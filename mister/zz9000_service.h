/*
 * MiSTer side of the ZZ9000-compatible RTG accelerator: mailbox service.
 *
 * Pure function of a mapped RTG window, so the same code runs in the MiSTer
 * main binary (minimig_zz9000.cpp maps /dev/mem and calls it from a thread)
 * and in the host test harness (tests/, plain malloc'd buffer).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZ9000_SERVICE_H
#define ZZ9000_SERVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum zz_poll_result {
	ZZ_POLL_NO_DRIVER = -1,   /* no REQ_MAGIC: Amiga driver not running */
	ZZ_POLL_IDLE      = 0,
	ZZ_POLL_ATTACHED  = 1,    /* attached to a new driver session */
	ZZ_POLL_OP        = 2,    /* executed one op */
};

struct zz_service {
	uint8_t  *fb;             /* RTG board offset 0 */
	uint32_t  fb_size;        /* MZZ_FB_SIZE */
	uint32_t  session;        /* attached session (valid if attached) */
	uint32_t  done;           /* last KICK completed */
	int       attached;
	int       swapped;        /* REQ_MAGIC seen byte-swapped: refuse */
	uint32_t  heartbeat;
	uint32_t  ops;
	uint32_t  rejected;
	uint16_t  last_op;
	void     *gfxdata;        /* private struct GFXData snapshot */
};

/* gfxdata must point at sizeof(struct GFXData) bytes (zz_service_gfxdata_size()). */
void zz_service_init(struct zz_service *s, uint8_t *fb, uint32_t fb_size, void *gfxdata);
unsigned zz_service_gfxdata_size(void);

/* One mailbox check: attach to a new session, or run one pending op. */
int  zz_service_poll(struct zz_service *s);

/* Bump the heartbeat word (call every ~100ms while attached). */
void zz_service_heartbeat(struct zz_service *s);

#ifdef __cplusplus
}
#endif

#endif
