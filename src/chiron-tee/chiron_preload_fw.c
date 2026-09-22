// SPDX-License-Identifier: GPL-2.0
/*
 * Push the Amlogic video firmware package into the secure world.
 *
 * This is a glibc replacement for Android's /vendor/bin/tee_preload_fw, which
 * is bionic-linked and cannot run under Armbian.  Behaviour reproduced from
 * vendor/lib64/libtee_load_video_fw.so ("Video firmware CA, Version
 * 1.0.1-gb173c81(build:4004)"):
 *
 *   - open a session on the Video firmware TA, passing the CA's own major and
 *     minor version as the first value parameter -- the TA logs it back as
 *     "The TA ver is v1.0";
 *   - send the whole package as TEEC_MEMREF_TEMP_INPUT in 128 KB chunks;
 *   - treat TEEC_ERROR_BUSY as an accepted chunk and advance by the returned
 *     memref size.  This is the vendor protocol's unusual flow-control result,
 *     not a request to submit the same bytes again.
 *
 * Until this has run, the secure world has no microcode to hand out and
 * tee_load_video_fw() from the kernel answers TEEC_ERROR_BAD_STATE (0xffff0007),
 * which leaves the T7 video decoder's processor unstarted.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tee_client_api.h>

#define VIDEO_FW_TA_UUID                                   \
	{                                                  \
		0x526fc4fc, 0x7ee6, 0x4a12,                \
		{ 0x96, 0xe3, 0x83, 0xda,                  \
		  0x95, 0x65, 0xbc, 0xe8 }                 \
	}

/* The vendor CA sends the package in 0x20000-byte pieces. */
#define CHUNK_SIZE	(128 * 1024)

/* Version the vendor CA reports to the TA, from sscanf("%d.%d") over its own
 * "1.0.1-gb173c81" string.
 */
#define CA_VERSION_MAJOR	1
#define CA_VERSION_MINOR	0

/* Keep the signed secure-world package separate from the ordinary T7 codec
 * firmware.  /lib/firmware/video/video_ucode.bin is intentionally a symlink
 * to video_ucode.bin.t7 for firmware.ko and is not accepted by the VideoFW
 * TA's RSA check.
 */
#define DEFAULT_FW_PATH		"/usr/local/lib/chiron-tee/video_ucode.secure.bin"

static unsigned char *read_file(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	unsigned char *buf;
	long len;

	if (!f) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0) {
		fprintf(stderr, "size %s: %s\n", path, strerror(errno));
		fclose(f);
		return NULL;
	}
	rewind(f);

	buf = malloc((size_t)len);
	if (!buf) {
		fprintf(stderr, "not enough memory for %ld bytes\n", len);
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
		fprintf(stderr, "read %s: short read\n", path);
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);

	*out_len = (size_t)len;
	return buf;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : DEFAULT_FW_PATH;
	uint32_t cmd = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 0;
	/* The TA rejects an oversized memref with "input data size exceeds limit",
	 * so keep the chunk adjustable rather than rebuilding to try a value.
	 */
	size_t chunk = argc > 3 ? (size_t)strtoul(argv[3], NULL, 0) : CHUNK_SIZE;
	/* The vendor entry point is load_sec_firmware(path, flag_a, flag_b): the
	 * first flag goes into the open-session value, the second becomes the
	 * command id. They are independent, both masked to one bit.
	 */
	uint32_t flag_a = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 0) & 1 : 0;
	TEEC_UUID uuid = VIDEO_FW_TA_UUID;
	TEEC_Context ctx;
	TEEC_Session sess;
	TEEC_Operation op;
	TEEC_SharedMemory shm;
	TEEC_Result res;
	uint32_t origin = 0;
	unsigned char *fw;
	size_t len, sent = 0;
	int rc = 1, use_shm = 0;

	fw = read_file(path, &len);
	if (!fw)
		return 1;

	/* Optional cap, to find where the TA's "input data size exceeds limit"
	 * threshold actually sits: it shrinks both the announced total and the
	 * amount sent.
	 */
	if (argc > 5) {
		size_t cap = (size_t)strtoul(argv[5], NULL, 0);

		if (cap && cap < len)
			len = cap;
	}

	res = TEEC_InitializeContext(NULL, &ctx);
	if (res != TEEC_SUCCESS) {
		fprintf(stderr, "TEEC_InitializeContext failed with code 0x%x\n", res);
		goto out_fw;
	}

	/* params[0] announces the whole package up front -- the TA sizes its
	 * buffer from it and answers "input data size exceeds limit" to the
	 * first chunk if this is too small. params[1] carries the CA version,
	 * which the TA logs back as "The TA ver is v1.0".
	 */
	memset(&op, 0, sizeof(op));
	op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT, TEEC_VALUE_INPUT,
					 TEEC_NONE, TEEC_NONE);
	op.params[0].value.a = (uint32_t)len;
	op.params[0].value.b = flag_a;
	op.params[1].value.a = CA_VERSION_MAJOR;
	op.params[1].value.b = CA_VERSION_MINOR;

	res = TEEC_OpenSession(&ctx, &sess, &uuid, TEEC_LOGIN_PUBLIC,
			       NULL, &op, &origin);
	if (res != TEEC_SUCCESS) {
		fprintf(stderr, "TEEC_OpenSession failed with code 0x%x origin 0x%x\n",
			res, origin);
		goto out_ctx;
	}

	/* A temporary memref is bounced through a driver-allocated buffer. If the
	 * TA sees a bogus size that way, registered shared memory is the fallback
	 * worth trying; select it with a sixth argument of "shm".
	 */
	use_shm = argc > 6 && strcmp(argv[6], "shm") == 0;
	if (use_shm) {
		memset(&shm, 0, sizeof(shm));
		shm.size = chunk;
		shm.flags = TEEC_MEM_INPUT;
		res = TEEC_AllocateSharedMemory(&ctx, &shm);
		if (res != TEEC_SUCCESS) {
			fprintf(stderr, "TEEC_AllocateSharedMemory failed 0x%x\n", res);
			goto out_sess;
		}
	}

	while (sent < len) {
		size_t n = len - sent;

		if (n > chunk)
			n = chunk;

		memset(&op, 0, sizeof(op));
		if (use_shm) {
			memcpy(shm.buffer, fw + sent, n);
			op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_PARTIAL_INPUT,
							 TEEC_NONE, TEEC_NONE, TEEC_NONE);
			op.params[0].memref.parent = &shm;
			op.params[0].memref.offset = 0;
			op.params[0].memref.size = n;
		} else {
		op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
						 TEEC_NONE, TEEC_NONE, TEEC_NONE);
		op.params[0].tmpref.buffer = fw + sent;
		op.params[0].tmpref.size = n;
		}

		res = TEEC_InvokeCommand(&sess, cmd, &op, &origin);

		/* Vendor libtee_load_video_fw.so explicitly accepts both SUCCESS and
		 * BUSY, then advances by the returned tmpref size. Resending on BUSY
		 * feeds the first chunk repeatedly until VideoFW TA rejects the stream
		 * with "input data size exceeds limit".
		 */
		if (res != TEEC_SUCCESS && res != TEEC_ERROR_BUSY) {
			fprintf(stderr,
				"TEEC_InvokeCommand failed with code 0x%x origin 0x%x at offset %zu\n",
				res, origin, sent);
			goto out_sess;
		}
		/* The TA writes back how much it actually consumed. */
		sent += op.params[0].tmpref.size ? op.params[0].tmpref.size : n;
	}

	printf("tee preload video fw ok (%zu bytes, cmd %u, chunk %zu)\n",
	       len, cmd, chunk);
	rc = 0;

out_sess:
	if (use_shm)
		TEEC_ReleaseSharedMemory(&shm);
	TEEC_CloseSession(&sess);
out_ctx:
	TEEC_FinalizeContext(&ctx);
out_fw:
	free(fw);
	if (rc)
		fprintf(stderr, "tee preload video fw fail\n");
	return rc;
}
