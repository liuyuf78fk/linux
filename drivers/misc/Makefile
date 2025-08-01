# SPDX-License-Identifier: GPL-2.0
#
# Makefile for misc devices that really don't fit anywhere else.
#

obj-$(CONFIG_IBM_ASM)		+= ibmasm/
obj-$(CONFIG_IBMVMC)		+= ibmvmc.o
obj-$(CONFIG_AD525X_DPOT)	+= ad525x_dpot.o
obj-$(CONFIG_AD525X_DPOT_I2C)	+= ad525x_dpot-i2c.o
obj-$(CONFIG_AD525X_DPOT_SPI)	+= ad525x_dpot-spi.o
obj-$(CONFIG_ATMEL_SSC)		+= atmel-ssc.o
obj-$(CONFIG_DUMMY_IRQ)		+= dummy-irq.o
obj-$(CONFIG_ICS932S401)	+= ics932s401.o
obj-$(CONFIG_LKDTM)		+= lkdtm/
obj-$(CONFIG_TI_FPC202)		+= ti_fpc202.o
obj-$(CONFIG_TIFM_CORE)       	+= tifm_core.o
obj-$(CONFIG_TIFM_7XX1)       	+= tifm_7xx1.o
obj-$(CONFIG_PHANTOM)		+= phantom.o
obj-$(CONFIG_RPMB)		+= rpmb-core.o
obj-$(CONFIG_QCOM_COINCELL)	+= qcom-coincell.o
obj-$(CONFIG_QCOM_FASTRPC)	+= fastrpc.o
obj-$(CONFIG_SENSORS_BH1770)	+= bh1770glc.o
obj-$(CONFIG_SENSORS_APDS990X)	+= apds990x.o
obj-$(CONFIG_ENCLOSURE_SERVICES) += enclosure.o
obj-$(CONFIG_KGDB_TESTS)	+= kgdbts.o
obj-$(CONFIG_TEST_MISC_MINOR)	+= misc_minor_kunit.o
obj-$(CONFIG_SGI_XP)		+= sgi-xp/
obj-$(CONFIG_SGI_GRU)		+= sgi-gru/
obj-$(CONFIG_SMPRO_ERRMON)	+= smpro-errmon.o
obj-$(CONFIG_SMPRO_MISC)	+= smpro-misc.o
obj-$(CONFIG_CS5535_MFGPT)	+= cs5535-mfgpt.o
obj-$(CONFIG_GEHC_ACHC)		+= gehc-achc.o
obj-$(CONFIG_HP_ILO)		+= hpilo.o
obj-$(CONFIG_APDS9802ALS)	+= apds9802als.o
obj-$(CONFIG_ISL29003)		+= isl29003.o
obj-$(CONFIG_ISL29020)		+= isl29020.o
obj-$(CONFIG_SENSORS_TSL2550)	+= tsl2550.o
obj-$(CONFIG_DS1682)		+= ds1682.o
obj-$(CONFIG_C2PORT)		+= c2port/
obj-$(CONFIG_HMC6352)		+= hmc6352.o
obj-y				+= eeprom/
obj-y				+= cb710/
obj-$(CONFIG_VMWARE_BALLOON)	+= vmw_balloon.o
obj-$(CONFIG_PCH_PHUB)		+= pch_phub.o
obj-y				+= lis3lv02d/
obj-$(CONFIG_ALTERA_STAPL)	+=altera-stapl/
obj-$(CONFIG_INTEL_MEI)		+= mei/
obj-$(CONFIG_VMWARE_VMCI)	+= vmw_vmci/
obj-$(CONFIG_LATTICE_ECP3_CONFIG)	+= lattice-ecp3-config.o
obj-$(CONFIG_SRAM)		+= sram.o
obj-$(CONFIG_SRAM_EXEC)		+= sram-exec.o
obj-$(CONFIG_GENWQE)		+= genwqe/
obj-$(CONFIG_DW_XDATA_PCIE)	+= dw-xdata-pcie.o
obj-$(CONFIG_PCI_ENDPOINT_TEST)	+= pci_endpoint_test.o
obj-$(CONFIG_OCXL)		+= ocxl/
obj-$(CONFIG_BCM_VK)		+= bcm-vk/
obj-y				+= cardreader/
obj-$(CONFIG_PVPANIC)   	+= pvpanic/
obj-$(CONFIG_UACCE)		+= uacce/
obj-$(CONFIG_XILINX_SDFEC)	+= xilinx_sdfec.o
obj-$(CONFIG_HISI_HIKEY_USB)	+= hisi_hikey_usb.o
obj-$(CONFIG_NTSYNC)		+= ntsync.o
obj-$(CONFIG_HI6421V600_IRQ)	+= hi6421v600-irq.o
obj-$(CONFIG_OPEN_DICE)		+= open-dice.o
obj-$(CONFIG_GP_PCI1XXXX)	+= mchp_pci1xxxx/
obj-$(CONFIG_VCPU_STALL_DETECTOR)	+= vcpu_stall_detector.o
obj-$(CONFIG_TMR_MANAGER)      += xilinx_tmr_manager.o
obj-$(CONFIG_TMR_INJECT)	+= xilinx_tmr_inject.o
obj-$(CONFIG_TPS6594_ESM)	+= tps6594-esm.o
obj-$(CONFIG_TPS6594_PFSM)	+= tps6594-pfsm.o
obj-$(CONFIG_NSM)		+= nsm.o
obj-$(CONFIG_MARVELL_CN10K_DPI)	+= mrvl_cn10k_dpi.o
lan966x-pci-objs		:= lan966x_pci.o
lan966x-pci-objs		+= lan966x_pci.dtbo.o
obj-$(CONFIG_MCHP_LAN966X_PCI)	+= lan966x-pci.o
obj-y				+= keba/
obj-y				+= amd-sbi/
obj-$(CONFIG_MISC_RP1)		+= rp1/
