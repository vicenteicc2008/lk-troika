/*
 * usb.c
 *
 *  Created on: 2019. 3. 25.
 *      Author: sunghyun.na
 */

#include <debug.h>
#include <string.h>
#include <reg.h>
#include <malloc.h>
#include <lk/init.h>
#include <list.h>
#include <err.h>

#include <usb-def.h>
#include "dev/usb/gadget.h"
#include "dev/usb/dwc3-config.h"
#include "dev/usb/phy-samsung-usb-cal.h"
#include "dev/usb/fastboot.h"
#include "platform/sfr.h"

#include <part.h>

#define SYSREG_USB_BASE			0x13020000
#define USB_SHARABLE_OFFSET		(0x700)
#define USB_SHARABLE_SHIFT		(12)

#define EXYNOS_POWER_BASE		0x10E60000

#define USB_LINK_BASE			0x13200000
#define USB_PHY_BASE			0x131F0000

/* PHY CONTROL */
#define USBDP_PHY_CONTROL_OFFSET	0x730
#define USB2_PHY_CONTROL_OFFSET		0x72c

#define USB_INT_NUM			370

static unsigned int dwc3_isr_num = (USB_INT_NUM + 32);

void gadget_probe_pid_vid_version(unsigned short *vid, unsigned short *pid, unsigned short *bcd_version)
{
	*vid = 0x18D1;
	*pid = 0x0002;
	*bcd_version = 0x0100;
}

static const char vendor_str[] = "Samsung Semiconductor, S.LSI Division";
static const char product_str[] = "Exynos9630 LK Bootloader";
static char serial_id[16] = "No Serial";

int gadget_get_vendor_string(void)
{
	return get_str_id(vendor_str, strlen(vendor_str));
}

int gadget_get_product_string(void)
{

	return get_str_id(product_str, strlen(product_str));
}

void reserve_serialno_string(void)
{
	char tmp[16];
	int i;

	memcpy(tmp, serial_id, sizeof(serial_id));

	for (i = 0; i < 16; i++)
		serial_id[i]  = tmp[15 - i];
}

void get_serialno(int *chip_id)
{
	chip_id[0] = readl(EXYNOS9630_PRO_ID + CHIPID0_OFFSET);
	chip_id[1] = readl(EXYNOS9630_PRO_ID + CHIPID1_OFFSET) & 0xFFFF;
}

static const char *make_serial_string(void)
{
	u8 i, j;
	int chip_id[2];

	if (strcmp(serial_id, "No Serial"))
		return serial_id;

	chip_id[0] = readl(EXYNOS9630_PRO_ID + CHIPID0_OFFSET);
	chip_id[1] = readl(EXYNOS9630_PRO_ID + CHIPID1_OFFSET) & 0xFFFF;
	for (j = 0; j < 2; j++) {
		u32 hex;
		char *str;

		hex = chip_id[j];
		str = &serial_id[j * 8];
		for (i = 0; i < 8; i++) {
			if ((hex & 0xF) > 9)
				*str++ = 'a' + (hex & 0xF) - 10;
			else
				*str++ = '0' + (hex & 0xF);
			hex >>= 4;
		}
	}

	reserve_serialno_string();

	return serial_id;
}

int gadget_get_serial_string(void)
{
	return get_str_id(make_serial_string(), 16);
}

const char *fastboot_get_product_string(void)
{
	return product_str;
}

const char *fastboot_get_serialno_string(void)
{
	return make_serial_string();
}

//#define FASTBOOT_VERSION_BOOTLOADER	"EXYNOS9630-LK-20190509-1"

static struct cmd_fastboot_variable fastboot_var_list[CMD_FASTBOOT_MAX_VAR_NR];
static int fastboot_var_nr = 0;

int fastboot_get_var_num(void)
{
	return fastboot_var_nr;
}

struct cmd_fastboot_variable *fastboot_get_var_head(void)
{
	return fastboot_var_list;
}

static int add_fastboot_variable(const char *name, const char *string)
{
	int name_len;
	int string_len;

	if (name != NULL) {
		name_len = strlen(name);
	} else {
		printf("Input string is null\n");
		return -1;
	}

	if (string != NULL) {
		string_len = strlen(string);
	} else {
		printf("Input string is null\n");
		return -1;
	}

	if (name_len < CMD_FASTBOOT_MAX_VAR_LEN) {
		strncpy((void *)&fastboot_var_list[fastboot_var_nr].name, name, name_len);
	} else {
		printf("Input string size is bigger than buffer size\n");
		return -1;
	}

	if (name_len < CMD_FASTBOOT_MAX_VAR_LEN) {
		strncpy((void *)&fastboot_var_list[fastboot_var_nr].string, string, string_len);
	} else {
		printf("Input string size is bigger than buffer size\n");
		return -1;
	}

	fastboot_var_nr++;

	return 0;
}

int init_fastboot_variables(void)
{
	char tmp[64] = {0};
	void *part;

	memset(fastboot_var_list, 0, sizeof(struct cmd_fastboot_variable) * CMD_FASTBOOT_MAX_VAR_NR);

	add_fastboot_variable("version-baseband", "N/A");
	add_fastboot_variable("version", FASTBOOT_VERSION);
	add_fastboot_variable("version-bootloader", FASTBOOT_VERSION_BOOTLOADER);
	add_fastboot_variable("product", "exynos9630");
	add_fastboot_variable("serialno", (const char *)serial_id);
	add_fastboot_variable("secure", "yes");
	add_fastboot_variable("unlocked", "yes");
	add_fastboot_variable("off-mode-charge", "0");
	add_fastboot_variable("variant", "exynos9630");
	add_fastboot_variable("battery-voltage", "2700mV");
	add_fastboot_variable("battery-soc-ok", "yes");
	add_fastboot_variable("partition-type:efs", "ext4");
	part = part_get("efs");
	sprintf(tmp, "0x%llx", part_get_size_in_bytes(part));
	add_fastboot_variable("partition-size:efs", (const char *)tmp);
	add_fastboot_variable("partition-type:efsbk", "ext4");
	part = part_get("efsbk");
	sprintf(tmp, "0x%llx", part_get_size_in_bytes(part));
	add_fastboot_variable("partition-size:efsbk", (const char *)tmp);
	add_fastboot_variable("partition-type:persist", "ext4");
	part = part_get("persist");
	sprintf(tmp, "0x%llx", part_get_size_in_bytes(part));
	add_fastboot_variable("partition-size:persist", (const char *)tmp);
	add_fastboot_variable("partition-type:metadata", "ext4");
	part = part_get("metadata");
	sprintf(tmp, "0x%llx", part_get_size_in_bytes(part));
	add_fastboot_variable("partition-size:metadata", (const char *)tmp);
	add_fastboot_variable("partition-type:system_a", "ext4");
	part = part_get("system_a");
	sprintf(tmp, "0x%llx", part_get_size_in_bytes(part));
	add_fastboot_variable("partition-size:system_a", (const char *)tmp);
	add_fastboot_variable("partition-type:system_b", "ext4");
	part = part_get("system_b");
	sprintf(tmp, "0x%llx", part_get_size_in_bytes(part));
	add_fastboot_variable("partition-size:system_b", (const char *)tmp);
	add_fastboot_variable("partition-type:vendor_a", "ext4");
	part = part_get("vendor_a");
	sprintf(tmp, "0x%llx", part_get_size_in_bytes(part));
	add_fastboot_variable("partition-size:vendor_a", (const char *)tmp);
	add_fastboot_variable("partition-type:vendor_b", "ext4");
	part = part_get("vendor_b");
	sprintf(tmp, "0x%llx", part_get_size_in_bytes(part));
	add_fastboot_variable("partition-size:vendor_b", (const char *)tmp);
	add_fastboot_variable("partition-type:userdata", "ext4");
	part = part_get("userdata");
	sprintf(tmp, "0x%llx", part_get_size_in_bytes(part));
	add_fastboot_variable("partition-size:userdata", (const char *)tmp);
	sprintf(tmp, "0x%x", CFG_FASTBOOT_TRANSFER_BUFFER_SIZE);
	add_fastboot_variable("max-download-size", (const char *)tmp);
	sprintf(tmp, "0x%x", 0x1000);
	add_fastboot_variable("erase-block-size", (const char *)tmp);
	add_fastboot_variable("logical-block-size", (const char *)tmp);
	add_fastboot_variable("has-slot:efs", "no");
	add_fastboot_variable("has-slot:efsbk", "no");
	add_fastboot_variable("has-slot:persist", "no");
	add_fastboot_variable("has-slot:metadata", "no");
	add_fastboot_variable("has-slot:system", "yes");
	add_fastboot_variable("has-slot:vendor", "yes");
	add_fastboot_variable("has-slot:userdata", "no");
	add_fastboot_variable("current-slot", "a");
	add_fastboot_variable("slot-count", "2");
	add_fastboot_variable("slot-successful", "a:yes");
	add_fastboot_variable("slot-unbootable", "a:no");
	add_fastboot_variable("slot-retry-count", "a:0");
	add_fastboot_variable("slot-successful", "b:no");
	add_fastboot_variable("slot-unbootable", "b:no");
	add_fastboot_variable("slot-retry-count", "b:7");

	return 0;
}

int dwc3_plat_init(struct dwc3_plat_config *plat_config)
{
	plat_config->base = (void *) USB_LINK_BASE;
	plat_config->num_hs_phy = 1;
	plat_config->array_intr = &dwc3_isr_num;
	plat_config->num_intr = 1;
	strcpy(plat_config->ssphy_type, "snps_gen1");

	return 0;
}

static struct dwc3_dev_config dwc3_dev_config = {
	.speed = "high",
	.m_uEventBufDepth = 64,
	.m_uCtrlBufSize = 128,
	.m_ucU1ExitValue = 10,
	.m_usU2ExitValue = 257,
};

int dwc3_dev_plat_init(void **base_addr, struct dwc3_dev_config **plat_config)
{
	*base_addr = (void *) (USB_LINK_BASE);
	*plat_config = &dwc3_dev_config;
	return 0;
}

static struct exynos_usb_tune_param usbcal_20phy_tune[] = {
	{ .name = "tx_pre_emp", .value = 0x3, },
	{ .name = "tx_vref", .value = 0x7, },
	{ .name = "rx_sqrx", .value = 0x5, },
	{ .name = "utim_clk", .value = USBPHY_UTMI_PHYCLOCK, },
	{ .value = EXYNOS_USB_TUNE_LAST, },
};

static struct exynos_usbphy_info usbphy_cal_info = {
	.version = EXYNOS_USBCON_VER_03_0_0,
	.refclk = USBPHY_REFCLK_DIFF_26MHZ,
	.refsel = USBPHY_REFSEL_CLKCORE,
	.not_used_vbus_pad = true,
	.regs_base = (void *) USB_PHY_BASE,
	.tune_param = usbcal_20phy_tune,
	.hs_rewa = 1,
};

static void register_phy_cal_infor(uint level)
{
	phy_usb_exynos_register_cal_infor(&usbphy_cal_info);
}
LK_INIT_HOOK(register_phy_cal_infor, &register_phy_cal_infor, LK_INIT_LEVEL_KERNEL);

void phy_usb_exynos_system_init(int num_phy_port, bool en)
{
	u32 reg;

	dprintf(ALWAYS, "%s called: %d\n", __func__, en);

	/* 2.0 HS PHY */
	/* PMU Isolation release */
	if (en)
		writel(1, (void *)(EXYNOS_POWER_BASE + USB2_PHY_CONTROL_OFFSET));
	else
		writel(0, (void *)(EXYNOS_POWER_BASE + USB2_PHY_CONTROL_OFFSET));

	if (en)
		writel(1, (void *)(EXYNOS_POWER_BASE + USBDP_PHY_CONTROL_OFFSET));
	else
		writel(0, (void *)(EXYNOS_POWER_BASE + USBDP_PHY_CONTROL_OFFSET));

	/* CCI Enable */
	reg = readl((void *)(SYSREG_USB_BASE + USB_SHARABLE_OFFSET));
	if (en)
		reg |= (0x3 << USB_SHARABLE_SHIFT);
	else
		reg &= ~(0x3 << USB_SHARABLE_SHIFT);
	writel(reg, SYSREG_USB_BASE + USB_SHARABLE_OFFSET);
}

/* Fastboot command related function */
#include <dev/rpmb.h>
#include <dev/scsi.h>
void platform_prepare_reboot(void)
{
	/*
	 * Send SSU to UFS. Something wrong on SSU should not
	 * affect reboot sequence.
	 */
	scsi_do_ssu();
}

void platform_do_reboot(const char *cmd_buf)
{
	if (!memcmp(cmd_buf, "reboot-bootloader", strlen("reboot-bootloader"))) {
		writel(REBOOT_MODE_FASTBOOT, EXYNOS9630_POWER_SYSIP_DAT0);
	} else if (!memcmp(cmd_buf, "reboot-recovery", strlen("reboot-recovery"))) {
		writel(REBOOT_MODE_RECOVERY, EXYNOS9630_POWER_SYSIP_DAT0);
		writel(0, CONFIG_RAMDUMP_SCRATCH);
	} else if (!memcmp(cmd_buf, "reboot-fastboot", strlen("reboot-fastboot"))) {
		writel(REBOOT_MODE_FASTBOOT_USER, EXYNOS9630_POWER_SYSIP_DAT0);
		writel(0, CONFIG_RAMDUMP_SCRATCH);
	} else {
		writel(0, EXYNOS9630_POWER_SYSIP_DAT0);
		writel(0, CONFIG_RAMDUMP_SCRATCH);
	}

	writel(readl(EXYNOS9630_SYSTEM_CONFIGURATION) | 0x2, EXYNOS9630_SYSTEM_CONFIGURATION);

	return;
}
