/*
 * Copyright@ Samsung Electronics Co. LTD
 *
 * This software is proprietary of Samsung Electronics.
 * No part of this software, either material or conceptual may be copied or distributed, transmitted,
 * transcribed, stored in a retrieval system or translated into any human or computer language in any form by any means,
 * electronic, mechanical, manual or otherwise, or disclosed
 * to third parties without the express written permission of Samsung Electronics.
 */

#include <arch.h>
#include <arch/arm64.h>
#ifdef USE_MMC0
#include <mmc.h>
#else
#include <dev/scsi.h>
#endif
#include <lib/bio.h>
#include <malloc.h>
#include <lib/console.h>
#include <dev/rpmb.h>
#include <platform/sfr.h>
#include <platform/smc.h>

#define SECU_PROT_IN    0
#define SECU_PROT_OUT   1

#define RPMB_BLOCK_SIZE         256
#define BOOT_RI_PARTITION       2  // Rollback Index Partiition number
#define BOOT_RI_TABLE_BLOCK     1  // Rollback Index block base
#define BOOT_RI_TABLE_BLOCK_CNT 2  // Rollback Index block count
#define BOOT_RI_TABLE_SIZE      ((RPMB_BLOCK_SIZE * BOOT_RI_TABLE_BLOCK_CNT) / sizeof(uint64_t))
#define BOOT_MAGIC              0x424F4F54  /* "BOOT" */

#define PERSIST_DATA_BLOCK      5
#define PERSIST_DATA_BLOCK_CNT  2
#define PERSIST_KEY_LEN         56
#define PERSIST_VALUE_LEN       64
#define PERSIST_DATA_LEN        128
#define PERSIST_DATA_CNT        ((RPMB_BLOCK_SIZE * PERSIST_DATA_BLOCK_CNT) / PERSIST_DATA_LEN)

#define INT2VOIDP(x)            (void *)(uintptr_t)(x)
#define INT2U8P(x)              (u8 *)(uintptr_t)(x)

struct boot_header {
	u32     magic;
	u32     ri_block_base;
	u32     ri_block_cnt;
};

struct persist_data {
	u8      key[PERSIST_KEY_LEN];
	u32     len;
	u8      resv[128-PERSIST_KEY_LEN-PERSIST_VALUE_LEN-4];
	u8      value[PERSIST_VALUE_LEN];
};

#define HEADER_SIZE (sizeof(struct boot_header))

struct header_block {
	struct boot_header header;
	char reserved[RPMB_BLOCK_SIZE - (HEADER_SIZE + 4)];
	uint32_t lock;
};

static uint32_t lock_state;
uint64_t rollbackIndex[BOOT_RI_TABLE_SIZE];
uint32_t table_init_state;
struct boot_header bootHeader;
struct persist_data persistentData[PERSIST_DATA_CNT];
static u8 nonce[NONCE_SIZE];

static void dump_packet(u8 * data, u32 len)
{
	u8 s[17];
	u32 i, j;
	s[16] = '\0';
	for (i = 0; i < len; i += 16) {
		printf("%06X :", i);
		for (j = 0; j < 16; j++) {
			printf(" %02X", data[i + j]);
			s[j] = (data[i + j] < ' ' ? '.' : (data[i + j] > '}' ? '.' : data[i + j]));
		}
		printf(" |%s|\n", s);
	}
	printf("\n");
}

static void swap_packet(u8 * p, u8 * d)
{
	int i;

	for (i = 0; i < 512; i++)
		d[i] = p[511 - i];
}

/* print byte string to hexa values */
static void print_byte_to_hex(const uint8_t * byte_array, size_t byte_length)
{
	size_t i;

	printf("\n");
	for (i = 0; i < byte_length; i++) {
		if ((0 != i) && (0 == (i % 16)))
			printf(" \n");
		printf(" %02X", byte_array[i]);
	}
	printf("\n");
}

/*
 * function to get RPMB raw keys from the REK-based KDF in CryptoManager.
 * @ key_len: RPMB key length in byte (this values is fixed to 32 for now)
 * @ rpmb_key: Pointer of the output buffer to store the RPMB raw key
 */
uint32_t get_RPMB_key(size_t key_len, uint8_t * rpmb_key)
{
	uint64_t r0 = 0;
	uint64_t r1 = 0;
	uint64_t r2 = 0;
	uint64_t r3 = 0;
	uint32_t retry_cnt = 0;
	uint32_t ret = RV_SUCCESS;

#ifdef CACHE_ENABLED
	CACHE_INVALIDATE(rpmb_key, key_len);
#endif
	do {
		if (++retry_cnt > MAX_SMC_RETRY_CNT) {
			ret = RV_BOOT_RPMB_EXCEED_SMC_RETRY_CNT;
			printf("[CM] RPMB: exceed maximum SMC retry count\n");
			return ret;
		}

		r0 = SMC_AARCH64_PREFIX | SMC_CM_RPMB;
		r1 = RPMB_GET_KEY;
		r2 = RPMB_KEY_LEN;
		r3 = (uint64_t)rpmb_key;

		ret = exynos_smc(r0, r1, r2, r3);

	} while (ret == RV_SYNC_AES_BUSY);

	if (ret != RV_SUCCESS) {
		printf("[CM] RPMB: error in CryptoManager F/W: 0x%X\n", ret);
		return ret;
	}
#ifdef CACHE_ENABLED
	CACHE_INVALIDATE(rpmb_key, key_len);
#endif

#ifdef RPMB_DEBUG
	dprintf(INFO, "[CM] RPMB: key was derived successfully\n");
#endif
	return ret;
}

/*
 * function to block get_PRMB_key()
 * this function can be used when the caller needs to block use of get_RPMB_key()
 */
uint32_t block_RPMB_key(void)
{
	uint64_t r0 = 0;
	uint64_t r1 = 0;
	uint64_t r2 = 0;
	uint64_t r3 = 0;
	uint32_t ret = RV_SUCCESS;

	r0 = SMC_AARCH64_PREFIX | SMC_CM_RPMB;
	r1 = RPMB_BLOCK_KEY;

	ret = exynos_smc(r0, r1, r2, r3);

	if (ret != RV_SUCCESS) {
		printf("[CM] RPMB: failed to block key: 0x%X\n", ret);
		return ret;
	}
#ifdef RPMB_DEBUG
	dprintf(INFO, "[CM] RPMB: key was blocked successfully\n");
#endif
	return ret;
}

/*
 * function to get hmac values from CryptoManager F/W with RPMB frame data.
 * @ input_data: pointer of the Input buffer storing RPMB frame data
 * @ input_len: input length in bytes (Maximum is 320)
 * @ output_data: pointer of the output buffer to store hmac-sha256 value
 */
uint32_t get_RPMB_hmac(const uint8_t * input_data, size_t input_len, uint8_t * output_data)
{
	uint64_t r0 = 0;
	uint64_t r1 = 0;
	uint64_t r2 = 0;
	uint64_t r3 = 0;
	uint32_t retry_cnt = 0;
	uint32_t ret = RV_SUCCESS;

	rpmb_param rpmb_data;

	rpmb_data.input_data = (uint64_t)input_data;
	rpmb_data.input_len = input_len;
	rpmb_data.output_data = (uint64_t)output_data;

#ifdef CACHE_ENABLED
	CACHE_CLEAN(&rpmb_data, sizeof(rpmb_data));
	CACHE_CLEAN(input_data, input_len);
	CACHE_INVALIDATE(output_data, RPMB_HMAC_LEN);
#endif

	/* call REK-based KBKDF in CryptoManager F/W with smc */
	do {
		if (++retry_cnt > MAX_SMC_RETRY_CNT) {
			ret = RV_BOOT_RPMB_EXCEED_SMC_RETRY_CNT;
			printf("[CM] RPMB: exceed maximum SMC retry count\n");
			return ret;
		}

		r0 = SMC_AARCH64_PREFIX | SMC_CM_RPMB;
		r1 = RPMB_GET_HMAC;
		r2 = (uint64_t)&rpmb_data;

		ret = exynos_smc(r0, r1, r2, r3);

	} while (ret == RV_SYNC_AES_BUSY);

	if (ret != RV_SUCCESS) {
		printf("[CM] RPMB: error in CryptoManager F/W: 0x%X\n", ret);
		return ret;
	}
#ifdef CACHE_ENABLED
	CACHE_INVALIDATE(output_data, RPMB_HMAC_LEN);
#endif
#ifdef RPMB_DEBUG
	dprintf(INFO, "[CM] RPMB: hmac calculation was finished successfully\n");
#endif
	return ret;
}

/*
 * function to block get_PRMB_hmac()
 * this function can be used when the caller needs to block use of get_RPMB_hmac()
 */
uint32_t block_RPMB_hmac(void)
{
	uint64_t r0 = 0;
	uint64_t r1 = 0;
	uint64_t r2 = 0;
	uint64_t r3 = 0;
	uint32_t ret = RV_SUCCESS;

	r0 = SMC_AARCH64_PREFIX | SMC_CM_RPMB;
	r1 = RPMB_BLOCK_HMAC;

	ret = exynos_smc(r0, r1, r2, r3);

	if (ret != RV_SUCCESS) {
		printf("[CM] RPMB: failed to block hmac: 0x%X\n", ret);
		return ret;
	}
#ifdef RPMB_DEBUG
	dprintf(INFO, "[CM] RPMB: hmac was blocked successfully\n");
#endif
	return ret;
}

uint32_t set_RPMB_provision(uint64_t state)
{
	uint64_t r0 = 0;
	uint64_t r1 = 0;
	uint64_t r2 = 0;
	uint64_t r3 = 0;
	uint32_t ret = RV_SUCCESS;

	r0 = SMC_AARCH64_PREFIX | SMC_SRPMB_PROVISION;
	r1 = state == 0?0:1;

	ret = exynos_smc(r0, r1, r2, r3);

	if (ret != RV_SUCCESS) {
		printf("RPMB: failed to set provision state: 0x%X\n", ret);
		return ret;
	}
#ifdef RPMB_DEBUG
	dprintf(INFO, "RPMB: successfully set provision state\n");
#endif
	return ret;
}

/* example function to test check above functions work correctly */
int do_rpmb_test(int argc, char *argv[])
{
	uint8_t rpmb_key[RPMB_KEY_LEN];
	uint32_t ret = RV_SUCCESS;

	/* expected Key in case of test devices */
	const uint8_t test_key[] = {
		0xA8, 0xF6, 0xFB, 0x7F, 0x4B, 0x04, 0x4E, 0xC5, 0xBB, 0xD4,
		0x67, 0x31, 0x3D, 0x02, 0x52, 0x9A, 0x86, 0x5B, 0x48, 0xDB,
		0x97, 0x94, 0xA5, 0x72, 0x48, 0xA4, 0xE7, 0x99, 0x81, 0xC3,
		0x67, 0x80
	};

	/* expected MAC in case of test devices */
	const uint8_t test_mac[] = {
		0x38, 0x39, 0xE8, 0xD0, 0x66, 0xE2, 0x00, 0x0E, 0x0B, 0x17,
		0xA0, 0x47, 0x23, 0xF0, 0x9B, 0x67, 0xA2, 0xC8, 0xB9, 0x77,
		0xF1, 0xB2, 0x9F, 0x4A, 0x6F, 0x8B, 0x04, 0xA3, 0xFB, 0x4D,
		0x01, 0x3E
	};

	/* test input vector got from FIPS PUB 198-1 */
	const uint8_t input_data[34] = { "Sample message for keylen<blocklen" };

	uint8_t output_data[32];

	dprintf(INFO, "[CM] RPMB: reference output for test devices:\n");
	dprintf(INFO, "[CM] RPMB: Key:");
	print_byte_to_hex(test_key, RPMB_HMAC_LEN);
	dprintf(INFO, "[CM] RPMB: MAC:");
	print_byte_to_hex(test_mac, RPMB_HMAC_LEN);
	dprintf(INFO, "\n");

	/* Test 1: RPMB key derivation */
	dprintf(INFO, "[CM] RPMB: Test1:: key derivation\n");

	ret = get_RPMB_key(RPMB_KEY_LEN, rpmb_key);
	if (ret != RV_SUCCESS) {
		dprintf(INFO, "[CM] RPMB: key derivation: fail: 0x%X\n", ret);
		return ret;
	}
	dprintf(INFO, "[CM] RPMB: key derivation: success\n");
	dprintf(INFO, "[CM] RPMB: key: ");
	print_byte_to_hex(rpmb_key, RPMB_KEY_LEN);
	dprintf(INFO, "\n");

	/* Test 2: RPMB key blocking */
	dprintf(INFO, "[CM] RPMB: Test2:: key blocking\n");

	ret = block_RPMB_key();
	if (ret != RV_SUCCESS) {
		dprintf(INFO, "[CM] RPMB: key blocking: fail: 0x%X\n", ret);
		return ret;
	}
	dprintf(INFO, "[CM] RPMB: key blocking: success\n\n");

	/* Test 3: RPMB key derivation after key blocking */
	dprintf(INFO, "[CM] RPMB: Test3:: key derivation after key blocking\n");

	ret = get_RPMB_key(RPMB_KEY_LEN, rpmb_key);
	if (ret == RV_RPMB_KEY_BLOCKED)
		dprintf(INFO, "[CM] RPMB: key was blocked already: 0x%X\n\n", ret);
	else
		dprintf(INFO, "[CM] RPMB: key wasn't blocked correctly: 0x%X\n\n", ret);

	/* Test 4: RPMB get hamc with input data */
	dprintf(INFO, "[CM] RPMB: Test4:: get hmac value\n");

	memset(output_data, 0, sizeof(output_data));
	ret = get_RPMB_hmac(input_data, sizeof(input_data), output_data);
	if (ret != RV_SUCCESS) {
		dprintf(INFO, "[CM] RPMB: get hamc value: fail: 0x%X\n", ret);
		return ret;
	}
	dprintf(INFO, "[CM] RPMB: get hmac value: success\n");
	dprintf(INFO, "[CM] RPMB: HMAC: ");
	print_byte_to_hex(output_data, RPMB_HMAC_LEN);
	dprintf(INFO, "\n");

	/* Test 5: RPMB hmac blocking */
	dprintf(INFO, "[CM] RPMB: Test5:: hmac blocking\n");

	ret = block_RPMB_hmac();
	if (ret != RV_SUCCESS) {
		dprintf(INFO, "[CM] RPMB: hmac blocking: fail: 0x%X\n", ret);
		return ret;
	}
	dprintf(INFO, "[CM] RPMB: hmac blocking: success\n\n");

	/* Test 6: RPMB hmac after hmac blocking */
	dprintf(INFO, "[CM] RPMB: Test6:: get hmac after hmac blocking\n");

	memset(output_data, 0, sizeof(output_data));
	ret = get_RPMB_hmac(input_data, sizeof(input_data), output_data);

	if (ret == RV_RPMB_HMAC_BLOCKED)
		dprintf(INFO, "[CM] RPMB: hmac was blocked already: 0x%X\n", ret);
	else
		dprintf(INFO, "[CM] RPMB: hmac wasn't blocked correctly: 0x%X\n", ret);

	return ret;
}

#ifdef USE_MMC0
static void mmc_report(struct rpmb_packet *packet, int sp_in_out)
{
	char in_out[25];

	if (sp_in_out)
		strcpy(in_out,"SECURITY PROTOCOL OUT");
	else
		strcpy(in_out,"SECURITY PROTOCOL IN");

	printf("RPMB: Auth. %s counter %d\n", in_out, packet->write_counter);
	printf("RPMB: Auth. %s address %x\n", in_out, packet->address);
	printf("RPMB: Auth. %s block count %d\n", in_out, packet->count);
	printf("RPMB: Auth. %s result %d\n", in_out, packet->result);
	printf("RPMB: Auth. %s response %x\n", in_out, packet->request);
}

static int emmc_rpmb_commands(struct rpmb_packet *packet)
{
	u32 i;
	int result = -1;
	u32 addr, start_blk, blk_cnt;
	u32 *addrp = NULL;
	uint8_t buf[RPMB_SIZE] __attribute__((__aligned__(CACHE_WRITEBACK_GRANULE_128)));
	uint8_t hmac[HMAC_CALC_SIZE_128_GRANULE] __attribute__((__aligned__(CACHE_WRITEBACK_GRANULE_128)));
	uint8_t output_data[CACHE_WRITEBACK_GRANULE_128] __attribute__((__aligned__(CACHE_WRITEBACK_GRANULE_128)));

	uint32_t ret = RV_SUCCESS;
	ssize_t cnt;
	bdev_t *dev;

	switch (packet->request) {
	case	1:
		/* Authentication key programming request */
#ifdef RPMB_DEBUG
		dprintf(INFO, "Write authentication KEY\n");
		dump_packet(packet->Key_MAC, HMAC_SIZE);
#endif

		memset(buf, 0, RPMB_SIZE);
		swap_packet((u8 *)packet, buf);

		/* Key restore */
		memcpy((void *)(buf + HMAC_START_BYTE), packet->Key_MAC, HMAC_SIZE);

#ifdef RPMB_DEBUG
		dprintf(INFO, "Authentication key programming request Packet\n");
		dump_packet((u8 *) packet, RPMB_SIZE);
		dprintf(INFO, "Authentication key programming request Packet (Swapped)\n");
		dump_packet(buf, RPMB_SIZE);
#endif

		dev = bio_open("mmcrpmb");
		if (dev == NULL) {
			printf("bio open fail\n");
			ret = -1;
			goto out;
		}

		dev->flags = BIO_FLAG_RELIABLE_WRITE;
		cnt = dev->new_write(dev, (void *)buf, 0, 1);
		if (cnt == 0) {
			printf("Authentication write command fail\n");
			bio_close(dev);
			ret = -1;
			goto out;
		}

#ifdef RPMB_DEBUG
		mmc_report(packet, SECU_PROT_OUT);
#endif

		memset((void *)packet, 0, RPMB_SIZE);
		packet->request = 0x05;
		swap_packet((u8 *) packet, buf);

#ifdef RPMB_DEBUG
		dprintf(INFO, "Result read request\n");
		dump_packet((u8 *) packet, RPMB_SIZE);
		dprintf(INFO, "Result read request (Swapped)\n");
		dump_packet(buf, RPMB_SIZE);
#endif

		dev->flags = 0;
		cnt = dev->new_write(dev, (void *)buf, 0, 1);
		if (cnt == 0) {
			printf("Result read request fail !!!\n");
			bio_close(dev);
			ret = -1;
			goto out;
		}
#ifdef RPMB_DEBUG
		mmc_report(packet, SECU_PROT_OUT);
#endif
		memset(buf, 0, RPMB_SIZE);
		dev->flags = 0;
		cnt = dev->new_read(dev, (void *)buf, 0, 1);
		if (cnt == 0) {
			printf("Result read fail !!!\n");
			bio_close(dev);
			ret = -1;
			goto out;
		}

		swap_packet(buf, (u8 *) packet);

#ifdef RPMB_DEBUG
		dprintf(INFO, "Authentication key programming response\n");
		dump_packet(buf, RPMB_SIZE);
		dprintf(INFO, "Authentication key programming response (Swapped)\n");
		dump_packet((u8 *) packet, RPMB_SIZE);
		mmc_report(packet, SECU_PROT_IN);
#endif

		bio_close(dev);
		break;

	case	2:
		/* Reading of the Write Counter value request */
		memset(buf, 0, RPMB_SIZE);
		memset(hmac, 0, sizeof(hmac));
		swap_packet((u8 *) packet, buf);

#ifdef RPMB_DEBUG
		dprintf(INFO, "Reading of the Write Counter value request\n");
		dump_packet((u8 *) packet, RPMB_SIZE);
		dprintf(INFO, "Reading of the Write Counter value request (Swapped)\n");
		dump_packet(buf, RPMB_SIZE);
#endif
		dev = bio_open("mmcrpmb");
		if (dev == NULL) {
			printf("bio open fail\n");
			ret = -1;
			goto out;
		}

		dev->flags = 0;
		cnt = dev->new_write(dev, (void *)buf, 0, 1);
		if (cnt == 0) {
			printf("Write counter read request fail !!!\n");
			bio_close(dev);
			ret = -1;
			goto out;
		}

#ifdef RPMB_DEBUG
		mmc_report(packet, SECU_PROT_OUT);
#endif
		memset((void *)buf, 0, RPMB_SIZE);

		dev->flags = 0;
		cnt = dev->new_read(dev, (void *)buf, 0, 1);
		if (cnt == 0) {
			printf("Write counter read fail !!!\n");
			bio_close(dev);
			ret = -1;
			goto out;
		}

		swap_packet(buf, (u8 *) packet);

#ifdef RPMB_DEBUG
		dprintf(INFO, "Reading of the Write Counter value response\n");
		dump_packet(buf, RPMB_SIZE);
		dprintf(INFO, "Reading of the Write Counter value response (Swapped)\n");
		dump_packet((u8 *) packet, RPMB_SIZE);
		dprintf(INFO, "RPMB: MAC\n");
		dump_packet(packet->Key_MAC, HMAC_SIZE);
		mmc_report(packet, SECU_PROT_IN);
#endif
		bio_close(dev);

		break;

	case	3:
		/* Authenticated data write request */
		addrp = (u32 *)packet->data;
		addr = *addrp;
		blk_cnt = packet->count;
		start_blk = packet->address;
		memset(buf, 0, RPMB_SIZE);
		memset(hmac, 0, sizeof(hmac));

#ifdef RPMB_DEBUG
		dprintf(INFO, "Authenticated data write request (Data only)\n");
		dump_packet( INT2U8P(addr), DATA_SIZE * blk_cnt);
		dprintf(INFO, "Authenticated data write request (Swapped)\n");
		dprintf(INFO, "HMAC calculatation\n");
#endif
		memset(output_data, 0, sizeof(output_data));

		/* Write Data reordering */
		for (i = 0; i < blk_cnt; i++) {
			/* Copy to packet data from buffer */
			memcpy(packet->data, INT2VOIDP(addr + (i * DATA_SIZE)), DATA_SIZE);

			/* Swap Copy to buffer from packet */
			swap_packet((u8 *) packet, (u8 *) (buf + (i * RPMB_SIZE)));

			/* Multiple block Data merge for hmac calculation. */
			memcpy((void *)(hmac + (i * HMAC_CALC_SIZE)),
			       (void *)(buf + DATA_START_BYTE + (i * RPMB_SIZE)), HMAC_CALC_SIZE);
		}

		/* hmac calculation include all block data. */
		ret = get_RPMB_hmac(hmac, blk_cnt * HMAC_CALC_SIZE, output_data);
		if (ret != RV_SUCCESS)
			printf("[CM] RPMB: get hamc value: fail: 0x%X\n", ret);
		else {
#ifdef RPMB_DEBUG
			dprintf(INFO, "[CM] RPMB: get hmac value: success\n");
			dprintf(INFO, "[CM] RPMB: HMAC: ");
			print_byte_to_hex(output_data, RPMB_HMAC_LEN);
			dprintf(INFO, "\n");
#endif
		}

		/* Write hmac to last block */
		memcpy((void *)(buf + HMAC_START_BYTE + (blk_cnt - 1) * RPMB_SIZE),
			(void *)(output_data), HMAC_SIZE);

#ifdef RPMB_DEBUG
		dprintf(INFO, "Authenticated data write request (with HMAC)\n");
		dump_packet((u8 *) (buf + (blk_cnt - 1) * RPMB_SIZE), RPMB_SIZE);
#endif

#ifdef RPMB_DEBUG
		dprintf(INFO, "Authenticated data write request (Swapped & HMAC included)\n");
		dump_packet((u8 *) (buf + ((blk_cnt - 1) * RPMB_SIZE)), RPMB_SIZE);
		/* HMAC calculation here */
		dprintf(INFO, "Send authenticated data write request\n");
#endif

		dev = bio_open("mmcrpmb");
		if (dev == NULL) {
			printf("bio open fail\n");
			ret = -1;
			goto out;
		}

		dev->flags = BIO_FLAG_RELIABLE_WRITE;
		cnt = dev->new_write(dev, (void *)buf, start_blk, blk_cnt);
		if (cnt == 0) {
			printf("RPMB: Write fail !!!\n");
			bio_close(dev);
			ret = -1;
			goto out;
		}

#ifdef RPMB_DEBUG
		mmc_report(packet, SECU_PROT_OUT);
#endif
		memset((void *)packet, 0, RPMB_SIZE);
		packet->request = 0x05;
		swap_packet((u8 *) packet, buf);

#ifdef RPMB_DEBUG
		dprintf(INFO, "Result write request\n");
		dump_packet((u8 *) packet, RPMB_SIZE);
		dprintf(INFO, "Result write request (Swapped)\n");
		dump_packet((u8 *) buf, RPMB_SIZE);
#endif

		dev->flags = 0;
		cnt = dev->new_write(dev, (void *)buf, 0, 1);
		if (cnt == 0) {
			printf("RPMB: Request read result fail !!!\n");
			bio_close(dev);
			ret = -1;
			goto out;
		}

#ifdef RPMB_DEBUG
		mmc_report(packet, SECU_PROT_OUT);
#endif
		memset((void *)buf, 0, RPMB_SIZE);

		dev->flags = 0;
		cnt = dev->new_read(dev, (void *)buf, 0, 1);
		if (cnt == 0) {
			printf("RPMB: Read result fail !!!\n");
			bio_close(dev);
			ret = -1;
			goto out;
		}

		swap_packet(buf, (u8 *) packet);

#ifdef RPMB_DEBUG
		dprintf(INFO, "Authenticated data write response\n");
		dump_packet(buf, RPMB_SIZE);
		dprintf(INFO, "Authenticated data write response (Swapped)\n");
		dump_packet((u8 *) packet, RPMB_SIZE);
		mmc_report(packet, SECU_PROT_IN);
		dprintf(INFO, "RPMB: MAC\n");
		dump_packet(packet->Key_MAC, HMAC_SIZE);
#endif
		bio_close(dev);
		break;

	case	4:
		/* Authenticated data read request */
		addrp = (u32 *)(packet->data);
		addr = *addrp;
		blk_cnt = *((u32 *)(packet->data)+1);
		start_blk = packet->address;
		*addrp = 0;
		*(addrp+1) = 0;

		memset(buf, 0, RPMB_SIZE);
		memset(hmac, 0, sizeof(hmac));
		swap_packet((u8 *) packet, buf);

#ifdef RPMB_DEBUG
		dprintf(INFO, "Authenticated data read request\n");
		dump_packet((u8 *) packet, RPMB_SIZE);
		dprintf(INFO, "Authenticated data read request (Swapped)\n");
		dump_packet(buf, RPMB_SIZE);
#endif

		dev = bio_open("mmcrpmb");
		if (dev == NULL) {
			printf("bio open fail\n");
			ret = -1;
			goto out;
		}

		dev->flags = 0;
		cnt = dev->new_write(dev, (void *)buf, 0, 1);
		if (cnt == 0) {
			printf("RPMB: Request read data fail !!!\n");
			bio_close(dev);
			ret = -1;
			goto out;
		}

#ifdef RPMB_DEBUG
		mmc_report(packet, SECU_PROT_OUT);
#endif
		memset((void *)buf, 0, RPMB_SIZE * blk_cnt);

		dev->flags = 0;
		cnt = dev->new_read(dev, (void *)buf, start_blk, blk_cnt);
		if (cnt == 0) {
			printf("RPMB: Read data fail !!!\n");
			bio_close(dev);
			ret = -1;
			goto out;
		}

#ifdef RPMB_DEBUG
		dprintf(INFO, "Authenticated data read response (Swapped)\n");
		dprintf(INFO, "HMAC calculatation\n");
#endif
		memset(output_data, 0, sizeof(output_data));


		/* Read hmac Data reordering */
		for (i = 0; i < blk_cnt; i++) {
			memcpy((void *)(hmac + (i * HMAC_CALC_SIZE)),
			       (void *)(buf + DATA_START_BYTE + (i * RPMB_SIZE)), HMAC_CALC_SIZE);
		}

		ret = get_RPMB_hmac(hmac, blk_cnt * HMAC_CALC_SIZE, output_data);
		if (ret != RV_SUCCESS)
			printf("RPMB: get hamc value: fail: 0x%X\n", ret);
		result = memcmp((void *)(output_data),
				(void *)(buf + HMAC_START_BYTE + ((blk_cnt - 1) * RPMB_SIZE)),
				HMAC_SIZE);

		/* Read Data reordering */
		for (i = 0; i < blk_cnt; i++) {
			swap_packet((u8 *) (buf + (i * RPMB_SIZE)), (u8 *) packet);
			memcpy(INT2VOIDP(addr + (i * DATA_SIZE)), packet->data, DATA_SIZE);
		}

		if (result != 0) {
			printf("HMAC compare fail !!\n");
			printf("HMAC Host value\n");
			dump_packet(output_data, HMAC_SIZE);

			printf("HMAC Device value blk_cnt %d i %d\n", blk_cnt, i);
			dump_packet((void *)(buf + HMAC_START_BYTE + ((blk_cnt - 1) * RPMB_SIZE)),
					HMAC_SIZE);

			printf("Authenticated data read response (Not Swapped)\n");
			dump_packet((void *)(buf + (blk_cnt - 1) * RPMB_SIZE), RPMB_SIZE);
			printf("Authenticated data read response (Swapped)\n");
			dump_packet((u8 *) packet, RPMB_SIZE);
		} else {
#ifdef RPMB_DEBUG
			dprintf(INFO, "HMAC compare success !!\n");
			dprintf(INFO, "RPMB: HMAC: ");
			print_byte_to_hex(output_data, RPMB_HMAC_LEN);
			dprintf(INFO, "\n");
			dprintf(INFO, "Authenticated data read response (Not Swapped)\n");
			dump_packet((void *)(buf + ((blk_cnt - 1) * RPMB_SIZE)), RPMB_SIZE);
			dprintf(INFO, "Authenticated data read response (Swapped)\n");
			dump_packet((u8 *) packet, RPMB_SIZE);
#endif
		}

#ifdef RPMB_DEBUG
		mmc_report(packet, SECU_PROT_IN);
		dprintf(INFO, "RPMB: MAC\n");
		dump_packet(packet->Key_MAC, HMAC_SIZE);
#endif
		bio_close(dev);
		break;
	}

out:
	return ret;
}

#else
static void ufs_upiu_report(struct rpmb_packet *packet, int sp_in_out)
{
	char in_out[25];

	if (sp_in_out)
		strcpy(in_out,"SECURITY PROTOCOL OUT");
	else
		strcpy(in_out,"SECURITY PROTOCOL IN");

	printf("RPMB: Auth. %s counter %d\n", in_out, packet->write_counter);
	printf("RPMB: Auth. %s address %x\n", in_out, packet->address);
	printf("RPMB: Auth. %s block count %d\n", in_out, packet->count);
	printf("RPMB: Auth. %s result %d\n", in_out, packet->result);
	printf("RPMB: Auth. %s response %x\n", in_out, packet->request);
}

static int ufs_rpmb_commands(struct rpmb_packet *packet)
{
	u8 *buf = NULL;
	u8 *hmac = NULL;
	u32 i;
	int result = -1;
	u32 addr, start_blk, blk_cnt;
	u32 *addrp = NULL;
	uint8_t output_data[CACHE_WRITEBACK_GRANULE_128] __attribute__((__aligned__(CACHE_WRITEBACK_GRANULE_128)));
	uint32_t ret = RV_SUCCESS;
	ssize_t cnt;
	bdev_t *dev;

	switch (packet->request) {
	case 1:
		/* Authentication key programming request */
#ifdef RPMB_DEBUG
		dprintf(INFO, "Write authentication KEY\n");
		dump_packet(packet->Key_MAC, HMAC_SIZE);
#endif
		buf = malloc(RPMB_SIZE);
		swap_packet((u8 *) packet, buf);

		/* Key restore */
		memcpy((void *)(buf + HMAC_START_BYTE), packet->Key_MAC, HMAC_SIZE);

#ifdef RPMB_DEBUG
		dprintf(INFO, "Authentication key programming request Packet\n");
		dump_packet((u8 *) packet, RPMB_SIZE);
		dprintf(INFO, "Authentication key programming request Packet (Swapped)\n");
		dump_packet(buf, RPMB_SIZE);
#endif
		dev = bio_open("scsirpmb");
		if (dev == NULL) {
			printf("bio open fail\n");
			ret = -1;
			goto out;
		}

		/* Send UPIU */
		cnt = dev->new_write(dev, (void *)buf, 0, 1);
		if (cnt == 0) {
			printf("Authentication write command fail\n");
			bio_close(dev);
			ret = -1;
			goto out;
		}

#ifdef RPMB_DEBUG
		ufs_upiu_report(packet, SECU_PROT_OUT);
#endif
		memset((void *)packet, 0, RPMB_SIZE);
		packet->request = 0x05;
		swap_packet((u8 *) packet, buf);

#ifdef RPMB_DEBUG
		dprintf(INFO, "Result read request\n");
		dump_packet((u8 *) packet, RPMB_SIZE);
		dprintf(INFO, "Result read request (Swapped)\n");
		dump_packet(buf, RPMB_SIZE);
#endif

		/* Send UPIU */
		cnt = dev->new_write(dev, (void *)buf, 0, 1);
		if (cnt == 0) {
			printf("Result read request fail !!!\n");
			bio_close(dev);
			ret = -1;
			goto out;
		}
#ifdef RPMB_DEBUG
		ufs_upiu_report(packet, SECU_PROT_OUT);
#endif
		memset((void *)buf, 0, RPMB_SIZE);

		/* Send UPIU */
		cnt = dev->new_read(dev, (void *)buf, 0, 1);
		if (cnt == 0) {
			printf("Result read fail !!!\n");
			bio_close(dev);
			ret = -1;
			goto out;
		}

		swap_packet(buf, (u8 *) packet);

#ifdef RPMB_DEBUG
		dprintf(INFO, "Authentication key programming response\n");
		dump_packet(buf, RPMB_SIZE);
		dprintf(INFO, "Authentication key programming response (Swapped)\n");
		dump_packet((u8 *) packet, RPMB_SIZE);
		ufs_upiu_report(packet, SECU_PROT_IN);
#endif

		bio_close(dev);
		break;

	case 2:
		/* Reading of the Write Counter value request */
		buf = malloc(RPMB_SIZE);
		hmac = malloc(HMAC_CALC_SIZE);

		swap_packet((u8 *) packet, buf);

#ifdef RPMB_DEBUG
		dprintf(INFO, "Reading of the Write Counter value request\n");
		dump_packet((u8 *) packet, RPMB_SIZE);
		dprintf(INFO, "Reading of the Write Counter value request (Swapped)\n");
		dump_packet(buf, RPMB_SIZE);
#endif

		dev = bio_open("scsirpmb");
		if (dev == NULL) {
			printf("bio open fail\n");
			free(hmac);
			ret = -1;
			goto out;
		}

		/* Send UPIU */
		cnt = dev->new_write(dev, (void *)buf, 0, 1);
		if (cnt == 0) {
			printf("Write counter read request fail !!!\n");
			bio_close(dev);
			free(hmac);
			ret = -1;
			goto out;
		}

#ifdef RPMB_DEBUG
		ufs_upiu_report(packet, SECU_PROT_OUT);
#endif
		memset((void *)buf, 0, RPMB_SIZE);

		/* Send UPIU */
		cnt = dev->new_read(dev, (void *)buf, 0, 1);
		if (cnt == 0) {
			printf("Write counter read fail !!!\n");
			bio_close(dev);
			free(hmac);
			ret = -1;
			goto out;
		}

		swap_packet(buf, (u8 *) packet);

#ifdef RPMB_DEBUG
		dprintf(INFO, "Reading of the Write Counter value response\n");
		dump_packet(buf, RPMB_SIZE);
		dprintf(INFO, "Reading of the Write Counter value response (Swapped)\n");
		dump_packet((u8 *) packet, RPMB_SIZE);
		dprintf(INFO, "RPMB: MAC\n");
		dump_packet(packet->Key_MAC, HMAC_SIZE);
		ufs_upiu_report(packet, SECU_PROT_IN);
#endif

		free(hmac);
		bio_close(dev);
		break;

	case 3:
		/* Authenticated data write request */
		addrp = (u32 *)packet->data;
		addr = *addrp;
		blk_cnt = packet->count;
		start_blk = packet->address;
		buf = malloc(RPMB_SIZE * blk_cnt);
		hmac = malloc(HMAC_CALC_SIZE * blk_cnt);

#ifdef RPMB_DEBUG
		dprintf(INFO, "Authenticated data write request (Data only)\n");
		dump_packet( INT2U8P(addr), DATA_SIZE * blk_cnt);
		dprintf(INFO, "Authenticated data write request (Swapped)\n");
		dprintf(INFO, "HMAC calculatation\n");
#endif
		memset(output_data, 0, sizeof(output_data));

		/* Write Data reordering */
		for (i = 0; i < blk_cnt; i++) {
			/* Copy to packet data from buffer */
			memcpy(packet->data, INT2VOIDP(addr + (i * DATA_SIZE)), DATA_SIZE);

			/* Swap Copy to buffer from packet */
			swap_packet((u8 *) packet, (u8 *) (buf + (i * RPMB_SIZE)));

			/* Multiple block Data merge for hmac calculation. */
			memcpy((void *)(hmac + (i * HMAC_CALC_SIZE)),
			       (void *)(buf + DATA_START_BYTE + (i * RPMB_SIZE)), HMAC_CALC_SIZE);
		}

		/* hmac calculation include all block data. */
		ret = get_RPMB_hmac(hmac, blk_cnt * HMAC_CALC_SIZE, output_data);

		/* Write hmac to last block */
		memcpy((void *)(buf + HMAC_START_BYTE + (blk_cnt - 1) * RPMB_SIZE),
			(void *)(output_data), HMAC_SIZE);

#ifdef RPMB_DEBUG
		dprintf(INFO, "Authenticated data write request (with HMAC)\n");
		dump_packet((u8 *) (buf + (blk_cnt - 1) * RPMB_SIZE), RPMB_SIZE);
#endif

		if (ret != RV_SUCCESS)
			printf("[CM] RPMB: get hamc value: fail: 0x%X\n", ret);
		else {
#ifdef RPMB_DEBUG
			dprintf(INFO, "[CM] RPMB: get hmac value: success\n");
			dprintf(INFO, "[CM] RPMB: HMAC: ");
			print_byte_to_hex(output_data, RPMB_HMAC_LEN);
			dprintf(INFO, "\n");
#endif
		}

#ifdef RPMB_DEBUG
		dprintf(INFO, "Authenticated data write request (Swapped & HMAC included)\n");
		dump_packet((u8 *) (buf + ((blk_cnt - 1) * RPMB_SIZE)), RPMB_SIZE);
		/* HMAC calculation here */
		dprintf(INFO, "Send authenticated data write request\n");
#endif

		dev = bio_open("scsirpmb");
		if (dev == NULL) {
			printf("bio open fail\n");
			free(hmac);
			ret = -1;
			goto out;
		}

		/* Send UPIU */
		cnt = dev->new_write(dev, (void *)buf, start_blk, blk_cnt);
		if (cnt == 0) {
			printf("RPMB: Write fail !!!\n");
			bio_close(dev);
			free(hmac);
			ret = -1;
			goto out;
		}

#ifdef RPMB_DEBUG
		ufs_upiu_report(packet, SECU_PROT_OUT);
#endif
		memset((void *)packet, 0, RPMB_SIZE);
		packet->request = 0x05;
		swap_packet((u8 *) packet, buf);

#ifdef RPMB_DEBUG
		dprintf(INFO, "Result write request\n");
		dump_packet((u8 *) packet, RPMB_SIZE);
		dprintf(INFO, "Result write request (Swapped)\n");
		dump_packet((u8 *) buf, RPMB_SIZE);
#endif

		/* Send UPIU */
		cnt = dev->new_write(dev, (void *)buf, 0, 1);
		if (cnt == 0) {
			printf("RPMB: Request read result fail !!!\n");
			bio_close(dev);
			free(hmac);
			ret = -1;
			goto out;
		}

#ifdef RPMB_DEBUG
		ufs_upiu_report(packet, SECU_PROT_OUT);
#endif
		memset((void *)buf, 0, RPMB_SIZE);

		/* Send UPIU */
		cnt = dev->new_read(dev, (void *)buf, 0, 1);
		if (cnt == 0) {
			printf("RPMB: Read result fail !!!\n");
			bio_close(dev);
			free(hmac);
			ret = -1;
			goto out;
		}

		swap_packet(buf, (u8 *) packet);

#ifdef RPMB_DEBUG
		dprintf(INFO, "Authenticated data write response\n");
		dump_packet(buf, RPMB_SIZE);
		dprintf(INFO, "Authenticated data write response (Swapped)\n");
		dump_packet((u8 *) packet, RPMB_SIZE);
		ufs_upiu_report(packet, SECU_PROT_IN);
		dprintf(INFO, "RPMB: MAC\n");
		dump_packet(packet->Key_MAC, HMAC_SIZE);
#endif
		free(hmac);
		bio_close(dev);
		break;

	case 4:
		/* Authenticated data read request */
		addrp = (u32 *)(packet->data);
		addr = *addrp;
		blk_cnt = *((u32 *)(packet->data)+1);
		start_blk = packet->address;
		*addrp = 0;
		*(addrp+1) = 0;

		buf = malloc(RPMB_SIZE * blk_cnt);
		hmac = malloc(HMAC_CALC_SIZE * blk_cnt);
		swap_packet((u8 *) packet, buf);

#ifdef RPMB_DEBUG
		dprintf(INFO, "Authenticated data read request\n");
		dump_packet((u8 *) packet, RPMB_SIZE);
		dprintf(INFO, "Authenticated data read request (Swapped)\n");
		dump_packet(buf, RPMB_SIZE);
#endif

		dev = bio_open("scsirpmb");
		if (dev == NULL) {
			printf("bio open fail\n");
			free(hmac);
			ret = -1;
			goto out;
		}

		/* Send UPIU */
		cnt = dev->new_write(dev, (void *)buf, 0, 1);
		if (cnt == 0) {
			printf("RPMB: Request read data fail !!!\n");
			bio_close(dev);
			free(hmac);
			ret = -1;
			goto out;
		}

#ifdef RPMB_DEBUG
		ufs_upiu_report(packet, SECU_PROT_OUT);
#endif
		memset((void *)buf, 0, RPMB_SIZE * blk_cnt);

		/* Send UPIU */
		cnt = dev->new_read(dev, (void *)buf, start_blk, blk_cnt);
		if (cnt == 0) {
			printf("RPMB: Read data fail !!!\n");
			bio_close(dev);
			free(hmac);
			ret = -1;
			goto out;
		}

#ifdef RPMB_DEBUG
		dprintf(INFO, "Authenticated data read response (Swapped)\n");
		dprintf(INFO, "HMAC calculatation\n");
#endif
		memset(output_data, 0, sizeof(output_data));

		/* Read hmac Data reordering */
		for (i = 0; i < blk_cnt; i++) {
			memcpy((void *)(hmac + (i * HMAC_CALC_SIZE)),
			       (void *)(buf + DATA_START_BYTE + (i * RPMB_SIZE)), HMAC_CALC_SIZE);
		}

		ret = get_RPMB_hmac(hmac, blk_cnt * HMAC_CALC_SIZE, output_data);
		result = memcmp((void *)(output_data),
				(void *)(buf + HMAC_START_BYTE + ((blk_cnt - 1) * RPMB_SIZE)),
				HMAC_SIZE);

		/* Read Data reordering */
		for (i = 0; i < blk_cnt; i++) {
			swap_packet((u8 *) (buf + (i * RPMB_SIZE)), (u8 *) packet);
			memcpy(INT2VOIDP(addr + (i * DATA_SIZE)), packet->data, DATA_SIZE);
		}

		if (ret != RV_SUCCESS)
			printf("RPMB: get hamc value: fail: 0x%X\n", ret);

		if (result != 0) {
			printf("HMAC compare fail !!\n");
			printf("HMAC Host value\n");
			dump_packet(output_data, HMAC_SIZE);

			printf("HMAC Device value blk_cnt %d i %d\n", blk_cnt, i);
			dump_packet((void *)(buf + HMAC_START_BYTE + ((blk_cnt - 1) * RPMB_SIZE)),
				    HMAC_SIZE);

			printf("Authenticated data read response (Not Swapped)\n");
			dump_packet((void *)(buf + (blk_cnt - 1) * RPMB_SIZE), RPMB_SIZE);
			printf("Authenticated data read response (Swapped)\n");
			dump_packet((u8 *) packet, RPMB_SIZE);
		} else {
#ifdef RPMB_DEBUG
			dprintf(INFO, "HMAC compare success !!\n");
			dprintf(INFO, "RPMB: HMAC: ");
			print_byte_to_hex(output_data, RPMB_HMAC_LEN);
			dprintf(INFO, "\n");
			dprintf(INFO, "Authenticated data read response (Not Swapped)\n");
			dump_packet((void *)(buf + ((blk_cnt - 1) * RPMB_SIZE)), RPMB_SIZE);
			dprintf(INFO, "Authenticated data read response (Swapped)\n");
			dump_packet((u8 *) packet, RPMB_SIZE);
#endif
		}
#ifdef RPMB_DEBUG
		ufs_upiu_report(packet, SECU_PROT_IN);
		dprintf(INFO, "RPMB: MAC\n");
		dump_packet(packet->Key_MAC, HMAC_SIZE);
#endif
		free(hmac);
		bio_close(dev);
		break;
	}

out:
	if (buf != NULL)
		free(buf);
	return ret;
}
#endif

int read_write_counter(void)
{
	int ret;
	struct rpmb_packet packet;

	memset((void *)&packet, 0, RPMB_SIZE);
	packet.request = 0x02;
#ifdef ENABLE_CM_NONCE
	memset(nonce, 0, NONCE_SIZE);
	ret = cm_get_random(nonce, NONCE_SIZE);
	if (ret != RV_SUCCESS) {
		printf("RPMB: fail to get NONCE\n");
		return ret;
	}
	memcpy(packet.nonce, nonce, NONCE_SIZE);
#ifdef RPMB_DEBUG
	dprintf(INFO, "RPMB: read_write_counter NONCE\n");
	print_byte_to_hex(packet.nonce, NONCE_SIZE);
#endif
#else
	for (i = 0; i < 16; i++)
		nonce[i] = packet.nonce[i] = i;
#endif
#ifdef USE_MMC0
	ret = emmc_rpmb_commands(&packet);
#else
	ret = ufs_rpmb_commands(&packet);
#endif
	if (ret != RV_SUCCESS)
		return ret;

	if (packet.result != 0){
		printf("read_write_counter packet result error = %d\n", packet.result );
		return packet.result;
	}
	if(memcmp(packet.nonce, nonce, NONCE_SIZE)) {
		printf("read_write_counter NONCE compare fail\n");
		return -1;
	}
	return RV_SUCCESS;
}

int authentication_key_programming(void)
{
	int ret;
	struct rpmb_packet packet;
	uint8_t rpmb_key[CACHE_WRITEBACK_GRANULE_128] __attribute__((__aligned__(CACHE_WRITEBACK_GRANULE_128)));
	memset((void *)&packet, 0, RPMB_SIZE);

	/* RPMB key derivation */
	dprintf(INFO, "RPMB key derivation\n");
	ret = get_RPMB_key(RPMB_KEY_LEN, rpmb_key);
#ifdef RPMB_DEBUG
	dprintf(INFO, "getting rpmb KEY from cm\n");
	dump_packet(rpmb_key, RPMB_KEY_LEN);
#endif
	memcpy(packet.Key_MAC, rpmb_key, RPMB_KEY_LEN);

	if (ret != RV_SUCCESS) {
		printf("key derivation: fail: 0x%X\n", ret);
		return ret;
	}
	dprintf(INFO, "key derivation: success\n");

	packet.request = 0x01;
#ifdef USE_MMC0
	ret = emmc_rpmb_commands(&packet);
#else
	ret = ufs_rpmb_commands(&packet);
#endif

	memset(packet.Key_MAC, 0x0, RPMB_KEY_LEN);
	memset(rpmb_key, 0x0, RPMB_KEY_LEN);

	if (ret != RV_SUCCESS) {
		printf("key_programming rpmb_commands return error\n");
		return ret;
	}

	if (packet.result != 0) {
		printf("%s rpmb_commands result error = %d\n", __func__, packet.result);
		return -1;
	}
	return RV_SUCCESS;
}

void rpmb_key_programming(void)
{
	int ret;

	// key program and set provision state
	// if (ret == Authentication key not yet programmed (07h)) key programming and if it is ok set_rpmb_provision(1) if not,  set_rpmb_provision(0)
	// if (ret == OK) set_rpmb_provision(1) already programmed
	// if (ret == other error)	set_rpmb_provision(1) already programmed

	ret = read_write_counter();
	if (ret == RPMB_AUTHEN_KEY_ERROR) {
		ret = authentication_key_programming();
		if (ret == RV_SUCCESS) {
			set_RPMB_provision(1);
		}
		else {
			set_RPMB_provision(0);
			printf("RPMB: ERR: key programming fail: 0x%x\n", ret);
		}
	}
	else if (ret != RV_SUCCESS) {
		set_RPMB_provision(1);
		dprintf(INFO, "RPMB: Read write counter fail but key may already programmed\n");
	}
	else { //(ret == RV_SUCCESS)
		set_RPMB_provision(1);
		dprintf(INFO, "RPMB: key already programmed\n");
	}

	ret = block_RPMB_key();
	if (ret != RV_SUCCESS)
		printf("RPMB: key blocking: fail: 0x%X\n", ret);
	else
		dprintf(INFO, "RPMB: key blocking: success\n");

#ifndef CONFIG_USE_AVB20
	ret = block_RPMB_hmac();
	if (ret != RV_SUCCESS)
		printf("RPMB: hmac blocking: fail: 0x%X\n", ret);
	else
		dprintf(INFO, "RPMB: hmac blocking: success\n");
#endif

}

static int rpmb_read_block(int addr, int blkcnt, u8 *buf)
{
	int block;
	int ret;
	uint64_t temp;
	uint32_t *addrp = NULL;
	struct rpmb_packet packet;

	for(block = 0 ; block < blkcnt ; block++) {
		memset((void *)&packet, 0,512);

		packet.request = 0x04;
#ifdef USE_MMC0
		packet.count = 0;
#else
		packet.count = 1;
#endif
		*((u32 *)(packet.data) + 1) = 1;

#ifdef ENABLE_CM_NONCE
		memset(nonce, 0, NONCE_SIZE);
		ret = cm_get_random(nonce, NONCE_SIZE);
		if (ret != RV_SUCCESS) {
			printf("RPMB: fail to get NONCE\n");
			return ret;
		}
		memcpy(packet.nonce, nonce, NONCE_SIZE);
#ifdef RPMB_DEBUG
		dprintf(INFO, "RPMB: rpmb_read_block NONCE\n");
		print_byte_to_hex(packet.nonce, NONCE_SIZE);
#endif
#else
		int i;
		for (i = 0 ; i < NONCE_SIZE ; i++) {
			nonce[i] = packet.nonce[i] = i;
		}
#endif

		temp = (uint64_t)buf;
		addrp = (uint32_t *)(packet.data);
		*addrp = (uint32_t)(temp + (RPMB_BLOCK_SIZE * block));

		packet.address = addr++;

#ifdef USE_MMC0
		ret = emmc_rpmb_commands(&packet);
#else
		ret = ufs_rpmb_commands(&packet);
#endif
		if(memcmp((u8 *)&packet.nonce, nonce, NONCE_SIZE)) {
			printf("Authentication read NONCE compare fail\n");
			return -1;
		}
		if (ret != RV_SUCCESS) {
			printf("RPMB: rpmb_read_block(%d) fail !!!\n", packet.address);
			return ret;
		}
	}

	return RV_SUCCESS;
}

static int rpmb_write_block(int addr, int blkcnt, u8 *buf)
{
	int block;
	int ret;
	uint64_t temp;
	uint32_t *addrp;
	uint32_t wc;
	struct rpmb_packet packet;

	memset((void *)&packet, 0, 512);
#ifdef ENABLE_CM_NONCE
	memset(nonce, 0, NONCE_SIZE);
	ret = cm_get_random(nonce, NONCE_SIZE);
	if (ret != RV_SUCCESS) {
		printf("RPMB: fail to get NONCE\n");
		return ret;
	}
	memcpy(packet.nonce, nonce, NONCE_SIZE);
#ifdef RPMB_DEBUG
	dprintf(INFO, "RPMB: rpmb_write_block NONCE\n");
	print_byte_to_hex(packet.nonce, NONCE_SIZE);
#endif
#else
	int i;
	for (i = 0 ; i < 16 ; i++)
		nonce[i] = packet.nonce[i] = i;
#endif

	packet.request = 0x02; // Read Write Counter
#ifdef USE_MMC0
	ret = emmc_rpmb_commands(&packet);
#else
	ret = ufs_rpmb_commands(&packet);
#endif
	if(memcmp((u8 *)&packet.nonce, nonce, NONCE_SIZE)) {
		printf("Authentication write NONCE compare fail\n");
		return -1;
	}
	if (ret != RV_SUCCESS) {
		printf("RPMB : fail to read write coutner !!!\n");
		return ret;
	}

	wc = packet.write_counter;

	for (block = 0 ; block < blkcnt ; block++) {
		memset((void *)&packet, 0, 512);

		packet.request = 0x03; // Write
		packet.count = 1;
		packet.write_counter = wc++;

		temp = (uint64_t)buf;
		addrp = (uint32_t *)(packet.data);
		*addrp = (uint32_t)(temp + (RPMB_BLOCK_SIZE * block));
		packet.address = addr++;

#ifdef USE_MMC0
		ret = emmc_rpmb_commands(&packet);
#else
		ret = ufs_rpmb_commands(&packet);
#endif
		if (ret != RV_SUCCESS) {
			printf("RPMB: write block (%d) fail !!!\n", packet.address);
			return ret;
		}

	}

	return RV_SUCCESS;
}

static int rpmb_init_table(void)
{
	int i;
	int ret;
	u8 buf[RPMB_BLOCK_SIZE];
	u32 addr, addr_base;
	struct boot_header *header;
	struct header_block *hblock;

	memset((void *)buf, 0, RPMB_BLOCK_SIZE);

	header = (struct boot_header*)buf;
	header->magic = BOOT_MAGIC;
	header->ri_block_base = BOOT_RI_TABLE_BLOCK;
	header->ri_block_cnt = BOOT_RI_TABLE_BLOCK_CNT;

	hblock  = (struct header_block *)buf;
	hblock->lock = 1;
	lock_state = 1;

	addr_base = RPMB_BLOCK_PER_PARTITION * (BOOT_RI_PARTITION - 1);

	ret = rpmb_write_block(addr_base, 1, buf);

	if (ret != RV_SUCCESS) {
		printf("RPMB : Header block write error!!!\n");
		return ret;
	}

	memcpy((void *)&bootHeader, buf, sizeof(struct boot_header));

	memset((void *)buf, 0, RPMB_BLOCK_SIZE);

	addr = addr_base + BOOT_RI_TABLE_BLOCK;
	for (i = 0 ; i < BOOT_RI_TABLE_BLOCK_CNT ; i++) {
		addr = addr + i;
		ret = rpmb_write_block(addr, 1, buf);
		if (ret) {
			printf("RPMB : Rollback Index #%d init fail!!!\n", i+1);
			return ret;
		}

	}

	addr = addr_base + PERSIST_DATA_BLOCK;
	for (i = 0 ; i < PERSIST_DATA_BLOCK_CNT ; i++) {
		addr = addr + i;
		ret = rpmb_write_block(addr, 1, buf);
		if (ret) {
			printf("RPMB : Persistent Data Block #%d init fail!!!\n", i+1);
			return ret;
		}

	}

	return RV_SUCCESS;
}

static int rpmb_ri_check_magic(void)
{
	int ret;
	u8 buf[RPMB_BLOCK_SIZE];
	uint32_t  addr;
	struct boot_header *header;
	struct header_block *hblock;

	addr = RPMB_BLOCK_PER_PARTITION * (BOOT_RI_PARTITION - 1);

	ret = rpmb_read_block(addr, 1, buf);

	if (ret != RV_SUCCESS) {
		printf("RPMB Header block read error\n");
		return ret;
	}

	header = (struct boot_header *)buf;
	hblock = (struct header_block *)buf;

	if (header->magic != BOOT_MAGIC) {
		ret = rpmb_init_table();
		if (ret) {
			printf("RPMB : table init fail\n");
			return ret;
		}
	} else {
		memcpy((void *)&bootHeader, header, sizeof(struct boot_header));
		lock_state = hblock->lock;
	}

	table_init_state = 1;
	return RV_SUCCESS;
}

static void rpmb_display_rollback_index(void)
{
	u32 i;
	u8 *buf;

	printf("rpmb_display_rollback_index\n");

	buf = (u8 *)rollbackIndex;

	for (i = 1 ; i <= (bootHeader.ri_block_cnt * RPMB_BLOCK_SIZE) ; i++) {
		printf("%02X ", buf[i-1]);
		if (i % 16 == 0) printf("\n");
		if (i % RPMB_BLOCK_SIZE == 0) printf("\n");
	}
	printf("\n");
}

static void rpmb_display_persistent_data(void)
{
	int i, j;

	printf("rpmb_display_persistent_data\n");

	for (i = 0 ; i < PERSIST_DATA_CNT ; i++) {
		if (persistentData[i].len != 0) {
			printf("KEY%d : %s\n", i, persistentData[i].key);
			printf("VAL%d : ", i);
			for (j = 1 ; j <= PERSIST_VALUE_LEN ; j++) {
				printf("%02X ", persistentData[i].value[j-1]);
				if (j % 16 == 0)
					printf("\n       ");
			}
			printf("\n");
		}
	}
	printf("\n");
}

static int rpmb_update_table_block(uint32_t block, u8 *buf)
{
	int ret;
	u32 addr;

	// Check If rollback index table is initilaized
	if (!table_init_state)
		return RV_RPMB_RI_TABLE_NOT_INITIALIZED;

	addr = RPMB_BLOCK_PER_PARTITION * (BOOT_RI_PARTITION - 1);
	addr = addr + block;

	ret = rpmb_write_block(addr, 1, buf);

	if (ret != RV_SUCCESS) {
		printf("RPMB: Update block #%d fail\n", addr);
		return ret;
	}

	return RV_SUCCESS;
}

static int rpmb_load_rollback_index(void)
{
	int i;
	int ret;
	u32 addr;
	u8 *buf;

	addr = RPMB_BLOCK_PER_PARTITION * (BOOT_RI_PARTITION - 1);
	addr = addr + BOOT_RI_TABLE_BLOCK;

	for (i = 0 ; i < BOOT_RI_TABLE_BLOCK_CNT ; i++) {
		buf = (u8 *)&rollbackIndex[(RPMB_BLOCK_SIZE / sizeof(uint64_t)) * i];

		ret = rpmb_read_block(addr + i, 1, buf);
		if (ret != RV_SUCCESS) {
			printf("RPMB:RI blk #%d rd err\n", i + 1);
			table_init_state = 0;
			return ret;
		}
	}
#ifdef RPMB_DEBUG
	rpmb_display_rollback_index();
#endif
	return RV_SUCCESS;
}

static int rpmb_load_persistent_data(void)
{
	int i;
	int ret;
	u32 addr;
	u8 *buf;

	addr = RPMB_BLOCK_PER_PARTITION * (BOOT_RI_PARTITION - 1);
	addr = addr + PERSIST_DATA_BLOCK;

	for (i = 0 ; i < PERSIST_DATA_BLOCK_CNT ; i++) {
		buf = (u8 *)&persistentData[(RPMB_BLOCK_SIZE / PERSIST_DATA_LEN) * i];

		ret = rpmb_read_block(addr + i, 1, buf);
		if (ret != RV_SUCCESS) {
			printf("RPMB:Perst blk #%d rd err\n", i + 1);
			table_init_state = 0;
			return ret;
		}
	}
#ifdef RPMB_DEBUG
	rpmb_display_persistent_data();
#endif
	return RV_SUCCESS;
}

int rpmb_load_boot_table(void)
{
	int ret;

	ret = rpmb_ri_check_magic();

	if (ret != RV_SUCCESS) {
		printf("RPMB : RI check magic fail\n");
		table_init_state = 0;
		return ret;
	}

	ret = rpmb_load_rollback_index();
	if (ret != RV_SUCCESS) {
		printf("RPMB : RI table load fail\n");
		table_init_state = 0;
		return ret;
	}

	ret = rpmb_load_persistent_data();
	if (ret != RV_SUCCESS) {
		printf("RPMB : Perst data load fail\n");
		table_init_state = 0;
		return ret;
	}

	printf("RPMB: rpmb_load_boot_table success\n");
	return RV_SUCCESS;
}

int rpmb_get_rollback_index(size_t loc, uint64_t *rollback_index)
{
	if (!rollback_index)
		return RV_RPMB_PARAM_NULL_POINTER;

	if (loc >= BOOT_RI_TABLE_SIZE)
		return RV_RPMB_INVALID_ROLLBACK_INDEX;

	if (table_init_state == 0) {
		printf("RPMB: table not initialized\n");
		return RV_RPMB_RI_TABLE_NOT_INITIALIZED;
	}

	*rollback_index = rollbackIndex[loc];

	return RV_SUCCESS;
}

int rpmb_set_rollback_index(size_t loc, uint64_t rollback_index)
{
	uint32_t block;
	int ret;
	u8 *buf;

	if (loc >= BOOT_RI_TABLE_SIZE)
		return RV_RPMB_INVALID_ROLLBACK_INDEX;

	if (table_init_state == 0)
		return RV_RPMB_RI_TABLE_NOT_INITIALIZED;

	rollbackIndex[loc] = rollback_index;


	block = (uint32_t)(loc / (RPMB_BLOCK_SIZE / sizeof(uint64_t)));

	buf = (u8 *)rollbackIndex + (RPMB_BLOCK_SIZE * block);

	ret = rpmb_update_table_block(BOOT_RI_TABLE_BLOCK + block, buf);

	if (ret != RV_SUCCESS) {
		printf("RPMB: set RI #%zd fail\n", loc);
		return ret;
	}

	return RV_SUCCESS;
}

static int rpmb_search_persistent_key(const char *name)
{
	int i;

	for (i = 0 ; i < PERSIST_DATA_CNT ; i++) {
		if (strcmp(name, (char *)persistentData[i].key) == 0)
			return i;
	}

	return -1;
}

static int rpmb_get_empty_persistent_data(void)
{
	int i;

	for (i = 0 ; i < PERSIST_DATA_CNT ; i++) {
		if (persistentData[i].key[0] == '\0')
			return i;
	}

	return -1;
}

int rpmb_read_persistent_value(const char *name,
                size_t buffer_size,
                uint8_t *out_buffer,
                size_t *out_num_bytes_read)
{
	int index;

	if (!name)
		return RV_RPMB_PARAM_NULL_POINTER;

	if (buffer_size > PERSIST_VALUE_LEN)
		return RV_RPMB_INVALID_PARAM;

	if (!out_buffer)
		return RV_RPMB_PARAM_NULL_POINTER;

	if (!out_num_bytes_read)
		return RV_RPMB_PARAM_NULL_POINTER;

	index = rpmb_search_persistent_key(name);

	if (index < 0) {
		out_num_bytes_read = 0;
		return RV_RPMB_PERSIST_NAME_NOT_FOUND;
	}

	if (buffer_size < persistentData[index].len) {
		*out_num_bytes_read = persistentData[index].len;
		return RV_RPMB_INVALID_PERSIST_DATA_SIZE;
	}

	*out_num_bytes_read = persistentData[index].len;

	memcpy((void *)out_buffer, persistentData[index].value,
			persistentData[index].len);

	return RV_SUCCESS;
}

int rpmb_write_persistent_value(const char *name,
                size_t value_size,
                const uint8_t *value)
{
	int index;
	int ret;
	u32 block;
	u8 *buf;

	if (!table_init_state)
		return RV_RPMB_RI_TABLE_NOT_INITIALIZED;

	if (!name)
		return RV_RPMB_PARAM_NULL_POINTER;

	if (!value)
		return RV_RPMB_PARAM_NULL_POINTER;

	if (!value_size)
		return RV_RPMB_INVALID_PARAM;

	index = rpmb_search_persistent_key(name);

	if (index < 0)
		index = rpmb_get_empty_persistent_data();

	if (index < 0)
		return RV_RPMB_PERSIST_NAME_NOT_FOUND;

	if (strlen(name) >= PERSIST_KEY_LEN)
		return RV_RPMB_INVALID_KEY_LEN;

	if (value_size > PERSIST_VALUE_LEN)
		return RV_RPMB_INVALID_PERSIST_DATA_SIZE;

	strncpy((char *)persistentData[index].key, (char *)name, PERSIST_KEY_LEN - 1);
	persistentData[index].len = value_size;
	memcpy((void *)persistentData[index].value, value, value_size);


	block = index / (RPMB_BLOCK_SIZE / PERSIST_DATA_LEN);
	buf = (u8 *)persistentData + (RPMB_BLOCK_SIZE * block);

	ret = rpmb_update_table_block(PERSIST_DATA_BLOCK + block, buf);

	if (ret != RV_SUCCESS) {
		printf("RPMB: Perst data blk(%d) update fail\n", block);
		return ret;
	}

	return RV_SUCCESS;
}

int rpmb_get_lock_state(uint32_t *state)
{
	// Check If rollback index table is initilaized
	if (!table_init_state)
		return RV_RPMB_RI_TABLE_NOT_INITIALIZED;

	*state = lock_state;

	return RV_SUCCESS;
}

int rpmb_set_lock_state(uint32_t state)
{
	int ret;
	u8 buf[RPMB_BLOCK_SIZE];
	struct boot_header *header;
	struct header_block *hblock;

	if (state != 0)
		state = 1;

	header = (struct boot_header *)buf;
	hblock = (struct header_block *)buf;


	memcpy((void *)header, (void *)&bootHeader, sizeof(struct boot_header));

	lock_state = state;
	hblock->lock = state;

	ret = rpmb_update_table_block(0, buf);

	if (ret != RV_SUCCESS) {
		printf("RPMB : Set LOCK state (%d) fail\n", state);
		return ret;
	}

	return RV_SUCCESS;
}
