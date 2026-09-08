/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Infineon Technologies AG,
 * SPDX-FileCopyrightText: or an affiliate of Infineon Technologies AG. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zephyr/drivers/adc/infineon_hppass_sar.h>
#include <zephyr/dt-bindings/adc/infineon-hppass-sar.h>

#if !DT_NODE_EXISTS(DT_PATH(zephyr_user)) || !DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
#error "No suitable devicetree overlay specified"
#endif

#define FIFO_BUFS     3
/* one 16-word buffer, drained in full each watermark hit */
#define FIFO_READ_MAX 16
#define FIR_IDX_0     0
#define FIR_IDX_1     1
/* Filter always holds 16 taps, however many are loaded */
#define FIR_TAPS      16

struct fifo_snapshot {
	uint32_t status;
	uint8_t count[FIFO_BUFS];
	int16_t val[FIFO_BUFS][FIFO_READ_MAX];
	uint8_t chan[FIFO_BUFS][FIFO_READ_MAX];
};

static struct fifo_snapshot fifo_latest;
static volatile bool fifo_pending;

static const int16_t fir_avg4[] = {
	0x2000,
	0x2000,
	0x2000,
	0x2000,
};

/* Buffer 0 always carries the raw channel, buffers 1 and 2 the FIR outputs. */
static const char *const fifo_labels[FIFO_BUFS] = {"RAW ", "FIR0", "FIR1"};

/* The watermark is level-sensitive, so the buffers have to be drained here. */
static void fifo_callback(const struct device *dev, uint32_t fifo_status, void *user_data)
{
	ARG_UNUSED(user_data);

	fifo_latest.status = fifo_status;

	for (uint8_t i = 0; i < FIFO_BUFS; i++) {
		int words = adc_ifx_hppass_sar_fifo_read_chan_id(
			dev, i, fifo_latest.val[i], fifo_latest.chan[i], FIFO_READ_MAX);

		fifo_latest.count[i] = (words > 0) ? (uint8_t)words : 0U;
	}

	fifo_pending = true;
}

#define DT_SPEC_AND_COMMA(node_id, prop, idx) ADC_DT_SPEC_GET_BY_IDX(node_id, idx),

/* Data of ADC io-channels specified in devicetree. */
static const struct adc_dt_spec adc_channels[] = {
	DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), io_channels, DT_SPEC_AND_COMMA)};

static bool is_fir_channel(uint8_t channel_id)
{
	return channel_id >= IFX_HPPASS_SAR_FIR0_CHANNEL;
}

static int setup_adc(void)
{
	int err;

	for (size_t i = 0U; i < ARRAY_SIZE(adc_channels); i++) {
		if (!adc_is_ready_dt(&adc_channels[i])) {
			printk("ADC controller device %s not ready\n", adc_channels[i].dev->name);
			return -ENODEV;
		}

		err = adc_channel_setup_dt(&adc_channels[i]);
		if (err < 0) {
			printk("Could not setup channel #%d (%d)\n", i, err);
			return err;
		}
	}

	err = adc_ifx_hppass_sar_fifo_set_callback(adc_channels[0].dev, fifo_callback, NULL);
	if (err < 0) {
		printk("Failed to set FIFO callback (%d)\n", err);
		return err;
	}

	return 0;
}

/* -EAGAIN here means no new result yet, e.g. the 16-sample warm-up. */
static void print_fir(const struct device *dev, uint8_t idx)
{
	int16_t result;
	int err = adc_ifx_hppass_sar_fir_read(dev, idx, &result);

	if (err == -EAGAIN) {
		printk("FIR[%u]: no new result\n", (unsigned int)idx);
	} else if (err < 0) {
		printk("FIR[%u]: read failed (%d)\n", (unsigned int)idx, err);
	} else {
		printk("FIR[%u]: %d\n", (unsigned int)idx, result);
	}
}

static void report_fir(const struct device *dev, uint32_t pass)
{
	int16_t coef[FIR_TAPS];
	int err;

	print_fir(dev, FIR_IDX_1);

	if (pass != 2U) {
		return;
	}

	err = adc_ifx_hppass_sar_fir_set_coefficients(dev, FIR_IDX_0, fir_avg4,
						      ARRAY_SIZE(fir_avg4));
	printk("FIR[0]: reload to 4-tap average -> %d\n", err);

	err = adc_ifx_hppass_sar_fir_get_coefficients(dev, FIR_IDX_0, coef, FIR_TAPS);
	if (err == 0) {
		printk("FIR[0]: taps now 0x%04x 0x%04x 0x%04x 0x%04x\n", (uint16_t)coef[0],
		       (uint16_t)coef[1], (uint16_t)coef[2], (uint16_t)coef[3]);
	}
}

int main(void)
{
	int err;
	uint32_t count = 0;
	uint16_t buf[ARRAY_SIZE(adc_channels)];
	uint16_t fir_buf[ARRAY_SIZE(adc_channels)];
	struct adc_sequence sequence = {
		.buffer = buf,
		.buffer_size = sizeof(buf),
		.resolution = 12,
	};
	/*
	 * Read the filter channels separately: a sequence fails as a whole, so a
	 * filter still warming up would take the raw channel reading down with it.
	 */
	struct adc_sequence fir_sequence = {
		.buffer = fir_buf,
		.buffer_size = sizeof(fir_buf),
		.resolution = 12,
	};

	err = setup_adc();
	if (err < 0) {
		printk("Failed to setup ADC (%d)\n", err);
		return 0;
	}

	/* Build a combined channel mask for all configured channels */
	for (size_t i = 0U; i < ARRAY_SIZE(adc_channels); i++) {
		if (is_fir_channel(adc_channels[i].channel_id) == true) {
			fir_sequence.channels |= BIT(adc_channels[i].channel_id);
		} else {
			sequence.channels |= BIT(adc_channels[i].channel_id);
		}
	}

	while (1) {
		/* An overlay may list filter channels only, leaving nothing to convert. */
		if (sequence.channels != 0U) {
			err = adc_read(adc_channels[0].dev, &sequence);
			if (err < 0) {
				printk("ADC read error (%d)\n", err);
				k_sleep(K_MSEC(500));
				continue;
			}
		}

		printk("ADC reading[%u]:\n", count++);
		size_t slot = 0U;

		for (size_t i = 0U; i < ARRAY_SIZE(adc_channels); i++) {
			int32_t val_mv;

			if (is_fir_channel(adc_channels[i].channel_id) == true) {
				continue;
			}

			val_mv = buf[slot++];

			printk("- %s, channel %d: %" PRId32, adc_channels[i].dev->name,
			       adc_channels[i].channel_id, val_mv);

			err = adc_raw_to_millivolts_dt(&adc_channels[i], &val_mv);
			if (err < 0) {
				printk(" (value in mV not available)\n");
			} else {
				printk(" = %" PRId32 " mV\n", val_mv);
			}
		}

		if (fir_sequence.channels != 0U) {
			err = adc_read(adc_channels[0].dev, &fir_sequence);
			if (err == -EAGAIN) {
				printk("FIR[0]: no result yet (channel %u)\n",
				       IFX_HPPASS_SAR_FIR0_CHANNEL);
			} else if (err < 0) {
				printk("FIR[0]: channel read error (%d)\n", err);
			} else {
				printk("FIR[0]: %d (channel %u)\n", (int16_t)fir_buf[0],
				       IFX_HPPASS_SAR_FIR0_CHANNEL);
			}
		}

		if (fifo_pending == true) {
			struct fifo_snapshot snap;
			unsigned int key;

			key = irq_lock();
			snap = fifo_latest;
			fifo_pending = false;
			irq_unlock(key);

			printk("FIFO: watermark event (status=0x%08x)\n", snap.status);

			for (uint8_t i = 0; i < FIFO_BUFS; i++) {
				printk("FIFO[%u] %s: %u words -", (unsigned int)i, fifo_labels[i],
				       (unsigned int)snap.count[i]);
				for (uint8_t j = 0; j < snap.count[i]; j++) {
					printk(" ch%u=%d", (unsigned int)snap.chan[i][j],
					       snap.val[i][j]);
				}
				printk("\n");
			}
		}

		report_fir(adc_channels[0].dev, count);

		k_sleep(K_MSEC(500));
	}
	return 0;
}
