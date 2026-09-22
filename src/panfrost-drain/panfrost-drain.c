// SPDX-License-Identifier: 0BSD
// Copyright (c) 2026 angellfear
/*
 * Wait for a Mesa panfrost context's last job before its fence is destroyed.
 *
 * Mesa's panfrost_destroy() releases the context without waiting for the jobs
 * it has already submitted. Those jobs point into Mesa's screen-wide shader and
 * descriptor pools, which are in no job's BO list, so the kernel does not keep
 * them alive: when a program exits without a glFinish(), the pools are freed
 * under the GPU and every job still queued takes an MMU fault and a 500 ms
 * scheduler timeout. `gst-launch-1.0 gltestsrc ! fakesink` shows it every time.
 *
 * The context's syncobj is the out-fence of its last job, and panfrost_destroy()
 * destroys it on the way out. So this library, preloaded into every process,
 * remembers which syncobjs a process handed to DRM_IOCTL_PANFROST_SUBMIT as an
 * out-fence, and waits on such a syncobj -- for at most five seconds -- before
 * letting DRM_IOCTL_SYNCOBJ_DESTROY through. That is the same wait the Mesa fix
 * adds at the start of panfrost_destroy(); drop this library once the archive
 * Mesa carries it.
 *
 * Every other ioctl passes straight through. A syncobj is only remembered after
 * DRM_IOCTL_VERSION says the fd belongs to panfrost, because the submit ioctl
 * number is simply the first driver-specific number on any DRM device.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <drm/drm.h>
#include <drm/panfrost_drm.h>

#define DRAIN_TIMEOUT_NS (5LL * 1000000000LL)
#define MAX_FENCES 64

/* The original struct: newer Mesa passes a longer one, and the size is part of
 * the request code, so requests are matched on type and number only. */
#define SUBMIT_MIN_SIZE 40

static int (*next_ioctl)(int, unsigned long, ...);

static struct fence {
	int fd;
	uint32_t handle;
} fences[MAX_FENCES];
static unsigned int nfences;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static int same_request(unsigned long req, unsigned long code)
{
	return _IOC_TYPE(req) == _IOC_TYPE(code) && _IOC_NR(req) == _IOC_NR(code);
}

static int is_panfrost(int fd)
{
	char name[16] = "";
	struct drm_version v = {
		.name_len = sizeof(name) - 1,
		.name = name,
	};

	if (next_ioctl(fd, DRM_IOCTL_VERSION, &v))
		return 0;
	return strcmp(name, "panfrost") == 0;
}

static void remember(int fd, uint32_t handle)
{
	unsigned int i;
	int known = 0;

	pthread_mutex_lock(&lock);
	for (i = 0; i < nfences; i++)
		if (fences[i].fd == fd && fences[i].handle == handle)
			known = 1;
	pthread_mutex_unlock(&lock);
	if (known || !is_panfrost(fd))
		return;

	pthread_mutex_lock(&lock);
	if (nfences < MAX_FENCES) {
		fences[nfences].fd = fd;
		fences[nfences].handle = handle;
		nfences++;
	}
	pthread_mutex_unlock(&lock);
}

static int forget(int fd, uint32_t handle)
{
	unsigned int i;
	int found = 0;

	pthread_mutex_lock(&lock);
	for (i = 0; i < nfences; i++)
		if (fences[i].fd == fd && fences[i].handle == handle) {
			fences[i] = fences[--nfences];
			found = 1;
			break;
		}
	pthread_mutex_unlock(&lock);
	return found;
}

static void drain(int fd, uint32_t handle)
{
	struct timespec now;
	struct drm_syncobj_wait w = {
		.handles = (uintptr_t)&handle,
		.count_handles = 1,
	};
	int saved = errno;

	clock_gettime(CLOCK_MONOTONIC, &now);
	/* The kernel takes an absolute CLOCK_MONOTONIC deadline. */
	w.timeout_nsec = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec +
			 DRAIN_TIMEOUT_NS;
	/* EINVAL for a syncobj that never received a fence, ETIME on timeout:
	 * either way the destroy goes ahead exactly as it would have. */
	next_ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &w);
	errno = saved;
}

__attribute__((constructor)) static void init(void)
{
	next_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
}

int ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	void *arg;
	int ret;

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);

	if (!next_ioctl) {
		init();
		if (!next_ioctl) {
			errno = ENOSYS;
			return -1;
		}
	}

	if (_IOC_TYPE(req) != DRM_IOCTL_BASE || !arg)
		return next_ioctl(fd, req, arg);

	if (same_request(req, DRM_IOCTL_SYNCOBJ_DESTROY)) {
		const struct drm_syncobj_destroy *d = arg;

		if (forget(fd, d->handle))
			drain(fd, d->handle);
		return next_ioctl(fd, req, arg);
	}

	ret = next_ioctl(fd, req, arg);

	if (ret == 0 && same_request(req, DRM_IOCTL_PANFROST_SUBMIT) &&
	    (_IOC_DIR(req) & _IOC_WRITE) && _IOC_SIZE(req) >= SUBMIT_MIN_SIZE) {
		const struct drm_panfrost_submit *s = arg;

		if (s->out_sync) {
			int saved = errno;

			remember(fd, s->out_sync);
			errno = saved;
		}
	}
	return ret;
}
