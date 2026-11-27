/* SPDX-License-Identifier: (GPL-2.0+ OR BSD-3-Clause) */
/*
 * Copyright (c) 2026 Siarhei Volkau
 * Author: Siarhei Volkau <lis8215@gmail.com>
 */

#ifndef DT_BINDINGS_RESET_HICHIP_HC16XX_RESET
#define DT_BINDINGS_RESET_HICHIP_HC16XX_RESET

#define HC16XX_GE_RESET		4
#define HC16XX_UART2_RESET	9
#define HC16XX_UART3_RESET	10
#define HC16XX_SPI_RESET	14
#define HC16XX_UART0_RESET	16
#define HC16XX_UART1_RESET	17
#define HC16XX_WDT_RESET	23
#define HC16XX_USB0_RESET	28
#define HC16XX_LVDS_RESET	40 /* 32 + 8 */
#define HC16XX_HDMI_I2C_RESET	44 /* 32 + 12 */
#define HC16XX_USB1_RESET	61 /* 32 + 29 */

#endif
