// ZZ9000-compatible blitter for the Minimig Z3 RTG board - MiSTer glue.
//
// Maps the RTG framebuffer window and runs zz9000_service.c from a thread.
// Polling policy: spin while the Amiga is drawing (ZZ ops are synchronous,
// so every microsecond of poll latency is a microsecond the 68k stalls),
// back off to 200us sleeps after a short idle period, and to 20ms while no
// driver has attached at all.
//
// The thread runs on the other core from the main loop. MiSTer pins itself
// to CPU1 and busy-polls there; a thread that inherits that affinity only
// gets CPU1 when the main thread's timeslice runs out, which measured as a
// flat ~0.5ms per op on hardware.

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../shmem.h"
#include "minimig_zz9000.h"
#include "zz9000/mister_zz9000.h"
#include "zz9000/zz9000_service.h"

#define ZZ_SPIN_US        5000     // keep spinning this long after the last op
#define ZZ_IDLE_SLEEP_US  200      // attached but idle
#define ZZ_NODRV_SLEEP_US 20000    // no driver: the Amiga handshake waits ~1s
#define ZZ_HEARTBEAT_US   100000
#define ZZ_CPU            0        // MiSTer's main loop owns CPU1

static pthread_t zz_thread;
static volatile int zz_running = 0;
static uint8_t *zz_fb = NULL;

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000u;
}

static void *zz_thread_main(void *)
{
	struct zz_service s;
	void *gfx = calloc(1, zz_service_gfxdata_size());
	if (!gfx) return NULL;

	zz_service_init(&s, zz_fb, MZZ_FB_SIZE, gfx);

	uint64_t last_op = 0, last_hb = 0;
	int was_attached = 0, warned_swap = 0;

	while (zz_running)
	{
		int r = zz_service_poll(&s);
		uint64_t t = now_us();

		if (r == ZZ_POLL_ATTACHED)
		{
			printf("ZZ9000: driver attached (session %08X)\n", s.session);
			was_attached = 1;
			last_op = t;
			continue;
		}
		if (r == ZZ_POLL_OP)
		{
			last_op = t;
			continue;
		}

		if (r == ZZ_POLL_NO_DRIVER)
		{
			if (was_attached)
			{
				printf("ZZ9000: driver detached after %u ops (%u rejected)\n", s.ops, s.rejected);
				was_attached = 0;
			}
			if (s.swapped && !warned_swap)
			{
				printf("ZZ9000: mailbox magic is byte-swapped - core without the RTG byte swap? not attaching\n");
				warned_swap = 1;
			}
			usleep(ZZ_NODRV_SLEEP_US);
			continue;
		}

		if (t - last_hb >= ZZ_HEARTBEAT_US)
		{
			zz_service_heartbeat(&s);
			last_hb = t;
		}

		if (t - last_op < ZZ_SPIN_US) sched_yield();
		else usleep(ZZ_IDLE_SLEEP_US);
	}

	free(gfx);
	return NULL;
}

void zz9000_stop(void)
{
	if (zz_running)
	{
		zz_running = 0;
		pthread_join(zz_thread, NULL);
	}
	if (zz_fb)
	{
		shmem_unmap(zz_fb, MZZ_FB_SIZE);
		zz_fb = NULL;
	}
}

void zz9000_start(void)
{
	zz9000_stop();

	zz_fb = (uint8_t *)shmem_map(MZZ_FB_PHYS, MZZ_FB_SIZE);
	if (!zz_fb)
	{
		printf("ZZ9000: cannot map RTG framebuffer at 0x%08X\n", (unsigned)MZZ_FB_PHYS);
		return;
	}

	// DDR3 survives a core reload: drop any stale request so a left-over
	// mailbox from the previous session is never mistaken for a live driver.
	*(volatile uint32_t *)(zz_fb + MZZ_MBOX_OFFSET + MZZ_MB_REQ_MAGIC) = 0;

	zz_running = 1;
	if (pthread_create(&zz_thread, NULL, zz_thread_main, NULL))
	{
		zz_running = 0;
		printf("ZZ9000: cannot start service thread\n");
		return;
	}

	cpu_set_t cpus;
	CPU_ZERO(&cpus);
	CPU_SET(ZZ_CPU, &cpus);
	if (pthread_setaffinity_np(zz_thread, sizeof(cpus), &cpus))
		printf("ZZ9000: cannot move service thread to CPU%d\n", ZZ_CPU);
	printf("ZZ9000: RTG blitter service started\n");
}
