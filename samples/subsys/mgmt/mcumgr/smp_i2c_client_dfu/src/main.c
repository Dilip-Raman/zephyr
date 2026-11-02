/*
 * Copyright (c) 2026 Aerlync Labs Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Firmware update of a target board over I2C, using the SMP client APIs.
 * The target runs an MCUmgr SMP server on the zephyr,smp-i2c transport. This
 * controller echoes the target, uploads a signed image to the second slot,
 * marks it for test, resets the target, and confirms the image once the
 * target answers again.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>

#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/smp/smp_client.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>
#include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt_client.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt_client.h>

/*
 * The signed image to upload is injected at build time from
 * CONFIG_APP_DFU_IMAGE_FILE (see CMakeLists.txt and Kconfig); it is not
 * committed to the tree.
 */
static const uint8_t dfu_image[] = {
#include "dfu_image.inc"
};
#define DFU_IMAGE_SIZE sizeof(dfu_image)

LOG_MODULE_REGISTER(smp_i2c_client_dfu, LOG_LEVEL_INF);

#define UPLOAD_CHUNK_SIZE 160U
#define SHA256_LEN        32U

/* Optional LEDs for visual feedback */
static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led1), gpios, {0});
static const struct gpio_dt_spec led_red = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});

static struct smp_client_object smp_client;
static struct os_mgmt_client os_client;
static struct img_mgmt_client img_client;
static struct mcumgr_image_data img_slots[4];

struct mcuboot_hdr {
	uint32_t ih_magic;
	uint32_t ih_load_addr;
	uint16_t ih_hdr_size;
	uint16_t ih_protect_tlv_size;
	uint32_t ih_img_size;
	uint32_t ih_flags;
	struct {
		uint8_t major;
		uint8_t minor;
		uint16_t revision;
		uint32_t build_num;
	} ih_ver;
	uint32_t _pad;
} __packed;

struct tlv_info {
	uint16_t it_magic;
	uint16_t it_tlv_tot;
} __packed;

struct tlv_entry {
	uint8_t it_type;
	uint8_t _pad;
	uint16_t it_len;
} __packed;

static bool extract_sha256(const uint8_t *img, size_t size, uint8_t *sha_out)
{
	if (size < sizeof(struct mcuboot_hdr)) {
		return false;
	}

	const struct mcuboot_hdr *hdr = (const struct mcuboot_hdr *)img;

	if (sys_le32_to_cpu(hdr->ih_magic) != 0x96f3b83d) {
		LOG_WRN("MCUboot magic missing in image");
		return false;
	}

	uint32_t hdr_sz = sys_le16_to_cpu(hdr->ih_hdr_size);
	uint32_t img_sz = sys_le32_to_cpu(hdr->ih_img_size);
	uint32_t prot_sz = sys_le16_to_cpu(hdr->ih_protect_tlv_size);
	size_t tlv_off = hdr_sz + img_sz + prot_sz;

	if (tlv_off + sizeof(struct tlv_info) > size) {
		LOG_WRN("Image too short for TLV area");
		return false;
	}

	const struct tlv_info *tinfo = (const struct tlv_info *)(img + tlv_off);

	if (sys_le16_to_cpu(tinfo->it_magic) != 0x6907) {
		LOG_WRN("TLV magic mismatch: 0x%04x", sys_le16_to_cpu(tinfo->it_magic));
		return false;
	}

	size_t cur = tlv_off + sizeof(struct tlv_info);
	size_t end = tlv_off + sys_le16_to_cpu(tinfo->it_tlv_tot);

	if (end > size) {
		end = size;
	}

	while (cur + sizeof(struct tlv_entry) <= end) {
		const struct tlv_entry *entry = (const struct tlv_entry *)(img + cur);
		uint16_t entry_len = sys_le16_to_cpu(entry->it_len);

		if (entry->it_type == 0x10 && entry_len == SHA256_LEN) {
			memcpy(sha_out, img + cur + sizeof(struct tlv_entry), SHA256_LEN);
			return true;
		}
		cur += sizeof(struct tlv_entry) + entry_len;
	}

	return false;
}

int main(void)
{
	int rc;
	uint8_t sha256[SHA256_LEN];
	bool has_sha;
	struct mcumgr_image_state state_res;

	if (led_green.port && gpio_is_ready_dt(&led_green)) {
		gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_INACTIVE);
	}
	if (led_red.port && gpio_is_ready_dt(&led_red)) {
		gpio_pin_configure_dt(&led_red, GPIO_OUTPUT_INACTIVE);
	}

	LOG_INF("SMP I2C client DFU, image size %u bytes", DFU_IMAGE_SIZE);

	has_sha = extract_sha256(dfu_image, DFU_IMAGE_SIZE, sha256);
	if (has_sha) {
		LOG_HEXDUMP_INF(sha256, SHA256_LEN, "image SHA-256:");
	}

	LOG_INF("Initializing SMP I2C client transport");
	rc = smp_client_object_init(&smp_client, SMP_I2C_CLIENT_TRANSPORT);
	if (rc != MGMT_ERR_EOK) {
		LOG_ERR("Failed to initialize SMP Client object: %d", rc);
		return rc;
	}

	os_mgmt_client_init(&os_client, &smp_client);
	img_mgmt_client_init(&img_client, &smp_client, ARRAY_SIZE(img_slots), img_slots);

	/* Allow target I2C bus listener to be ready */
	k_msleep(2000);

	/* Step 1: echo the target to confirm the link is up. */
	LOG_INF("[1/6] Echo ping to target");
	rc = os_mgmt_client_echo(&os_client, "ping", 4);
	if (rc != MGMT_ERR_EOK) {
		LOG_ERR("Echo failed: %d (check I2C wiring and target firmware)", rc);
		if (led_red.port) {
			gpio_pin_set_dt(&led_red, 1);
		}
		return rc;
	}
	LOG_INF("Target reachable over I2C");

	/* Step 2: read the current image state. */
	LOG_INF("[2/6] Reading image state from target");
	rc = img_mgmt_client_state_read(&img_client, &state_res);
	if (rc == MGMT_ERR_EOK) {
		LOG_INF("Target reported %d image slots:", state_res.image_list_length);
		for (int i = 0; i < state_res.image_list_length; i++) {
			LOG_INF("  Slot %u: version=%s active=%d confirmed=%d pending=%d",
				state_res.image_list[i].slot_num, state_res.image_list[i].version,
				state_res.image_list[i].flags.active,
				state_res.image_list[i].flags.confirmed,
				state_res.image_list[i].flags.pending);
		}
	} else {
		LOG_WRN("Failed to read image state: %d (continuing anyway)", rc);
	}

	/* Step 3: start the upload session to slot 1. */
	LOG_INF("[3/6] Starting upload to slot 1 (%u bytes)", DFU_IMAGE_SIZE);
	rc = img_mgmt_client_upload_init(&img_client, DFU_IMAGE_SIZE, 0,
					 has_sha ? (const char *)sha256 : NULL);
	if (rc != MGMT_ERR_EOK) {
		LOG_ERR("img_mgmt_client_upload_init failed: %d", rc);
		return rc;
	}

	/* Step 4: stream the image in chunks. */
	LOG_INF("[4/6] Uploading image");
	size_t offset = 0;
	int chunk_idx = 0;
	int total_chunks = (DFU_IMAGE_SIZE + UPLOAD_CHUNK_SIZE - 1) / UPLOAD_CHUNK_SIZE;
	int64_t t_start = k_uptime_get();

	while (offset < DFU_IMAGE_SIZE) {
		size_t len = MIN(UPLOAD_CHUNK_SIZE, DFU_IMAGE_SIZE - offset);
		struct mcumgr_image_upload upload_res;

		rc = img_mgmt_client_upload(&img_client, &dfu_image[offset], len, &upload_res);
		if (rc != MGMT_ERR_EOK || upload_res.status != MGMT_ERR_EOK) {
			LOG_ERR("Upload error at offset %zu: rc=%d status=%d", offset, rc,
				upload_res.status);
			if (led_red.port) {
				gpio_pin_set_dt(&led_red, 1);
			}
			return rc ? rc : upload_res.status;
		}

		chunk_idx++;
		if (chunk_idx % 25 == 1 || chunk_idx == total_chunks) {
			LOG_INF("  Chunk %3d/%d | offset %5zu / %5u B (%3zu%%)", chunk_idx,
				total_chunks, offset + len, DFU_IMAGE_SIZE,
				((offset + len) * 100) / DFU_IMAGE_SIZE);
		}

		offset += len;
	}

	int64_t duration_ms = k_uptime_get() - t_start;

	LOG_INF("Upload complete in %lld ms (approx %.2f kB/s)", duration_ms,
		(double)DFU_IMAGE_SIZE / (double)duration_ms);

	/* Step 5: mark slot 1 for a test swap, not yet confirmed. */
	LOG_INF("[5/6] Marking slot 1 as the test image");
	rc = img_mgmt_client_state_write(&img_client, has_sha ? (const char *)sha256 : NULL,
					 has_sha ? sizeof(sha256) : 0, false, &state_res);
	if (rc != MGMT_ERR_EOK) {
		LOG_ERR("Failed to stage Slot 1 as test image: %d", rc);
		return rc;
	}
	LOG_INF("Slot 1 staged for a test swap on next boot");

	/* Step 6: reset, health-check the new image, then confirm it. */
	LOG_INF("[6/6] Resetting target");
	rc = os_mgmt_client_reset(&os_client);
	if (rc != MGMT_ERR_EOK) {
		LOG_WRN("Reset command returned %d (target may have rebooted immediately)", rc);
	}

	LOG_INF("Waiting for the target to reboot and MCUboot to swap");
	k_msleep(4000);

	bool target_online = false;

	for (int retry = 1; retry <= 10; retry++) {
		k_msleep(1000);
		rc = os_mgmt_client_echo(&os_client, "healthcheck", 11);
		if (rc == MGMT_ERR_EOK) {
			target_online = true;
			LOG_INF("Target online after %d s", retry + 4);
			break;
		}
	}

	if (!target_online) {
		/*
		 * The new image never answered. Do not confirm it. A swap-based
		 * MCUboot then reverts to the previous image on the next boot.
		 * An overwrite-only MCUboot (CONFIG_BOOT_UPGRADE_ONLY, as on
		 * FRDM-MCXN236) has already replaced the old image, so there is
		 * nothing to revert to.
		 */
		LOG_ERR("Target did not respond after DFU, not confirming the new image");
		if (led_red.port) {
			gpio_pin_set_dt(&led_red, 1);
		}
		return -ETIMEDOUT;
	}

	/*
	 * The new image booted and answers SMP. Confirm the running image now
	 * (hash = NULL) so MCUboot keeps it. With a swap-based MCUboot,
	 * confirming only after this health check keeps the revert-on-failure
	 * safety net; with overwrite-only there is no revert path to protect.
	 */
	rc = img_mgmt_client_state_write(&img_client, NULL, 0, true, &state_res);
	if (rc != MGMT_ERR_EOK) {
		LOG_ERR("Failed to confirm the running image: %d", rc);
		if (led_red.port) {
			gpio_pin_set_dt(&led_red, 1);
		}
		return rc;
	}
	LOG_INF("Running image confirmed permanent.");

	rc = img_mgmt_client_state_read(&img_client, &state_res);
	if (rc == MGMT_ERR_EOK && state_res.image_list_length > 0) {
		LOG_INF("Active Image in Slot 0: Version %s (Confirmed=%d, Active=%d)",
			state_res.image_list[0].version, state_res.image_list[0].flags.confirmed,
			state_res.image_list[0].flags.active);
	}
	if (led_green.port) {
		gpio_pin_set_dt(&led_green, 1);
	}
	LOG_INF("DFU complete, target running the confirmed image");

	return 0;
}
