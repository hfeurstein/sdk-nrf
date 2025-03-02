/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @brief Header containing the structure declarations for the Bus Abstraction
 * Layer (BAL) of the Wi-Fi driver.
 */

#ifndef __BAL_STRUCTS_H__
#define __BAL_STRUCTS_H__

#include "osal_ops.h"
#include "bal_ops.h"

struct wifi_nrf_bal_cfg_params {
	unsigned long addr_pktram_base;
};

/**
 * struct wifi_nrf_bal_priv - Structure to hold context information for the BAL
 * @opriv: Pointer to the OSAL context.
 * @bus_priv: Pointer to a specific bus context.
 * @ops: Pointer to bus operations to be provided by a specific bus
 *       implementation.
 *
 * This structure maintains the context information necessary for the
 * operation of the BAL. Some of the elements of the structure need to be
 * initialized during the initialization of the BAL while others need to
 * be kept updated over the duration of the BAL operation.
 */
struct wifi_nrf_bal_priv {
	struct wifi_nrf_osal_priv *opriv;
	void *bus_priv;
	struct wifi_nrf_bal_ops *ops;

	enum wifi_nrf_status (*init_dev_callbk_fn)(void *ctx);

	void (*deinit_dev_callbk_fn)(void *ctx);

	enum wifi_nrf_status (*intr_callbk_fn)(void *ctx);
};


struct wifi_nrf_bal_dev_ctx {
	struct wifi_nrf_bal_priv *bpriv;
	void *hal_dev_ctx;
	void *bus_dev_ctx;
#ifdef RPU_SLEEP_SUPPORT
	bool rpu_fw_booted;
#endif /* RPU_SLEEP_SUPPORT */
};
#endif /* __BAL_STRUCTS_H__ */
