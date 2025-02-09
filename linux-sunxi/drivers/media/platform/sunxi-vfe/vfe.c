/*
 * sunxi Camera Interface  driver
 * Author: raymonxiu
 */
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/mutex.h>
#include <linux/videodev2.h>
#include <linux/delay.h>
#include <linux/string.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 20)
#include <linux/freezer.h>
#endif

#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/moduleparam.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-common.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-dma-contig.h>

#include <linux/regulator/consumer.h>
#ifdef CONFIG_DEVFREQ_DRAM_FREQ_WITH_SOFT_NOTIFY
#include <linux/sunxi_dramfreq.h>
#endif

#include "vfe.h"

#include "bsp_common.h"
#include "lib/bsp_isp_algo.h"
#include "csi_cci/bsp_cci.h"
#include "csi_cci/cci_helper.h"
#include "config.h"
#include "device/camera_cfg.h"
#include "utility/sensor_info.h"
#include "utility/vfe_io.h"
#include "csi/sunxi_csi.h"
#include "sunxi_isp.h"
#include "mipi_csi/sunxi_mipi.h"

#define IS_FLAG(x, y) (((x)&(y)) == y)
#define CLIP_MAX(x, max) ((x) > max ? max : x)

#define VFE_MAJOR_VERSION 1
#define VFE_MINOR_VERSION 0
#define VFE_RELEASE       0
#define VFE_VERSION \
  KERNEL_VERSION(VFE_MAJOR_VERSION, VFE_MINOR_VERSION, VFE_RELEASE)
#define VFE_MODULE_NAME "sunxi_vfe"
#define VID_N_OFF      8

#define MCLK_OUT_RATE   (24*1000*1000)
#define MAX_FRAME_MEM   (150*1024*1024)
#define MIN_WIDTH       (32)
#define MIN_HEIGHT      (32)
#define MAX_WIDTH       (4800)
#define MAX_HEIGHT      (4800)
#define DUMP_CSI       (1 << 0)
#define DUMP_ISP      (1 << 1)

/* #define _REGULATOR_CHANGE_ */
struct vfe_dev *vfe_dev_gbl[2];

static char ccm[I2C_NAME_SIZE] = "";
static uint i2c_addr = 0xff;

static char act_name[I2C_NAME_SIZE] = "";
static uint act_slave = 0xff;
static uint define_sensor_list = 0xff;
static uint vfe_i2c_dbg;
static uint isp_log;
static uint vips = 0xffff;

static int touch_flash_flag;
static int ev_cumul;

unsigned int isp_reparse_flag;

static unsigned int frame_cnt;
static unsigned int vfe_dump;
struct mutex probe_hdl_lock;
struct file *fp_dbg;
static char LogFileName[128] = "/system/etc/hawkview/log.bin";

module_param_string(ccm, ccm, sizeof(ccm), S_IRUGO | S_IWUSR);
module_param(i2c_addr, uint, S_IRUGO | S_IWUSR);

module_param_string(act_name, act_name, sizeof(act_name), S_IRUGO | S_IWUSR);
module_param(act_slave, uint, S_IRUGO | S_IWUSR);
module_param(define_sensor_list, uint, S_IRUGO | S_IWUSR);
module_param(vfe_i2c_dbg, uint, S_IRUGO | S_IWUSR);
module_param(isp_log, uint, S_IRUGO | S_IWUSR);
module_param(vips, uint, S_IRUGO | S_IWUSR);

static ssize_t vfe_dbg_en_show(struct device *dev,
		    struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", vfe_dbg_en);
}

static ssize_t vfe_dbg_en_store(struct device *dev,
		    struct device_attribute *attr,
		    const char *buf, size_t count)
{
	int err;
	unsigned long val;

	err = kstrtoul(buf, 10, &val);
	if (err) {
		vfe_print("Invalid size\n");
		return err;
	}

	if (val < 0 || val > 1) {
		vfe_print("Invalid value, 0~1 is expected!\n");
	} else {
		vfe_dbg_en = val;
		vfe_print("vfe_dbg_en = %ld\n", val);
	}

	return count;
}

static ssize_t vfe_dbg_lv_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", vfe_dbg_lv);
}

static ssize_t vfe_dbg_lv_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	int err;
	unsigned long val;

	err = kstrtoul(buf, 10, &val);
	if (err) {
		vfe_print("Invalid size\n");
		return err;
	}

	if (val < 0 || val > 4) {
		vfe_print("Invalid value, 0~3 is expected!\n");
	} else {
		vfe_dbg_lv = val;
		vfe_print("vfe_dbg_lv = %d\n", vfe_dbg_lv);
	}

	return count;
}

static ssize_t isp_reparse_flag_show(struct device *dev,
		    struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", isp_reparse_flag);
}

static ssize_t isp_reparse_flag_store(struct device *dev,
		    struct device_attribute *attr,
		    const char *buf, size_t count)
{
	int err;
	unsigned long val;

	err = kstrtoul(buf, 10, &val);
	if (err) {
		vfe_print("Invalid size\n");
		return err;
	}

	if (val < 0 || val > 4) {
		vfe_print("Invalid value, 0~1 is expected!\n");
	} else {
		isp_reparse_flag = val;
		vfe_print("isp_reparse_flag = %ld\n", val);
	}

	return count;
}
static ssize_t vfe_dbg_dump_show(struct device *dev,
		    struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", vfe_dump);
}

static ssize_t vfe_dbg_dump_store(struct device *dev,
		    struct device_attribute *attr,
		    const char *buf, size_t count)
{
	int err;
	unsigned long val;

	err = kstrtoul(buf, 10, &val);
	if (err) {
		vfe_print("Invalid size\n");
		return err;
	}

	if (val < 0 || val > 3) {
		vfe_print("Invalid value, 0~3 is expected!\n");
	} else {
		vfe_dump = val;
		vfe_print("vfe_dump = %ld\n", val);
	}

	return count;
}

static DEVICE_ATTR(vfe_dbg_en, S_IRUGO | S_IWUSR | S_IWGRP,
		vfe_dbg_en_show, vfe_dbg_en_store);

static DEVICE_ATTR(vfe_dbg_lv, S_IRUGO | S_IWUSR | S_IWGRP,
		vfe_dbg_lv_show, vfe_dbg_lv_store);
static DEVICE_ATTR(vfe_dump, S_IRUGO | S_IWUSR | S_IWGRP,
		vfe_dbg_dump_show, vfe_dbg_dump_store);
static DEVICE_ATTR(isp_reparse_flag, S_IRUGO | S_IWUSR | S_IWGRP,
		isp_reparse_flag_show, isp_reparse_flag_store);

static struct attribute *vfe_attributes[] = {
	&dev_attr_vfe_dbg_en.attr,
	&dev_attr_vfe_dbg_lv.attr,
	&dev_attr_vfe_dump.attr,
	&dev_attr_isp_reparse_flag.attr,
	NULL
};

static struct attribute_group vfe_attribute_group = {
	.name = "vfe_attr",
	.attrs = vfe_attributes
};

static struct vfe_fmt formats[] = {
	{
		.name         = "planar YUV 422",
		.fourcc       = V4L2_PIX_FMT_YUV422P,
		.depth        = 16,
		.planes_cnt   = 3,
	},
	{
		.name         = "planar YUV 420",
		.fourcc       = V4L2_PIX_FMT_YUV420,
		.depth        = 12,
		.planes_cnt   = 3,
	},
	{
		.name         = "planar YVU 420",
		.fourcc       = V4L2_PIX_FMT_YVU420,
		.depth        = 12,
		.planes_cnt   = 3,
	},
	{
		.name         = "planar YUV 422 UV combined",
		.fourcc       = V4L2_PIX_FMT_NV16,
		.depth        = 16,
		.planes_cnt   = 2,
	},
	{
		.name         = "planar YUV 420 UV combined",
		.fourcc       = V4L2_PIX_FMT_NV12,
		.depth        = 12,
		.planes_cnt   = 2,
	},
	{
		.name         = "planar YUV 422 VU combined",
		.fourcc       = V4L2_PIX_FMT_NV61,
		.depth        = 16,
		.planes_cnt   = 2,
	},
	{
		.name         = "planar YUV 420 VU combined",
		.fourcc       = V4L2_PIX_FMT_NV21,
		.depth        = 12,
		.planes_cnt   = 2,
	},
	{
		.name         = "MB YUV420",
		.fourcc       = V4L2_PIX_FMT_HM12,
		.depth        = 12,
		.planes_cnt   = 2,
	},
	{
		.name         = "YUV422 YUYV",
		.fourcc       = V4L2_PIX_FMT_YUYV,
		.depth        = 16,
		.planes_cnt   = 1,
	},
	{
		.name         = "YUV422 YVYU",
		.fourcc       = V4L2_PIX_FMT_YVYU,
		.depth        = 16,
		.planes_cnt   = 1,
	},
	{
		.name         = "YUV422 UYVY",
		.fourcc       = V4L2_PIX_FMT_UYVY,
		.depth        = 16,
		.planes_cnt   = 1,
	},
	{
		.name         = "YUV422 VYUY",
		.fourcc       = V4L2_PIX_FMT_VYUY,
		.depth        = 16,
		.planes_cnt   = 1,
	},
	{
		.name         = "RAW Bayer BGGR 8bit",
		.fourcc       = V4L2_PIX_FMT_SBGGR8,
		.depth        = 8,
		.planes_cnt   = 1,
	},
	{
		.name         = "RAW Bayer GBRG 8bit",
		.fourcc       = V4L2_PIX_FMT_SGBRG8,
		.depth        = 8,
		.planes_cnt   = 1,
	},
	{
		.name         = "RAW Bayer GRBG 8bit",
		.fourcc       = V4L2_PIX_FMT_SGRBG8,
		.depth        = 8,
		.planes_cnt   = 1,
	},
	{
		.name         = "RAW Bayer RGGB 8bit",
		.fourcc       = V4L2_PIX_FMT_SGRBG8,
		.depth        = 8,
		.planes_cnt   = 1,
	},
	{
		.name         = "RAW Bayer BGGR 10bit",
		.fourcc       = V4L2_PIX_FMT_SBGGR10,
		.depth        = 8,
		.planes_cnt   = 1,
	},
	{
		.name         = "RAW Bayer GBRG 10bit",
		.fourcc       = V4L2_PIX_FMT_SGBRG10,
		.depth        = 8,
		.planes_cnt   = 1,
	},
	{
		.name         = "RAW Bayer GRBG 10bit",
		.fourcc       = V4L2_PIX_FMT_SGRBG10,
		.depth        = 8,
		.planes_cnt   = 1,
	},
	{
		.name         = "RAW Bayer RGGB 10bit",
		.fourcc       = V4L2_PIX_FMT_SGRBG10,
		.depth        = 8,
		.planes_cnt   = 1,
	},
	{
		.name         = "RAW Bayer BGGR 12bit",
		.fourcc       = V4L2_PIX_FMT_SBGGR12,
		.depth        = 8,
		.planes_cnt   = 1,
	},
	{
		.name         = "RAW Bayer GBRG 12bit",
		.fourcc       = V4L2_PIX_FMT_SGBRG12,
		.depth        = 8,
		.planes_cnt   = 1,
	},
	{
		.name         = "RAW Bayer GRBG 12bit",
		.fourcc       = V4L2_PIX_FMT_SGRBG12,
		.depth        = 8,
		.planes_cnt   = 1,
	},
	{
		.name         = "RAW Bayer RGGB 12bit",
		.fourcc       = V4L2_PIX_FMT_SGRBG12,
		.depth        = 8,
		.planes_cnt   = 1,
	},
};

u32 try_yuv422_bus[] = {
	MEDIA_BUS_FMT_VYUY10_2X10,
	MEDIA_BUS_FMT_UYVY10_2X10,
	MEDIA_BUS_FMT_YUYV10_2X10,
	MEDIA_BUS_FMT_YVYU10_2X10,
	MEDIA_BUS_FMT_UYVY8_2X8,
	MEDIA_BUS_FMT_VYUY8_2X8,
	MEDIA_BUS_FMT_YUYV8_2X8,
	MEDIA_BUS_FMT_YVYU8_2X8,
	MEDIA_BUS_FMT_YUYV10_1X20,
	MEDIA_BUS_FMT_YVYU10_1X20,
	MEDIA_BUS_FMT_UYVY8_1X16,
	MEDIA_BUS_FMT_VYUY8_1X16,
	MEDIA_BUS_FMT_YUYV8_1X16,
	MEDIA_BUS_FMT_YVYU8_1X16,
	MEDIA_BUS_FMT_AYUV8_1X32,
};

u32 try_yuv420_bus[] = {
	MEDIA_BUS_FMT_VYUY10_1X20,
	MEDIA_BUS_FMT_UYVY10_1X20,
};

u32 try_bayer_rgb_bus[] = {
	MEDIA_BUS_FMT_SBGGR12_1X12,
	MEDIA_BUS_FMT_SGBRG12_1X12,
	MEDIA_BUS_FMT_SGRBG12_1X12,
	MEDIA_BUS_FMT_SRGGB12_1X12,
	MEDIA_BUS_FMT_SBGGR10_1X10,
	MEDIA_BUS_FMT_SGBRG10_1X10,
	MEDIA_BUS_FMT_SGRBG10_1X10,
	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_SBGGR10_DPCM8_1X8,
	MEDIA_BUS_FMT_SGBRG10_DPCM8_1X8,
	MEDIA_BUS_FMT_SGRBG10_DPCM8_1X8,
	MEDIA_BUS_FMT_SRGGB10_DPCM8_1X8,
	MEDIA_BUS_FMT_SBGGR10_2X8_PADHI_BE,
	MEDIA_BUS_FMT_SBGGR10_2X8_PADHI_LE,
	MEDIA_BUS_FMT_SBGGR10_2X8_PADLO_BE,
	MEDIA_BUS_FMT_SBGGR10_2X8_PADLO_LE,
	MEDIA_BUS_FMT_SBGGR10_ALAW8_1X8,
	MEDIA_BUS_FMT_SGBRG10_ALAW8_1X8,
	MEDIA_BUS_FMT_SGRBG10_ALAW8_1X8,
	MEDIA_BUS_FMT_SRGGB10_ALAW8_1X8,
	MEDIA_BUS_FMT_SBGGR8_1X8,
	MEDIA_BUS_FMT_SGBRG8_1X8,
	MEDIA_BUS_FMT_SGRBG8_1X8,
	MEDIA_BUS_FMT_SRGGB8_1X8,
};

u32 try_rgb565_bus[] = {
	MEDIA_BUS_FMT_ARGB8888_1X32,
	MEDIA_BUS_FMT_BGR565_2X8_BE,
	MEDIA_BUS_FMT_BGR565_2X8_LE,
	MEDIA_BUS_FMT_RGB565_2X8_BE,
	MEDIA_BUS_FMT_RGB565_2X8_LE,
};

u32 try_rgb888_bus[] = {
	MEDIA_BUS_FMT_RBG888_1X24,
};

struct vfe_fmt_code vfe_fmt_code[] = {
	{
		.bus_name = "bayer",
		.pix_name = "yuv422/yuv420",
		.mbus_code = try_bayer_rgb_bus,
		.size = ARRAY_SIZE(try_bayer_rgb_bus),
	},
	{
		.bus_name = "yuv422",
		.pix_name = "yuv422/yuv420",
		.mbus_code = try_yuv422_bus,
		.size = ARRAY_SIZE(try_yuv422_bus),
	},
	{
		.bus_name = "yuv420",
		.pix_name = "yuv422/yuv420",
		.mbus_code = try_yuv420_bus,
		.size = ARRAY_SIZE(try_yuv420_bus),
	},
	{
		.bus_name = "rgb565",
		.pix_name = "rgb565",
		.mbus_code = try_rgb565_bus,
		.size = ARRAY_SIZE(try_rgb565_bus),
	},
	{
		.bus_name = "rgb888",
		.pix_name = "rgb888/prgb888",
		.mbus_code = try_rgb888_bus,
		.size = ARRAY_SIZE(try_rgb888_bus),
	},
};

static int isp_resource_request(struct vfe_dev *dev)
{
	unsigned int isp_used_flag = 0, i;
	void *pa_base, *va_base, *dma_base;
	int ret;

	/* requeset for isp table and statistic buffer */
	for (i = 0; i < dev->dev_qty; i++) {
		if (dev->ccm_cfg[i]->is_isp_used && dev->ccm_cfg[i]->is_bayer_raw) {
			dev->isp_lut_tbl_buf_mm[i].size = ISP_LINEAR_LUT_LENS_GAMMA_MEM_SIZE;
			ret = os_mem_alloc(&dev->isp_lut_tbl_buf_mm[i]);
			if (!ret) {
				pa_base = dev->isp_lut_tbl_buf_mm[i].phy_addr;
				va_base = dev->isp_lut_tbl_buf_mm[i].vir_addr;
				dma_base = dev->isp_lut_tbl_buf_mm[i].dma_addr;
				dev->isp_tbl_addr[i].isp_def_lut_tbl_paddr = (void *)(pa_base + ISP_LUT_MEM_OFS);
				dev->isp_tbl_addr[i].isp_def_lut_tbl_dma_addr = (void *)(dma_base + ISP_LUT_MEM_OFS);
				dev->isp_tbl_addr[i].isp_def_lut_tbl_vaddr = (void *)(va_base + ISP_LUT_MEM_OFS);
				dev->isp_tbl_addr[i].isp_lsc_tbl_paddr = (void *)(pa_base + ISP_LENS_MEM_OFS);
				dev->isp_tbl_addr[i].isp_lsc_tbl_dma_addr = (void *)(dma_base + ISP_LENS_MEM_OFS);
				dev->isp_tbl_addr[i].isp_lsc_tbl_vaddr = (void *)(va_base + ISP_LENS_MEM_OFS);
				dev->isp_tbl_addr[i].isp_gamma_tbl_paddr = (void *)(pa_base + ISP_GAMMA_MEM_OFS);
				dev->isp_tbl_addr[i].isp_gamma_tbl_dma_addr = (void *)(dma_base + ISP_GAMMA_MEM_OFS);
				dev->isp_tbl_addr[i].isp_gamma_tbl_vaddr = (void *)(va_base + ISP_GAMMA_MEM_OFS);

				dev->isp_tbl_addr[i].isp_linear_tbl_paddr = (void *)(pa_base + ISP_LINEAR_MEM_OFS);
				dev->isp_tbl_addr[i].isp_linear_tbl_dma_addr = (void *)(dma_base + ISP_LINEAR_MEM_OFS);
				dev->isp_tbl_addr[i].isp_linear_tbl_vaddr = (void *)(va_base + ISP_LINEAR_MEM_OFS);
				vfe_dbg(0, "isp_def_lut_tbl_vaddr[%d] = %p\n", i, dev->isp_tbl_addr[i].isp_def_lut_tbl_vaddr);
				vfe_dbg(0, "isp_lsc_tbl_vaddr[%d] = %p\n", i, dev->isp_tbl_addr[i].isp_lsc_tbl_vaddr);
				vfe_dbg(0, "isp_gamma_tbl_vaddr[%d] = %p\n", i, dev->isp_tbl_addr[i].isp_gamma_tbl_vaddr);
			} else {
				vfe_err("isp lut_lens_gamma table request pa failed!\n");
				return -ENOMEM;
			}
		}

		if (dev->ccm_cfg[i]->is_isp_used && dev->ccm_cfg[i]->is_bayer_raw) {
			dev->isp_drc_tbl_buf_mm[i].size = ISP_DRC_DISC_MEM_SIZE;
			ret = os_mem_alloc(&dev->isp_drc_tbl_buf_mm[i]);
			if (!ret) {
				pa_base = dev->isp_drc_tbl_buf_mm[i].phy_addr;
				va_base = dev->isp_drc_tbl_buf_mm[i].vir_addr;
				dma_base = dev->isp_drc_tbl_buf_mm[i].dma_addr;

				dev->isp_tbl_addr[i].isp_drc_tbl_paddr = (void *)(pa_base + ISP_DRC_MEM_OFS);
				dev->isp_tbl_addr[i].isp_drc_tbl_dma_addr = (void *)(dma_base + ISP_DRC_MEM_OFS);
				dev->isp_tbl_addr[i].isp_drc_tbl_vaddr = (void *)(va_base + ISP_DRC_MEM_OFS);

				dev->isp_tbl_addr[i].isp_disc_tbl_paddr = (void *)(pa_base + ISP_DISC_MEM_OFS);
				dev->isp_tbl_addr[i].isp_disc_tbl_dma_addr = (void *)(dma_base + ISP_DISC_MEM_OFS);
				dev->isp_tbl_addr[i].isp_disc_tbl_vaddr = (void *)(va_base + ISP_DISC_MEM_OFS);

				vfe_dbg(0, "isp_drc_tbl_vaddr[%d] = %p\n", i, dev->isp_tbl_addr[i].isp_drc_tbl_vaddr);
			} else {
				vfe_err("isp drc table request pa failed!\n");
				return -ENOMEM;
			}
		}
	}

	for (i = 0; i < dev->dev_qty; i++) {
		if (dev->ccm_cfg[i]->is_isp_used && dev->ccm_cfg[i]->is_bayer_raw) {
			isp_used_flag = 1;
			break;
		}
	}

	if (isp_used_flag) {
		for (i = 0; i < MAX_ISP_STAT_BUF; i++) {
			dev->isp_stat_buf_mm[i].size = ISP_STAT_TOTAL_SIZE;
			ret = os_mem_alloc(&dev->isp_stat_buf_mm[i]);
			if (!ret) {
				pa_base = dev->isp_stat_buf_mm[i].phy_addr;
				va_base = dev->isp_stat_buf_mm[i].vir_addr;
				dma_base = dev->isp_stat_buf_mm[i].dma_addr;
				INIT_LIST_HEAD(&dev->isp_stat_bq.isp_stat[i].queue);
				dev->isp_stat_bq.isp_stat[i].id = i;
				dev->isp_stat_bq.isp_stat[i].paddr = (void *)(pa_base);
				dev->isp_stat_bq.isp_stat[i].dma_addr = (void *)(dma_base);
				dev->isp_stat_bq.isp_stat[i].isp_stat_buf.stat_buf = (void *)(va_base);
				dev->isp_stat_bq.isp_stat[i].isp_stat_buf.buf_size = ISP_STAT_TOTAL_SIZE;
				dev->isp_stat_bq.isp_stat[i].isp_stat_buf.buf_status = BUF_IDLE;
				vfe_dbg(0, "dev->isp_stat_bq.isp_stat[i].isp_stat_buf.stat_buf[%d] = %p\n", i, dev->isp_stat_bq.isp_stat[i].isp_stat_buf.stat_buf);
			} else {
				vfe_err("isp statistic buffer request pa failed!\n");
				return -ENOMEM;
			}
		}
	}

	return 0;
}
static int vfe_device_regulator_get(struct ccm_config  *ccm_cfg);
static int vfe_device_regulator_put(struct ccm_config  *ccm_cfg);
static int vfe_set_sensor_power_on(struct vfe_dev *dev);
static int vfe_set_sensor_power_off(struct vfe_dev *dev);

static void isp_resource_release(struct vfe_dev *dev)
{
	unsigned int isp_used_flag = 0, i;

	/* release isp table and statistic buffer */
	for (i = 0; i < dev->dev_qty; i++) {
		if (dev->ccm_cfg[i]->is_isp_used && dev->ccm_cfg[i]->is_bayer_raw) {
			os_mem_free(&dev->isp_lut_tbl_buf_mm[i]);
			os_mem_free(&dev->isp_drc_tbl_buf_mm[i]);
		}
	}

	for (i = 0; i < dev->dev_qty; i++) {
		if (dev->ccm_cfg[i]->is_isp_used && dev->ccm_cfg[i]->is_bayer_raw) {
			isp_used_flag = 1;
			break;
		}
	}

	if (isp_used_flag) {
		for (i = 0; i < MAX_ISP_STAT_BUF; i++) {
			os_mem_free(&dev->isp_stat_buf_mm[i]);
		}
	}
}

static int vfe_is_generating(struct vfe_dev *dev)
{
	int ret;
	unsigned long flags = 0;

	spin_lock_irqsave(&dev->slock, flags);
	ret = test_bit(0, &dev->generating);
	spin_unlock_irqrestore(&dev->slock, flags);
	return ret;
}

static void vfe_start_generating(struct vfe_dev *dev)
{
	unsigned long flags = 0;

	spin_lock_irqsave(&dev->slock, flags);
	set_bit(0, &dev->generating);
	spin_unlock_irqrestore(&dev->slock, flags);
}

static void vfe_stop_generating(struct vfe_dev *dev)
{
	unsigned long flags = 0;

	spin_lock_irqsave(&dev->slock, flags);
	dev->first_flag[dev->cur_ch] = 0;
	clear_bit(0, &dev->generating);
	spin_unlock_irqrestore(&dev->slock, flags);
}

static int vfe_is_opened(struct vfe_dev *dev)
{
	 int ret;

	 mutex_lock(&dev->opened_lock);
	 ret = test_bit(0, &dev->opened);
	 mutex_unlock(&dev->opened_lock);
	 return ret;
}

static void vfe_start_opened(struct vfe_dev *dev)
{
	 mutex_lock(&dev->opened_lock);
	 set_bit(0, &dev->opened);
	 mutex_unlock(&dev->opened_lock);
}

static void vfe_stop_opened(struct vfe_dev *dev)
{
	 mutex_lock(&dev->opened_lock);
	 clear_bit(0, &dev->opened);
	 mutex_unlock(&dev->opened_lock);
}

static void update_ccm_info(struct vfe_dev *dev, struct ccm_config *ccm_cfg)
{
	dev->sd			= ccm_cfg->sd;
	dev->sd_act		= ccm_cfg->sd_act;
	dev->is_isp_used	= ccm_cfg->is_isp_used;
	dev->is_bayer_raw	= ccm_cfg->is_bayer_raw;
	dev->power		= &ccm_cfg->power;
	dev->gpio		= ccm_cfg->gpio;
	dev->flash_used		= ccm_cfg->flash_used;
	dev->flash_type		= ccm_cfg->flash_type;

	/* print change */
	vfe_dbg(0, "ccm_cfg pt = %p\n", ccm_cfg);
	vfe_dbg(0, "ccm_cfg->sd = %p\n", ccm_cfg->sd);
	vfe_dbg(0, "module is_isp_used = %d is_bayer_raw= %d\n", dev->is_isp_used, dev->is_bayer_raw);
}

static void update_isp_setting(struct vfe_dev *dev)
{
	dev->isp_3a_result_pt = &dev->isp_3a_result[dev->input];
	dev->isp_gen_set_pt = dev->isp_gen_set[dev->input];
	dev->isp_gen_set_pt->module_cfg.isp_platform_id = dev->platform_id;
	if (dev->is_bayer_raw) {
		mutex_init(&dev->isp_3a_result_mutex);
		dev->isp_gen_set_pt->module_cfg.lut_src0_table = dev->isp_tbl_addr[dev->input].isp_def_lut_tbl_vaddr;
		dev->isp_gen_set_pt->module_cfg.gamma_table = dev->isp_tbl_addr[dev->input].isp_gamma_tbl_vaddr;
		dev->isp_gen_set_pt->module_cfg.lens_table = dev->isp_tbl_addr[dev->input].isp_lsc_tbl_vaddr;
		dev->isp_gen_set_pt->module_cfg.linear_table = dev->isp_tbl_addr[dev->input].isp_linear_tbl_vaddr;
		dev->isp_gen_set_pt->module_cfg.disc_table = dev->isp_tbl_addr[dev->input].isp_disc_tbl_vaddr;
		if (dev->is_isp_used)
			bsp_isp_update_lut_lens_gamma_table(&dev->isp_tbl_addr[dev->input]);
	}
	dev->isp_gen_set_pt->module_cfg.drc_table = dev->isp_tbl_addr[dev->input].isp_drc_tbl_vaddr;
	if (dev->is_isp_used)
		bsp_isp_update_drc_table(&dev->isp_tbl_addr[dev->input]);
}

static inline void vfe_set_addr(struct vfe_dev *dev, struct vfe_buffer *buffer, int ch)
{
	dma_addr_t addr_org;
	struct vb2_buffer *vb_buf = &buffer->vb.vb2_buf;

	if (dev->special_active == 1) {
		if (buffer == NULL || buffer->paddr == NULL) {
			vfe_err("%s buffer is NULL!\n", __func__);
			return;
		}
	} else {
		if (vb_buf == NULL || vb_buf->planes[0].mem_priv == NULL) {
			vfe_err("vb_buf->priv is NULL!\n");
			return;
		}
	}

	if (dev->special_active == 1)
		addr_org = (dma_addr_t)buffer->paddr;
	else
		addr_org = vb2_dma_contig_plane_dma_addr(vb_buf, 0)
				- CPU_DRAM_PADDR_ORG + HW_DMA_OFFSET;

	if (dev->is_isp_used) {
		sunxi_isp_set_output_addr(dev->isp_sd, addr_org);
	} else {
#if defined(CH_OUTPUT_IN_DIFFERENT_VIDEO)
		bsp_csi_set_ch_addr(dev->csi_sel, ch, addr_org);
#else
		bsp_csi_set_addr(dev->csi_sel, addr_org);
#endif
	}
	vfe_dbg(3, "csi_buf_addr_orginal=%pa\n", &addr_org);
}

static void vfe_init_isp_log(struct vfe_dev *dev)
{
	if (isp_log == 1) {
		fp_dbg = cfg_open_file(LogFileName);
		dev->isp_gen_set[0]->enable_log = 1;
		dev->isp_gen_set[1]->enable_log = 1;
		if (IS_ERR(fp_dbg))
			vfe_err("open log.txt error.");
	} else {
		dev->isp_gen_set[0]->enable_log = 0;
		dev->isp_gen_set[1]->enable_log = 0;
	}

}
static void vfe_exit_isp_log(struct vfe_dev *dev)
{
	if (isp_log == 1)
		cfg_close_file(fp_dbg);
}
static void vfe_dump_isp_log(struct vfe_dev *dev)
{

	/* dump isp log. */
	if (isp_log == 1 && (frame_cnt % 4 == 0)) {
		if (cfg_write_file(fp_dbg, dev->isp_gen_set_pt->stat.hist_buf, ISP_STAT_HIST_MEM_SIZE) < 0) {
			vfe_err("dump isp hist faild.");
			return;
		}
		if (cfg_write_file(fp_dbg, dev->isp_gen_set_pt->stat.ae_buf, ISP_STAT_AE_MEM_SIZE) < 0) {
			vfe_err("dump isp ae faild.");
		}
		if (cfg_write_file(fp_dbg, (char *)dev->isp_gen_set_pt->awb_buf, 3*ISP_STAT_AWB_WIN_MEM_SIZE) < 0) {
			vfe_err("dump awb log faild.");
		}
		/*
		if (cfg_write_file(fp_dbg, dev->isp_gen_set_pt->stat.af_buf, ISP_STAT_AF_MEM_SIZE) < 0) {
			vfe_err("dump isp log faild.");
		}
		if (cfg_write_file(fp_dbg, "0123456789abcdef\n", 16) < 0) {
			vfe_err("/system/etc/hawkview/log.txt write test failed.");
		}
		*/
	}
}

static void isp_isr_bh_handle(struct work_struct *work)
{
	struct actuator_ctrl_word_t  vcm_ctrl;
	struct vfe_dev *dev = container_of(work, struct vfe_dev, isp_isr_bh_task);

	FUNCTION_LOG;
	if (vfe_dump & DUMP_ISP) {
		if (9 == (frame_cnt % 10)) {
			sunxi_isp_dump_regs(dev->isp_sd);
		}
	}
	if (dev->is_bayer_raw) {
		mutex_lock(&dev->isp_3a_result_mutex);
		if (isp_reparse_flag == 1) {
			vfe_print("ISP reparse ini file!\n");
			if (read_ini_info(dev, dev->input, "/system/etc/hawkview/")) {
				vfe_warn("ISP reparse ini fail, please check isp config!\n");
				goto ISP_REPARSE_END;
			}
			isp_param_init(dev->isp_gen_set_pt);
			isp_config_init(dev->isp_gen_set_pt);
			isp_module_init(dev->isp_gen_set_pt, dev->isp_3a_result_pt);
ISP_REPARSE_END:
			isp_reparse_flag = 0;
		}
		if (isp_reparse_flag == 2) {
			vfe_reg_set((void __iomem *) (unsigned long)(ISP_REGS_BASE+0x10), (1 << 20));
		}
		if (isp_reparse_flag == 3) {
			vfe_reg_clr_set((void __iomem *) (unsigned long)(ISP_REGS_BASE+0x10), (0xF << 16), (1 << 16));
			vfe_reg_set((void __iomem *) (unsigned long)(ISP_REGS_BASE+0x10), (1 << 20));
		}
		if (isp_reparse_flag == 4) {
			/* vfe_reg_clr_set(IO_ADDRESS(ISP_REGS_BASE+0x10), (0xF << 16), (1 << 16)); */
			vfe_reg_clr((void __iomem *) (unsigned long)(ISP_REGS_BASE+0x10), (1 << 20));
			vfe_reg_clr((void __iomem *) (unsigned long)(ISP_REGS_BASE+0x10), (0xF << 16));
		}
		vfe_dump_isp_log(dev);
		isp_isr(dev->isp_gen_set_pt, dev->isp_3a_result_pt);
		if ((dev->ctrl_para.prev_focus_pos != dev->isp_3a_result_pt->real_vcm_pos  ||
				dev->isp_gen_set_pt->isp_ini_cfg.isp_test_settings.isp_test_mode != 0 ||
				dev->isp_gen_set_pt->isp_ini_cfg.isp_test_settings.af_en == 0) && dev->sd_act) {
			vcm_ctrl.code =  dev->isp_3a_result_pt->real_vcm_pos;
			vcm_ctrl.sr = 0x0;
			if (v4l2_subdev_call(dev->sd_act, core, ioctl, ACT_SET_CODE, &vcm_ctrl)) {
				vfe_warn("set vcm error!\n");
			} else {
				dev->ctrl_para.prev_focus_pos = dev->isp_3a_result_pt->real_vcm_pos;
			}
		}

		mutex_unlock(&dev->isp_3a_result_mutex);
	} else {
		isp_isr(dev->isp_gen_set_pt, NULL);
	}

	FUNCTION_LOG;
}

int set_sensor_shutter(struct vfe_dev *dev, int shutter)
{
	struct v4l2_control ctrl;

	if (shutter <= 0)
		return -EINVAL;

	ctrl.id = V4L2_CID_EXPOSURE;
	ctrl.value = shutter;
	if (v4l2_subdev_call(dev->sd, core, s_ctrl, &ctrl) != 0) {
		vfe_err("set sensor exposure line error!\n");
		return -EPERM;
	} else
		dev->ctrl_para.prev_exp_line = shutter;

	return 0;
}

int set_sensor_gain(struct vfe_dev *dev, int gain)
{
	struct v4l2_control ctrl;

	if (gain < 16)
		return -EINVAL;

	ctrl.id = V4L2_CID_GAIN;
	ctrl.value = gain;
	if (v4l2_subdev_call(dev->sd, core, s_ctrl, &ctrl) != 0) {
		vfe_err("set sensor gain error!\n");
		return -EPERM;
	} else
		dev->ctrl_para.prev_ana_gain = gain;

	return 0;
}

int set_sensor_shutter_and_gain(struct vfe_dev *dev)
{
	struct sensor_exp_gain exp_gain;

	exp_gain.exp_val = dev->isp_3a_result_pt->exp_line_num;
	exp_gain.gain_val = dev->isp_3a_result_pt->exp_analog_gain;
	if (exp_gain.gain_val < 16 || exp_gain.exp_val <= 0)
		return -EINVAL;

	if (v4l2_subdev_call(dev->sd, core, ioctl, ISP_SET_EXP_GAIN, &exp_gain) != 0) {
		vfe_warn("set ISP_SET_EXP_GAIN error, set V4L2_CID_EXPOSURE!\n");
		return -EPERM;
	} else {
		dev->ctrl_para.prev_exp_line = exp_gain.exp_val;
		dev->ctrl_para.prev_ana_gain = exp_gain.gain_val;
	}

	return 0;
}

static int isp_s_ctrl_torch_open(struct vfe_dev *dev)
{
	if (dev->isp_gen_set_pt->exp_settings.flash_mode == FLASH_MODE_OFF)
		return 0;

	if (((dev->isp_gen_set_pt->exp_settings.tbl_cnt > (dev->isp_gen_set_pt->exp_settings.tbl_max_ind - 25)) ||
			dev->isp_gen_set_pt->exp_settings.flash_mode == FLASH_MODE_ON)) {
		vfe_dbg(0, "open flash when nigth mode\n");
		io_set_flash_ctrl(dev->flash_sd, SW_CTRL_TORCH_ON);
		touch_flash_flag = 1;
	}

	return 0;
}

static int isp_s_ctrl_torch_close(struct vfe_dev *dev)
{
	if (dev->isp_gen_set_pt->exp_settings.flash_mode == FLASH_MODE_OFF)
		return 0;

	if (touch_flash_flag == 1) {
		vfe_dbg(0, "close flash when nigth mode\n");
		io_set_flash_ctrl(dev->flash_sd, SW_CTRL_FLASH_OFF);
		touch_flash_flag = 0;
	}

	return 0;
}

static int isp_streamoff_torch_and_flash_close(struct vfe_dev *dev)
{
	if (dev->isp_gen_set_pt->exp_settings.flash_mode == FLASH_MODE_OFF)
		return 0;

	if (touch_flash_flag == 1 || dev->isp_gen_set_pt->exp_settings.flash_open == 1) {
		vfe_dbg(0, "close flash when nigth mode\n");
		io_set_flash_ctrl(dev->flash_sd, SW_CTRL_FLASH_OFF);
		touch_flash_flag = 0;
	}

	return 0;
}

static int isp_set_capture_flash(struct vfe_dev *dev)
{
	if (dev->isp_gen_set_pt->exp_settings.flash_mode == FLASH_MODE_OFF)
		return 0;

	if (dev->isp_gen_set_pt->take_pic_start_cnt == 1) {
		if (dev->isp_gen_set_pt->exp_settings.tbl_cnt > (dev->isp_gen_set_pt->exp_settings.tbl_max_ind - 40) ||
				dev->isp_gen_set_pt->exp_settings.flash_mode == FLASH_MODE_ON) {
			vfe_dbg(0, "open torch when nigth mode\n");
			io_set_flash_ctrl(dev->flash_sd, SW_CTRL_TORCH_ON);
			dev->isp_gen_set_pt->exp_settings.flash_open = 1;
		}
	}

	if (dev->isp_gen_set_pt->exp_settings.flash_open == 1 && dev->isp_gen_set_pt->take_pic_start_cnt ==
					dev->isp_gen_set_pt->isp_ini_cfg.isp_tunning_settings.flash_delay_frame) {
		vfe_dbg(0, "open flash when nigth mode\n");
		dev->isp_gen_set_pt->exp_settings.exposure_lock = ISP_TRUE;
		ev_cumul = get_pre_ev_cumul(dev->isp_gen_set_pt, dev->isp_3a_result_pt);
		if (ev_cumul >= 100) {
			dev->isp_gen_set_pt->exp_settings.tbl_cnt = CLIP(dev->isp_gen_set_pt->exp_settings.expect_tbl_cnt, 1,
						dev->isp_gen_set_pt->exp_settings.tbl_max_ind);
		} else if (ev_cumul >= dev->isp_gen_set_pt->isp_ini_cfg.isp_tunning_settings.flash_gain * 100 / 256 && ev_cumul < 100) {
			dev->isp_gen_set_pt->exp_settings.tbl_cnt = CLIP(dev->isp_gen_set_pt->exp_settings.expect_tbl_cnt, 1,
						dev->isp_gen_set_pt->exp_settings.tbl_max_ind);
		} else if (ev_cumul >= -25 && ev_cumul < dev->isp_gen_set_pt->isp_ini_cfg.isp_tunning_settings.flash_gain * 100 / 256) {
			dev->isp_gen_set_pt->exp_settings.tbl_cnt = CLIP(dev->isp_gen_set_pt->exp_settings.expect_tbl_cnt +
						ev_cumul * dev->isp_gen_set_pt->isp_ini_cfg.isp_tunning_settings.flash_gain / 256, 1,
						dev->isp_gen_set_pt->exp_settings.tbl_max_ind);
		} else {
			dev->isp_gen_set_pt->exp_settings.tbl_cnt = CLIP(dev->isp_gen_set_pt->exp_settings.expect_tbl_cnt +
						ev_cumul * dev->isp_gen_set_pt->isp_ini_cfg.isp_tunning_settings.flash_gain / 256, 1,
						dev->isp_gen_set_pt->exp_settings.tbl_max_ind);
		}
		config_sensor_next_exposure(dev->isp_gen_set_pt, dev->isp_3a_result_pt);
		io_set_flash_ctrl(dev->flash_sd, SW_CTRL_FLASH_OFF);
	}

	if (dev->isp_gen_set_pt->exp_settings.flash_open == 1 && dev->isp_gen_set_pt->take_pic_start_cnt ==
					dev->isp_gen_set_pt->isp_ini_cfg.isp_tunning_settings.flash_delay_frame + 1) {
		io_set_flash_ctrl(dev->flash_sd, SW_CTRL_FLASH_ON);
	}

	if (dev->isp_gen_set_pt->take_pic_start_cnt == 7 + dev->isp_gen_set_pt->isp_ini_cfg.isp_tunning_settings.flash_delay_frame) {
		vfe_dbg(0, "close flash when nigth mode\n");
		io_set_flash_ctrl(dev->flash_sd, SW_CTRL_FLASH_OFF);
		dev->isp_gen_set_pt->exp_settings.tbl_cnt = CLIP(dev->isp_gen_set_pt->exp_settings.expect_tbl_cnt,
				1, dev->isp_gen_set_pt->exp_settings.tbl_max_ind);
		dev->isp_gen_set_pt->exp_settings.exposure_lock = ISP_FALSE;
		dev->isp_gen_set_pt->exp_settings.flash_open = 0;
	}

	if (dev->isp_gen_set_pt->exp_settings.flash_open == 0 && touch_flash_flag == 1 &&
			(dev->isp_3a_result_pt->af_status == AUTO_FOCUS_STATUS_REACHED ||
			dev->isp_3a_result_pt->af_status == AUTO_FOCUS_STATUS_FAILED ||
			dev->isp_3a_result_pt->af_status == AUTO_FOCUS_STATUS_FINDED)) {
		vfe_dbg(0, "close flash when touch nigth mode\n");
		io_set_flash_ctrl(dev->flash_sd, SW_CTRL_FLASH_OFF);
		touch_flash_flag = 0;
	}

	return 0;
}

static void isp_isr_set_sensor_handle(struct work_struct *work)
{
	struct vfe_dev *dev = container_of(work, struct vfe_dev, isp_isr_set_sensor_task);

	if (dev->is_bayer_raw) {
		mutex_lock(&dev->isp_3a_result_mutex);
		isp_set_capture_flash(dev);
		if (dev->isp_gen_set_pt->isp_ini_cfg.isp_3a_settings.adaptive_frame_rate == 1 ||
				dev->isp_gen_set_pt->isp_ini_cfg.isp_3a_settings.force_frame_rate == 1 ||
				dev->isp_gen_set_pt->isp_ini_cfg.isp_3a_settings.high_quality_mode_en == 1) {
			vfe_dbg(0, "combinate shutter = %d, gain =%d\n", dev->isp_3a_result_pt->exp_line_num,
					dev->isp_3a_result_pt->exp_analog_gain);
			if (set_sensor_shutter_and_gain(dev) != 0) {
				set_sensor_shutter(dev, dev->isp_3a_result_pt->exp_line_num);
				set_sensor_gain(dev, dev->isp_3a_result_pt->exp_analog_gain);
			}
		} else {
			vfe_dbg(0, "separate shutter = %d, gain =%d\n", dev->isp_3a_result_pt->exp_line_num/16,
					dev->isp_3a_result_pt->exp_analog_gain);
			set_sensor_shutter(dev, dev->isp_3a_result_pt->exp_line_num);
			set_sensor_gain(dev, dev->isp_3a_result_pt->exp_analog_gain);
		}
		mutex_unlock(&dev->isp_3a_result_mutex);
	}
}

static void vfe_isp_stat_parse(struct isp_gen_settings *isp_gen)
{
	unsigned long buffer_addr = (unsigned long)isp_gen->stat.stat_buf_whole->stat_buf;

	isp_gen->stat.hist_buf = (void *) (buffer_addr);
	isp_gen->stat.ae_buf =  (void *) (buffer_addr + ISP_STAT_AE_MEM_OFS);
	isp_gen->stat.awb_buf = (void *) (buffer_addr + ISP_STAT_AWB_MEM_OFS);
	isp_gen->stat.af_buf = (void *) (buffer_addr + ISP_STAT_AF_MEM_OFS);
	isp_gen->stat.afs_buf = (void *) (buffer_addr + ISP_STAT_AFS_MEM_OFS);
	isp_gen->stat.awb_win_buf = (void *) (buffer_addr + ISP_STAT_AWB_WIN_MEM_OFS);
}

/*
 *  the interrupt routine
 */

static irqreturn_t vfe_isr(int irq, void *priv)
{
	int i;
	unsigned long flags;
	struct vfe_buffer *buf;
	struct vfe_dev *dev = (struct vfe_dev *)priv;
	struct vfe_dmaqueue *dma_q = &dev->vidq[dev->cur_ch];
	struct vfe_dmaqueue *done = NULL;
	int need_callback = 0, current_ch = 0;
	struct csi_int_status status;
	struct vfe_isp_stat_buf_queue *isp_stat_bq = &dev->isp_stat_bq;
	struct vfe_isp_stat_buf *stat_buf_pt;

	FUNCTION_LOG;

	if (dev->special_active == 1) {
		dma_q = &dev->vidq_special;
		done = &dev->done_special;
		need_callback = 0;
	}

	if (vfe_is_generating(dev) == 0) {
		bsp_csi_int_clear_status(dev->csi_sel, dev->cur_ch, CSI_INT_ALL);
		if (dev->is_isp_used)
			bsp_isp_clr_irq_status(ISP_IRQ_EN_ALL);

		return IRQ_HANDLED;
	}

	FUNCTION_LOG;
	spin_lock_irqsave(&dev->slock, flags);
	FUNCTION_LOG;

#if defined(CH_OUTPUT_IN_DIFFERENT_VIDEO)
	for (i = 0; i < MAX_CH_NUM; i++) {
		bsp_csi_int_get_status(dev->csi_sel, i, &status);
		if (status.frame_done == 1) {
			current_ch = i;
			dma_q = &dev->vidq[current_ch];
			break;
		}
	}
#else
	bsp_csi_int_get_status(dev->csi_sel, dev->cur_ch, &status);
	current_ch = dev->cur_ch;
	dma_q = &dev->vidq[current_ch];
#endif
	vfe_dbg(0, "csi ch %d interrupt, dev->cur_ch is %d!\n", current_ch, dev->cur_ch);

	if ((status.capture_done == 0) && (status.frame_done == 0) && (status.vsync_trig == 0)) {
		vfe_print("enter vfe int for nothing\n");
		bsp_csi_int_clear_status(dev->csi_sel, current_ch, CSI_INT_ALL);
		if (dev->is_isp_used)
			bsp_isp_clr_irq_status(ISP_IRQ_EN_ALL);

		spin_unlock_irqrestore(&dev->slock, flags);
		return IRQ_HANDLED;
	}
	if (dev->is_isp_used && dev->is_bayer_raw) {
		/* update_sensor_setting: */
		if (status.vsync_trig) {
			if ((dev->capture_mode == V4L2_MODE_VIDEO) || (dev->capture_mode == V4L2_MODE_PREVIEW)) {
				vfe_dbg(3, "call set sensor task schedule!\n");
				schedule_work(&dev->isp_isr_set_sensor_task);
			}
			bsp_csi_int_clear_status(dev->csi_sel, current_ch, CSI_INT_VSYNC_TRIG);
			spin_unlock_irqrestore(&dev->slock, flags);
			return IRQ_HANDLED;
		}
	}

	if (vfe_dump & DUMP_CSI) {
		if (5 == frame_cnt % 10) {
			sunxi_csi_dump_regs(dev->csi_sd);
		}
	}
	frame_cnt++;

	/* exception handle: */
	if ((status.buf_0_overflow) || (status.buf_1_overflow) || (status.buf_2_overflow) || (status.hblank_overflow)) {
		if ((status.buf_0_overflow) || (status.buf_1_overflow) || (status.buf_2_overflow)) {
			bsp_csi_int_clear_status(dev->csi_sel, current_ch,
					CSI_INT_BUF_0_OVERFLOW |
					CSI_INT_BUF_1_OVERFLOW |
					CSI_INT_BUF_2_OVERFLOW);
			vfe_err("fifo overflow\n");
		}
		if (status.hblank_overflow) {
			bsp_csi_int_clear_status(dev->csi_sel, current_ch, CSI_INT_HBLANK_OVERFLOW);
			vfe_err("hblank overflow\n");
		}
		vfe_err("reset csi module\n");
		bsp_csi_reset(dev->csi_sel);
		if (dev->is_isp_used)
			goto isp_exp_handle;
		else
			goto unlock;
	}
isp_exp_handle:
	if (dev->is_isp_used) {
		if (bsp_isp_get_irq_status(SRC0_FIFO_INT_EN)) {
			vfe_err("isp source0 fifo overflow\n");
			bsp_isp_clr_irq_status(SRC0_FIFO_INT_EN);
			goto unlock;
		}
	}
	vfe_dbg(3, "status vsync = %d, framedone = %d, capdone = %d\n",
			status.vsync_trig, status.frame_done, status.capture_done);
	if (dev->capture_mode == V4L2_MODE_IMAGE) {
		if (dev->is_isp_used)
			bsp_isp_irq_disable(FINISH_INT_EN);
		else
			bsp_csi_int_disable(dev->csi_sel, current_ch, CSI_INT_CAPTURE_DONE);
		vfe_print("ch%d capture image mode!\n", current_ch);
		buf = list_entry(dma_q->active.next, struct vfe_buffer, list);
		list_del(&buf->list);
		if (dev->special_active == 1) {
			list_add_tail(&buf->list, &done->active);
			need_callback = 1;
		} else {
			vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
		}
		goto unlock;
	} else {
		if (dev->is_isp_used)
			bsp_isp_irq_disable(FINISH_INT_EN);
		else
			bsp_csi_int_disable(dev->csi_sel, current_ch, CSI_INT_FRAME_DONE);
		if (dev->first_flag[current_ch] == 0) {
			dev->first_flag[current_ch]++;
			vfe_print("ch%d capture video mode!\n", current_ch);
			goto set_isp_stat_addr;
		}
		if (dev->first_flag[current_ch] == 1) {
			dev->first_flag[current_ch]++;
			vfe_print("ch%d capture video first frame done!\n", current_ch);
		}

		/* video buffer handle: */
		if ((&dma_q->active) == dma_q->active.next->next->next) {
			vfe_warn("Only two buffer left for csi ch%d\n", current_ch);
			dev->first_flag[current_ch] = 0;
			goto unlock;
		}
		buf = list_entry(dma_q->active.next, struct vfe_buffer, list);

		/* Nobody is waiting on this buffer*/
		if (!dev->special_active) {
			if (!waitqueue_active(&buf->vb.vb2_buf.vb2_queue->done_wq)) {
				vfe_warn("Nobody is waiting on buf = 0x%p, ch is %d\n", buf, current_ch);
			}
		}
		list_del(&buf->list);
		v4l2_get_timestamp(&buf->vb.timestamp);

		vfe_dbg(2, "video buffer frame interval = %ld\n", buf->vb.timestamp.tv_sec * 1000000 + buf->vb.timestamp.tv_usec
			- (dev->sec*1000000+dev->usec));
		dev->sec = buf->vb.timestamp.tv_sec;
		dev->usec = buf->vb.timestamp.tv_usec;
	/*	buf->vb.image_quality = dev->isp_3a_result_pt->image_quality.dwval; */

		if (dev->special_active == 1) {
			list_add_tail(&buf->list, &done->active);
			need_callback = 1;
		} else {
			vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
		}
		/* isp_stat_handle: */

		if (dev->is_isp_used && dev->is_bayer_raw) {
			list_for_each_entry(stat_buf_pt, &isp_stat_bq->locked, queue) {
				if (stat_buf_pt->isp_stat_buf.buf_status == BUF_LOCKED) {
					vfe_dbg(3, "find isp stat buf locked!\n");
					vfe_dbg(3, "isp_stat_bq->locked = %p\n", &isp_stat_bq->locked);
					vfe_dbg(3, "isp_stat_bq->locked.next = %p\n", isp_stat_bq->locked.next);
					vfe_dbg(3, "isp_stat_bq->isp_stat[%d].queue = %p\n", stat_buf_pt->id, &isp_stat_bq->isp_stat[stat_buf_pt->id].queue);
					vfe_dbg(3, "isp_stat_bq->isp_stat[%d].queue.prev = %p\n", stat_buf_pt->id, isp_stat_bq->isp_stat[stat_buf_pt->id].queue.prev);
					vfe_dbg(3, "isp_stat_bq->isp_stat[%d].queue.next = %p\n", stat_buf_pt->id, isp_stat_bq->isp_stat[stat_buf_pt->id].queue.next);
					goto set_next_output_addr;
				}
			}
			for (i = 0; i < MAX_ISP_STAT_BUF; i++) {
				stat_buf_pt = &isp_stat_bq->isp_stat[i];
				if (stat_buf_pt->isp_stat_buf.buf_status == BUF_IDLE) {
					vfe_dbg(3, "find isp stat buf idle!\n");
					list_move_tail(&stat_buf_pt->queue, &isp_stat_bq->active);
					stat_buf_pt->isp_stat_buf.buf_status = BUF_ACTIVE;
				}
			}

			vfe_dbg(3, "before list empty isp_stat_bq->active = %p\n", &isp_stat_bq->active);
			vfe_dbg(3, "before list empty isp_stat_bq->active.prev = %p\n", isp_stat_bq->active.prev);
			vfe_dbg(3, "before list empty isp_stat_bq->active.next = %p\n", isp_stat_bq->active.next);

			/* judge if the isp stat queue has been written to the last */
			if (list_empty(&isp_stat_bq->active)) {
				vfe_err("No active isp stat queue to serve\n");
				goto set_next_output_addr;
			}
			vfe_dbg(3, "after list empty isp_stat_bq->active = %p\n", &isp_stat_bq->active);
			vfe_dbg(3, "after list empty isp_stat_bq->active.prev = %p\n", isp_stat_bq->active.prev);
			vfe_dbg(3, "after list empty isp_stat_bq->active.next = %p\n", isp_stat_bq->active.next);

			/*
			delete the ready buffer from the actvie queue
			add the ready buffer to the locked queue
			stat_buf_pt = list_first_entry(&isp_stat_bq->active, struct vfe_isp_stat_buf, queue);
			*/
			stat_buf_pt = list_entry(isp_stat_bq->active.next, struct vfe_isp_stat_buf, queue);

			list_move_tail(&stat_buf_pt->queue, &isp_stat_bq->locked);
			stat_buf_pt->isp_stat_buf.buf_status = BUF_LOCKED;
			dev->isp_gen_set_pt->stat.stat_buf_whole = &isp_stat_bq->isp_stat[stat_buf_pt->id].isp_stat_buf;
			vfe_isp_stat_parse(dev->isp_gen_set_pt);
			isp_stat_bq->isp_stat[stat_buf_pt->id].isp_stat_buf.frame_number++;

			if ((&isp_stat_bq->active) == isp_stat_bq->active.next->next) {
				vfe_warn("No more isp stat free frame on next time\n");
				goto set_next_output_addr;
			}
		}
	}

set_isp_stat_addr:
	if (dev->is_isp_used && dev->is_bayer_raw) {
		/* stat_buf_pt = list_entry(isp_stat_bq->active.next->next, struct vfe_isp_stat_buf, queue); */
		stat_buf_pt = list_entry(isp_stat_bq->active.next, struct vfe_isp_stat_buf, queue);
		bsp_isp_set_statistics_addr((unsigned long)(stat_buf_pt->dma_addr));
	}
set_next_output_addr:
	if (list_empty(&dma_q->active) || dma_q->active.next->next == (&dma_q->active)) {
		vfe_print("No active queue to serve\n");
		goto unlock;
	}
	buf = list_entry(dma_q->active.next->next, struct vfe_buffer, list);
	vfe_set_addr(dev, buf, current_ch);

unlock:
#if defined(CH_OUTPUT_IN_DIFFERENT_VIDEO)
	bsp_csi_int_clear_status(dev->csi_sel, current_ch, CSI_INT_FRAME_DONE);
	bsp_csi_int_clear_status(dev->csi_sel, current_ch, CSI_INT_CAPTURE_DONE);
	if ((dev->capture_mode == V4L2_MODE_VIDEO) ||
	    (dev->capture_mode == V4L2_MODE_PREVIEW))
		bsp_csi_int_enable(dev->csi_sel, current_ch, CSI_INT_FRAME_DONE);
#endif
	spin_unlock_irqrestore(&dev->slock, flags);

	if (dev->special_active && need_callback && dev->vfe_buffer_process)
		dev->vfe_buffer_process(dev->id);

	if (((dev->capture_mode == V4L2_MODE_VIDEO) || (dev->capture_mode == V4L2_MODE_PREVIEW))
		&& dev->is_isp_used && bsp_isp_get_irq_status(FINISH_INT_EN)) {
		/* if(bsp_isp_get_para_ready()) */
		vfe_dbg(3, "call tasklet schedule!\n");
		bsp_isp_clr_para_ready();
		schedule_work(&dev->isp_isr_bh_task);
		bsp_isp_set_para_ready();
	}
#if !defined(CH_OUTPUT_IN_DIFFERENT_VIDEO)
	if (dev->is_isp_used) {
		bsp_isp_clr_irq_status(FINISH_INT_EN);
		bsp_isp_irq_enable(FINISH_INT_EN);
	} else {
		bsp_csi_int_clear_status(dev->csi_sel, dev->cur_ch, CSI_INT_FRAME_DONE);
		bsp_csi_int_clear_status(dev->csi_sel, dev->cur_ch, CSI_INT_CAPTURE_DONE);
		if ((dev->capture_mode == V4L2_MODE_VIDEO) ||
		    (dev->capture_mode == V4L2_MODE_PREVIEW))
			bsp_csi_int_enable(dev->csi_sel, dev->cur_ch, CSI_INT_FRAME_DONE);
	}
#endif
	return IRQ_HANDLED;
}

/*
 * Videobuf operations
 */
static int queue_setup(struct vb2_queue *vq, const void *parg,
				unsigned int *nbuffers, unsigned int *nplanes,
				unsigned int sizes[], void *alloc_ctxs[])
{
	struct vfe_dev *dev = vb2_get_drv_priv(vq);
	unsigned int size;
	int buf_max_flag = 0;

	vfe_dbg(1, "queue_setup\n");

	size = dev->buf_byte_size;

	if (size == 0)
		return -EINVAL;

	if (*nbuffers == 0)
		*nbuffers = 8;

	while (size * *nbuffers > MAX_FRAME_MEM) {
		(*nbuffers)--;
		buf_max_flag = 1;
		if (*nbuffers == 0)
			vfe_err("one buffer size larger than max frame memory! buffer count = %d\n,", *nbuffers);
	}

	if (buf_max_flag == 0) {
		if (dev->capture_mode == V4L2_MODE_IMAGE) {
			if (*nbuffers != 1) {
				*nbuffers = 1;
				vfe_err("buffer count is set to 1 in image capture mode\n");
			}
		} else {
			if (*nbuffers < 3) {
				*nbuffers = 3;
				vfe_err("buffer count is invalid, set to 3 in video capture\n");
			}
		}
	}

	*nplanes = 1;
	sizes[0] = size;
	alloc_ctxs[0] = dev->alloc_ctx[dev->cur_ch];

	vfe_print("%s, buffer count=%d, size=%d\n", __func__, *nbuffers, size);

	return 0;
}

static int buffer_prepare(struct vb2_buffer *vb)
{
	struct vfe_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vvb = container_of(vb, struct vb2_v4l2_buffer, vb2_buf);
	struct vfe_buffer *buf = container_of(vvb, struct vfe_buffer, vb);
	unsigned long size;

	vfe_dbg(1, "buffer_prepare\n");

	if (dev->width < MIN_WIDTH || dev->width  > MAX_WIDTH ||
			dev->height < MIN_HEIGHT || dev->height > MAX_HEIGHT)
		return -EINVAL;

	size = dev->buf_byte_size;

	if (vb2_plane_size(vb, 0) < size) {
		vfe_err("%s data will not fit into plane (%lu < %lu)\n",
					__func__, vb2_plane_size(vb, 0), size);
		return -EINVAL;
	}

	vb2_set_plane_payload(&buf->vb.vb2_buf, 0, size);

	vb->planes[0].m.offset = vb2_dma_contig_plane_dma_addr(vb, 0);

	return 0;
}

static void buffer_queue(struct vb2_buffer *vb)
{
	struct vfe_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vvb = container_of(vb, struct vb2_v4l2_buffer, vb2_buf);
	struct vfe_buffer *buf = container_of(vvb, struct vfe_buffer, vb);
	struct vfe_dmaqueue *vidq = &dev->vidq[dev->cur_ch];
	unsigned long flags = 0;

	vfe_dbg(1, "ch%d buffer_queue\n", dev->cur_ch);
	spin_lock_irqsave(&dev->slock, flags);
	list_add_tail(&buf->list, &vidq->active);
	spin_unlock_irqrestore(&dev->slock, flags);
}

static int start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct vfe_dev *dev = vb2_get_drv_priv(vq);

	vfe_dbg(1, "%s\n", __func__);
	if (dev->cur_ch == 0)
		vfe_start_generating(dev);

	return 0;
}

/* abort streaming and wait for last buffer */
static void stop_streaming(struct vb2_queue *vq)
{
	struct vfe_dev *dev = vb2_get_drv_priv(vq);
	struct vfe_dmaqueue *dma_q = &dev->vidq[dev->cur_ch];
	unsigned long flags = 0;

	vfe_dbg(1, "%s\n", __func__);

	if (dev->cur_ch == 0)
		vfe_stop_generating(dev);

	spin_lock_irqsave(&dev->slock, flags);
	/* Release all active buffers */
	while (!list_empty(&dma_q->active)) {
		struct vfe_buffer *buf;

		buf = list_entry(dma_q->active.next, struct vfe_buffer, list);
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		vfe_dbg(2, "[%p/%d] done\n", buf, buf->vb.vb2_buf.index);
	}
	spin_unlock_irqrestore(&dev->slock, flags);
}

static void vfe_lock(struct vb2_queue *vq)
{
	struct vfe_dev *dev = vb2_get_drv_priv(vq);

	mutex_lock(&dev->buf_lock);
}

static void vfe_unlock(struct vb2_queue *vq)
{
	struct vfe_dev *dev = vb2_get_drv_priv(vq);

	mutex_unlock(&dev->buf_lock);
}

static const struct vb2_ops vfe_video_qops = {
	.queue_setup		= queue_setup,
	.buf_prepare		= buffer_prepare,
	.buf_queue		= buffer_queue,
	.start_streaming	= start_streaming,
	.stop_streaming	= stop_streaming,
	.wait_prepare		= vfe_unlock,
	.wait_finish		= vfe_lock,
};

/*
 * IOCTL vidioc handling
 */
static int vidioc_querycap(struct file *file, void  *priv,
	  struct v4l2_capability *cap)
{
	struct vfe_dev *dev = video_drvdata(file);

	strcpy(cap->driver, "sunxi-vfe");
	strcpy(cap->card, "sunxi-vfe");
	strlcpy(cap->bus_info, dev->v4l2_dev.name, sizeof(cap->bus_info));

	cap->version = VFE_VERSION;
	cap->capabilities = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | \
			V4L2_CAP_READWRITE | V4L2_CAP_DEVICE_CAPS;

	cap->device_caps |= V4L2_CAP_EXT_PIX_FORMAT;

	return 0;
}

static int vidioc_enum_fmt_vid_cap(struct file *file, void  *priv,
	  struct v4l2_fmtdesc *f)
{
	struct vfe_fmt *fmt;

	vfe_dbg(0, "vidioc_enum_fmt_vid_cap\n");

	if (f->index > ARRAY_SIZE(formats) - 1)
		return -EINVAL;

	fmt = &formats[f->index];

	strlcpy(f->description, fmt->name, sizeof(f->description));
	f->pixelformat = fmt->fourcc;

	return 0;
}


static int vidioc_enum_framesizes(struct file *file, void *fh,
	  struct v4l2_frmsizeenum *fsize)
{
	struct vfe_dev *dev = video_drvdata(file);
	struct v4l2_subdev_frame_size_enum fse;
	struct v4l2_frmsize_discrete *discrete = &fsize->discrete;
	int ret;

	vfe_dbg(0, "vidioc_enum_framesizes\n");

	fse.index = fsize->index;
	if (dev == NULL || dev->sd->ops->pad->enum_frame_size == NULL)
		return -EINVAL;

	ret = v4l2_subdev_call(dev->sd, pad, enum_frame_size, NULL, &fse);
	if (ret)
		vfe_err("enum frame sizes fail!!\n");

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	discrete->width = fse.max_width;
	discrete->height = fse.max_height;

	vfe_dbg(0, "vidioc_enum_framesizes, width=%d, height=%d\n", discrete->width, discrete->height);

	return ret;
}

static int vidioc_g_fmt_vid_cap(struct file *file, void *priv,
							struct v4l2_format *f)
{
	struct vfe_dev *dev = video_drvdata(file);

	f->fmt.pix.width        = dev->width;
	f->fmt.pix.height       = dev->height;
	f->fmt.pix.field	= dev->fmt.field;
	f->fmt.pix.pixelformat  = dev->fmt.bus_pix_code;

	return 0;
}

static u32 *try_fmt_from_sensor(struct vfe_dev *dev, char *bus_name,
			char *pix_name, struct v4l2_subdev_format *fmat) {
	struct vfe_fmt_code *vfe_formats, *def_fmt = NULL;
	int ret = 0;
	u32 *mbus_code;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vfe_fmt_code); i++) {
		vfe_formats = &vfe_fmt_code[i];

		if (bus_name == vfe_formats->bus_name && pix_name == vfe_formats->pix_name) {
			def_fmt = vfe_formats;
			mbus_code = vfe_formats->mbus_code;
			for (i = 0; i < def_fmt->size; ++i) {
				fmat->format.code = mbus_code[i];
				ret = v4l2_subdev_call(dev->sd, pad, get_fmt, NULL, fmat);
				if (ret >= 0) {
					vfe_dbg(0, "try %s bus ok when pix fmt is %s!\n", bus_name, pix_name);

					return &fmat->format.code;
				}

			}

		}
	}

	vfe_err("try %s bus error when pix fmt is %s!\n", bus_name, pix_name);

	return NULL;
}

static u32 *try_fmt_internal(struct vfe_dev *dev, struct v4l2_format *f)
{
	enum pixel_fmt pix_fmt;
	enum pixel_fmt_type pix_fmt_type;
	struct v4l2_subdev_format fmat;
	struct v4l2_mbus_framefmt *ccm_fmt = &fmat.format;
	u32 *bus_pix_code = NULL;

	vfe_dbg(0, "try_fmt_internal\n");

	/*judge the resolution*/
	if (f->fmt.pix.width > MAX_WIDTH || f->fmt.pix.height > MAX_HEIGHT) {
		vfe_err("size is too large,automatically set to maximum!\n");
		f->fmt.pix.width = MAX_WIDTH;
		f->fmt.pix.height = MAX_HEIGHT;
	}

	pix_fmt = pix_fmt_v4l2_to_common(f->fmt.pix.pixelformat);
	pix_fmt_type = find_pixel_fmt_type(pix_fmt);

	ccm_fmt->width = f->fmt.pix.width;
	ccm_fmt->height = f->fmt.pix.height;
	ccm_fmt->field = f->fmt.pix.field;
	/* find the expect bus format via frame format list */
	if (pix_fmt_type == YUV422_PL || pix_fmt_type == YUV422_SPL || \
			pix_fmt_type == YUV420_PL || pix_fmt_type == YUV420_SPL
#ifdef CONFIG_ARCH_SUN3IW1P1
      || pix_fmt_type == YUV420_MB
#endif
	  ) {
		if (dev->is_isp_used && dev->is_bayer_raw) {
			bus_pix_code = try_fmt_from_sensor(dev, "bayer", "yuv422/yuv420", &fmat);
			if (bus_pix_code == NULL) {
				bus_pix_code = try_fmt_from_sensor(dev, "yuv422", "yuv422/yuv420", &fmat);
				if (bus_pix_code == NULL) {
					if (pix_fmt_type == YUV420_PL || pix_fmt_type == YUV420_SPL) {
						bus_pix_code = try_fmt_from_sensor(dev, "yuv420", "yuv422/yuv420", &fmat);
					} else {
						return NULL;
					}
				}
			}
		} else {
			bus_pix_code = try_fmt_from_sensor(dev, "yuv422", "yuv422/yuv420", &fmat);
			if (bus_pix_code == NULL) {
				if (pix_fmt_type == YUV420_PL || pix_fmt_type == YUV420_SPL) {

				} else {
					return NULL;
				}
			}
		}
	} else if (pix_fmt_type == YUV422_INTLVD) {
		bus_pix_code = try_fmt_from_sensor(dev, "yuv422", "yuv422/yuv420", &fmat);
	} else if (pix_fmt_type == BAYER_RGB) {
		bus_pix_code = try_fmt_from_sensor(dev, "bayer", "yuv422/yuv420", &fmat);
	} else if (pix_fmt_type == RGB565) {
		bus_pix_code = try_fmt_from_sensor(dev, "rgb565", "rgb565", &fmat);
	} else if (pix_fmt_type == RGB888 || pix_fmt_type == PRGB888) {
		bus_pix_code = try_fmt_from_sensor(dev, "rgb888", "rgb888/prgb888", &fmat);
	} else {
		return NULL;
	}
	if (bus_pix_code == NULL)
		return NULL;

	f->fmt.pix.width = ccm_fmt->width;
	f->fmt.pix.height = ccm_fmt->height;

	vfe_dbg(0, "bus pixel code = %x at %s\n", *bus_pix_code, __func__);
	vfe_dbg(0, "pix->width = %d at %s\n", f->fmt.pix.width, __func__);
	vfe_dbg(0, "pix->height = %d at %s\n", f->fmt.pix.height, __func__);

	return bus_pix_code;
}

static int vidioc_try_fmt_vid_cap(struct file *file, void *priv,
							struct v4l2_format *f)
{
	struct vfe_dev *dev = video_drvdata(file);
	u32 *bus_pix_code;

	vfe_dbg(0, "vidioc_try_fmt_vid_cap\n");

	bus_pix_code = try_fmt_internal(dev, f);
	if (!bus_pix_code) {
		vfe_err("pixel format (0x%08x) width %d height %d invalid at %s.\n", \
			f->fmt.pix.pixelformat, f->fmt.pix.width, f->fmt.pix.height, __func__);
		return -EINVAL;
	}

	return 0;
}

static int s_fmt_internal(struct vfe_dev *dev, void *priv,
		struct v4l2_format *f)
{
	struct v4l2_subdev_format fmat;
	struct v4l2_mbus_framefmt *ccm_fmt = &fmat.format;
	struct v4l2_mbus_config mbus_cfg;
	u32 bus_pix_code;
	struct sensor_win_size win_cfg;
	struct main_channel_cfg main_cfg;
	struct v4l2_subdev_format csi_fmt;
	struct v4l2_subdev_format mipi_fmt;
	int ret;

	vfe_dbg(0, "vidioc_s_fmt_vid_cap\n");

	if (vfe_is_generating(dev)) {
		vfe_err("%s device busy\n", __func__);
		return -EBUSY;
	}

	bus_pix_code = *try_fmt_internal(dev, f);
	if (!bus_pix_code) {
		vfe_err("pixel format (0x%08x) width %d height %d invalid at %s.\n", \
			f->fmt.pix.pixelformat, f->fmt.pix.width, f->fmt.pix.height, __func__);
		ret = -EINVAL;
		goto out;
	}
	vfe_dbg(0, "bus pixel code = %x at %s\n", bus_pix_code, __func__);
	vfe_dbg(0, "pix->width = %d at %s\n", f->fmt.pix.width, __func__);
	vfe_dbg(0, "pix->height = %d at %s\n", f->fmt.pix.height, __func__);

	/* get current win configs */
	memset(&win_cfg, 0, sizeof(struct sensor_win_size));
	ret = v4l2_subdev_call(dev->sd, core, ioctl, GET_CURRENT_WIN_CFG, &win_cfg);

	ret = v4l2_subdev_call(dev->sd, video, g_mbus_config, &mbus_cfg);
	if (ret < 0) {
		vfe_err("v4l2 sub device sensor g_mbus_config error!\n");
		goto out;
	}

	ret = v4l2_subdev_call(dev->csi_sd, video, s_mbus_config, &mbus_cfg);
	if (ret < 0) {
		vfe_err("v4l2 sub device csi s_mbus_config error!\n");
		goto out;
	}

	if (mbus_cfg.type == V4L2_MBUS_CSI2) {
		ret = v4l2_subdev_call(dev->mipi_sd, video, s_mbus_config, &mbus_cfg);
		if (ret < 0) {
			vfe_err("v4l2 sub device mipi s_mbus_config error!\n");
			goto out;
		}

		mipi_fmt.reserved[0] = win_cfg.mipi_bps;
		mipi_fmt.format.code = bus_pix_code;
		mipi_fmt.format.field = f->fmt.pix.field;

		ret = v4l2_subdev_call(dev->mipi_sd, pad, set_fmt, NULL, &mipi_fmt);
		if (ret < 0) {
			vfe_err("v4l2 sub device mipi set_fmt error!\n");
			goto out;
		}
		usleep_range(1000, 2000);
		v4l2_subdev_call(dev->mipi_sd, core, s_power, 0);
		bsp_mipi_csi_dphy_enable(dev->mipi_sel);
		v4l2_subdev_call(dev->mipi_sd, core, s_power, 1);
		usleep_range(10000, 12000);
	}

	/* init device */
	ccm_fmt->code = bus_pix_code;
	ccm_fmt->width = f->fmt.pix.width;
	ccm_fmt->height = f->fmt.pix.height;
	ccm_fmt->field = f->fmt.pix.field;

	if (dev->capture_mode == V4L2_MODE_IMAGE)
		sunxi_flash_check_to_start(dev->flash_sd, SW_CTRL_FLASH_ON);
	else
		sunxi_flash_stop(dev->flash_sd);

	ret = v4l2_subdev_call(dev->sd, pad, set_fmt, NULL, &fmat);
	if (ret < 0) {
		vfe_err("v4l2 sub device sensor s_mbus_fmt error!\n");
		goto out;
	}
	/*
	prepare the vfe bsp parameter
	assuming using single channel
	*/
	csi_fmt.format = fmat.format;
	csi_fmt.format.reserved[0] = win_cfg.hoffset;
	csi_fmt.format.reserved[1] = win_cfg.voffset;
	csi_fmt.reserved[0] = f->fmt.pix.pixelformat;

	ret = v4l2_subdev_call(dev->csi_sd, pad, set_fmt, NULL, &csi_fmt);
	if (ret < 0) {
		vfe_err("v4l2 sub device csi set_fmt error!\n");
		goto out;
	}

	dev->fmt.bus_pix_code = bus_pix_code;
	dev->fmt.field = ccm_fmt->field;

	if (dev->is_isp_used) {
		main_cfg.pix = f->fmt.pix;
		main_cfg.win_cfg = win_cfg;
		main_cfg.bus_code = find_bus_type((enum bus_pixelcode)(bus_pix_code));
		ret = v4l2_subdev_call(dev->isp_sd, core, ioctl, VIDIOC_SUNXI_ISP_MAIN_CH_CFG, &main_cfg);
		if (ret < 0)
			vfe_err("vidioc_set_main_channel error! ret = %d\n", ret);
		dev->isp_gen_set_pt->double_ch_flag = 0;
		dev->buf_byte_size = main_cfg.pix.sizeimage;
		vfe_print("dev->buf_byte_size = %d, double_ch_flag = %d\n", dev->buf_byte_size, dev->isp_gen_set_pt->double_ch_flag);
	} else {
		v4l2_subdev_call(dev->csi_sd, core, ioctl, VIDIOC_SUNXI_CSI_GET_FRM_SIZE, &dev->buf_byte_size);
	}
	dev->thumb_width  = 0;
	dev->thumb_height = 0;
	dev->width  = ccm_fmt->width;
	dev->height = ccm_fmt->height;

	dev->mbus_type = mbus_cfg.type;
	if (dev->is_isp_used == 1) {
		vfe_dbg(0, "isp_module_init start!\n");
		if (dev->is_bayer_raw == 1) {
			if (0 == win_cfg.width_input || 0 == win_cfg.height_input) {
				win_cfg.width_input = win_cfg.width;
				win_cfg.height_input = win_cfg.height;
				}
			dev->isp_gen_set_pt->stat.pic_size.width = win_cfg.width_input;
			dev->isp_gen_set_pt->stat.pic_size.height = win_cfg.height_input;

			dev->isp_gen_set_pt->stat.hoffset = win_cfg.hoffset;
			dev->isp_gen_set_pt->stat.voffset = win_cfg.voffset;
			dev->isp_gen_set_pt->stat.hts = win_cfg.hts;
			dev->isp_gen_set_pt->stat.vts = win_cfg.vts;
			dev->isp_gen_set_pt->stat.pclk = win_cfg.pclk;
			dev->isp_gen_set_pt->stat.fps_fixed = win_cfg.fps_fixed;
			dev->isp_gen_set_pt->stat.bin_factor = win_cfg.bin_factor;
			dev->isp_gen_set_pt->stat.intg_min = win_cfg.intg_min;
			dev->isp_gen_set_pt->stat.intg_max = win_cfg.intg_max;
			dev->isp_gen_set_pt->stat.gain_min = win_cfg.gain_min;
			dev->isp_gen_set_pt->stat.gain_max = win_cfg.gain_max;

			if (dev->capture_mode == V4L2_MODE_IMAGE)
				dev->isp_gen_set_pt->sensor_mod = CAPTURE_MODE;
			else if (dev->capture_mode == V4L2_MODE_VIDEO)
				dev->isp_gen_set_pt->sensor_mod = VIDEO_MODE;
			else
				dev->isp_gen_set_pt->sensor_mod = PREVIEW_MODE;

			isp_module_init(dev->isp_gen_set_pt, dev->isp_3a_result_pt);
			dev->ctrl_para.prev_exp_line = 0;
			dev->ctrl_para.prev_ana_gain = 1;
			if (set_sensor_shutter_and_gain(dev) != 0) {
				set_sensor_shutter(dev, dev->isp_3a_result_pt->exp_line_num);
				set_sensor_gain(dev, dev->isp_3a_result_pt->exp_analog_gain);
			}
			usleep_range(50000, 60000);
		} else
			isp_module_init(dev->isp_gen_set_pt, NULL);

		vfe_dbg(0, "isp_module_init end!\n");
	}

	ret = 0;
out:
	return ret;
}

static int vidioc_s_fmt_vid_cap(struct file *file, void *priv,
			struct v4l2_format *f)
{
	struct vfe_dev *dev = video_drvdata(file);
	struct video_device *vdev = video_devdata(file);

	dev->cur_ch = (vdev->num - VID_N_OFF) < 0 ? 0 : (vdev->num - VID_N_OFF);
	if (dev->cur_ch > 0)
		return 0;

	return s_fmt_internal(dev, priv, f);
}

static int vidioc_reqbufs(struct file *file, void *priv,
				struct v4l2_requestbuffers *p)
{
	struct vfe_dev *dev = video_drvdata(file);
	struct video_device *vdev = video_devdata(file);

	dev->cur_ch = (vdev->num - VID_N_OFF) < 0 ? 0 : (vdev->num - VID_N_OFF);

	vfe_dbg(0, "vidioc_reqbufs\n");

	return vb2_reqbufs(&dev->vb_vidq[dev->cur_ch], p);
}

static int vidioc_querybuf(struct file *file, void *priv, struct v4l2_buffer *p)
{
	struct vfe_dev *dev = video_drvdata(file);
	struct video_device *vdev = video_devdata(file);

	dev->cur_ch = (vdev->num - VID_N_OFF) < 0 ? 0 : (vdev->num - VID_N_OFF);

	return vb2_querybuf(&dev->vb_vidq[dev->cur_ch], p);
}

static int vidioc_qbuf(struct file *file, void *priv, struct v4l2_buffer *p)
{
	struct vfe_dev *dev = video_drvdata(file);
	struct video_device *vdev = video_devdata(file);

	dev->cur_ch = (vdev->num - VID_N_OFF) < 0 ? 0 : (vdev->num - VID_N_OFF);

	return vb2_qbuf(&dev->vb_vidq[dev->cur_ch], p);
}

static int vidioc_dqbuf(struct file *file, void *priv, struct v4l2_buffer *p)
{
	struct vfe_dev *dev = video_drvdata(file);
	struct video_device *vdev = video_devdata(file);

	dev->cur_ch = (vdev->num - VID_N_OFF) < 0 ? 0 : (vdev->num - VID_N_OFF);

	vfe_dbg(2, "vidioc dqbuf\n");
	return vb2_dqbuf(&dev->vb_vidq[dev->cur_ch], p, file->f_flags & O_NONBLOCK);
}

#define CSI_PIN1_REG 0xf1c20890
#define CSI_PIN2_REG 0xf1c20894


static int __vfe_streamon(struct vfe_dev *dev, void *priv, enum v4l2_buf_type i)
{
	struct vfe_dmaqueue *dma_q = &dev->vidq[dev->cur_ch];
	struct vfe_isp_stat_buf_queue *isp_stat_bq = &dev->isp_stat_bq;
	struct vfe_buffer *buf = NULL;
	struct vfe_isp_stat_buf *stat_buf_pt;
	int ret = 0, j;

#ifdef CONFIG_ARCH_SUN3IW1P1
	int val;

	writel(0x22222222, CSI_PIN1_REG);

	val = readl(CSI_PIN2_REG) & 0xffff0000;
	val |= 0x2222;
	writel(val, CSI_PIN2_REG);
#endif
	mutex_lock(&dev->stream_lock);
	vfe_dbg(0, "video stream on\n");
	if (i != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		ret = -EINVAL;
		goto streamon_unlock;
	}
	if (dev->cur_ch == 0) {
		if (vfe_is_generating(dev)) {
			vfe_err("stream has been already on\n");
			ret = -1;
			goto streamon_unlock;
		}
		bsp_csi_enable(dev->csi_sel);
		bsp_csi_disable(dev->csi_sel);
		bsp_csi_enable(dev->csi_sel);
	}

	if (dev->is_isp_used) {
		v4l2_subdev_call(dev->isp_sd, video, s_stream, 1);
		bsp_isp_enable();
	}
	if (dev->is_isp_used && dev->is_bayer_raw) {
		/* initial for isp statistic buffer queue */
		INIT_LIST_HEAD(&isp_stat_bq->active);
		INIT_LIST_HEAD(&isp_stat_bq->locked);
		for (j = 0; j < MAX_ISP_STAT_BUF; j++) {
			isp_stat_bq->isp_stat[j].isp_stat_buf.buf_status = BUF_ACTIVE;
			list_add_tail(&isp_stat_bq->isp_stat[j].queue, &isp_stat_bq->active);
		}
	}
	if (dev->special_active == 1) {
		dma_q = &dev->vidq_special;
		vfe_start_generating(dev);
	} else {
		ret = vb2_streamon(&dev->vb_vidq[dev->cur_ch], i);
		if (ret)
			goto streamon_unlock;
	}
	if (!list_empty(&dma_q->active)) {
		buf = list_entry(dma_q->active.next, struct vfe_buffer, list);
	} else {
		vfe_err("stream on, but no buffer now.\n");
		goto streamon_unlock;
	}
	vfe_set_addr(dev, buf, dev->cur_ch);
#if defined(CH_OUTPUT_IN_DIFFERENT_VIDEO)
	if ((dev->cur_ch == 0) && (dev->id == 0)) {
		int ch = 0;
		dma_addr_t dma_addr;

		for (ch = 1; ch < MAX_CH_NUM; ch++) {
			dev->buf_ext[ch].size = dev->buf_byte_size;
			ret = os_mem_alloc(&dev->buf_ext[ch]);
			if (ret)
				goto streamon_unlock;

			dma_addr = (dma_addr_t)dev->buf_ext[ch].dma_addr;
			bsp_csi_set_ch_addr(dev->csi_sel, ch, dma_addr);
		}
	}
#endif
	if (dev->is_isp_used && dev->is_bayer_raw) {
		stat_buf_pt = list_entry(isp_stat_bq->active.next, struct vfe_isp_stat_buf, queue);
		if (stat_buf_pt == NULL) {
			vfe_err("stat_buf_pt =null");
		} else{
			bsp_isp_set_statistics_addr((unsigned long)(stat_buf_pt->dma_addr));
		}
	}

	if (dev->is_isp_used) {
		bsp_isp_set_para_ready();
		bsp_isp_clr_irq_status(ISP_IRQ_EN_ALL);
		bsp_isp_irq_enable(FINISH_INT_EN | SRC0_FIFO_INT_EN);
		if (dev->is_isp_used && dev->is_bayer_raw)
			bsp_csi_int_enable(dev->csi_sel, dev->cur_ch, CSI_INT_VSYNC_TRIG);
	} else {
		bsp_csi_int_clear_status(dev->csi_sel, dev->cur_ch, CSI_INT_ALL);
		bsp_csi_int_enable(dev->csi_sel, dev->cur_ch, CSI_INT_CAPTURE_DONE | \
						      CSI_INT_FRAME_DONE | \
						      CSI_INT_BUF_0_OVERFLOW | \
						      CSI_INT_BUF_1_OVERFLOW | \
						      CSI_INT_BUF_2_OVERFLOW | \
						      CSI_INT_HBLANK_OVERFLOW);
	}
#if defined(CONFIG_ARCH_SUN8IW8P1)
	if (dev->mbus_type == V4L2_MBUS_CSI2)
		bsp_mipi_csi_protocol_enable(dev->mipi_sel);

	usleep_range(10000, 11000);

	if (dev->capture_mode == V4L2_MODE_IMAGE) {
		if (dev->is_isp_used)
			bsp_isp_image_capture_start();
	} else {
		if (dev->is_isp_used)
			bsp_isp_video_capture_start();
	}
	v4l2_subdev_call(dev->csi_sd, video, s_stream, 1);
#else
	if (dev->capture_mode == V4L2_MODE_IMAGE) {
		if (dev->is_isp_used)
			bsp_isp_image_capture_start();
	} else {
		if (dev->is_isp_used)
			bsp_isp_video_capture_start();
	}
	if (dev->cur_ch == 0)
		v4l2_subdev_call(dev->csi_sd, video, s_stream, 1);
	if (dev->mbus_type == V4L2_MBUS_CSI2)
		bsp_mipi_csi_protocol_enable(dev->mipi_sel);
#endif
streamon_unlock:
	mutex_unlock(&dev->stream_lock);

	return ret;
}

static int vidioc_streamon(struct file *file, void *priv, enum v4l2_buf_type i)
{
	struct vfe_dev *dev = video_drvdata(file);
	struct video_device *vdev = video_devdata(file);

	dev->cur_ch = (vdev->num - VID_N_OFF) < 0 ? 0 : (vdev->num - VID_N_OFF);

	return __vfe_streamon(dev, priv, i);
}

static int __vfe_streamoff(struct vfe_dev *dev, void *priv, enum v4l2_buf_type i)
{
	struct vfe_dmaqueue *dma_q = &dev->vidq[dev->cur_ch];
	struct vfe_dmaqueue *donelist = NULL;
	struct vfe_buffer *buffer = NULL;
	unsigned long flags = 0;
	int ret = 0;

#ifdef CONFIG_ARCH_SUN3IW1P1
	u32 val = 0;
#endif

	mutex_lock(&dev->stream_lock);
	vfe_dbg(0, "video stream off\n");
	if (dev->cur_ch == 0) {
		if (!vfe_is_generating(dev)) {
			vfe_err("stream has been already off\n");
			ret = 0;
			goto streamoff_unlock;
		}
		isp_streamoff_torch_and_flash_close(dev);
	}

	if (dev->is_isp_used) {
		vfe_dbg(0, "disable isp int in streamoff\n");
		bsp_isp_irq_disable(ISP_IRQ_EN_ALL);
		bsp_isp_clr_irq_status(ISP_IRQ_EN_ALL);
	} else {
		vfe_dbg(0, "disable csi int in streamoff\n");
		bsp_csi_int_disable(dev->csi_sel, dev->cur_ch, CSI_INT_ALL);
		bsp_csi_int_clear_status(dev->csi_sel, dev->cur_ch, CSI_INT_ALL);
	}
	if (dev->cur_ch == 0)
		v4l2_subdev_call(dev->csi_sd, video, s_stream, 0);
	if (dev->capture_mode == V4L2_MODE_IMAGE) {
		if (dev->is_isp_used)
			bsp_isp_image_capture_stop();
		vfe_dbg(0, "dev->capture_mode = %d\n", dev->capture_mode);
	} else {
		if (dev->is_isp_used)
			bsp_isp_video_capture_stop();
		vfe_dbg(0, "dev->capture_mode = %d\n", dev->capture_mode);
	}
	if (i != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		ret = -EINVAL;
		goto streamoff_unlock;
	}
	if (dev->mbus_type == V4L2_MBUS_CSI2)
		bsp_mipi_csi_protocol_disable(dev->mipi_sel);

	if (dev->special_active == 1) {
		dma_q = &dev->vidq_special;
		donelist = &dev->done_special;
		vfe_stop_generating(dev);
		spin_lock_irqsave(&dev->slock, flags);
		while (!list_empty(&dma_q->active)) {
			buffer = list_first_entry(&dma_q->active, struct vfe_buffer, list);
			list_del(&buffer->list);
			list_add(&buffer->list, &donelist->active);
		}
		spin_unlock_irqrestore(&dev->slock, flags);
	} else {
		ret = vb2_streamoff(&dev->vb_vidq[dev->cur_ch], i);
		if (ret != 0) {
			vfe_err("videobu_streamoff error!\n");
			goto streamoff_unlock;
		}
	}

#if defined(CH_OUTPUT_IN_DIFFERENT_VIDEO)
	if ((dev->cur_ch == 0) && (dev->id == 0)) {
		int ch = 0;

		for (ch = 1; ch < MAX_CH_NUM; ch++)
			os_mem_free(&dev->buf_ext[ch]);
	}
#endif

	if (dev->is_isp_used)
		bsp_isp_disable();

	if (dev->cur_ch == 0)
		bsp_csi_disable(dev->csi_sel);
#ifdef CONFIG_ARCH_SUN3IW1P1
	writel(0x7777777, CSI_PIN1_REG);
	val = readl(CSI_PIN2_REG) & 0xffff0000;
	val |= 0x7777;
	writel(val, CSI_PIN2_REG);
#endif

streamoff_unlock:
	mutex_unlock(&dev->stream_lock);

	return ret;
}

static int vidioc_streamoff(struct file *file, void *priv, enum v4l2_buf_type i)
{
	struct vfe_dev *dev = video_drvdata(file);
	struct video_device *vdev = video_devdata(file);

	dev->cur_ch = (vdev->num - VID_N_OFF) < 0 ? 0 : (vdev->num - VID_N_OFF);

	return __vfe_streamoff(dev, priv, i);
}

static int vidioc_enum_input(struct file *file, void *priv,
	struct v4l2_input *inp)
{
	struct vfe_dev *dev = video_drvdata(file);

	if (inp->index > dev->dev_qty-1) {
		vfe_err("input index(%d) > dev->dev_qty(%d)-1 invalid!\n",
						inp->index, dev->dev_qty);
		return -EINVAL;
	}
	if (dev->device_valid_flag[inp->index] == 0) {
		vfe_err("input index(%d) > dev->dev_qty(%d)-1 invalid!, device_valid_flag[%d] = %d\n",
			inp->index, dev->dev_qty, inp->index, dev->device_valid_flag[inp->index]);
		return -EINVAL;
	}
	inp->type = V4L2_INPUT_TYPE_CAMERA;

	return 0;
}

static int vidioc_g_input(struct file *file, void *priv, unsigned int *i)
{
	struct vfe_dev *dev = video_drvdata(file);

	*i = dev->input;
	return 0;
}

static int vfe_ctrl_para_reset(struct vfe_dev *dev)
{
	dev->ctrl_para.gsensor_rot = 0;
	dev->ctrl_para.prev_exp_line = 16;
	dev->ctrl_para.prev_ana_gain = 16;
	dev->ctrl_para.prev_focus_pos = 50;

	return 0;
}

static int internal_s_input(struct vfe_dev *dev, unsigned int i)
{
	struct v4l2_control ctrl;
	struct sensor_item sensor_info;
	unsigned long core_clk;
	int ret;

	if (i > dev->dev_qty-1) {
		vfe_err("set input i(%d)>dev_qty(%d)-1 error!\n", i, dev->dev_qty);
		return -EINVAL;
	}

	if (i == dev->input)
		return 0;

	if (dev->input != -1) {
	/*Power down current device*/
		if (dev->sd_act != NULL)
			v4l2_subdev_call(dev->sd_act, core, ioctl, ACT_SOFT_PWDN, 0);

		ret = vfe_set_sensor_power_off(dev);
		if (ret < 0)
			goto altend;
	}
	vfe_dbg(0, "input_num = %d\n", i);

	dev->input = i;

	/* Alternate the device info and select target device*/
	update_ccm_info(dev, dev->ccm_cfg[i]);

	/* set vfe core clk rate for each sensor! */
	if (get_sensor_info(dev->ccm_cfg[i]->ccm, &sensor_info) == 0) {
		core_clk = sensor_info.core_clk_for_sensor;
	} else {
		core_clk = CSI_CORE_CLK_RATE;
	}
	v4l2_subdev_call(dev->csi_sd, core, ioctl, VIDIOC_SUNXI_CSI_SET_CORE_CLK, &core_clk);

	/* alternate isp setting */
	update_isp_setting(dev);
	if (dev->is_bayer_raw)
		isp_param_init(dev->isp_gen_set_pt);

	if (dev->flash_used == 1)
		sunxi_flash_info_init(dev->flash_sd);

	/* Initial target device */
	ret = vfe_set_sensor_power_on(dev);
#ifdef USE_SPECIFIC_CCI
	csi_cci_init_helper(dev->cci_sel);
#endif
	if (ret != 0) {
		vfe_err("sensor standby off error when selecting target device!\n");
		goto altend;
	}

	ret = v4l2_subdev_call(dev->sd, core, init, 0);
	if (ret != 0) {
		vfe_err("sensor initial error when selecting target device!\n");
		goto altend;
	}

	if (dev->sd_act != NULL) {
		struct actuator_para_t vcm_para;

		vcm_para.active_min = dev->isp_gen_set_pt->isp_ini_cfg.isp_3a_settings.vcm_min_code;
		vcm_para.active_max = dev->isp_gen_set_pt->isp_ini_cfg.isp_3a_settings.vcm_max_code;
		vfe_dbg(0, "min/max=%d/%d\n", dev->isp_gen_set_pt->isp_ini_cfg.isp_3a_settings.vcm_min_code,
		dev->isp_gen_set_pt->isp_ini_cfg.isp_3a_settings.vcm_max_code);
		v4l2_subdev_call(dev->sd_act, core, ioctl, ACT_INIT, &vcm_para);
	}

	bsp_csi_disable(dev->csi_sel);
	if (dev->is_isp_used) {
		vfe_ctrl_para_reset(dev);
		bsp_isp_disable();
		bsp_isp_enable();
		bsp_isp_init(&dev->isp_init_para);
		/* Set the initial flip */
		ctrl.id = V4L2_CID_VFLIP;
		ctrl.value = dev->ccm_cfg[i]->vflip;
		ret = v4l2_subdev_call(dev->isp_sd, core, s_ctrl, &ctrl);
		if (ret != 0) {
			vfe_err("isp s_ctrl V4L2_CID_VFLIP error when vidioc_s_input!input_num = %d\n", i);
		}
		ctrl.id = V4L2_CID_VFLIP_THUMB;
		ctrl.value = dev->ccm_cfg[i]->vflip;
		ret = v4l2_subdev_call(dev->isp_sd, core, s_ctrl, &ctrl);
		if (ret != 0)
			vfe_err("isp s_ctrl V4L2_CID_VFLIP_THUMB error when vidioc_s_input!input_num = %d\n", i);

		ctrl.id = V4L2_CID_HFLIP;
		ctrl.value = dev->ccm_cfg[i]->hflip;
		ret = v4l2_subdev_call(dev->isp_sd, core, s_ctrl, &ctrl);
		if (ret != 0)
			vfe_err("isp s_ctrl V4L2_CID_HFLIP error when vidioc_s_input!input_num = %d\n", i);

		ctrl.id = V4L2_CID_HFLIP_THUMB;
		ctrl.value = dev->ccm_cfg[i]->hflip;
		ret = v4l2_subdev_call(dev->isp_sd, core, s_ctrl, &ctrl);
		if (ret != 0)
			vfe_err("isp s_ctrl V4L2_CID_HFLIP error when vidioc_s_input!input_num = %d\n", i);
	} else {
		/* bsp_isp_exit(); */
		/* Set the initial flip */
		ctrl.id = V4L2_CID_VFLIP;
		ctrl.value = dev->ccm_cfg[i]->vflip;
		ret = v4l2_subdev_call(dev->sd, core, s_ctrl, &ctrl);
		if (ret != 0)
			vfe_err("sensor sensor_s_ctrl V4L2_CID_VFLIP error when vidioc_s_input!input_num = %d\n", i);

		ctrl.id = V4L2_CID_HFLIP;
		ctrl.value = dev->ccm_cfg[i]->hflip;
		ret = v4l2_subdev_call(dev->sd, core, s_ctrl, &ctrl);
		if (ret != 0)
			vfe_err("sensor sensor_s_ctrl V4L2_CID_HFLIP error when vidioc_s_input!input_num = %d\n", i);
	}

	ret = 0;
altend:
	dev->vfe_s_input_flag = 1;
	return ret;
}

static int vidioc_s_input(struct file *file, void *priv, unsigned int i)
{
	struct vfe_dev *dev = video_drvdata(file);
	struct video_device *vdev = video_devdata(file);

	dev->cur_ch = (vdev->num - VID_N_OFF) < 0 ? 0 : (vdev->num - VID_N_OFF);
	if (dev->cur_ch > 0)
		return 0;

	vfe_dbg(0, "%s ,input_num = %d\n", __func__, i);
	return internal_s_input(dev, i);
}

struct vfe_command {
	char name[32];
	int v4l2_item;
	int isp_item;
};

static struct vfe_command vfe_power_line_frequency[] = {

	{"frequency disabled", V4L2_CID_POWER_LINE_FREQUENCY_DISABLED, FREQUENCY_DISABLED,},
	{"frequency 50hz", V4L2_CID_POWER_LINE_FREQUENCY_50HZ, FREQUENCY_50HZ,},
	{"frequency 60hz", V4L2_CID_POWER_LINE_FREQUENCY_60HZ, FREQUENCY_60HZ,},
	{"frequency auto", V4L2_CID_POWER_LINE_FREQUENCY_AUTO, FREQUENCY_AUTO,},
};

static struct vfe_command vfe_colorfx[] = {
	{"NONE        ",     V4L2_COLORFX_NONE,		COLORFX_NONE,},
	{"BW          ",     V4L2_COLORFX_BW,		COLORFX_BW,},
	{"SEPIA       ",     V4L2_COLORFX_SEPIA,	COLORFX_SEPIA,},
	{"NEGATIVE    ",     V4L2_COLORFX_NEGATIVE,	COLORFX_NEGATIVE,},
	{"EMBOSS      ",     V4L2_COLORFX_EMBOSS,	COLORFX_EMBOSS,},
	{"SKETCH      ",     V4L2_COLORFX_SKETCH,	COLORFX_SKETCH,},
	{"SKY_BLUE    ",     V4L2_COLORFX_SKY_BLUE,	COLORFX_SKY_BLUE,},
	{"GRASS_GREEN ",     V4L2_COLORFX_GRASS_GREEN,	COLORFX_GRASS_GREEN,},
	{"SKIN_WHITEN ",     V4L2_COLORFX_SKIN_WHITEN,	COLORFX_SKIN_WHITEN,},
	{"VIVID       ",     V4L2_COLORFX_VIVID,	COLORFX_VIVID,},
	{"AQUA        ",     V4L2_COLORFX_AQUA,		COLORFX_AQUA,},
	{"ART_FREEZE  ",     V4L2_COLORFX_ART_FREEZE,	COLORFX_ART_FREEZE,},
	{"SILHOUETTE  ",     V4L2_COLORFX_SILHOUETTE,	COLORFX_SILHOUETTE,},
	{"SOLARIZATION",     V4L2_COLORFX_SOLARIZATION,	COLORFX_SOLARIZATION,},
	{"ANTIQUE     ",     V4L2_COLORFX_ANTIQUE,	COLORFX_ANTIQUE,},
	{"SET_CBCR    ",     V4L2_COLORFX_SET_CBCR,	COLORFX_SET_CBCR,},
};

static struct vfe_command vfe_ae_mode[] = {
	{"EXPOSURE_AUTO",		V4L2_EXPOSURE_AUTO,			EXP_AUTO,},
	{"EXPOSURE_MANUAL",		V4L2_EXPOSURE_MANUAL,			EXP_MANUAL,},
	{"EXPOSURE_SHUTTER_PRIORITY",	V4L2_EXPOSURE_SHUTTER_PRIORITY,		EXP_AUTO,},
	{"EXPOSURE_APERTURE_PRIORITY",	V4L2_EXPOSURE_APERTURE_PRIORITY,	EXP_AUTO,},
};

static struct vfe_command vfe_wb[] = {
	{"WB_MANUAL       ",     V4L2_WHITE_BALANCE_MANUAL,	WB_MANUAL,},
	{"WB_AUTO         ",     V4L2_WHITE_BALANCE_AUTO,	WB_AUTO,},
	{"WB_INCANDESCENT ",     V4L2_WHITE_BALANCE_INCANDESCENT,	WB_INCANDESCENT,},
	{"WB_FLUORESCENT  ",     V4L2_WHITE_BALANCE_FLUORESCENT,	WB_FLUORESCENT,},
	{"WB_FLUORESCENT_H",     V4L2_WHITE_BALANCE_FLUORESCENT_H,    WB_FLUORESCENT_H,},
	{"WB_HORIZON      ",     V4L2_WHITE_BALANCE_HORIZON,    WB_HORIZON,},
	{"WB_DAYLIGHT     ",     V4L2_WHITE_BALANCE_DAYLIGHT,    WB_DAYLIGHT,},
	{"WB_FLASH        ",     V4L2_WHITE_BALANCE_FLASH,    WB_FLASH,},
	{"WB_CLOUDY       ",     V4L2_WHITE_BALANCE_CLOUDY,    WB_CLOUDY,},
	{"WB_SHADE        ",     V4L2_WHITE_BALANCE_SHADE,    WB_SHADE,},
};

static struct vfe_command vfe_iso[] = {
	{"ISO_SENSITIVITY_MANUAL",  V4L2_ISO_SENSITIVITY_MANUAL, ISO_MANUAL,},
	{"ISO_SENSITIVITY_AUTO",   V4L2_ISO_SENSITIVITY_AUTO,	ISO_AUTO,},
};
static struct vfe_command vfe_scene[] = {
	{"SCENE_MODE_NONE        ",     V4L2_SCENE_MODE_NONE,	SCENE_MODE_NONE},
	{"SCENE_MODE_BACKLIGHT   ",     V4L2_SCENE_MODE_BACKLIGHT,  SCENE_MODE_BACKLIGHT,},
	{"SCENE_MODE_BEACH_SNOW  ",     V4L2_SCENE_MODE_BEACH_SNOW,  SCENE_MODE_BEACH_SNOW,},
	{"SCENE_MODE_CANDLE_LIGHT",     V4L2_SCENE_MODE_CANDLE_LIGHT,  SCENE_MODE_CANDLE_LIGHT,},
	{"SCENE_MODE_DAWN_DUSK   ",     V4L2_SCENE_MODE_DAWN_DUSK,  SCENE_MODE_DAWN_DUSK,},
	{"SCENE_MODE_FALL_COLORS ",     V4L2_SCENE_MODE_FALL_COLORS,  SCENE_MODE_FALL_COLORS,},
	{"SCENE_MODE_FIREWORKS   ",     V4L2_SCENE_MODE_FIREWORKS,  SCENE_MODE_FIREWORKS,},
	{"SCENE_MODE_LANDSCAPE   ",     V4L2_SCENE_MODE_LANDSCAPE,  SCENE_MODE_LANDSCAPE,},
	{"SCENE_MODE_NIGHT       ",     V4L2_SCENE_MODE_NIGHT,  SCENE_MODE_NIGHT,},
	{"SCENE_MODE_PARTY_INDOOR",     V4L2_SCENE_MODE_PARTY_INDOOR,  SCENE_MODE_PARTY_INDOOR,},
	{"SCENE_MODE_PORTRAIT    ",     V4L2_SCENE_MODE_PORTRAIT,  SCENE_MODE_PORTRAIT,},
	{"SCENE_MODE_SPORTS      ",     V4L2_SCENE_MODE_SPORTS,  SCENE_MODE_SPORTS,},
	{"SCENE_MODE_SUNSET      ",     V4L2_SCENE_MODE_SUNSET,  SCENE_MODE_SUNSET,},
	{"SCENE_MODE_TEXT        ",     V4L2_SCENE_MODE_TEXT,  SCENE_MODE_TEXT,},
};

static struct vfe_command vfe_af_range[] = {
	{"AF_RANGE_AUTO",	V4L2_AUTO_FOCUS_RANGE_AUTO,	AF_RANGE_AUTO,},
	{"AF_RANGE_NORMAL",	V4L2_AUTO_FOCUS_RANGE_NORMAL,	AF_RANGE_NORMAL,},
	{"AF_RANGE_MACRO",	V4L2_AUTO_FOCUS_RANGE_MACRO,	AF_RANGE_MACRO,},
	{"AF_RANGE_INFINITY",	V4L2_AUTO_FOCUS_RANGE_INFINITY, AF_RANGE_INFINITY,},
};

static struct vfe_command vfe_flash_mode[] = {
	{"FLASH_LED_MODE_NONE	 ",	V4L2_FLASH_LED_MODE_NONE,	FLASH_MODE_OFF,},
	{"FLASH_LED_MODE_FLASH	 ",	V4L2_FLASH_LED_MODE_FLASH,	FLASH_MODE_ON,},
	{"FLASH_LED_MODE_TORCH	 ",	V4L2_FLASH_LED_MODE_TORCH,	FLASH_MODE_TORCH,},
	{"FLASH_LED_MODE_AUTO	 ",	V4L2_FLASH_LED_MODE_AUTO,	FLASH_MODE_AUTO,},
	{"FLASH_LED_MODE_RED_EYE",	V4L2_FLASH_LED_MODE_RED_EYE,	FLASH_MODE_RED_EYE,},
};

static struct vfe_command vfe_focus_status[] = {
	{"V4L2_AUTO_FOCUS_STATUS_IDLE	 ",	V4L2_AUTO_FOCUS_STATUS_IDLE,	AUTO_FOCUS_STATUS_IDLE,	},
	{"V4L2_AUTO_FOCUS_STATUS_BUSY	 ",	V4L2_AUTO_FOCUS_STATUS_BUSY,	AUTO_FOCUS_STATUS_BUSY, },
	{"V4L2_AUTO_FOCUS_STATUS_REACHED	 ",	V4L2_AUTO_FOCUS_STATUS_REACHED,		AUTO_FOCUS_STATUS_REACHED,},
	{"V4L2_AUTO_FOCUS_STATUS_BUSY	 ",	V4L2_AUTO_FOCUS_STATUS_BUSY,	V4L2_AUTO_FOCUS_STATUS_BUSY,},
	{"V4L2_AUTO_FOCUS_STATUS_BUSY",	V4L2_AUTO_FOCUS_STATUS_BUSY,	AUTO_FOCUS_STATUS_REFOCUS,},
	{"V4L2_AUTO_FOCUS_STATUS_BUSY	 ",	V4L2_AUTO_FOCUS_STATUS_BUSY,	AUTO_FOCUS_STATUS_FINDED,},
	{"V4L2_AUTO_FOCUS_STATUS_FAILED",	V4L2_AUTO_FOCUS_STATUS_FAILED,	AUTO_FOCUS_STATUS_FAILED,},
};

enum vfe_command_tpye {
	VFE_POWER_LINE_FREQUENCY,
	VFE_COLORFX,
	VFE_AE_MODE,
	VFE_WB,
	VFE_ISO,
	VFE_SCENE,
	VFE_AF_RANGE,
	VFE_FLASH_MODE,
	VFE_FOCUS_STATUS,
	VFE_COMMAND_MAX,
};

struct vfe_command_adapter {
	struct vfe_command *cmd_pt;
	int					size;
};

struct vfe_command_adapter vfe_cmd_adapter[] = {

	{&vfe_power_line_frequency[0], ARRAY_SIZE(vfe_power_line_frequency)},
	{&vfe_colorfx[0], ARRAY_SIZE(vfe_colorfx)},
	{&vfe_ae_mode[0], ARRAY_SIZE(vfe_ae_mode)},
	{&vfe_wb[0], ARRAY_SIZE(vfe_wb)},
	{&vfe_iso[0], ARRAY_SIZE(vfe_iso)},
	{&vfe_scene[0], ARRAY_SIZE(vfe_scene)},
	{&vfe_af_range[0], ARRAY_SIZE(vfe_af_range)},
	{&vfe_flash_mode[0], ARRAY_SIZE(vfe_flash_mode)},
	{&vfe_focus_status[0], ARRAY_SIZE(vfe_focus_status)},
};

enum {
	V4L2_TO_ISP,
	ISP_TO_V4L2,
};

int vfe_v4l2_isp(int type, int cmd, int flag)
{
	struct vfe_command_adapter cmd_adapter;
	int i;

	if (type >= ARRAY_SIZE(vfe_cmd_adapter))
		vfe_err("vfe command tpye ERR, type = %d\n", type);

	cmd_adapter = vfe_cmd_adapter[type];
	if (flag == V4L2_TO_ISP) {
		for (i = 0; i < cmd_adapter.size; i++) {
			if (cmd == cmd_adapter.cmd_pt[i].v4l2_item) {
				vfe_dbg(0, "vfe set %s, cmd = %d\n", cmd_adapter.cmd_pt[i].name, cmd);
				return cmd_adapter.cmd_pt[i].isp_item;
			}
		}
	} else if (flag == ISP_TO_V4L2) {
		for (i = 0; i < cmd_adapter.size; i++) {
			if (cmd == cmd_adapter.cmd_pt[i].isp_item) {
				vfe_dbg(0, "vfe get %s, cmd = %d\n", cmd_adapter.cmd_pt[i].name, cmd);
				return cmd_adapter.cmd_pt[i].v4l2_item;
			}
		}
	}
	vfe_err("command conver ERR, cmd = %d\n", cmd);

	return 0;
}

static int vidioc_g_parm(struct file *file, void *priv,
						struct v4l2_streamparm *parms)
{
	struct vfe_dev *dev = video_drvdata(file);
	int ret;

	ret = v4l2_subdev_call(dev->sd, video, g_parm, parms);
	if (ret < 0)
		vfe_warn("v4l2 sub device g_parm fail!\n");

	return ret;
}

static int vidioc_s_parm(struct file *file, void *priv,
						struct v4l2_streamparm *parms)
{
	struct vfe_dev *dev = video_drvdata(file);
	int ret;

	if (parms->parm.capture.capturemode != V4L2_MODE_VIDEO && \
				parms->parm.capture.capturemode != V4L2_MODE_IMAGE && \
				parms->parm.capture.capturemode != V4L2_MODE_PREVIEW) {
		parms->parm.capture.capturemode = V4L2_MODE_PREVIEW;
	}

	dev->capture_mode = parms->parm.capture.capturemode;

	ret = v4l2_subdev_call(dev->sd, video, s_parm, parms);
	if (ret < 0)
		vfe_warn("v4l2 sub device s_parm error!\n");

	ret = v4l2_subdev_call(dev->csi_sd, video, s_parm, parms);
	if (ret < 0)
		vfe_warn("v4l2 sub device s_parm error!\n");

	return ret;
}

int isp_ae_stat_req(struct file *file, struct v4l2_fh *fh, struct isp_stat_buf *ae_buf)
{
	struct vfe_dev *dev = video_drvdata(file);
	int ret = 0;

	ae_buf->buf_size = ISP_STAT_AE_MEM_SIZE;
	ret = copy_to_user(ae_buf->buf,
			    dev->isp_gen_set_pt->stat.ae_buf,
			    ae_buf->buf_size);
	return ret;
}

int isp_gamma_req(struct file *file, struct v4l2_fh *fh, struct isp_stat_buf *gamma_buf)
{
	struct vfe_dev *dev = video_drvdata(file);
	int ret = 0;

	gamma_buf->buf_size = ISP_GAMMA_MEM_SIZE;
	ret = copy_to_user(gamma_buf->buf,
			    dev->isp_gen_set_pt->isp_ini_cfg.isp_tunning_settings.gamma_tbl_post,
			    gamma_buf->buf_size);
	return ret;
}

int isp_hist_stat_req(struct file *file, struct v4l2_fh *fh, struct isp_stat_buf *hist_buf)
{
	struct vfe_dev *dev = video_drvdata(file);
	int ret = 0;

	hist_buf->buf_size = ISP_STAT_HIST_MEM_SIZE;
	ret = copy_to_user(hist_buf->buf,
			    dev->isp_gen_set_pt->stat.hist_buf,
			    hist_buf->buf_size);
	return ret;
}

int isp_af_stat_req(struct file *file, struct v4l2_fh *fh, struct isp_stat_buf *af_buf)
{
	struct vfe_dev *dev = video_drvdata(file);
	int ret = 0;

	af_buf->buf_size = ISP_STAT_AF_MEM_SIZE;

	ret = copy_to_user(af_buf->buf,
			    dev->isp_gen_set_pt->stat.af_buf,
			    af_buf->buf_size);
	return 0;
}

static int isp_exif_req(struct file *file, struct v4l2_fh *fh, struct isp_exif_attribute *exif_attr)
{
	struct vfe_dev *dev = video_drvdata(file);
	struct sensor_exif_attribute exif;

	if (dev->isp_gen_set_pt && dev->is_bayer_raw) {
		exif_attr->fnumber = dev->isp_gen_set_pt->isp_ini_cfg.isp_3a_settings.fno;
		if (exif_attr->fnumber < 40)
			exif_attr->fnumber = 240;
		exif_attr->focal_length = dev->isp_gen_set_pt->isp_ini_cfg.isp_tunning_settings.focus_length;
		if (dev->isp_gen_set_pt->isp_ini_cfg.isp_test_settings.isp_test_mode == ISP_TEST_ALL_ENABLE) {
			if (exif_attr->focal_length < 40)
				exif_attr->focal_length = 400;
		} else
			exif_attr->focal_length = dev->isp_3a_result_pt->real_vcm_pos;

		exif_attr->brightness = dev->isp_gen_set_pt->ae_lum;
		exif_attr->exposure_bias = dev->isp_gen_set_pt->exp_settings.exp_compensation;
		exif_attr->flash_fire = dev->isp_gen_set_pt->exp_settings.flash_open;
		exif_attr->iso_speed = (dev->isp_3a_result_pt->exp_analog_gain * dev->isp_3a_result_pt->exp_digital_gain) * 50 / 4096;
		exif_attr->exposure_time.numerator = 1;
		exif_attr->shutter_speed.numerator = 1;
		if (dev->isp_3a_result_pt->exp_time != 0) {
			exif_attr->exposure_time.denominator = 1000000/dev->isp_3a_result_pt->exp_time;
			exif_attr->shutter_speed.denominator = 1000000/dev->isp_3a_result_pt->exp_time;
		} else {
			exif_attr->exposure_time.denominator = 10000;
			exif_attr->shutter_speed.denominator = 10000;
		}
		exif_attr->reserved[0] = dev->isp_3a_result_pt->real_vcm_pos;
		exif_attr->reserved[1] = dev->isp_gen_set_pt->color_temp;
	} else {
		if (v4l2_subdev_call(dev->sd, core, ioctl, GET_SENSOR_EXIF, &exif) != 0) {
			exif_attr->fnumber = 240;
			exif_attr->focal_length = 180;
			exif_attr->brightness = 128;
			exif_attr->exposure_bias = dev->ctrl_para.exp_bias;
			exif_attr->flash_fire = 0;
			exif_attr->iso_speed = 200;
			exif_attr->exposure_time.numerator = 1;
			exif_attr->exposure_time.denominator = 20;
			exif_attr->shutter_speed.numerator = 1;
			exif_attr->shutter_speed.denominator = 24;
		} else {
			exif_attr->fnumber = exif.fnumber;
			exif_attr->focal_length = exif.focal_length;
			exif_attr->brightness = exif.brightness;
			exif_attr->exposure_bias = dev->ctrl_para.exp_bias;
			exif_attr->flash_fire = exif.flash_fire;
			exif_attr->iso_speed = exif.iso_speed;
			exif_attr->exposure_time.numerator = exif.exposure_time_num;
			exif_attr->exposure_time.denominator = exif.exposure_time_den;
			exif_attr->shutter_speed.numerator = exif.exposure_time_num;
			exif_attr->shutter_speed.denominator = exif.exposure_time_den;
		}
	}

	return 0;
}

static int __isp_auto_focus_win(struct vfe_dev *dev, struct v4l2_win_setting *af_win)
{
	int i;
	struct ccm_config *ccm_curr = &dev->ccm_cfg_content[dev->input];

	if (af_win->win_num == 0) {
		bsp_isp_s_auto_focus_win_num(dev->isp_gen_set_pt, AF_AUTO_WIN, NULL);
	} else if (af_win->win_num > V4L2_MAX_WIN_NUM) {
		return -EINVAL;
	} else {
		struct v4l2_win_coordinate *win_coor = &af_win->coor[0];

		for (i = 0; i < af_win->win_num; i++) {
			if (ccm_curr->vflip == 1) {
				dev->ctrl_para.af_coor[i].y1 = -win_coor[0].y1;
				dev->ctrl_para.af_coor[i].y2 = -win_coor[0].y2;
			} else {
				dev->ctrl_para.af_coor[i].y1 = win_coor[i].y1;
				dev->ctrl_para.af_coor[i].y2 = win_coor[i].y2;
			}
			if (ccm_curr->hflip == 1) {
				dev->ctrl_para.af_coor[i].x1 = -win_coor[0].x1;
				dev->ctrl_para.af_coor[i].x2 = -win_coor[0].x2;
			} else {
				dev->ctrl_para.af_coor[i].x1 = win_coor[0].x1;
				dev->ctrl_para.af_coor[i].x2 = win_coor[0].x2;
			}
		}
		bsp_isp_s_auto_focus_win_num(dev->isp_gen_set_pt, AF_NUM_WIN, &dev->ctrl_para.af_coor[0]);
	}

	return 0;
}

static int __isp_auto_exp_win(struct vfe_dev *dev, struct v4l2_win_setting *ae_win)
{
	int i;
	struct ccm_config *ccm_curr = &dev->ccm_cfg_content[dev->input];

	if (ae_win->win_num == 0) {
		bsp_isp_s_auto_exposure_win_num(dev->isp_gen_set_pt, AE_AUTO_WIN, NULL);
	} else if (ae_win->win_num > V4L2_MAX_WIN_NUM) {
		return -EINVAL;
	} else {
		struct v4l2_win_coordinate *win_coor = &ae_win->coor[0];

		for (i = 0; i < ae_win->win_num; i++) {
			if (ccm_curr->vflip == 1) {
				dev->ctrl_para.ae_coor[i].y1 = -win_coor[0].y1;
				dev->ctrl_para.ae_coor[i].y2 = -win_coor[0].y2;
			} else {
				dev->ctrl_para.ae_coor[i].y1 = win_coor[i].y1;
				dev->ctrl_para.ae_coor[i].y2 = win_coor[i].y2;
			}
			if (ccm_curr->hflip == 1) {
				dev->ctrl_para.ae_coor[i].x1 = -win_coor[0].x1;
				dev->ctrl_para.ae_coor[i].x2 = -win_coor[0].x2;
			} else {
				dev->ctrl_para.ae_coor[i].x1 = win_coor[0].x1;
				dev->ctrl_para.ae_coor[i].x2 = win_coor[0].x2;
			}
		}
		dev->isp_gen_set_pt->win.hist_coor.x1 = win_coor[0].x1;
		dev->isp_gen_set_pt->win.hist_coor.y1 = win_coor[0].y1;
		dev->isp_gen_set_pt->win.hist_coor.x2 = win_coor[0].x2;
		dev->isp_gen_set_pt->win.hist_coor.y2 = win_coor[0].y2;
		bsp_isp_s_auto_exposure_win_num(dev->isp_gen_set_pt, AE_SINGLE_WIN, &dev->ctrl_para.ae_coor[0]);
	}

	return 0;
}

int vidioc_auto_focus_win(struct file *file, struct v4l2_fh *fh, struct v4l2_win_setting *af_win)
{
	struct vfe_dev *dev = video_drvdata(file);
	int ret = 0;

	if (dev->isp_gen_set_pt && dev->is_bayer_raw)
		ret = __isp_auto_focus_win(dev, af_win);
	else
		ret = v4l2_subdev_call(dev->sd, core, ioctl, SET_AUTO_FOCUS_WIN, af_win);

	return ret;
}

int vidioc_auto_exposure_win(struct file *file, struct v4l2_fh *fh, struct v4l2_win_setting *exp_win)
{
	struct vfe_dev *dev = video_drvdata(file);
	int ret = 0;

	if (dev->isp_gen_set_pt && dev->is_bayer_raw)
		ret = __isp_auto_exp_win(dev, exp_win);
	else
		ret = v4l2_subdev_call(dev->sd, core, ioctl, SET_AUTO_EXPOSURE_WIN, exp_win);

	return ret;
}

int vidioc_hdr_ctrl(struct file *file, struct v4l2_fh *fh, struct isp_hdr_ctrl *hdr)
{
	struct vfe_dev *dev = video_drvdata(file);

	if (dev->isp_gen_set_pt && dev->is_bayer_raw) {
		if (hdr->flag == HDR_CTRL_SET) {
			bsp_isp_s_hdr(dev->isp_gen_set_pt, (struct hdr_setting_t *)(&hdr->hdr_t));
			dev->isp_3a_result_pt->image_quality.bits.hdr_cnt = 0;
		} else {
			hdr->count = dev->isp_gen_set_pt->hdr_setting.frames_count - 1;
			memcpy(&hdr->hdr_t, &dev->isp_gen_set_pt->hdr_setting, sizeof(struct isp_hdr_setting_t));
		}

		return 0;
	}

	return -EINVAL;
}

int vidioc_set_subchannel(struct file *file, struct v4l2_fh *fh, struct v4l2_pix_format *sub)
{
	int ret = 0;
	struct vfe_dev *dev = video_drvdata(file);

	if (!dev->is_isp_used) {
		vfe_err("isp must be set first when set subchannel\n");
		return -1;
	}
	ret = v4l2_subdev_call(dev->isp_sd, core, ioctl, VIDIOC_SUNXI_ISP_SUB_CH_CFG, sub);
	if (ret < 0) {
		vfe_err("vidioc_set_subchannel error! ret = %d\n", ret);
		return ret;
	}

	dev->buf_byte_size = sub->sizeimage;
	dev->isp_gen_set_pt->double_ch_flag = 1;
	dev->thumb_width  = sub->width;
	dev->thumb_height = sub->height;

	return ret;
}

int vidioc_set_rotchannel(struct file *file, struct v4l2_fh *fh, struct rot_channel_cfg *rot)
{
	int ret = 0;
	struct vfe_dev *dev = video_drvdata(file);

	if (!dev->is_isp_used) {
		vfe_err("isp must be set first when set rotchannel\n");
		return -1;
	}
	ret = v4l2_subdev_call(dev->isp_sd, core, ioctl, VIDIOC_SUNXI_ISP_ROT_CH_CFG, rot);
	if (ret < 0) {
		vfe_err("vidioc_set_rotchannel error! ret = %d\n", ret);
		return ret;
	}
	dev->buf_byte_size = rot->pix.sizeimage;

	return ret;
}
static long vfe_param_handler(struct file *file, void *priv,
		bool valid_prio, unsigned int cmd, void *param)
{
	int ret = 0;
	struct v4l2_fh *fh = (struct v4l2_fh *)priv;
	struct isp_stat_buf *stat = (struct isp_stat_buf *)param;

	switch (cmd) {
	case VIDIOC_ISP_AE_STAT_REQ:
		ret = isp_ae_stat_req(file, fh, stat);
		break;
	case VIDIOC_ISP_AF_STAT_REQ:
		ret = isp_af_stat_req(file, fh, stat);
		break;
	case VIDIOC_ISP_HIST_STAT_REQ:
		ret = isp_hist_stat_req(file, fh, stat);
		break;
	case VIDIOC_ISP_EXIF_REQ:
		ret = isp_exif_req(file, fh, (struct isp_exif_attribute *)param);
		break;
	case VIDIOC_ISP_GAMMA_REQ:
		ret = isp_gamma_req(file, fh, stat);
		break;
	case VIDIOC_AUTO_FOCUS_WIN:
		ret = vidioc_auto_focus_win(file, fh, (struct v4l2_win_setting *)param);
		break;
	case VIDIOC_AUTO_EXPOSURE_WIN:
		ret = vidioc_auto_exposure_win(file, fh, (struct v4l2_win_setting *)param);
		break;
	case VIDIOC_HDR_CTRL:
		ret = vidioc_hdr_ctrl(file, fh, (struct isp_hdr_ctrl *)param);
		break;
	case VIDIOC_SET_SUBCHANNEL:
		ret = vidioc_set_subchannel(file, fh, (struct v4l2_pix_format *)param);
		break;
	case VIDIOC_SET_ROTCHANNEL:
		ret = vidioc_set_rotchannel(file, fh, (struct rot_channel_cfg *)param);
		break;
	default:
		ret = -ENOTTY;
	}

	return ret;
}

static ssize_t vfe_read(struct file *file, char __user *data, size_t count, loff_t *ppos)
{
	struct vfe_dev *dev = video_drvdata(file);
	struct video_device *vdev = video_devdata(file);
	int ret = 0;

	mutex_lock(&dev->buf_lock);
	dev->cur_ch = (vdev->num - VID_N_OFF) < 0 ? 0 : (vdev->num - VID_N_OFF);
	if (vfe_is_generating(dev)) {
		ret = vb2_read(&dev->vb_vidq[dev->cur_ch], data, count, ppos,
					file->f_flags & O_NONBLOCK);
	} else {
		vfe_err("csi is not generating!\n");
		ret = -EINVAL;
	}
	mutex_unlock(&dev->buf_lock);

	return ret;
}

static unsigned int vfe_poll(struct file *file, struct poll_table_struct *wait)
{
	struct vfe_dev *dev = video_drvdata(file);
	struct video_device *vdev = video_devdata(file);
	int ret = 0;

	mutex_lock(&dev->buf_lock);
	dev->cur_ch = (vdev->num - VID_N_OFF) < 0 ? 0 : (vdev->num - VID_N_OFF);
	if (vfe_is_generating(dev)) {
		ret = vb2_poll(&dev->vb_vidq[dev->cur_ch], file, wait);
	} else {
		vfe_err("csi is not generating!\n");
		ret = -EINVAL;
	}
	mutex_unlock(&dev->buf_lock);

	return ret;
}

void vfe_clk_open(struct vfe_dev *dev)
{
	vfe_print("..........................vfe clk open!.......................\n");
	v4l2_subdev_call(dev->csi_sd, core, s_power, 1);
	v4l2_subdev_call(dev->mipi_sd, core, s_power, 1);
}

void vfe_clk_close(struct vfe_dev *dev)
{
	vfe_print("..........................vfe clk close!.......................\n");
	v4l2_subdev_call(dev->csi_sd, core, s_power, 0);
	v4l2_subdev_call(dev->mipi_sd, core, s_power, 0);
}

static void vfe_suspend_trip(struct vfe_dev *dev);
static void vfe_resume_trip(struct vfe_dev *dev);

static int __vfe_open(struct vfe_dev *dev)
{
	int ret;

	vfe_print("vfe_open\n");
	if (vfe_is_opened(dev)) {
		vfe_err("device open busy\n");
		ret = -EBUSY;
		goto open_end;
	}
#ifdef CONFIG_DEVFREQ_DRAM_FREQ_WITH_SOFT_NOTIFY
	dramfreq_master_access(MASTER_CSI, true);
#endif
	vfe_resume_trip(dev);
#ifdef USE_SPECIFIC_CCI
	csi_cci_init_helper(dev->cci_sel);
#endif
	if (dev->ccm_cfg[0]->is_isp_used || dev->ccm_cfg[1]->is_isp_used) {
		/* must be after ahb and core clock enable */
		ret = v4l2_subdev_call(dev->isp_sd, core, init, 0);
		if (ret < 0) {
			vfe_err("ISP init error at %s\n", __func__);
			return ret;
		}
		ret = isp_resource_request(dev);
		if (ret) {
			vfe_err("isp_resource_request error at %s\n", __func__);
			return ret;
		}
		vfe_dbg(0, "tasklet init !\n");
		INIT_WORK(&dev->isp_isr_bh_task, isp_isr_bh_handle);
		INIT_WORK(&dev->isp_isr_set_sensor_task, isp_isr_set_sensor_handle);
	}
	dev->input = -1;
	vfe_start_opened(dev);
	vfe_init_isp_log(dev);
open_end:
	if (ret != 0)
		vfe_print("vfe_open busy\n");
	else
		vfe_print("vfe_open ok\n");

	return ret;
}

static int vfe_open(struct file *file)
{
	struct vfe_dev *dev = video_drvdata(file);
	struct video_device *vdev = video_devdata(file);
	int ret = 0;

	mutex_lock(&dev->buf_lock);
	dev->cur_ch = (vdev->num - VID_N_OFF) < 0 ? 0 : (vdev->num - VID_N_OFF);
	dev->first_flag[dev->cur_ch] = 0;
	if (dev->cur_ch > 0) {
		mutex_unlock(&dev->buf_lock);
		return 0;
	}
	dev->special_active = 0;
	ret = __vfe_open(dev);
	mutex_unlock(&dev->buf_lock);

	return ret;
}

static int __vfe_close(struct vfe_dev *dev)
{
	int ret;

	vfe_print("vfe_close\n");
	vfe_stop_generating(dev);
	if (dev->vfe_s_input_flag == 1) {
		if (dev->sd_act != NULL)
			v4l2_subdev_call(dev->sd_act, core, ioctl, ACT_SOFT_PWDN, 0);

		ret = vfe_set_sensor_power_off(dev);
		if (ret != 0)
			vfe_err("sensor power off error at device number when csi close!\n");

		dev->vfe_s_input_flag = 0;
	} else
		vfe_print("vfe select input flag = %d, s_input have not be used .\n", dev->vfe_s_input_flag);
	/* hardware */
	bsp_csi_int_disable(dev->csi_sel, dev->cur_ch, CSI_INT_ALL);
	v4l2_subdev_call(dev->csi_sd, video, s_stream, 0);
	bsp_csi_disable(dev->csi_sel);
	if (dev->is_isp_used)
		bsp_isp_disable();
	if (dev->mbus_type == V4L2_MBUS_CSI2) {
		bsp_mipi_csi_protocol_disable(dev->mipi_sel);
		bsp_mipi_csi_dphy_disable(dev->mipi_sel);
		bsp_mipi_csi_dphy_exit(dev->mipi_sel);
	}
	if (dev->is_isp_used)
		bsp_isp_exit();
	flush_delayed_work(&dev->probe_work);

	if (dev->ccm_cfg[0]->is_isp_used || dev->ccm_cfg[1]->is_isp_used) {
		flush_work(&dev->isp_isr_bh_task);
		flush_work(&dev->isp_isr_set_sensor_task);
		/* resource */
		isp_resource_release(dev);
	}
	if (dev->is_bayer_raw)
		mutex_destroy(&dev->isp_3a_result_mutex);
	vfe_stop_opened(dev);
	dev->ctrl_para.prev_exp_line = 0;
	dev->ctrl_para.prev_ana_gain = 1;
	vfe_suspend_trip(dev);
	vfe_print("vfe_close end\n");
	vfe_exit_isp_log(dev);
#ifdef CONFIG_DEVFREQ_DRAM_FREQ_WITH_SOFT_NOTIFY
	dramfreq_master_access(MASTER_CSI, false);
#endif
	return 0;
}

static int vfe_close(struct file *file)
{
	struct vfe_dev *dev = video_drvdata(file);
	struct video_device *vdev = video_devdata(file);
	int ret = 0;

	mutex_lock(&dev->buf_lock);
	dev->cur_ch = (vdev->num - VID_N_OFF) < 0 ? 0 : (vdev->num - VID_N_OFF);
	vb2_queue_release(&dev->vb_vidq[dev->cur_ch]);
	if (dev->cur_ch > 0) {
		mutex_unlock(&dev->buf_lock);
		return 0;
	}
	ret = __vfe_close(dev);
	mutex_unlock(&dev->buf_lock);

	return ret;
}

static int vfe_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct vfe_dev *dev = video_drvdata(file);
	struct video_device *vdev = video_devdata(file);
	int ret = 0;

	mutex_lock(&dev->buf_lock);
	dev->cur_ch = (vdev->num - VID_N_OFF) < 0 ? 0 : (vdev->num - VID_N_OFF);
	ret = vb2_mmap(&dev->vb_vidq[dev->cur_ch], vma);
	mutex_unlock(&dev->buf_lock);

	return ret;
}

static int vfe_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	int ret = 0;
	struct vfe_dev *dev = container_of(ctrl->handler, struct vfe_dev, ctrl_handler);
	struct v4l2_control c;

	c.id = ctrl->id;

	if (dev->is_isp_used && dev->is_bayer_raw) {
		switch (ctrl->id) {
		case V4L2_CID_EXPOSURE:
			v4l2_subdev_call(dev->sd, core, g_ctrl, &c);
			ctrl->val = c.value;
			break;
		case V4L2_CID_GAIN:
			if (dev->isp_gen_set_pt->isp_ini_cfg.isp_test_settings.isp_test_mode == ISP_TEST_ALL_ENABLE ||
					dev->isp_gen_set_pt->isp_ini_cfg.isp_test_settings.isp_test_mode == ISP_TEST_MANUAL) {
				ctrl->val = CLIP(CLIP(dev->isp_3a_result_pt->exp_analog_gain, 16, 255) |
				(CLIP(dev->isp_gen_set_pt->sharp_cfg_to_hal[1], 0, 4095) << V4L2_SHARP_LEVEL_SHIFT)  |
				(CLIP(dev->isp_gen_set_pt->sharp_cfg_to_hal[0], 0, 63) << V4L2_SHARP_MIN_SHIFT) |
				(CLIP(dev->isp_gen_set_pt->isp_ini_cfg.isp_tunning_settings.color_denoise_level, 0, 31) << V4L2_NDF_SHIFT), 0, 0xffffffff);
			} else {
				ctrl->val = CLIP(dev->isp_3a_result_pt->exp_analog_gain, 16, 255);
			}
			break;
		case V4L2_CID_HOR_VISUAL_ANGLE:
			ctrl->val = dev->isp_gen_set_pt->isp_ini_cfg.isp_tunning_settings.hor_visual_angle;
			break;
		case V4L2_CID_VER_VISUAL_ANGLE:
			ctrl->val = dev->isp_gen_set_pt->isp_ini_cfg.isp_tunning_settings.ver_visual_angle;
			break;
		case V4L2_CID_FOCUS_LENGTH:
			ctrl->val = dev->isp_gen_set_pt->isp_ini_cfg.isp_tunning_settings.focus_length;
			break;
		case V4L2_CID_R_GAIN:
			ctrl->val = dev->isp_gen_set_pt->module_cfg.wb_gain_cfg.wb_gain.r_gain;
			break;
		case V4L2_CID_G_GAIN:
			ctrl->val = dev->isp_gen_set_pt->module_cfg.wb_gain_cfg.wb_gain.gr_gain;
			break;
		case V4L2_CID_B_GAIN:
			ctrl->val = dev->isp_gen_set_pt->module_cfg.wb_gain_cfg.wb_gain.b_gain;
			break;
		case V4L2_CID_3A_LOCK:
			if (dev->isp_gen_set_pt->exp_settings.exposure_lock == ISP_TRUE)
				ctrl->val |= V4L2_LOCK_EXPOSURE;
			else
				ctrl->val &= ~V4L2_LOCK_EXPOSURE;

			if (dev->isp_gen_set_pt->wb_settings.white_balance_lock == ISP_TRUE)
				ctrl->val |= V4L2_LOCK_WHITE_BALANCE;
			else
				ctrl->val &= ~V4L2_LOCK_WHITE_BALANCE;

			if (dev->isp_gen_set_pt->af_settings.focus_lock == ISP_TRUE)
				ctrl->val |= V4L2_LOCK_FOCUS;
			else
				ctrl->val &= ~V4L2_LOCK_FOCUS;

			break;
		case V4L2_CID_AUTO_FOCUS_STATUS:  /* Read-Only */
			ctrl->val = vfe_v4l2_isp(VFE_FOCUS_STATUS, dev->isp_3a_result_pt->af_status, ISP_TO_V4L2);
			break;
		case V4L2_CID_SENSOR_TYPE:
			ctrl->val = dev->is_bayer_raw;
			break;
		default:
			return -EINVAL;
		}
		vfe_dbg(0, "vfe_g_volatile_ctrl: %s, last value: 0x%x\n", ctrl->name, ctrl->val);

		return 0;
	} else {
		switch (ctrl->id) {
		case V4L2_CID_SENSOR_TYPE:
			c.value = dev->is_bayer_raw;
			break;
		case V4L2_CID_FLASH_LED_MODE:
			ret = v4l2_subdev_call(dev->flash_sd, core, g_ctrl, &c);
			break;
		case V4L2_CID_AUTO_FOCUS_STATUS:
			ret = v4l2_subdev_call(dev->sd, core, g_ctrl, &c);
			if (c.value != V4L2_AUTO_FOCUS_STATUS_BUSY)
				sunxi_flash_stop(dev->flash_sd);
			break;
		default:
			ret = v4l2_subdev_call(dev->sd, core, g_ctrl, &c);
			break;
		}
		ctrl->val = c.value;
		if (ret < 0)
			vfe_warn("v4l2 sub device g_ctrl fail!\n");
	}
	return ret;
}

static int vfe_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct vfe_dev *dev = container_of(ctrl->handler, struct vfe_dev, ctrl_handler);
	int ret = 0;
	struct actuator_ctrl_word_t vcm_ctrl;
	struct v4l2_control c;

	c.id = ctrl->id;
	c.value = ctrl->val;
	vfe_dbg(0, "s_ctrl: %s, set value: 0x%x\n", ctrl->name, ctrl->val);

	if (dev->is_isp_used && dev->is_bayer_raw) {
		switch (ctrl->id) {
		case V4L2_CID_BRIGHTNESS:
			bsp_isp_s_brightness(dev->isp_gen_set_pt, ctrl->val);
			break;
		case V4L2_CID_CONTRAST:
			bsp_isp_s_contrast(dev->isp_gen_set_pt, ctrl->val);
			break;
		case V4L2_CID_SATURATION:
			bsp_isp_s_saturation(dev->isp_gen_set_pt, ctrl->val * 25);
			break;
		case V4L2_CID_HUE:
			bsp_isp_s_hue(dev->isp_gen_set_pt, ctrl->val);
			break;
		case V4L2_CID_AUTO_WHITE_BALANCE:
			if (ctrl->val == 0)
				bsp_isp_s_auto_white_balance(dev->isp_gen_set_pt, WB_MANUAL);
			else
				bsp_isp_s_auto_white_balance(dev->isp_gen_set_pt, WB_AUTO);
			dev->ctrl_para.auto_wb = ctrl->val;
			break;
		case V4L2_CID_EXPOSURE:
			ret = v4l2_subdev_call(dev->sd, core, s_ctrl, &c);
			break;
		case V4L2_CID_AUTOGAIN:
			if (ctrl->val == 0)
				bsp_isp_s_exposure(dev->isp_gen_set_pt, ISO_MANUAL);
			else
				bsp_isp_s_exposure(dev->isp_gen_set_pt, ISO_AUTO);
			break;
		case V4L2_CID_GAIN:
			ret = v4l2_subdev_call(dev->sd, core, s_ctrl, &c);
			break;
		case V4L2_CID_POWER_LINE_FREQUENCY:
			bsp_isp_s_power_line_frequency(dev->isp_gen_set_pt,
					vfe_v4l2_isp(VFE_POWER_LINE_FREQUENCY, ctrl->val, V4L2_TO_ISP));
			break;
		case V4L2_CID_HUE_AUTO:
			bsp_isp_s_hue_auto(dev->isp_gen_set_pt, ctrl->val);
			break;
		case V4L2_CID_WHITE_BALANCE_TEMPERATURE:
			bsp_isp_s_white_balance_temperature(dev->isp_gen_set_pt, ctrl->val);
			break;
		case V4L2_CID_SHARPNESS:
			bsp_isp_s_sharpness(dev->isp_gen_set_pt, ctrl->val);
			break;
		case V4L2_CID_CHROMA_AGC:
			bsp_isp_s_chroma_agc(dev->isp_gen_set_pt, ctrl->val);
			break;
		case V4L2_CID_COLORFX:
			bsp_isp_s_colorfx(dev->isp_gen_set_pt, vfe_v4l2_isp(VFE_COLORFX, ctrl->val, V4L2_TO_ISP));
			break;
		case V4L2_CID_AUTOBRIGHTNESS:
			bsp_isp_s_auto_brightness(dev->isp_gen_set_pt, ctrl->val);
			break;
		case V4L2_CID_BAND_STOP_FILTER:
			bsp_isp_s_band_stop_filter(dev->isp_gen_set_pt, ctrl->val);
			break;
		case V4L2_CID_ILLUMINATORS_1:
			bsp_isp_s_illuminators_1(dev->isp_gen_set_pt, ctrl->val);
			break;
		case V4L2_CID_ILLUMINATORS_2:
			bsp_isp_s_illuminators_2(dev->isp_gen_set_pt, ctrl->val);
			break;
		case V4L2_CID_EXPOSURE_AUTO:
			bsp_isp_s_exposure_auto(dev->isp_gen_set_pt,
					vfe_v4l2_isp(VFE_AE_MODE, ctrl->val, V4L2_TO_ISP));
			dev->ctrl_para.exp_auto_mode = ctrl->val;
			break;
		case V4L2_CID_EXPOSURE_ABSOLUTE:
			bsp_isp_s_exposure_absolute(dev->isp_gen_set_pt, ctrl->val);
			break;
		case V4L2_CID_EXPOSURE_AUTO_PRIORITY:
			bsp_isp_s_exposure_auto_priority(dev->isp_gen_set_pt, ctrl->val);
			break;
		case V4L2_CID_FOCUS_ABSOLUTE:
			bsp_isp_s_focus_absolute(dev->isp_gen_set_pt, ctrl->val);
			break;
		case V4L2_CID_FOCUS_RELATIVE:
			bsp_isp_s_focus_relative(dev->isp_gen_set_pt, ctrl->val);
			break;
		case V4L2_CID_FOCUS_AUTO:
			dev->isp_3a_result_pt->af_status = AUTO_FOCUS_STATUS_REFOCUS;
			bsp_isp_s_focus_auto(dev->isp_gen_set_pt, ctrl->val);
			dev->ctrl_para.auto_focus = ctrl->val;
			break;
		case V4L2_CID_AUTO_EXPOSURE_BIAS:
			bsp_isp_s_auto_exposure_bias(dev->isp_gen_set_pt, ctrl->qmenu_int[ctrl->val]);
			dev->ctrl_para.exp_bias = ctrl->qmenu_int[ctrl->val];
			break;
		case V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE:
			bsp_isp_s_auto_n_preset_white_balance(dev->isp_gen_set_pt, vfe_v4l2_isp(VFE_WB, ctrl->val, V4L2_TO_ISP));
			break;
		case V4L2_CID_WIDE_DYNAMIC_RANGE:
			bsp_isp_s_wide_dynamic_rage(dev->isp_gen_set_pt, ctrl->val);
			break;
		case V4L2_CID_IMAGE_STABILIZATION:
			bsp_isp_s_image_stabilization(dev->isp_gen_set_pt, ctrl->val);
			break;
		case V4L2_CID_ISO_SENSITIVITY:
			bsp_isp_s_iso_sensitivity(dev->isp_gen_set_pt, ctrl->qmenu_int[ctrl->val]);
			break;
		case V4L2_CID_ISO_SENSITIVITY_AUTO:
			bsp_isp_s_iso_sensitivity_auto(dev->isp_gen_set_pt, vfe_v4l2_isp(VFE_ISO, ctrl->val, V4L2_TO_ISP));
			break;
		case V4L2_CID_EXPOSURE_METERING:
			ret = -EINVAL;
			break;
		case V4L2_CID_SCENE_MODE:
			bsp_isp_s_scene_mode(dev->isp_gen_set_pt, vfe_v4l2_isp(VFE_SCENE, ctrl->val, V4L2_TO_ISP));
			break;
		case V4L2_CID_3A_LOCK:
			if (dev->ctrl_para.exp_auto_mode != V4L2_EXPOSURE_MANUAL) {
				if (IS_FLAG(ctrl->val, V4L2_LOCK_EXPOSURE))
					dev->isp_gen_set_pt->exp_settings.exposure_lock = ISP_TRUE;
				else
					dev->isp_gen_set_pt->exp_settings.exposure_lock = ISP_FALSE;
			}
			if (dev->ctrl_para.auto_wb == 1) {
				if (IS_FLAG(ctrl->val, V4L2_LOCK_WHITE_BALANCE))
					dev->isp_gen_set_pt->wb_settings.white_balance_lock = ISP_TRUE;
				else
					dev->isp_gen_set_pt->wb_settings.white_balance_lock = ISP_FALSE;
			}
			if (dev->ctrl_para.auto_focus == 1) {
				if (IS_FLAG(ctrl->val, V4L2_LOCK_FOCUS)) {
					dev->isp_gen_set_pt->af_settings.focus_lock = ISP_TRUE;
				} else {
					dev->isp_gen_set_pt->af_settings.focus_lock = ISP_FALSE;
				}
			}
			break;
		case V4L2_CID_AUTO_FOCUS_START:
			dev->isp_3a_result_pt->af_status = AUTO_FOCUS_STATUS_REFOCUS;
			bsp_isp_s_auto_focus_start(dev->isp_gen_set_pt, ctrl->val);
			isp_s_ctrl_torch_open(dev);
			break;
		case V4L2_CID_AUTO_FOCUS_STOP:
			vfe_dbg(0, "V4L2_CID_AUTO_FOCUS_STOP\n");
			bsp_isp_s_auto_focus_stop(dev->isp_gen_set_pt, ctrl->val);
			isp_s_ctrl_torch_close(dev);
			break;
		case V4L2_CID_AUTO_FOCUS_RANGE:
			bsp_isp_s_auto_focus_range(dev->isp_gen_set_pt, vfe_v4l2_isp(VFE_AF_RANGE, ctrl->val, V4L2_TO_ISP));
			break;
		case V4L2_CID_FLASH_LED_MODE:
			bsp_isp_s_flash_mode(dev->isp_gen_set_pt, vfe_v4l2_isp(VFE_FLASH_MODE, ctrl->val, V4L2_TO_ISP));
			if (ctrl->val == V4L2_FLASH_LED_MODE_TORCH)
				io_set_flash_ctrl(dev->flash_sd, SW_CTRL_TORCH_ON);
			else if (ctrl->val == V4L2_FLASH_LED_MODE_NONE)
				io_set_flash_ctrl(dev->flash_sd, SW_CTRL_FLASH_OFF);
			break;
		case V4L2_CID_AUTO_FOCUS_INIT:
			break;
		case V4L2_CID_AUTO_FOCUS_RELEASE:
			break;
		case V4L2_CID_GSENSOR_ROTATION:
			bsp_isp_s_gsensor_rotation(dev->isp_gen_set_pt, ctrl->val);
			dev->ctrl_para.gsensor_rot = ctrl->val;
			break;
		case V4L2_CID_TAKE_PICTURE:
			bsp_isp_s_take_pic(dev->isp_gen_set_pt, ctrl->val);
			break;
		case V4L2_CID_R_GAIN:
			bsp_isp_s_r_gain(dev->isp_gen_set_pt, ctrl->val);
			break;
		case V4L2_CID_G_GAIN:
			bsp_isp_s_g_gain(dev->isp_gen_set_pt, ctrl->val);
			break;
		case V4L2_CID_B_GAIN:
			bsp_isp_s_b_gain(dev->isp_gen_set_pt, ctrl->val);
			break;
		default:
			ret = -EINVAL;
			break;
		}
		if (ret < 0)
			vfe_warn("v4l2 isp s_ctrl fail!\n");
	} else {
		switch (ctrl->id) {
		case V4L2_CID_FOCUS_ABSOLUTE:
			vcm_ctrl.code = ctrl->val;
			vcm_ctrl.sr = 0x0;
			ret = v4l2_subdev_call(dev->sd_act, core, ioctl, ACT_SET_CODE, &vcm_ctrl);
			break;
		case V4L2_CID_FLASH_LED_MODE:
			ret = v4l2_subdev_call(dev->flash_sd, core, s_ctrl, &c);
			break;
		case V4L2_CID_AUTO_FOCUS_START:
			sunxi_flash_check_to_start(dev->flash_sd, SW_CTRL_TORCH_ON);
			ret = v4l2_subdev_call(dev->sd, core, s_ctrl, &c);
			break;
		case V4L2_CID_AUTO_FOCUS_STOP:
			sunxi_flash_stop(dev->flash_sd);
			ret = v4l2_subdev_call(dev->sd, core, s_ctrl, &c);
			break;
		case V4L2_CID_AUTO_EXPOSURE_BIAS:
			c.value = ctrl->qmenu_int[ctrl->val];
			ret = v4l2_subdev_call(dev->sd, core, s_ctrl, &c);
			break;
		default:
			ret = v4l2_subdev_call(dev->sd, core, s_ctrl, &c);
			break;
		}
		if (ret < 0)
			vfe_warn("v4l2 sensor s_ctrl fail!\n");
	}
	return ret;
}
#ifdef CONFIG_COMPAT
struct isp_stat_buf32 {
	compat_caddr_t buf;
	__u32 buf_size;
};

static int get_isp_stat_buf32(struct isp_stat_buf *kp, struct isp_stat_buf32 __user *up)
{
	u32 tmp;

	if (!access_ok(VERIFY_READ, up, sizeof(struct isp_stat_buf32)) ||
		get_user(kp->buf_size, &up->buf_size) ||
		get_user(tmp, &up->buf))
			return -EFAULT;
	kp->buf = compat_ptr(tmp);

	return 0;
}

static int put_isp_stat_buf32(struct isp_stat_buf *kp, struct isp_stat_buf32 __user *up)
{
	u32 tmp = (u32)((unsigned long)kp->buf);

	if (!access_ok(VERIFY_WRITE, up, sizeof(struct isp_stat_buf32)) ||
		put_user(kp->buf_size, &up->buf_size) ||
		put_user(tmp, &up->buf))
			return -EFAULT;

	return 0;
}

#define VIDIOC_ISP_AE_STAT_REQ32	_IOWR('V', BASE_VIDIOC_PRIVATE + 1, struct isp_stat_buf32)
#define VIDIOC_ISP_HIST_STAT_REQ32	_IOWR('V', BASE_VIDIOC_PRIVATE + 2, struct isp_stat_buf32)
#define VIDIOC_ISP_AF_STAT_REQ32	_IOWR('V', BASE_VIDIOC_PRIVATE + 3, struct isp_stat_buf32)
#define VIDIOC_ISP_GAMMA_REQ32	_IOWR('V', BASE_VIDIOC_PRIVATE + 5, struct isp_stat_buf32)

static long vfe_compat_ioctl32(struct file *file, unsigned int cmd, unsigned long arg)
{
	union {
		struct isp_stat_buf isb;
	} karg;
	void __user *up = compat_ptr(arg);
	int compatible_arg = 1;
	long err = 0;

	switch (cmd) {
	case VIDIOC_ISP_AE_STAT_REQ32:
		cmd = VIDIOC_ISP_AE_STAT_REQ;
		break;
	case VIDIOC_ISP_HIST_STAT_REQ32:
		cmd = VIDIOC_ISP_HIST_STAT_REQ;
		break;
	case VIDIOC_ISP_AF_STAT_REQ32:
		cmd = VIDIOC_ISP_AF_STAT_REQ;
		break;
	case VIDIOC_ISP_GAMMA_REQ32:
		cmd = VIDIOC_ISP_GAMMA_REQ;
		break;
	}
	switch (cmd) {
	case VIDIOC_ISP_AE_STAT_REQ:
	case VIDIOC_ISP_HIST_STAT_REQ:
	case VIDIOC_ISP_AF_STAT_REQ:
	case VIDIOC_ISP_GAMMA_REQ:
		err = get_isp_stat_buf32(&karg.isb, up);
		compatible_arg = 0;
		break;
	}
	if (err)
		return err;
	if (compatible_arg)
		err = video_ioctl2(file, cmd, (unsigned long)up);
	else {
		mm_segment_t old_fs = get_fs();

		set_fs(KERNEL_DS);
		if (file->f_op->unlocked_ioctl)
			err = file->f_op->unlocked_ioctl(file, cmd, (unsigned long)&karg);
		else
			err = -ENOIOCTLCMD;
		set_fs(old_fs);
	}
	switch (cmd) {
	case VIDIOC_ISP_AE_STAT_REQ:
	case VIDIOC_ISP_HIST_STAT_REQ:
	case VIDIOC_ISP_AF_STAT_REQ:
	case VIDIOC_ISP_GAMMA_REQ:
		if (put_isp_stat_buf32(&karg.isb, up))
			err = -EFAULT;
		break;
	}
	return err;
}
#endif

int vfe_open_special(int id)
{
	struct vfe_dev *dev = vfe_dev_gbl[id];
	struct vfe_dmaqueue *active = &dev->vidq_special;
	struct vfe_dmaqueue *done = &dev->done_special;

	INIT_LIST_HEAD(&active->active);
	INIT_LIST_HEAD(&done->active);
	dev->special_active = 1;

	return __vfe_open(dev);
}
EXPORT_SYMBOL(vfe_open_special);

int vfe_s_input_special(int id, int sel)
{
	struct vfe_dev *dev = vfe_dev_gbl[id];

	return internal_s_input(dev, sel);
}
EXPORT_SYMBOL(vfe_s_input_special);

int vfe_close_special(int id)
{
	struct vfe_dev *dev = vfe_dev_gbl[id];
	struct vfe_dmaqueue *active = &dev->vidq_special;
	struct vfe_dmaqueue *done = &dev->done_special;

	INIT_LIST_HEAD(&active->active);
	INIT_LIST_HEAD(&done->active);
	dev->special_active = 0;

	return __vfe_close(dev);
}
EXPORT_SYMBOL(vfe_close_special);

int vfe_s_fmt_special(int id, struct v4l2_format *f)
{
	struct vfe_dev *dev = vfe_dev_gbl[id];

	return s_fmt_internal(dev, NULL, f);
}
EXPORT_SYMBOL(vfe_s_fmt_special);

int vfe_g_fmt_special(int id, struct v4l2_format *f)
{
	struct vfe_dev *dev = vfe_dev_gbl[id];

	f->fmt.pix.width        = dev->width;
	f->fmt.pix.height       = dev->height;
	f->fmt.pix.field	= dev->fmt.field;
	f->fmt.pix.pixelformat  = dev->fmt.bus_pix_code;

	return 0;
}
EXPORT_SYMBOL(vfe_g_fmt_special);

int vfe_dqbuffer_special(int id, struct vfe_buffer **buf)
{
	int ret = 0;
	unsigned long flags = 0;
	struct vfe_dev *dev = vfe_dev_gbl[id];
	struct vfe_dmaqueue *done = &dev->done_special;

	spin_lock_irqsave(&dev->slock, flags);
	if (!list_empty(&done->active)) {
		*buf = list_first_entry(&done->active, struct vfe_buffer, list);
		list_del(&((*buf)->list));
		(*buf)->state = VB2_BUF_STATE_DEQUEUED;
	} else {
		vfe_err("there is no done buf, please wait\n");
		ret = -1;
	}
	spin_unlock_irqrestore(&dev->slock, flags);

	return ret;
}
EXPORT_SYMBOL(vfe_dqbuffer_special);

int vfe_qbuffer_special(int id, struct vfe_buffer *buf)
{
	struct vfe_dev *dev = vfe_dev_gbl[id];
	struct vfe_dmaqueue *vidq = &dev->vidq_special;
	unsigned long flags = 0;

	if (buf == NULL) {
		vfe_err("buf is NULL, cannot qbuf\n");
		return -1;
	}

	spin_lock_irqsave(&dev->slock, flags);
	list_add_tail(&buf->list, &vidq->active);
	buf->state = VB2_BUF_STATE_QUEUED;
	spin_unlock_irqrestore(&dev->slock, flags);

	return 0;
}
EXPORT_SYMBOL(vfe_qbuffer_special);

int vfe_streamon_special(int id, enum v4l2_buf_type i)
{
	struct vfe_dev *dev = vfe_dev_gbl[id];

	return __vfe_streamon(dev, NULL, i);
}
EXPORT_SYMBOL(vfe_streamon_special);

int vfe_streamoff_special(int id, enum v4l2_buf_type i)
{
	struct vfe_dev *dev = vfe_dev_gbl[id];

	return __vfe_streamoff(dev, NULL, i);
}
EXPORT_SYMBOL(vfe_streamoff_special);

void vfe_register_buffer_done_callback(int id, void *func)
{
	struct vfe_dev *dev = vfe_dev_gbl[id];

	dev->vfe_buffer_process = func;
}
EXPORT_SYMBOL(vfe_register_buffer_done_callback);

/* ------------------------------------------------------------------
	File operations for the device
   ------------------------------------------------------------------*/

static const struct v4l2_ctrl_ops vfe_ctrl_ops = {
	.g_volatile_ctrl = vfe_g_volatile_ctrl,
	.s_ctrl = vfe_s_ctrl,
};

static const struct v4l2_file_operations vfe_fops = {
	.owner          = THIS_MODULE,
	.open           = vfe_open,
	.release        = vfe_close,
	.read           = vfe_read,
	.poll           = vfe_poll,
	.unlocked_ioctl = video_ioctl2,
#ifdef CONFIG_COMPAT
	.compat_ioctl32 = vfe_compat_ioctl32,
#endif
	.mmap           = vfe_mmap,
};

static const struct v4l2_ioctl_ops vfe_ioctl_ops = {
	.vidioc_querycap          = vidioc_querycap,
	.vidioc_enum_fmt_vid_cap  = vidioc_enum_fmt_vid_cap,
	.vidioc_enum_framesizes   = vidioc_enum_framesizes,
	.vidioc_g_fmt_vid_cap     = vidioc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap   = vidioc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap     = vidioc_s_fmt_vid_cap,
	.vidioc_reqbufs           = vidioc_reqbufs,
	.vidioc_querybuf          = vidioc_querybuf,
	.vidioc_qbuf              = vidioc_qbuf,
	.vidioc_dqbuf             = vidioc_dqbuf,
	.vidioc_enum_input        = vidioc_enum_input,
	.vidioc_g_input           = vidioc_g_input,
	.vidioc_s_input           = vidioc_s_input,
	.vidioc_streamon          = vidioc_streamon,
	.vidioc_streamoff         = vidioc_streamoff,
	.vidioc_g_parm            = vidioc_g_parm,
	.vidioc_s_parm            = vidioc_s_parm,
	.vidioc_default		 = vfe_param_handler,
};

static struct video_device vfe_template[] = {

	[0] = {
		.name       = "vfe_0",
		.fops       = &vfe_fops,
		.ioctl_ops  = &vfe_ioctl_ops,
		.release    = video_device_release,
	},
	[1] = {
		.name       = "vfe_1",
		.fops       = &vfe_fops,
		.ioctl_ops  = &vfe_ioctl_ops,
		.release    = video_device_release,
	},
};

static int vfe_pin_config(struct vfe_dev *dev, int enable)
{
	char pinctrl_names[10] = "";
#ifdef VFE_GPIO
	if (!IS_ERR_OR_NULL(dev->pctrl))
		devm_pinctrl_put(dev->pctrl);

	if (enable == 1)
		strcpy(pinctrl_names, "default");
	else
		strcpy(pinctrl_names, "sleep");

	dev->pctrl = devm_pinctrl_get_select(&dev->pdev->dev, pinctrl_names);
	if (IS_ERR_OR_NULL(dev->pctrl))
		vfe_err("vip%d request pinctrl handle for device [%s] failed!\n", dev->id, dev_name(&dev->pdev->dev));
		return -EINVAL;

	usleep_range(5000, 6000);
#else
	void __iomem *gpio_base;

	vfe_print("directly write pin config @ FPGA\n");
	gpio_base = ioremap(GPIO_REGS_VBASE, 0x120);
	if (!gpio_base) {
		printk("gpio_base directly write pin config EIO\n");
		return -EIO;
	}
#ifdef FPGA_PIN	/* Direct write for pin of FPGA */
	writel(0x33333333, (gpio_base+0x90));
	writel(0x33333333, (gpio_base+0x94));
	writel(0x03333333, (gpio_base+0x98));
#else /* Direct write for pin of IC */
	writel(0x22222222, (gpio_base+0x90));
	writel(0x10222222, (gpio_base+0x94));
	writel(0x11111111, (gpio_base+0x98));
#endif
#endif

	return 0;
}

static int vfe_pin_release(struct vfe_dev *dev)
{
#ifdef VFE_GPIO
	devm_pinctrl_put(dev->pctrl);
#endif
	return 0;
}

static int vfe_request_gpio(struct vfe_dev *dev)
{
#ifdef VFE_GPIO
	unsigned int i, j;

	for (i = 0; i < dev->dev_qty; i++) {
		for (j = 0; j < MAX_GPIO_NUM; j++) {
			os_gpio_request(&dev->ccm_cfg[i]->gpio[j], 1);
			/* os_gpio_set(&dev->ccm_cfg[i]->gpio[j],1); */
		}
	}
#endif
	return 0;
}

static int vfe_gpio_config(struct vfe_dev *dev, int bon)
{
#ifdef VFE_GPIO
	unsigned int i, j;
	struct vfe_gpio_cfg gpio_item;

	for (i = 0; i < dev->dev_qty; i++) {
		for (j = 0; j < MAX_GPIO_NUM; j++) {
			memcpy(&gpio_item, &dev->ccm_cfg[i]->gpio[j], sizeof(struct vfe_gpio_cfg));
			if (bon == 0)
				gpio_item.mul_sel = GPIO_DISABLE;
			os_gpio_set(&gpio_item, 1);
		}
	}
#endif
	return 0;
}

static void vfe_gpio_release(struct vfe_dev *dev)
{
#ifdef VFE_GPIO
	unsigned int i, j;

	for (i = 0; i < dev->dev_qty; i++) {
		for (j = 0; j < MAX_GPIO_NUM; j++)
			os_gpio_release(dev->ccm_cfg[i]->gpio[j].gpio, 1);
	}
#endif
}

static int vfe_resource_request(struct platform_device *pdev, struct vfe_dev *dev)
{
	int ret;
	struct device_node *np = pdev->dev.of_node;

	vfe_dbg(0, "get irq resource\n");
	/*get irq resource*/
	dev->irq = irq_of_parse_and_map(np, 0);
	if (dev->irq <= 0) {
		vfe_err("failed to get IRQ resource\n");
		return -ENXIO;
	}
#define INTC_EN_IRQ1			(0xf1c20400 + 0x24)
#define INTC_PEND_REG1		    (0xf1c20400 + 0x14)
#define INTC_FF_REG1		    (0xf1c20400 + 0x54)

#define CSI_BASE_REG			 0xf1cb0000
#define CSI_IRQ_EN			    (0xf1cb0000 + 0x30)
#define CSI_IRQ_ST			    (0xf1cb0000 + 0x34)

#define IRQ_EN	(1<<0)
#ifdef CONFIG_ARCH_SUN3IW1P1
	ret = request_irq(dev->irq, vfe_isr, 0, "csi_irq", dev);
	if (ret) {
				vfe_err("failed to install irq (%d)\n", ret);
				return -ENXIO;
	}
	writel(CSI_INT_ALL, CSI_IRQ_EN);
	writel(0xff, CSI_BASE_REG);

	pr_info("%s: csi irq(%d) enable\n", __func__, dev->irq);
#else
#ifndef FPGA_VER
	ret = request_irq(dev->irq, vfe_isr, IRQF_SHARED, pdev->name, dev);
#else
	ret = request_irq(dev->irq, vfe_isr, IRQF_SHARED, pdev->name, dev);
#endif
	if (ret) {
		vfe_err("failed to install irq (%d)\n", ret);
		return -ENXIO;
	}
	vfe_dbg(0, "clock resource\n");
#endif

	vfe_dbg(0, "get pin resource\n");
	/* request gpio */
	vfe_request_gpio(dev);

	return 0;
}

static void vfe_resource_release(struct vfe_dev *dev)
{
	vfe_gpio_release(dev);
	vfe_pin_release(dev);
	/* vfe_clk_release(dev); */
	if (dev->irq > 0)
		free_irq(dev->irq, dev);
}
static int vfe_set_sensor_power_on(struct vfe_dev *dev)
{
	int ret = 0;
#ifdef _REGULATOR_CHANGE_
	vfe_device_regulator_get(dev->ccm_cfg[dev->input]);
	dev->power = &dev->ccm_cfg[dev->input]->power;
#endif

#if defined(CONFIG_ARCH_SUN8IW6P1) || defined(CONFIG_ARCH_SUN8IW7P1)
#else
	if (!IS_ERR_OR_NULL(dev->sd))
		vfe_set_pmu_channel(dev->sd, IOVDD, ON);

	usleep_range(10000, 12000);
#endif
	ret = v4l2_subdev_call(dev->sd, core, s_power, CSI_SUBDEV_PWR_ON);
	dev->vfe_sensor_power_cnt++;
	vfe_dbg(0, "power_on______________________________\n");
	return ret;
}

static int vfe_set_sensor_power_off(struct vfe_dev *dev)
{
	int ret = 0;

	if (dev->vfe_sensor_power_cnt > 0) {
		ret = v4l2_subdev_call(dev->sd, core, s_power, CSI_SUBDEV_PWR_OFF);
#if defined(CONFIG_ARCH_SUN8IW6P1) || defined(CONFIG_ARCH_SUN8IW7P1)
#else
		usleep_range(10000, 12000);
		if (!IS_ERR_OR_NULL(dev->sd))
			vfe_set_pmu_channel(dev->sd, IOVDD, OFF);
#endif
		dev->vfe_sensor_power_cnt--;
	} else {
		vfe_warn("Sensor is already power off!\n");
		dev->vfe_sensor_power_cnt = 0;
	}

#ifdef _REGULATOR_CHANGE_
	vfe_device_regulator_put(dev->ccm_cfg[dev->input]);
#endif
	vfe_dbg(0, "power_off______________________________\n");

	return ret;
}

static const char *vfe_regulator_name[] = {
	VFE_ISP_REGULATOR,
	VFE_CSI_REGULATOR,
};

static int vfe_get_regulator(struct vfe_dev *dev)
{
	struct regulator *regul;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vfe_regulator_name); ++i) {
		if (strcmp(vfe_regulator_name[i], "") != 0) {
			regul = regulator_get(NULL, vfe_regulator_name[i]);
			if (IS_ERR_OR_NULL(regul)) {
				vfe_err("get regulator vfe system power error, i = %d!\n", i);
				regul = NULL;
			}
		} else
			regul = NULL;

		dev->vfe_system_power[i] = regul;
	}

	return 0;
}
static int vfe_enable_regulator_all(struct vfe_dev *dev)
{
	unsigned int i, ret = -1;

	for (i = 0; i < ARRAY_SIZE(vfe_regulator_name); ++i) {
		if (dev->vfe_system_power[i] != NULL)
			ret = regulator_enable(dev->vfe_system_power[i]);
	}
	usleep_range(5000, 6000);

	return ret;
}

static int vfe_disable_regulator_all(struct vfe_dev *dev)
{
	unsigned int i, ret = -1;

	for (i = 0; i < ARRAY_SIZE(vfe_regulator_name); ++i) {
		if (dev->vfe_system_power[i] != NULL)
			ret = regulator_disable(dev->vfe_system_power[i]);
	}

	return ret;
}

static int vfe_put_regulator(struct vfe_dev *dev)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vfe_regulator_name); ++i) {
		if (dev->vfe_system_power[i] != NULL)
			regulator_put(dev->vfe_system_power[i]);
	}

	return 0;
}

static int vfe_device_regulator_get(struct ccm_config  *ccm_cfg)
{
#ifdef VFE_PMU
	/*power issue*/
	ccm_cfg->power.iovdd = NULL;
	ccm_cfg->power.avdd = NULL;
	ccm_cfg->power.dvdd = NULL;
	ccm_cfg->power.afvdd = NULL;
	ccm_cfg->power.flvdd = NULL;

	if (strcmp(ccm_cfg->iovdd_str, "")) {
		ccm_cfg->power.iovdd = regulator_get(NULL, ccm_cfg->iovdd_str);
		if (IS_ERR_OR_NULL(ccm_cfg->power.iovdd)) {
			vfe_err("get regulator csi_iovdd error!\n");
			goto regulator_get_err;
		}
	}
	if (strcmp(ccm_cfg->avdd_str, "")) {
		ccm_cfg->power.avdd = regulator_get(NULL, ccm_cfg->avdd_str);
		if (IS_ERR_OR_NULL(ccm_cfg->power.avdd)) {
			vfe_err("get regulator csi_avdd error!\n");
			goto regulator_get_err;
		}
	}
	if (strcmp(ccm_cfg->dvdd_str, "")) {
		ccm_cfg->power.dvdd = regulator_get(NULL, ccm_cfg->dvdd_str);
		if (IS_ERR_OR_NULL(ccm_cfg->power.dvdd)) {
			vfe_err("get regulator csi_dvdd error!\n");
			goto regulator_get_err;
		}
	}
	if (strcmp(ccm_cfg->afvdd_str, "")) {
		ccm_cfg->power.afvdd = regulator_get(NULL, ccm_cfg->afvdd_str);
		if (IS_ERR_OR_NULL(ccm_cfg->power.afvdd)) {
			vfe_err("get regulator csi_afvdd error!\n");
			goto regulator_get_err;
		}
	}
	if (strcmp(ccm_cfg->flvdd_str, "")) {
		ccm_cfg->power.flvdd = regulator_get(NULL, ccm_cfg->flvdd_str);
		if (IS_ERR_OR_NULL(ccm_cfg->power.flvdd)) {
			vfe_err("get regulator csi_flvdd error!\n");
			goto regulator_get_err;
		}
	}

	return 0;
regulator_get_err:
	return -1;
#else
	return 0;
#endif
}

static int vfe_device_regulator_put(struct ccm_config  *ccm_cfg)
{
	/*power issue*/
	if (!IS_ERR_OR_NULL(ccm_cfg->power.iovdd))
		regulator_put(ccm_cfg->power.iovdd);
	if (!IS_ERR_OR_NULL(ccm_cfg->power.avdd))
		regulator_put(ccm_cfg->power.avdd);
	if (!IS_ERR_OR_NULL(ccm_cfg->power.dvdd))
		regulator_put(ccm_cfg->power.dvdd);
	if (!IS_ERR_OR_NULL(ccm_cfg->power.afvdd))
		regulator_put(ccm_cfg->power.afvdd);
	if (!IS_ERR_OR_NULL(ccm_cfg->power.flvdd))
		regulator_put(ccm_cfg->power.flvdd);

	return 0;
}

static int vfe_sensor_check(struct vfe_dev *dev)
{
	int ret = 0;
	struct v4l2_subdev *sd = dev->sd;

	vfe_print("Check sensor!\n");
	vfe_set_sensor_power_on(dev);
#ifdef USE_SPECIFIC_CCI
	csi_cci_init_helper(dev->cci_sel);
#endif
	ret = (v4l2_subdev_call(sd, core, init, 0) < 0) ? -1 : 0;
	vfe_set_sensor_power_off(dev);
	if (vfe_i2c_dbg == 1) {
		vfe_print("NOTE: Sensor i2c dbg, it's always power on and register success!..................\n");
		ret = 0;
		vfe_set_sensor_power_on(dev);
	}
#ifdef USE_SPECIFIC_CCI
	csi_cci_exit_helper(dev->cci_sel);
#endif
	return ret;
}

#ifdef USE_SPECIFIC_CCI
static int vfe_sensor_subdev_register_check(struct vfe_dev *dev,
					struct v4l2_device *v4l2_dev,
					struct ccm_config  *ccm_cfg,
					struct i2c_board_info *sensor_i2c_board)
{
	int ret;

	ccm_cfg->sd = NULL;
	ccm_cfg->sd = cci_bus_match(ccm_cfg->ccm, dev->cci_sel, sensor_i2c_board->addr);/* ccm_cfg->i2c_addr >> 1);*/
	if (ccm_cfg->sd) {
		ret = v4l2_device_register_subdev(&dev->v4l2_dev, ccm_cfg->sd);
		vfe_print("v4l2_device_register_subdev return %d\n", ret);
		if (ret < 0)
			ccm_cfg->sd = NULL;
	}
	update_ccm_info(dev, ccm_cfg);
	if (IS_ERR_OR_NULL(ccm_cfg->sd)) {
		vfe_err("Error registering v4l2 subdevice No such device!\n");
		return -ENODEV;
	} else
		vfe_print("registered sensor subdev is OK!\n");
	/* Subdev register is OK, check sensor init! */
	return vfe_sensor_check(dev);
}

static int vfe_sensor_subdev_unregister(struct v4l2_device *v4l2_dev,
					struct ccm_config  *ccm_cfg,
					struct i2c_board_info *sensor_i2c_board)
{
	struct cci_driver *cci_driv = v4l2_get_subdevdata(ccm_cfg->sd);

	if (IS_ERR_OR_NULL(cci_driv))
		return -ENODEV;
	vfe_print("vfe sensor subdev unregister!\n");
	v4l2_device_unregister_subdev(ccm_cfg->sd);
	cci_bus_match_cancel(cci_driv);

	return 0;
}
static int vfe_actuator_subdev_register(struct vfe_dev *dev,
					struct ccm_config  *ccm_cfg,
					struct i2c_board_info *act_i2c_board)
{
	ccm_cfg->sd_act = NULL;
	ccm_cfg->sd_act = cci_bus_match(ccm_cfg->act_name, dev->cci_sel, act_i2c_board->addr);/* ccm_cfg->i2c_addr >> 1); */
	/* reg_sd_act: */
	if (!ccm_cfg->sd_act) {
		vfe_err("Error registering v4l2 act subdevice!\n");
		return  -EINVAL;
	} else
		vfe_print("registered actuator device succeed! act_name is %s\n", ccm_cfg->act_name);

	ccm_cfg->act_ctrl = (struct actuator_ctrl_t *)container_of(ccm_cfg->sd_act, struct actuator_ctrl_t, sdev);
	/* printk("ccm_cfg->act_ctrl=%x\n",(unsigned int )ccm_cfg->act_ctrl); */

	return 0;
}
#else /* NOT defind USE_SPECIFIC_CCI */
static int vfe_sensor_subdev_register_check(struct vfe_dev *dev,
					struct v4l2_device *v4l2_dev,
					struct ccm_config  *ccm_cfg,
					struct i2c_board_info *sensor_i2c_board)
{
	struct i2c_adapter *i2c_adap = i2c_get_adapter(ccm_cfg->twi_id);

	if (i2c_adap == NULL) {
		vfe_err("request i2c adapter failed!\n");
		return -EFAULT;
	}
	ccm_cfg->sd = v4l2_i2c_new_subdev_board(v4l2_dev, i2c_adap, sensor_i2c_board, NULL);
	if (IS_ERR_OR_NULL(ccm_cfg->sd)) {
		i2c_put_adapter(i2c_adap);
		vfe_err("Error registering v4l2 subdevice No such device!\n");
		return -ENODEV;
	} else
		vfe_print("registered sensor subdev is OK!\n");

	update_ccm_info(dev, ccm_cfg);
	/* Subdev register is OK, check sensor init! */
	return vfe_sensor_check(dev);
}
static int vfe_sensor_subdev_unregister(struct v4l2_device *v4l2_dev,
					struct ccm_config  *ccm_cfg,
					struct i2c_board_info *sensor_i2c_board)
{
	struct i2c_client *client = v4l2_get_subdevdata(ccm_cfg->sd);
	struct i2c_adapter *adapter;

	if (!client)
		return -ENODEV;
	vfe_print("vfe sensor subdev unregister!\n");
	v4l2_device_unregister_subdev(ccm_cfg->sd);
	adapter = client->adapter;
	i2c_unregister_device(client);
	if (adapter)
		i2c_put_adapter(adapter);

	return 0;
}
static int vfe_actuator_subdev_register(struct vfe_dev *dev,
					struct ccm_config  *ccm_cfg,
					struct i2c_board_info *act_i2c_board)
{
	struct v4l2_device *v4l2_dev = &dev->v4l2_dev;
	struct i2c_adapter *i2c_adap_act = i2c_get_adapter(ccm_cfg->twi_id);/* must use the same twi_channel with sensor */

	if (i2c_adap_act == NULL) {
		vfe_err("request act i2c adapter failed\n");
		return  -EINVAL;
	}

	ccm_cfg->sd_act = NULL;
	act_i2c_board->addr = (unsigned short)(ccm_cfg->act_slave>>1);
	strcpy(act_i2c_board->type, ccm_cfg->act_name);
	ccm_cfg->sd_act = v4l2_i2c_new_subdev_board(v4l2_dev, i2c_adap_act, act_i2c_board, NULL);
	/* reg_sd_act: */
	if (!ccm_cfg->sd_act) {
		vfe_err("Error registering v4l2 act subdevice!\n");
		return  -EINVAL;
	} else
		vfe_print("registered actuator device succeed!\n");

	ccm_cfg->act_ctrl = (struct actuator_ctrl_t *)container_of(ccm_cfg->sd_act, struct actuator_ctrl_t, sdev);

	return 0;
}
#endif
static void cpy_ccm_power_settings(struct ccm_config *ccm_cfg)
{
	strcpy(ccm_cfg->iovdd_str, ccm_cfg->sensor_cfg_ini->sub_power_str[ENUM_IOVDD]);
	ccm_cfg->power.iovdd_vol = ccm_cfg->sensor_cfg_ini->sub_power_vol[ENUM_IOVDD];

	strcpy(ccm_cfg->avdd_str, ccm_cfg->sensor_cfg_ini->sub_power_str[ENUM_AVDD]);
	ccm_cfg->power.avdd_vol = ccm_cfg->sensor_cfg_ini->sub_power_vol[ENUM_AVDD];

	strcpy(ccm_cfg->dvdd_str, ccm_cfg->sensor_cfg_ini->sub_power_str[ENUM_DVDD]);
	ccm_cfg->power.dvdd_vol = ccm_cfg->sensor_cfg_ini->sub_power_vol[ENUM_DVDD];

	strcpy(ccm_cfg->afvdd_str, ccm_cfg->sensor_cfg_ini->sub_power_str[ENUM_AFVDD]);
	ccm_cfg->power.afvdd_vol = ccm_cfg->sensor_cfg_ini->sub_power_vol[ENUM_AFVDD];
}

static int cpy_ccm_sub_device_cfg(struct ccm_config *ccm_cfg, int n)
{
	strcpy(ccm_cfg->ccm, ccm_cfg->sensor_cfg_ini->camera_inst[n].name);
	if (strcmp(ccm_cfg->sensor_cfg_ini->camera_inst[n].isp_cfg_name, ""))
		strcpy(ccm_cfg->isp_cfg_name, ccm_cfg->sensor_cfg_ini->camera_inst[n].isp_cfg_name);
	else
		strcpy(ccm_cfg->isp_cfg_name, ccm_cfg->ccm);

	ccm_cfg->i2c_addr = ccm_cfg->sensor_cfg_ini->camera_inst[n].i2c_addr;
	ccm_cfg->hflip = ccm_cfg->sensor_cfg_ini->camera_inst[n].hflip;
	ccm_cfg->vflip = ccm_cfg->sensor_cfg_ini->camera_inst[n].vflip;
	ccm_cfg->hflip_thumb = ccm_cfg->sensor_cfg_ini->camera_inst[n].hflip;
	ccm_cfg->vflip_thumb = ccm_cfg->sensor_cfg_ini->camera_inst[n].vflip;
	ccm_cfg->power.stby_mode = ccm_cfg->sensor_cfg_ini->camera_inst[n].stdby_mode;
	if (ccm_cfg->sensor_cfg_ini->camera_inst[n].sensor_type == SENSOR_RAW) {
		ccm_cfg->is_bayer_raw = 1;
		ccm_cfg->is_isp_used = 1;
	} else if (ccm_cfg->sensor_cfg_ini->camera_inst[n].sensor_type == SENSOR_YUV) {
		ccm_cfg->is_bayer_raw = 0;
		ccm_cfg->is_isp_used = 0;
	} else {
		ccm_cfg->is_bayer_raw = 0;
		ccm_cfg->is_isp_used = 1;
	}
	strcpy(ccm_cfg->act_name, ccm_cfg->sensor_cfg_ini->camera_inst[n].act_name);
	if (strcmp(ccm_cfg->act_name, "")) {
		ccm_cfg->act_used = 1;
		vfe_print("VCM driver name is \"%s\".\n", ccm_cfg->act_name);
	}
	ccm_cfg->act_slave = ccm_cfg->sensor_cfg_ini->camera_inst[n].act_i2c_addr;

	return 0;
}

static const char * const sensor_info_type[] = {
	"YUV",
	"RAW",
	NULL,
};

static struct v4l2_subdev *vfe_sensor_register_check(struct vfe_dev *dev,
						struct v4l2_device *v4l2_dev,
						struct ccm_config  *ccm_cfg,
						struct i2c_board_info *sensor_i2c_board,
						int input_num)
{
	int sensor_cnt, ret, sensor_num;
	struct sensor_item sensor_info;

	if (dev->vip_define_sensor_list == 1) {
		sensor_num = ccm_cfg->sensor_cfg_ini->detect_sensor_num;
		if (ccm_cfg->sensor_cfg_ini->detect_sensor_num == 0)
			sensor_num = 1;
	} else
		sensor_num = 1;

	for (sensor_cnt = 0; sensor_cnt < sensor_num; sensor_cnt++) {
		if (dev->vip_define_sensor_list == 1) {
			if (ccm_cfg->sensor_cfg_ini->detect_sensor_num > 0)
				cpy_ccm_sub_device_cfg(ccm_cfg, sensor_cnt);
		}
		if (get_sensor_info(ccm_cfg->ccm, &sensor_info) == 0) {
			if (ccm_cfg->i2c_addr != sensor_info.i2c_addr)
				vfe_warn("Sensor info \"%s\" i2c_addr is different from sys_config!\n", sensor_info.sensor_name);

			if (ccm_cfg->is_bayer_raw != sensor_info.sensor_type) {
				vfe_warn("Camer detect \"%s\" fmt is different from sys_config!\n",
								sensor_info_type[sensor_info.sensor_type]);
				vfe_warn("Apply detect  fmt = %d replace sys_config fmt = %d!\n",
								sensor_info.sensor_type, ccm_cfg->is_bayer_raw);
				ccm_cfg->is_bayer_raw = sensor_info.sensor_type;
			}
			if (sensor_info.sensor_type == SENSOR_RAW)
				ccm_cfg->is_isp_used = 1;

			else
				ccm_cfg->act_used = 0;

			vfe_print("Find sensor name is \"%s\", i2c address is %x, type is \"%s\" !\n",
								sensor_info.sensor_name, sensor_info.i2c_addr,
								sensor_info_type[sensor_info.sensor_type]);
		}
		sensor_i2c_board->addr = (unsigned short)(ccm_cfg->i2c_addr>>1);
		strcpy(sensor_i2c_board->type, ccm_cfg->ccm);

		vfe_print("Sub device register \"%s\" i2c_addr = 0x%x start!\n", sensor_i2c_board->type, ccm_cfg->i2c_addr);
		ret = vfe_sensor_subdev_register_check(dev, v4l2_dev, ccm_cfg, sensor_i2c_board);
		if (ret == -1) {
			vfe_sensor_subdev_unregister(v4l2_dev, ccm_cfg, sensor_i2c_board);
			vfe_print("Sub device register \"%s\" failed!\n", sensor_i2c_board->type);
			ccm_cfg->sd = NULL;
			continue;
		} else if (ret == ENODEV || ret == EFAULT)
			continue;
		else if (ret == 0) {
			vfe_print("Sub device register \"%s\" is OK!\n", sensor_i2c_board->type);
			break;
		}
	}
	return ccm_cfg->sd;
}

static const struct v4l2_ctrl_config custom_ctrls[] = {
	{
		.ops = &vfe_ctrl_ops,
		.id = V4L2_CID_HOR_VISUAL_ANGLE,
		.name = "Horizontal Visual Angle",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0,
		.max = 360,
		.step = 1,
		.def = 60,
		.flags = V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_READ_ONLY,
	},
	{
		.ops = &vfe_ctrl_ops,
		.id = V4L2_CID_VER_VISUAL_ANGLE,
		.name = "Vertical Visual Angle",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0,
		.max = 360,
		.step = 1,
		.def = 60,
		.flags = V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_READ_ONLY,
	},
	{
		.ops = &vfe_ctrl_ops,
		.id = V4L2_CID_FOCUS_LENGTH,
		.name = "Focus Length",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0,
		.max = 1000,
		.step = 1,
		.def = 280,
		.flags = V4L2_CTRL_FLAG_VOLATILE,
	},
	{
		.ops = &vfe_ctrl_ops,
		.id = V4L2_CID_R_GAIN,
		.name = "R GAIN",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 32,
		.max = 1024,
		.step = 1,
		.def = 256,
		.flags = V4L2_CTRL_FLAG_VOLATILE,
	},
	{
		.ops = &vfe_ctrl_ops,
		.id = V4L2_CID_G_GAIN,
		.name = "G GAIN",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 32,
		.max = 1024,
		.step = 1,
		.def = 256,
		.flags = V4L2_CTRL_FLAG_VOLATILE,
	},
	{
		.ops = &vfe_ctrl_ops,
		.id = V4L2_CID_B_GAIN,
		.name = "B GAIN",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 32,
		.max = 1024,
		.step = 1,
		.def = 256,
		.flags = V4L2_CTRL_FLAG_VOLATILE,
	},
	{
		.ops = &vfe_ctrl_ops,
		.id = V4L2_CID_AUTO_FOCUS_INIT,
		.name = "AutoFocus Initial",
		.type = V4L2_CTRL_TYPE_BUTTON,
		.min = 0,
		.max = 0,
		.step = 0,
		.def = 0,
	},
	{
		.ops = &vfe_ctrl_ops,
		.id = V4L2_CID_AUTO_FOCUS_RELEASE,
		.name = "AutoFocus Release",
		.type = V4L2_CTRL_TYPE_BUTTON,
		.min = 0,
		.max = 0,
		.step = 0,
		.def = 0,
	},
	{
		.ops = &vfe_ctrl_ops,
		.id = V4L2_CID_GSENSOR_ROTATION,
		.name = "Gsensor Rotaion",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = -180,
		.max = 180,
		.step = 90,
		.def = 0,
	},
	{
		.ops = &vfe_ctrl_ops,
		.id = V4L2_CID_TAKE_PICTURE,
		.name = "Take Picture",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0,
		.max = 16,
		.step = 1,
		.def = 0,
	},
	{
		.ops = &vfe_ctrl_ops,
		.id = V4L2_CID_SENSOR_TYPE,
		.name = "Sensor type",
		.type = V4L2_CTRL_TYPE_MENU,
		.min = 0,
		.max = 1,
		.def = 0,
		.menu_skip_mask = 0x0,
		.qmenu = sensor_info_type,
		.flags = V4L2_CTRL_FLAG_VOLATILE,
	},
};
static const s64 iso_qmenu[] = {
	50, 100, 200, 400, 800,
};
static const s64 exp_bias_qmenu[] = {
	-4, -3, -2, -1, 0, 1, 2, 3, 4,
};

static int vfe_init_controls(struct v4l2_ctrl_handler *hdl)
{
	struct v4l2_ctrl *ctrl;
	unsigned int i, ret = 0;

	v4l2_ctrl_handler_init(hdl, 37 + ARRAY_SIZE(custom_ctrls));
	v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_BRIGHTNESS, 0, 255, 1, 128);
	v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_CONTRAST, 0, 128, 1, 0);
	v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_SATURATION, -4, 4, 1, 0);
	v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_HUE, -180, 180, 1, 0);
	v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_AUTO_WHITE_BALANCE, 0, 1, 1, 1);
	ctrl = v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_EXPOSURE, 0, 65536*16, 1, 0);
	if (ctrl != NULL)
		ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;
	v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_AUTOGAIN, 0, 1, 1, 1);
	ctrl = v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_GAIN, 1*16, 64*16-1, 1, 1*16);
	if (ctrl != NULL)
		ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;
	v4l2_ctrl_new_std_menu(hdl, &vfe_ctrl_ops, V4L2_CID_POWER_LINE_FREQUENCY,
		V4L2_CID_POWER_LINE_FREQUENCY_AUTO, 0, V4L2_CID_POWER_LINE_FREQUENCY_AUTO);
	v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_HUE_AUTO, 0, 1, 1, 1);
	v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_WHITE_BALANCE_TEMPERATURE, 2800, 10000, 1, 6500);
	v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_SHARPNESS, -32, 32, 1, 0);
	v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_CHROMA_AGC, 0, 1, 1, 1);
	v4l2_ctrl_new_std_menu(hdl, &vfe_ctrl_ops, V4L2_CID_COLORFX,
		V4L2_COLORFX_SET_CBCR, 0, V4L2_COLORFX_NONE);
	v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_AUTOBRIGHTNESS, 0, 1, 1, 1);
	v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_BAND_STOP_FILTER, 0, 1, 1, 1);
	v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_ILLUMINATORS_1, 0, 1, 1, 0);
	v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_ILLUMINATORS_2, 0, 1, 1, 0);
	v4l2_ctrl_new_std_menu(hdl, &vfe_ctrl_ops, V4L2_CID_EXPOSURE_AUTO,
		V4L2_EXPOSURE_APERTURE_PRIORITY, 0, V4L2_EXPOSURE_AUTO);
	v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_EXPOSURE_ABSOLUTE, 1, 1000000, 1, 1);
	v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_EXPOSURE_AUTO_PRIORITY, 0, 1, 1, 0);
	v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_FOCUS_ABSOLUTE, 0, 127, 1, 0);
	v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_FOCUS_RELATIVE, -127, 127, 1, 0);
	v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_FOCUS_AUTO, 0, 1, 1, 1);
	v4l2_ctrl_new_int_menu(hdl, &vfe_ctrl_ops, V4L2_CID_AUTO_EXPOSURE_BIAS, ARRAY_SIZE(exp_bias_qmenu) - 1,
		ARRAY_SIZE(exp_bias_qmenu)/2, exp_bias_qmenu);
	v4l2_ctrl_new_std_menu(hdl, &vfe_ctrl_ops, V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE,
		V4L2_WHITE_BALANCE_SHADE, 0, V4L2_WHITE_BALANCE_AUTO);
	v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_WIDE_DYNAMIC_RANGE, 0, 1, 1, 0);
	v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_IMAGE_STABILIZATION, 0, 1, 1, 0);
	v4l2_ctrl_new_int_menu(hdl, &vfe_ctrl_ops, V4L2_CID_ISO_SENSITIVITY, ARRAY_SIZE(iso_qmenu) - 1,
		ARRAY_SIZE(iso_qmenu)/2 - 1, iso_qmenu);
	v4l2_ctrl_new_std_menu(hdl, &vfe_ctrl_ops, V4L2_CID_ISO_SENSITIVITY_AUTO,
		V4L2_ISO_SENSITIVITY_AUTO, 0, V4L2_ISO_SENSITIVITY_AUTO);
	v4l2_ctrl_new_std_menu(hdl, &vfe_ctrl_ops, V4L2_CID_SCENE_MODE,
		V4L2_SCENE_MODE_TEXT, 0, V4L2_SCENE_MODE_NONE);
	ctrl = v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_3A_LOCK, 0, 7, 0, 0);
	if (ctrl != NULL)
		ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;
	v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_AUTO_FOCUS_START, 0, 0, 0, 0);
	v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_AUTO_FOCUS_STOP, 0, 0, 0, 0);
	ctrl = v4l2_ctrl_new_std(hdl, &vfe_ctrl_ops, V4L2_CID_AUTO_FOCUS_STATUS, 0, 7, 0, 0);
	if (ctrl != NULL)
		ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;
	v4l2_ctrl_new_std_menu(hdl, &vfe_ctrl_ops, V4L2_CID_AUTO_FOCUS_RANGE,
		V4L2_AUTO_FOCUS_RANGE_INFINITY, 0, V4L2_AUTO_FOCUS_RANGE_AUTO);
	v4l2_ctrl_new_std_menu(hdl, &vfe_ctrl_ops, V4L2_CID_FLASH_LED_MODE,
		V4L2_FLASH_LED_MODE_RED_EYE, 0, V4L2_FLASH_LED_MODE_NONE);

	for (i = 0; i < ARRAY_SIZE(custom_ctrls); i++)
		v4l2_ctrl_new_custom(hdl, &custom_ctrls[i], NULL);

	if (hdl->error) {
		ret = hdl->error;
		v4l2_ctrl_handler_free(hdl);
	}

	return ret;
}

static void probe_work_handle(struct work_struct *work)
{
	struct vfe_dev *dev = container_of(work, struct vfe_dev, probe_work.work);
	int ret = 0, i, video_cnt = 1, video_num;
	int input_num;
	int device_valid_count = 0;
	struct video_device *vfd;
	struct vb2_queue *q;

	mutex_lock(&probe_hdl_lock);
	vfe_print("probe_work_handle start!\n");
	vfe_dbg(0, "v4l2_device_register\n");
	/* v4l2 device register */
	ret = v4l2_device_register(&dev->pdev->dev, &dev->v4l2_dev);
	if (ret) {
		vfe_err("Error registering v4l2 device\n");
		goto probe_hdl_end;
	}

	ret = vfe_init_controls(&dev->ctrl_handler);
	if (ret) {
		vfe_err("Error v4l2 ctrls new!!\n");
		goto probe_hdl_unreg_dev;
	}
	dev->v4l2_dev.ctrl_handler = &dev->ctrl_handler;

	dev_set_drvdata(&dev->pdev->dev, (dev));
	vfe_dbg(0, "v4l2 subdev register\n");
	/* v4l2 subdev register */
	/*Register ISP subdev*/
	sunxi_isp_get_subdev(&dev->isp_sd, dev->isp_sel);
	sunxi_isp_register_subdev(&dev->v4l2_dev, dev->isp_sd);
	/*Register CSI subdev*/
	sunxi_csi_get_subdev(&dev->csi_sd, dev->csi_sel);
	sunxi_csi_register_subdev(&dev->v4l2_dev, dev->csi_sd);
	/*Register MIPI subdev*/
	sunxi_mipi_get_subdev(&dev->mipi_sd, dev->mipi_sel);
	sunxi_mipi_register_subdev(&dev->v4l2_dev, dev->mipi_sd);
	/*Register flash subdev*/
	if (dev->ccm_cfg[0]->flash_used || dev->ccm_cfg[1]->flash_used) {
		sunxi_flash_get_subdev(&dev->flash_sd, dev->flash_sel);
		sunxi_flash_register_subdev(&dev->v4l2_dev, dev->flash_sd);
	}
	/*Register Sensor subdev*/
	dev->is_same_module = 0;
#ifdef CONFIG_PM_RUNTIME
	pm_runtime_enable(&dev->pdev->dev);
#endif
	vfe_resume_trip(dev);
	for (input_num = 0; input_num < dev->dev_qty; input_num++) {
		vfe_print("v4l2 subdev register input_num = %d\n", input_num);
		if (!strcmp(dev->ccm_cfg[input_num]->ccm, "")) {
			vfe_err("Sensor name is NULL!\n");
			goto snesor_register_end;
		}
		if (dev->is_same_module) {
			dev->ccm_cfg[input_num]->sd = dev->ccm_cfg[input_num-1]->sd;
			vfe_dbg(0, "num = %d , sd_0 = %p, sd_1 = %p\n", input_num,
					dev->ccm_cfg[input_num]->sd, dev->ccm_cfg[input_num-1]->sd);
			goto snesor_register_end;
		}

		if ((dev->dev_qty > 1) && (input_num+1 < dev->dev_qty)) {
			if ((!strcmp(dev->ccm_cfg[input_num]->ccm, dev->ccm_cfg[input_num+1]->ccm)))
				dev->is_same_module = 1;
		}
		if (dev->vip_define_sensor_list == 1) {
			if (dev->ccm_cfg[input_num]->sensor_cfg_ini->power_settings_enable == 1)
				cpy_ccm_power_settings(dev->ccm_cfg[input_num]);
		}
#ifdef _REGULATOR_CHANGE_
#else
		if (vfe_device_regulator_get(dev->ccm_cfg[input_num])) {
			vfe_err("vfe_device_regulator_get error at input_num = %d\n", input_num);
			goto snesor_register_end;
		}
#endif
		vfe_print("vfe sensor detect start! input_num = %d\n", input_num);
		dev->input = input_num;
		if (vfe_sensor_register_check(dev, &dev->v4l2_dev, dev->ccm_cfg[input_num],
					&dev->dev_sensor[input_num], input_num) == NULL) {
			vfe_err("vfe sensor register check error at input_num = %d\n", input_num);
			dev->device_valid_flag[input_num] = 0;
			/* goto snesor_register_end; */
		} else {
			dev->device_valid_flag[input_num] = 1;
			device_valid_count++;
		}
		if (dev->ccm_cfg[input_num]->is_isp_used && dev->ccm_cfg[input_num]->is_bayer_raw) {
			if (read_ini_info(dev, input_num, "/system/etc/hawkview/"))
				vfe_warn("read ini info fail\n");
		}

		if (dev->ccm_cfg[input_num]->act_used == 1) {
			dev->dev_act[input_num].addr = (unsigned short)(dev->ccm_cfg[input_num]->act_slave>>1);
			strcpy(dev->dev_act[input_num].type, dev->ccm_cfg[input_num]->act_name);
			vfe_actuator_subdev_register(dev, dev->ccm_cfg[input_num], &dev->dev_act[input_num]);
		}
snesor_register_end:
		vfe_dbg(0, "dev->ccm_cfg[%d] = %p\n", input_num, dev->ccm_cfg[input_num]);
		vfe_dbg(0, "dev->ccm_cfg[%d]->sd = %p\n", input_num, dev->ccm_cfg[input_num]->sd);
		vfe_dbg(0, "dev->ccm_cfg[%d]->power.iovdd = %p\n", input_num, dev->ccm_cfg[input_num]->power.iovdd);
		vfe_dbg(0, "dev->ccm_cfg[%d]->power.avdd = %p\n", input_num, dev->ccm_cfg[input_num]->power.avdd);
		vfe_dbg(0, "dev->ccm_cfg[%d]->power.dvdd = %p\n", input_num, dev->ccm_cfg[input_num]->power.dvdd);
		vfe_dbg(0, "dev->ccm_cfg[%d]->power.afvdd = %p\n", input_num, dev->ccm_cfg[input_num]->power.afvdd);
	}
	dev->input = -1;
	/*video device register */
#if defined(CH_OUTPUT_IN_DIFFERENT_VIDEO)
	if (dev->id == 0)
		video_cnt = MAX_CH_NUM;
	else
		video_cnt = 1;
#else
	video_cnt = 1;
#endif
	for (i = 0; i < video_cnt; i++) {
		vfd = video_device_alloc();
		if (!vfd) {
			vfe_err("Error video_device_alloc!!\n");
			goto close_clk_pin_power;
		}
		*vfd = vfe_template[dev->id];
		vfd->v4l2_dev = &dev->v4l2_dev;
		if (device_valid_count != 0) {
#if defined(CH_OUTPUT_IN_DIFFERENT_VIDEO)
			if (dev->id == 0)
				video_num = i + VID_N_OFF;
			else
				video_num = dev->id;
#else
			video_num = dev->id;
#endif
			ret = video_register_device(vfd, VFL_TYPE_GRABBER, video_num);
			if (ret < 0) {
				vfe_err("Error video_register_device!!\n");
				goto probe_hdl_rel_vdev;
			}
		}

		vfd->lock = &dev->buf_lock;
		video_set_drvdata(vfd, dev);

		dev->vfd[i] = vfd;
		vfe_print("V4L2 device registered as %s\n", video_device_node_name(vfd));

		/* Initialize videobuf2 queue as per the buffer type */
		dev->alloc_ctx[i] = vb2_dma_contig_init_ctx(&dev->pdev->dev);
		if (IS_ERR(dev->alloc_ctx[i])) {
			vfe_err("Failed to get the context\n");
			goto probe_hdl_rel_vdev;
		}
		/* initialize queue */
		q = &dev->vb_vidq[i];
		q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF | VB2_READ;
		q->drv_priv = dev;
		q->buf_struct_size = sizeof(struct vfe_buffer);
		q->ops = &vfe_video_qops;
		q->mem_ops = &vb2_dma_contig_memops;
		q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
		q->lock = &dev->buf_lock;

		ret = vb2_queue_init(q);
		if (ret) {
			vfe_err("vb2_queue_init() failed\n");
			vb2_dma_contig_cleanup_ctx(dev->alloc_ctx[i]);
			goto probe_hdl_rel_vdev;
		}

		INIT_LIST_HEAD(&dev->vidq[i].active);
	}

	/* Now that everything is fine, let's add it to device list */
	list_add_tail(&dev->devlist, &devlist);

	ret = sysfs_create_group(&dev->pdev->dev.kobj, &vfe_attribute_group);
	vfe_suspend_trip(dev);
	vfe_print("probe_work_handle end!\n");
	mutex_unlock(&probe_hdl_lock);
	return;
probe_hdl_rel_vdev:
	video_device_release(vfd);
	vfe_print("video_device_release @ probe_hdl!\n");
close_clk_pin_power:
	vfe_suspend_trip(dev);
probe_hdl_unreg_dev:
	vfe_print("v4l2_device_unregister @ probe_hdl!\n");
	v4l2_device_unregister(&dev->v4l2_dev);
probe_hdl_end:
	vfe_err("Failed to install at probe handle\n");
	mutex_unlock(&probe_hdl_lock);
	return;
}

static int vfe_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct vfe_dev *dev;
	int ret = 0;
	int input_num;
	unsigned int i;

	vfe_dbg(0, "vfe_probe\n");

	/*request mem for dev*/
	dev = kzalloc(sizeof(struct vfe_dev), GFP_KERNEL);
	if (!dev) {
		vfe_err("request dev mem failed!\n");
		ret = -ENOMEM;
		goto ekzalloc;
	}
	for (i = 0; i < MAX_INPUT_NUM; i++) {
		dev->isp_gen_set[i] = kzalloc(sizeof(struct isp_gen_settings), GFP_KERNEL);
		if (!dev->isp_gen_set[i]) {
			vfe_err("request isp_gen_settings mem failed!\n");
			return -ENOMEM;
		}
	}

	pdev->id = of_alias_get_id(np, "vfe");
	if (pdev->id < 0) {
		vfe_err("VFE failed to get alias id\n");
		ret = -EINVAL;
		goto freedev;
	}

	of_property_read_u32(np, "cci_sel", &dev->cci_sel);
	of_property_read_u32(np, "csi_sel", &dev->csi_sel);
	of_property_read_u32(np, "mipi_sel", &dev->mipi_sel);
	of_property_read_u32(np, "isp_sel", &dev->isp_sel);

	dev->platform_id = SUNXI_PLATFORM_ID;

	dev->id = pdev->id;
	dev->pdev = pdev;
	dev->generating = 0;
	dev->opened = 0;
	dev->vfe_sensor_power_cnt = 0;
	dev->vfe_s_input_flag = 0;

	vfe_print("pdev->id = %d\n", pdev->id);
	vfe_print("dev->cci_sel = %d\n", dev->cci_sel);
	vfe_print("dev->csi_sel = %d\n", dev->csi_sel);
	vfe_print("dev->mipi_sel = %d\n", dev->mipi_sel);
	vfe_print("dev->isp_sel = %d\n", dev->isp_sel);

	vfe_dev_gbl[dev->id] = dev;

	spin_lock_init(&dev->slock);
	vfe_dbg(0, "fetch sys_config\n");
	/* fetch sys_config! */
	for (input_num = 0; input_num < MAX_INPUT_NUM; input_num++) {
		dev->ccm_cfg[input_num] = &dev->ccm_cfg_content[input_num];
		vfe_dbg(0, "dev->ccm_cfg[%d] = %p\n", input_num, dev->ccm_cfg[input_num]);
		dev->ccm_cfg[input_num]->i2c_addr = i2c_addr;
		strcpy(dev->ccm_cfg[input_num]->ccm, ccm);
		strcpy(dev->ccm_cfg[input_num]->isp_cfg_name, ccm);
		dev->ccm_cfg[input_num]->act_slave = act_slave;
		strcpy(dev->ccm_cfg[input_num]->act_name, act_name);
		dev->vip_define_sensor_list = define_sensor_list;
	}

	ret = fetch_config(dev);
	if (ret) {
		vfe_err("Error at fetch_config\n");
		goto freedev;
	}

	if (vips != 0xffff) {
		printk("vips input 0x%x\n", vips);
		dev->ccm_cfg[0]->is_isp_used = (vips&0xf0)>>4;
		dev->ccm_cfg[0]->is_bayer_raw = (vips&0xf00)>>8;
		if ((vips&0xff0) == 0)
			dev->ccm_cfg[0]->act_used = 0;
	}
	vfe_get_regulator(dev);
	ret = vfe_resource_request(pdev, dev);
	if (ret < 0)
		goto freedev;
	/*initial parameter */
	dev->cur_ch = 0;
	dev->isp_init_para.isp_src_ch_mode = ISP_SINGLE_CH;
	for (i = 0; i < MAX_ISP_SRC_CH_NUM; i++)
		dev->isp_init_para.isp_src_ch_en[i] = 0;

	dev->isp_init_para.isp_src_ch_en[dev->id] = 1;

	INIT_DELAYED_WORK(&dev->probe_work, probe_work_handle);
	mutex_init(&dev->stream_lock);
	mutex_init(&dev->opened_lock);
	mutex_init(&dev->buf_lock);
	schedule_delayed_work(&dev->probe_work, msecs_to_jiffies(1));
	/* initial state */
	dev->capture_mode = V4L2_MODE_PREVIEW;
	return 0;
freedev:
	kfree(dev);
ekzalloc:
	vfe_print("vfe probe err!\n");
	return ret;
}

static int vfe_release(void)
{
	struct vfe_dev *dev;
	struct list_head *list;

	vfe_dbg(0, "vfe_release\n");
	while (!list_empty(&devlist)) {
		list = devlist.next;
		list_del(list);
		dev = list_entry(list, struct vfe_dev, devlist);
		kfree(dev);
	}
	vfe_print("vfe_release ok!\n");

	return 0;
}

static int vfe_remove(struct platform_device *pdev)
{
	struct vfe_dev *dev = (struct vfe_dev *)dev_get_drvdata(&pdev->dev);
	int input_num, i, video_cnt = 1;

	/*Unegister ISP subdev*/
	sunxi_isp_unregister_subdev(dev->isp_sd);
	sunxi_isp_put_subdev(&dev->isp_sd, dev->isp_sel);
	/*Unegister CSI subdev*/
	sunxi_csi_unregister_subdev(dev->csi_sd);
	sunxi_csi_put_subdev(&dev->csi_sd, dev->csi_sel);
	/*Unegister MIPI subdev*/
	sunxi_mipi_unregister_subdev(dev->mipi_sd);
	sunxi_mipi_put_subdev(&dev->mipi_sd, dev->mipi_sel);
	/*Unegister flash subdev*/
	if (dev->flash_used == 1) {
		sunxi_flash_unregister_subdev(dev->flash_sd);
		sunxi_flash_put_subdev(&dev->flash_sd, dev->flash_sel);
	}
	mutex_destroy(&dev->stream_lock);
	mutex_destroy(&dev->opened_lock);
	mutex_destroy(&dev->buf_lock);
	sysfs_remove_group(&dev->pdev->dev.kobj, &vfe_attribute_group);
#ifdef USE_SPECIFIC_CCI
	csi_cci_bus_unmatch_helper(dev->cci_sel);
#endif
	vfe_put_regulator(dev);
#ifdef CONFIG_PM_RUNTIME
	pm_runtime_disable(&dev->pdev->dev);
#endif
	for (input_num = 0; input_num < dev->dev_qty; input_num++) {
#ifdef _REGULATOR_CHANGE_
#else
		vfe_device_regulator_put(dev->ccm_cfg[input_num]);
#endif
		if (!dev->ccm_cfg[input_num]->sensor_cfg_ini)
			kfree(dev->ccm_cfg[input_num]->sensor_cfg_ini);
	}
	vfe_resource_release(dev);

#if defined(CH_OUTPUT_IN_DIFFERENT_VIDEO)
	if (dev->id == 0)
		video_cnt = MAX_CH_NUM;
	else
		video_cnt = 1;
#else
	video_cnt = 1;
#endif
	for (i = 0; i < video_cnt; i++) {
		v4l2_info(&dev->v4l2_dev, "unregistering %s\n",
					video_device_node_name(dev->vfd[i]));
		video_unregister_device(dev->vfd[i]);
		vb2_dma_contig_cleanup_ctx(dev->alloc_ctx[i]);
	}

	v4l2_device_unregister(&dev->v4l2_dev);
	v4l2_ctrl_handler_free(&dev->ctrl_handler);
	for (i = 0; i < MAX_INPUT_NUM; i++)
		kfree(dev->isp_gen_set[i]);

	vfe_print("vfe_remove ok!\n");

	return 0;
}

static void vfe_suspend_helper(struct vfe_dev *dev)
{
	if (vfe_i2c_dbg) {
		vfe_print("vfe_i2c_dbg: pin power and clk will not close!!\n");
		return;
	}
	vfe_clk_close(dev);
	vfe_disable_regulator_all(dev);
	vfe_pin_config(dev, 0);
	vfe_gpio_config(dev, 0);
}

static void vfe_resume_helper(struct vfe_dev *dev)
{
	vfe_pin_config(dev, 1);
	vfe_gpio_config(dev, 1);
	vfe_enable_regulator_all(dev);
	vfe_clk_open(dev);
}

static void vfe_suspend_trip(struct vfe_dev *dev)
{
#ifdef CONFIG_PM_RUNTIME
	pm_runtime_put_sync(&dev->pdev->dev);/* call pm_runtime suspend */
#else
	vfe_suspend_helper(dev);
#endif
}
static void vfe_resume_trip(struct vfe_dev *dev)
{
#ifdef CONFIG_PM_RUNTIME
	pm_runtime_get_sync(&dev->pdev->dev);/* call pm_runtime resume */
#else
	vfe_resume_helper(dev);
#endif
}
#ifdef CONFIG_PM_RUNTIME
static int vfe_runtime_suspend(struct device *d)
{
	struct vfe_dev *dev = (struct vfe_dev *)dev_get_drvdata(d);

	vfe_print("vfe_runtime_suspend\n");
	vfe_suspend_helper(dev);

	return 0;
}
static int vfe_runtime_resume(struct device *d)
{
	struct vfe_dev *dev = (struct vfe_dev *)dev_get_drvdata(d);

	vfe_print("vfe_runtime_resume\n");
	vfe_resume_helper(dev);

	return 0;
}
static int vfe_runtime_idle(struct device *d)
{
	if (d) {
		pm_runtime_mark_last_busy(d);
		pm_request_autosuspend(d);
	} else
		vfe_err("%s, vfe device is null\n", __func__);

	return 0;
}
#endif
static int vfe_suspend(struct device *d)
{
	struct vfe_dev *dev = (struct vfe_dev *)dev_get_drvdata(d);

	vfe_print("vfe suspend\n");
	if (vfe_is_opened(dev)) {
		vfe_err("FIXME: dev %s, err happened when calling %s.", dev_name(&dev->pdev->dev), __func__);
		return -1;
	}

	return 0;
}
static int vfe_resume(struct device *d)
{
	vfe_print("vfe resume\n");

	return 0;
}

static void vfe_shutdown(struct platform_device *pdev)
{
#if defined(CONFIG_PM_RUNTIME) || defined(CONFIG_SUSPEND)
	vfe_print("Defined suspend!\n");
#else
	struct vfe_dev *dev = (struct vfe_dev *)dev_get_drvdata(&pdev->dev);
	unsigned int input_num;
	int ret = 0;
	/* close all the device power */
	for (input_num = 0; input_num < dev->dev_qty; input_num++) {
		/* update target device info and select it */
		update_ccm_info(dev, dev->ccm_cfg[input_num]);
		ret = vfe_set_sensor_power_off(dev);
		if (ret != 0) {
			vfe_err("sensor power off error at device number %d when csi close!\n", input_num);
		}
	}
#endif
	vfe_print("Vfe Shutdown!\n");
}

static const struct dev_pm_ops vfe_runtime_pm_ops = {

#ifdef CONFIG_PM_RUNTIME
	.runtime_suspend	= vfe_runtime_suspend,
	.runtime_resume	= vfe_runtime_resume,
	.runtime_idle		= vfe_runtime_idle,
#endif
	.suspend	= vfe_suspend,
	.resume	= vfe_resume,
};

static const struct of_device_id sunxi_vfe_match[] = {
	{ .compatible = "allwinner,sunxi-vfe", },
	{},
};

static struct platform_driver vfe_driver = {
	.probe    = vfe_probe,
	.remove   = vfe_remove,
	.shutdown = vfe_shutdown,
	.driver = {
		.name   = VFE_MODULE_NAME,
		.owner  = THIS_MODULE,
	.of_match_table = sunxi_vfe_match,
	.pm     = &vfe_runtime_pm_ops,
	}
};

static int __init vfe_init(void)
{
	int ret;

	vfe_print("Welcome to Video Front End driver\n");
	mutex_init(&probe_hdl_lock);
	sunxi_csi_platform_register();
	sunxi_isp_platform_register();
	sunxi_mipi_platform_register();
	sunxi_flash_platform_register();
	ret = platform_driver_register(&vfe_driver);
	if (ret) {
		vfe_err("platform driver register failed\n");
		return ret;
	}
	vfe_print("vfe_init end\n");
	return 0;
}

static void __exit vfe_exit(void)
{
	vfe_print("vfe_exit\n");
	platform_driver_unregister(&vfe_driver);
	vfe_print("platform_driver_unregister\n");
	vfe_release();
	mutex_destroy(&probe_hdl_lock);
	sunxi_csi_platform_unregister();
	sunxi_isp_platform_unregister();
	sunxi_mipi_platform_unregister();
	sunxi_flash_platform_unregister();
	vfe_print("vfe_exit end\n");
}

module_init(vfe_init);
module_exit(vfe_exit);

MODULE_AUTHOR("raymonxiu");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("Video front end driver for sunxi");
