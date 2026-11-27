/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _DT_BINDINGS_CLOCK_HICHIP_HC16XX_H
#define _DT_BINDINGS_CLOCK_HICHIP_HC16XX_H

/*
 * Root clocks
 */
#define HC16XX_CLK_DUMMY              0

/*
 * PLL outputs
 */
#define HC16XX_CLK_MCPU_PLL           1
#define HC16XX_CLK_SCPU_PLL           2
#define HC16XX_CLK_DRAM_PLL           3

/*
 * Complex clocks
 */
#define HC16XX_CLK_MCPU               10
#define HC16XX_CLK_SCPU               11
#define HC16XX_CLK_SDMMC_CARD         12

/*
 * Gate clocks
 */
#define HC16XX_CLK_SB_27M             25
#define HC16XX_CLK_SB_108M            26
#define HC16XX_CLK_SB_1843K           27

#define HC16XX_CLK_SFSPI              28
#define HC16XX_CLK_SDMMC              29
#define HC16XX_CLK_NAND               30
#define HC16XX_CLK_DESCRAMBLE         31
#define HC16XX_CLK_TSG                32
#define HC16XX_CLK_DEMUX1             33
#define HC16XX_CLK_DEMUX2_CI          34
#define HC16XX_CLK_TS_SWITCH          35
#define HC16XX_CLK_ETH_MAC            36
#define HC16XX_CLK_SAR_ADC            37
#define HC16XX_CLK_VDEC               38
#define HC16XX_CLK_DDR_PHY_APB        39
#define HC16XX_CLK_DFI                40
#define HC16XX_CLK_UART0              41
#define HC16XX_CLK_UART1              42
#define HC16XX_CLK_I2C0               43
#define HC16XX_CLK_HDMI_TX_PHY        44
#define HC16XX_CLK_WATCHDOG           45
#define HC16XX_CLK_TIMER              46
#define HC16XX_CLK_I2C1               47
#define HC16XX_CLK_I2C2               48
#define HC16XX_CLK_I2C3               49
#define HC16XX_CLK_SB_APB             50

#define HC16XX_CLK_VOU_HD             51
#define HC16XX_CLK_VE                 52
#define HC16XX_CLK_DE                 53
#define HC16XX_CLK_TVENC_SD           54
#define HC16XX_CLK_GE                 55
#define HC16XX_CLK_TVENC_HD           56
#define HC16XX_CLK_VIN                57
#define HC16XX_CLK_DE_HDMI            58
#define HC16XX_CLK_VDAC1              59
#define HC16XX_CLK_VDAC2              60
#define HC16XX_CLK_VDAC3              61
#define HC16XX_CLK_UART23             62
#define HC16XX_CLK_SGDMA1             63
#define HC16XX_CLK_SGDMA2             64
#define HC16XX_CLK_HDMI_TX            65
#define HC16XX_CLK_AUDIO              66
#define HC16XX_CLK_SPI                67
#define HC16XX_CLK_DVI                68
#define HC16XX_CLK_VOU_HD_EXT         69
#define HC16XX_CLK_SP0                70
#define HC16XX_CLK_DDP_SP0            71
#define HC16XX_CLK_TSDMA              72
#define HC16XX_CLK_JENC               73
#define HC16XX_CLK_USB0               74
#define HC16XX_CLK_USB1               75
#define HC16XX_CLK_HDMI_TX_PHY_SYS    76
#define HC16XX_CLK_MIPI               77
#define HC16XX_CLK_BOOTROM            78
#define HC16XX_CLK_TVDEC              79

#define HC16XX_CLK_NR_CLKS            80

#endif
