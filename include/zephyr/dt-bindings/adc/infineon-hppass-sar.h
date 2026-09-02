/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Infineon Technologies AG,
 * SPDX-FileCopyrightText: or an affiliate of Infineon Technologies AG. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_ADC_INFINEON_HPPASS_SAR_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_ADC_INFINEON_HPPASS_SAR_H_

/* Trigger sources for a SAR sequencer group (SEQ_ENTRY.TR_SEL, Regs TRM §20.1.5). */
#define IFX_HPPASS_SAR_TRIG_DISABLED 0
#define IFX_HPPASS_SAR_TRIG_0        1
#define IFX_HPPASS_SAR_TRIG_1        2
#define IFX_HPPASS_SAR_TRIG_2        3
#define IFX_HPPASS_SAR_TRIG_3        4
#define IFX_HPPASS_SAR_TRIG_4        5
#define IFX_HPPASS_SAR_TRIG_5        6
#define IFX_HPPASS_SAR_TRIG_6        7
#define IFX_HPPASS_SAR_TRIG_7        8
#define IFX_HPPASS_SAR_TRIG_AC       9

/* FIFO buffer routing (CHAN_CFG.FIFO_SEL, Regs TRM §20.1.12). */
#define IFX_HPPASS_SAR_FIFO_DISABLED 0
#define IFX_HPPASS_SAR_FIFO_0        1
#define IFX_HPPASS_SAR_FIFO_1        2
#define IFX_HPPASS_SAR_FIFO_2        3
#define IFX_HPPASS_SAR_FIFO_3        4

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_ADC_INFINEON_HPPASS_SAR_H_ */
