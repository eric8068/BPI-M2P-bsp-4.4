/*
 * Allwinner SoCs hdmi driver.
 *
 * Copyright (C) 2016 Allwinner.
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include "drv_hdmi_i.h"
#include "hdmi_core.h"
#include "../disp/disp_sys_intf.h"
#include "../disp/dev_disp.h"
#include <linux/regulator/consumer.h>
#include <linux/clk-provider.h>
#include <linux/clk/sunxi.h>
#include <linux/sunxi-clk-prepare.h>
#if defined(CONFIG_EXTCON)
#include <linux/extcon.h>
#endif
#include <linux/sunxi-sid.h>

static u32 io_enable_count;

static struct semaphore *run_sem;
static struct task_struct *HDMI_task;
static struct task_struct *cec_task;
static bool hdmi_cec_support;
static char hdmi_power[25];
static char hdmi_io_regulator[25];
static bool hdmi_power_used;
static bool hdmi_io_regulator_used;
static bool hdmi_used;
static bool boot_hdmi;
#if defined(CONFIG_COMMON_CLK)
static struct clk *hdmi_clk;
static struct clk *hdmi_ddc_clk;
static struct clk *hdmi_clk_parent;
#if defined(CONFIG_ARCH_SUN8IW12)
static struct clk *hdmi_cec_clk;
#endif
#endif
static u32 power_enable_count;
static u32 io_regulator_enable_count;
static u32 clk_enable_count;
static struct mutex mlock;
#if defined(CONFIG_SND_SUNXI_SOC_SUNXI_HDMIAUDIO)
static bool audio_enable;
#endif
static bool b_hdmi_suspend;
static bool b_hdmi_suspend_pre;

static struct cdev *my_cdev;
static dev_t devid;
static struct class *hdmi_class;
hdmi_info_t ghdmi;

void hdmi_delay_ms(unsigned long ms)
{
	u32 timeout = ms*HZ/1000;

	set_current_state(TASK_INTERRUPTIBLE);
	schedule_timeout(timeout);
}

void hdmi_delay_us(unsigned long us)
{
	udelay(us);
}

unsigned int hdmi_get_soc_version(void)
{
	unsigned int version = 0;
#if defined(CONFIG_ARCH_SUN8IW7)
#if defined(SUN8IW7P1_REV_A) || defined(SUN8IW7P2_REV_B)
	unsigned int chip_ver = sunxi_get_soc_ver();

	switch (chip_ver) {
	case SUN8IW7P1_REV_A:
	case SUN8IW7P2_REV_A:
		version = 0;
		break;
	case SUN8IW7P1_REV_B:
	case SUN8IW7P2_REV_B:
		version = 1;
	}
#else
	version = 1;
#endif /*endif  SUN8IW7P1_REV_A*/
#endif
	return version;
}

static int hdmi_parse_io_config(void)
{
#ifndef CONFIG_ARCH_SUN8IW7
	disp_sys_pin_set_state("hdmi", DISP_PIN_STATE_ACTIVE);
#endif
	return 0;
}

static int hdmi_io_config(u32 bon)
{
#ifndef CONFIG_ARCH_SUN8IW7
	return disp_sys_pin_set_state(
	    "hdmi", (bon == 1) ? DISP_PIN_STATE_ACTIVE : DISP_PIN_STATE_SLEEP);
#else
	return 0;
#endif
}

#if defined(CONFIG_COMMON_CLK)
static int hdmi_clk_enable(void)
{
	int ret = 0;

	if (hdmi_clk)
		ret = clk_prepare_enable(hdmi_clk);
	if (ret != 0)
		return ret;

	if (hdmi_ddc_clk)
		ret = clk_prepare_enable(hdmi_ddc_clk);

#if defined(CONFIG_ARCH_SUN8IW12)
	if (hdmi_cec_clk)
		ret = clk_prepare_enable(hdmi_cec_clk);
#endif /*endif CONFIG_ARCH_SUN8IW12 */

	if (ret != 0)
		clk_disable(hdmi_clk);

	return ret;
}

static int hdmi_clk_disable(void)
{
	if (hdmi_clk)
		clk_disable(hdmi_clk);
	if (hdmi_ddc_clk)
		clk_disable(hdmi_ddc_clk);

#if defined(CONFIG_ARCH_SUN8IW12)
	if (hdmi_cec_clk)
		clk_disable(hdmi_cec_clk);
#endif /*endif CONFIG_ARCH_SUN8IW12 */

	return 0;
}

static int hdmi_clk_config(u32 vic)
{
	int index = 0;

	index = hdmi_core_get_video_info(vic);

	if (hdmi_clk)
		clk_set_rate(hdmi_clk, video_timing[index].pixel_clk);

	return 0;
}
#else
static int hdmi_clk_enable(void){}
static int hdmi_clk_disable(void){}
static int hdmi_clk_config(u32 vic) {}
#endif

/* hdmi_clk_enable_prepare - prepare for hdmi enable
 * if there is some other clk will affect hdmi module,
 * should enable these clk before enable hdmi
 */
int hdmi_clk_enable_prepare(void)
{
	int ret = 0;

	pr_warn("%s()L%d\n", __func__, __LINE__);
	if (hdmi_clk)
		ret = sunxi_clk_enable_prepare(hdmi_clk);
	if (ret != 0)
		return ret;

	return ret;
}

/* hdmi_clk_disable_prepare - prepare for hdmi disable
 * if there is some other clk will affect hdmi module,
 * should disable these clk after disable hdmi
 */
int hdmi_clk_disable_prepare(void)
{
	pr_warn("%s()L%d\n", __func__, __LINE__);
	if (hdmi_clk)
		sunxi_clk_disable_prepare(hdmi_clk);

	return 0;
}

unsigned int hdmi_clk_get_div(void)
{
	unsigned long rate = 1, rate_parent = 1;
	unsigned int div = 4;

	if (!hdmi_clk || !hdmi_clk_parent) {
		pr_warn("%s, get clk div fail\n", __func__);
		goto exit;
	}

	if (hdmi_clk)
		rate = clk_get_rate(hdmi_clk);
	if (hdmi_clk_parent)
		rate_parent = clk_get_rate(hdmi_clk_parent);

	if (rate != 0)
		div = rate_parent / rate;
	else
		pr_warn("%s, hdmi clk rate is ZERO!\n", __func__);

exit:
	return div;
}

#ifdef CONFIG_AW_AXP
static int hdmi_power_enable(char *name)
{
	struct regulator *regu = NULL;
	int ret = -1;

	regu = regulator_get(NULL, name);
	if (IS_ERR(regu)) {
		pr_err("%s: some error happen, fail to get regulator %s\n",
				__func__, name);
		goto exit;
	}

	/* enalbe regulator */
	ret = regulator_enable(regu);
	if (ret != 0) {
		pr_err("%s: some error happen, fail to enable regulator %s!\n",
				__func__, name);
		goto exit1;
	} else {
		hdmi_inf("suceess to enable regulator %s!\n", name);
	}

exit1:
	/* put regulater, when module exit */
	regulator_put(regu);
exit:
	return ret;
}

static int hdmi_power_disable(char *name)
{
	struct regulator *regu = NULL;
	int ret = 0;

	regu = regulator_get(NULL, name);
	if (IS_ERR(regu)) {
		hdmi_wrn("%s: some error happen, fail to get regulator %s\n",
				__func__, name);
		goto exit;
	}

	/* disalbe regulator */
	ret = regulator_disable(regu);
	if (ret != 0) {
		hdmi_wrn(
		    "%s: some error happen, fail to disable regulator %s!\n",
		    __func__, name);
		goto exit1;
	} else {
		hdmi_inf("suceess to disable regulator %s!\n", name);
	}

exit1:
	/* put regulater, when module exit */
	regulator_put(regu);
exit:
	return ret;
}
#else
static int hdmi_power_enable(char *name) {return 0; }
static int hdmi_power_disable(char *name) {return 0; }
#endif

static s32 hdmi_enable(void)
{
	hdmi_inf("[hdmi_enable]\n");

	mutex_lock(&mlock);
	if (ghdmi.bopen != 1) {
		hdmi_clk_config(ghdmi.mode);
		hdmi_core_set_video_enable(1);
		ghdmi.bopen = 1;
	}
	mutex_unlock(&mlock);
	return 0;
}

static s32 hdmi_disable(void)
{
	hdmi_inf("[hdmi_disable]\n");

	mutex_lock(&mlock);
	if (ghdmi.bopen != 0) {
		hdmi_core_set_video_enable(0);
		ghdmi.bopen = 0;
	}
	mutex_unlock(&mlock);
	return 0;
}

static struct disp_hdmi_mode hdmi_mode_tbl[] = {
	{DISP_TV_MOD_480I,                HDMI1440_480I,     },
	{DISP_TV_MOD_576I,                HDMI1440_576I,     },
	{DISP_TV_MOD_480P,                HDMI480P,          },
	{DISP_TV_MOD_576P,                HDMI576P,          },
	{DISP_TV_MOD_720P_50HZ,           HDMI720P_50,       },
	{DISP_TV_MOD_720P_60HZ,           HDMI720P_60,       },
	{DISP_TV_MOD_1080I_50HZ,          HDMI1080I_50,      },
	{DISP_TV_MOD_1080I_60HZ,          HDMI1080I_60,      },
	{DISP_TV_MOD_1080P_24HZ,          HDMI1080P_24,      },
	{DISP_TV_MOD_1080P_50HZ,          HDMI1080P_50,      },
	{DISP_TV_MOD_1080P_60HZ,          HDMI1080P_60,      },
	{DISP_TV_MOD_1080P_25HZ,          HDMI1080P_25,      },
	{DISP_TV_MOD_1080P_30HZ,          HDMI1080P_30,      },
	{DISP_TV_MOD_1080P_24HZ_3D_FP,    HDMI1080P_24_3D_FP,},
	{DISP_TV_MOD_720P_50HZ_3D_FP,     HDMI720P_50_3D_FP, },
	{DISP_TV_MOD_720P_60HZ_3D_FP,     HDMI720P_60_3D_FP, },
	{DISP_TV_MOD_3840_2160P_30HZ,     HDMI3840_2160P_30, },
	{DISP_TV_MOD_3840_2160P_25HZ,     HDMI3840_2160P_25, },
	{DISP_TV_MOD_3840_2160P_24HZ,     HDMI3840_2160P_24, },
	{DISP_TV_MOD_4096_2160P_24HZ,     HDMI4096_2160P_24, },
	{DISP_TV_MOD_1024_600P,           HDMI1024_600,      },
	{DISP_TV_MOD_1280_800P,           HDMI1280_800,      },
	{DISP_TV_MOD_800_480P,            HDMI800_480,       },
	{DISP_TV_MOD_400_1280P,           HDMI400_1280,      },
};

static u32 hdmi_get_vic(u32 mode)
{
	u32 hdmi_mode = DISP_TV_MOD_720P_50HZ;
	u32 i;
	bool find = false;

	for (i = 0; i < sizeof(hdmi_mode_tbl)/sizeof(struct disp_hdmi_mode);
			i++) {
		if (hdmi_mode_tbl[i].mode == mode) {
			hdmi_mode = hdmi_mode_tbl[i].hdmi_mode;
			find = true;
			break;
		}
	}

	if (false == find)
		pr_warn("[HDMI]can't find vic for mode(%d)\n", mode);

	return hdmi_mode;
}

static s32 hdmi_set_display_mode(u32 mode)
{
	u32 hdmi_mode;
	u32 i;
	bool find = false;

	hdmi_inf("[hdmi_set_display_mode],mode:%d\n", mode);

	for (i = 0; i < sizeof(hdmi_mode_tbl)/sizeof(struct disp_hdmi_mode);
			i++) {
		if (hdmi_mode_tbl[i].mode == (enum disp_tv_mode)mode) {
			hdmi_mode = hdmi_mode_tbl[i].hdmi_mode;
			find = true;
			break;
		}
	}

	if (find) {
		ghdmi.mode = hdmi_mode;
		return hdmi_core_set_video_mode(hdmi_mode);
	}
	hdmi_wrn("unsupported video mode %d when set display mode\n", mode);
	return -1;

}

#if defined(CONFIG_SND_SUNXI_SOC_SUNXI_HDMIAUDIO)
static s32 hdmi_audio_enable(u8 mode, u8 channel)
{
	hdmi_inf("[hdmi_audio_enable],mode:%d,ch:%d\n", mode, channel);
	mutex_lock(&mlock);
	audio_enable = mode;
	mutex_unlock(&mlock);
	return hdmi_core_set_audio_enable(audio_enable);
}

static s32 hdmi_set_audio_para(hdmi_audio_t *audio_para)
{
	hdmi_inf("[hdmi_set_audio_para]\n");
	return hdmi_core_audio_config(audio_para);
}
#endif

static s32 hdmi_mode_support(u32 mode)
{
	u32 hdmi_mode;
	u32 i;
	bool find = false;

	for (i = 0; i < sizeof(hdmi_mode_tbl)/sizeof(struct disp_hdmi_mode);
			i++) {
		if (hdmi_mode_tbl[i].mode == (enum disp_tv_mode)mode) {
			hdmi_mode = hdmi_mode_tbl[i].hdmi_mode;
			find = true;
			break;
		}
	}

	if (find)
		return hdmi_core_mode_support(hdmi_mode);
	else
		return 0;
}

static s32 hdmi_get_HPD_status(void)
{
	return hdmi_core_hpd_check();
}

static s32 hdmi_get_hdcp_enable(void)
{
	return hdmi_core_get_hdcp_enable();
}

static s32 hdmi_get_video_timming_info(struct disp_video_timings **video_info)
{
	struct disp_video_timings *info;
	int ret = -1;
	int i, list_num;

	info = video_timing;
	list_num = hdmi_core_get_list_num();
	for (i = 0; i < list_num; i++) {
		if (info->vic == ghdmi.mode) {
			*video_info = info;
			ret = 0;
			break;
		}
		info++;
	}
	return ret;
}

static s32 hdmi_get_input_csc(void)
{
	return hdmi_core_get_csc_type();
}

static int hdmi_run_thread(void *parg)
{
	while (1) {
		if (kthread_should_stop())
			break;

		mutex_lock(&mlock);
		if (false == b_hdmi_suspend) {
			/* normal state */
			b_hdmi_suspend_pre = b_hdmi_suspend;
			mutex_unlock(&mlock);
			hdmi_core_loop();

			if (false == b_hdmi_suspend) {
				if (hdmi_get_hdcp_enable() == 1)
					hdmi_delay_ms(100);
				else
					hdmi_delay_ms(200);
			}
		} else {
			/* suspend state */
			if (false == b_hdmi_suspend_pre) {
				/* first time after enter suspend state */
				/* hdmi_core_enter_lp(); */
			}
			b_hdmi_suspend_pre = b_hdmi_suspend;
			mutex_unlock(&mlock);
		}
	}

	return 0;
}

void cec_msg_sent(char *buf)
{
	char *envp[2];

	envp[0] = buf;
	envp[1] = NULL;
	kobject_uevent_env(&ghdmi.dev->kobj, KOBJ_CHANGE, envp);
}

#define CEC_BUF_SIZE 32
static int cec_thread(void *parg)
{
	int ret = 0;
	char buf[CEC_BUF_SIZE];
	unsigned char msg;

	while (1) {
		if (kthread_should_stop())
			break;

		mutex_lock(&mlock);
		ret = -1;
		if (false == b_hdmi_suspend)
			ret = hdmi_core_cec_get_simple_msg(&msg);
		mutex_unlock(&mlock);
		if (ret == 0) {
			memset(buf, 0, CEC_BUF_SIZE);
			snprintf(buf, sizeof(buf), "CEC_MSG=0x%x", msg);
			cec_msg_sent(buf);
		}
		hdmi_delay_ms(10);
	}

	return 0;
}

#if defined(CONFIG_EXTCON)
static const unsigned int hdmi_cable[] = {
	EXTCON_DISP_HDMI,
	EXTCON_NONE,
};
static struct extcon_dev extcon_hdmi = {
	.name = "hdmi",
};

/* s32 disp_set_hdmi_detect(bool hpd); */
static void hdmi_report_hpd_work(struct work_struct *work)
{
	if (hdmi_get_HPD_status()) {
		extcon_set_state(&extcon_hdmi, STATUE_OPEN);
		disp_set_hdmi_detect(1);
		hdmi_inf("switch_set_state 1\n");
	} else {
		extcon_set_state(&extcon_hdmi, STATUE_CLOSE);
		disp_set_hdmi_detect(0);
		hdmi_inf("switch_set_state 0\n");
	}
}

s32 hdmi_hpd_state(u32 state)
{
	if (state == 0)
		extcon_set_state(&extcon_hdmi, STATUE_CLOSE);
	else
		extcon_set_state(&extcon_hdmi, STATUE_OPEN);

	return 0;
}
#else
static void hdmi_report_hpd_work(struct work_struct *work)
{
}

s32 hdmi_hpd_state(u32 state)
{
	return 0;
}
#endif
/**
 * hdmi_hpd_report - report hdmi hot plug state to user space
 * @hotplug:	0: hdmi plug out;   1:hdmi plug in
 *
 * always return success.
 */
s32 hdmi_hpd_event(void)
{
	schedule_work(&ghdmi.hpd_work);
	return 0;
}

static s32 hdmi_suspend(void)
{
	hdmi_core_update_detect_time(0);
	if (hdmi_used && (false == b_hdmi_suspend)) {
		if (HDMI_task) {
			kthread_stop(HDMI_task);
			HDMI_task = NULL;
		}
		if (hdmi_cec_support && cec_task) {
			kthread_stop(cec_task);
			cec_task = NULL;
		}
		mutex_lock(&mlock);
		b_hdmi_suspend = true;
		hdmi_core_enter_lp();
		if (clk_enable_count != 0) {
			hdmi_clk_disable();
			clk_enable_count--;
		}
		if (io_enable_count != 0) {
			hdmi_io_config(0);
			io_enable_count--;
		}
		if ((hdmi_power_used) && (power_enable_count != 0)) {
			hdmi_power_disable(hdmi_power);
			power_enable_count--;
		}
		if (hdmi_io_regulator_used && io_regulator_enable_count) {
			hdmi_power_disable(hdmi_io_regulator);
			--io_regulator_enable_count;
		}
		mutex_unlock(&mlock);
		pr_info("[HDMI]hdmi suspend\n");
	}

	return 0;
}

extern int sunxi_smc_refresh_hdcp(void);
static s32 hdmi_resume(void)
{
	int ret;
	#ifdef CONFIG_SUNXI_SMC
	/*the hdcp keysram is powered off when main cpu suspend,
	* so refresh hdcp key here, it is serial call.
	*/
	if (sunxi_smc_refresh_hdcp()) {
			pr_warn("refresh hdcp key failed!");
			return -1;
	}
	#endif

	mutex_lock(&mlock);
	if (hdmi_used && (true == b_hdmi_suspend)) {
		/* normal state */
		if (clk_enable_count == 0) {
			ret = hdmi_clk_enable();
			if (ret == 0)
				clk_enable_count++;
			else {
				pr_warn("fail to enable hdmi's clock\n");
				goto exit;
			}
		}
		if (hdmi_io_regulator_used && !io_regulator_enable_count) {
			hdmi_power_enable(hdmi_io_regulator);
			++io_regulator_enable_count;
		}
		if ((hdmi_power_used) && (power_enable_count == 0)) {
			hdmi_power_enable(hdmi_power);
			power_enable_count++;
		}
		if (io_enable_count == 0) {
			hdmi_io_config(1);
			io_enable_count++;
		}
		/* first time after exit suspend state */
		hdmi_core_exit_lp();

		HDMI_task = kthread_create(hdmi_run_thread,
				(void *)0, "hdmi proc");
		if (IS_ERR(HDMI_task)) {
			s32 err = 0;

			pr_warn("Unable to start kernel thread %s.\n\n",
				"hdmi proc");
			err = PTR_ERR(HDMI_task);
			HDMI_task = NULL;
		} else
			wake_up_process(HDMI_task);

		if (hdmi_cec_support) {
			cec_task = kthread_create(cec_thread, (void *)0,
				"cec proc");
			if (IS_ERR(cec_task)) {
				s32 err = 0;

				pr_warn("Unable to start kernel thread %s.\n\n",
					"cec proc");
				err = PTR_ERR(cec_task);
				cec_task = NULL;
			} else
				wake_up_process(cec_task);
		}

		pr_info("[HDMI]hdmi resume\n");
	}

exit:
	mutex_unlock(&mlock);

	hdmi_core_update_detect_time(200);/* 200ms */
	b_hdmi_suspend = false;

	return  0;
}

#if defined(CONFIG_SND_SUNXI_SOC_SUNXI_HDMIAUDIO)
extern void audio_set_hdmi_func(__audio_hdmi_func *hdmi_func);
#endif
/* extern s32 disp_set_hdmi_func(struct disp_device_func *func); */
/* extern unsigned int disp_boot_para_parse(const char *name); */

s32 hdmi_init(struct platform_device *pdev)
{
#if defined(CONFIG_SND_SUNXI_SOC_SUNXI_HDMIAUDIO)
	__audio_hdmi_func audio_func;
#if defined(CONFIG_SND_SUNXI_SOC_AUDIOHUB_INTERFACE)
	__audio_hdmi_func audio_func_muti;
#endif
#endif
	unsigned int value, output_type0, output_mode0;
	unsigned int output_type1, output_mode1;
	struct disp_device_func disp_func;
	int ret = 0;
	uintptr_t reg_base;

	hdmi_used = 0;
	b_hdmi_suspend_pre = b_hdmi_suspend = false;
	hdmi_power_used = 0;
	hdmi_used = 1;

	/*  parse boot para */
	value = disp_boot_para_parse("boot_disp");
	output_type0 = (value >> 8) & 0xff;
	output_mode0 = (value) & 0xff;

	output_type1 = (value >> 24) & 0xff;
	output_mode1 = (value >> 16) & 0xff;
	if ((output_type0 == DISP_OUTPUT_TYPE_HDMI) ||
		(output_type1 == DISP_OUTPUT_TYPE_HDMI)) {
		boot_hdmi = true;
		ghdmi.bopen = 1;
		ghdmi.mode = (output_type0 ==
			DISP_OUTPUT_TYPE_HDMI) ? output_mode0:output_mode1;
		ghdmi.mode = hdmi_get_vic(ghdmi.mode);
	}
	/* iomap */
	reg_base = (uintptr_t __force)of_iomap(pdev->dev.of_node, 0);
	if (reg_base == 0) {
		dev_err(&pdev->dev, "unable to map hdmi registers\n");
		ret = -EINVAL;
		goto err_iomap;
	}
	hdmi_core_set_base_addr(reg_base);

	/* get clk */
	hdmi_clk = of_clk_get(pdev->dev.of_node, 0);
	if (IS_ERR(hdmi_clk)) {
		dev_err(&pdev->dev, "fail to get clk for hdmi\n");
		goto err_clk_get;
	}

	hdmi_clk_parent = clk_get_parent(hdmi_clk);
	if (IS_ERR(hdmi_clk_parent)) {
		dev_err(&pdev->dev, "fail to get clk parent for hdmi\n");
		goto err_power;
	}

	clk_enable_count = __clk_get_enable_count(hdmi_clk);
	hdmi_ddc_clk = of_clk_get(pdev->dev.of_node, 1);
	if (IS_ERR(hdmi_ddc_clk)) {
		dev_err(&pdev->dev, "fail to get clk for hdmi ddc\n");
		goto err_power;
	}

#if defined(CONFIG_ARCH_SUN8IW12)
	hdmi_cec_clk = of_clk_get(pdev->dev.of_node, 2);
	if (IS_ERR_OR_NULL(hdmi_cec_clk)) {
		dev_err(&pdev->dev, "fail to get hdmi_cec_clk\n");
		goto err_power;
	}
#endif /*endif CONFIG_ARCH_SUN8IW12 */
	/* parse io config */
	hdmi_parse_io_config();
	mutex_init(&mlock);

	if (io_enable_count == 0) {
		hdmi_io_config(1);
		io_enable_count++;
	}
	mutex_lock(&mlock);
	if (clk_enable_count == 0) {
		hdmi_wrn("hdmi_clk_enable\n");
		ret = hdmi_clk_enable();
		clk_enable_count++;
	}
	mutex_unlock(&mlock);
	if (ret != 0) {
		clk_enable_count--;
		dev_err(&pdev->dev, "fail to enable hdmi clk\n");
		goto err_clk_enable;
	}

	INIT_WORK(&ghdmi.hpd_work, hdmi_report_hpd_work);

#if defined(CONFIG_EXTCON)
	extcon_hdmi.supported_cable = hdmi_cable;
	extcon_dev_register_attr(&extcon_hdmi, &pdev->dev);
#endif

#ifndef CONFIG_ARCH_SUN8IW7
	ret = disp_sys_script_get_item("hdmi", "hdmi_io_regulator",
				       (int *)hdmi_io_regulator, 2);
	if (ret == 2) {
		mutex_lock(&mlock);
		ret = hdmi_power_enable(hdmi_io_regulator);
		mutex_unlock(&mlock);
		if (ret) {
			dev_err(&pdev->dev, "fail to enable hdmi io power %s\n",
				hdmi_io_regulator);
		} else {
			++io_regulator_enable_count;
			hdmi_io_regulator_used = 1;
		}
	}


	ret = disp_sys_script_get_item("hdmi", "hdmi_power",
			(int *)hdmi_power, 2);
	if (ret == 2) {
		hdmi_power_used = 1;
		if (hdmi_power_used) {
			pr_info("[HDMI] power %s\n", hdmi_power);
			mutex_lock(&mlock);
			ret = hdmi_power_enable(hdmi_power);
			power_enable_count++;
			mutex_unlock(&mlock);
			if (ret != 0) {
				power_enable_count--;
				dev_err(&pdev->dev,
				"fail to enable hdmi power %s\n", hdmi_power);
				goto err_power;
			}
		}
	}
#endif


	ret = disp_sys_script_get_item("hdmi", "hdmi_cts_compatibility",
					&value, 1);
	if (ret == 1)
		hdmi_core_set_cts_enable(value);
	ret = disp_sys_script_get_item("hdmi", "hdmi_hdcp_enable", &value, 1);
	if (ret == 1)
		hdmi_core_set_hdcp_enable(value);

	ret = disp_sys_script_get_item("hdmi", "hdmi_hpd_mask", &value, 1);
	if (ret == 1)
		hdmi_hpd_mask = value;

	ret = disp_sys_script_get_item("hdmi", "hdmi_cec_support", &value, 1);
	if ((ret == 1) && (value == 1))
		hdmi_cec_support = true;
	hdmi_core_cec_enable(hdmi_cec_support);
	pr_info("[HDMI] cec support = %d\n", hdmi_cec_support);

	hdmi_core_initial(boot_hdmi);

	run_sem = kmalloc(sizeof(struct semaphore), GFP_KERNEL | __GFP_ZERO);
	if (!run_sem) {
		dev_err(&pdev->dev, "fail to kmalloc memory for run_sem\n");
		goto err_sem;
	}
	sema_init((struct semaphore *)run_sem, 0);

	HDMI_task = kthread_create(hdmi_run_thread, (void *)0, "hdmi proc");
	if (IS_ERR(HDMI_task)) {
		s32 err = 0;

		dev_err(&pdev->dev, "Unable to start kernel thread %s.\n\n",
			"hdmi proc");
		err = PTR_ERR(HDMI_task);
		HDMI_task = NULL;
		goto err_thread;
	}
	wake_up_process(HDMI_task);

	if (hdmi_cec_support) {
		cec_task = kthread_create(cec_thread, (void *)0, "cec proc");
		if (IS_ERR(cec_task)) {
			s32 err = 0;

			dev_err(&pdev->dev,
				"Unable to start kernel thread %s.\n\n",
				"cec proc");
			err = PTR_ERR(cec_task);
			cec_task = NULL;
			goto err_thread;
		}
		wake_up_process(cec_task);
	}

#if defined(CONFIG_SND_SUNXI_SOC_SUNXI_HDMIAUDIO)
	audio_func.hdmi_audio_enable = hdmi_audio_enable;
	audio_func.hdmi_set_audio_para = hdmi_set_audio_para;
	audio_set_hdmi_func(&audio_func);
#if defined(CONFIG_SND_SUNXI_SOC_AUDIOHUB_INTERFACE)
	audio_func_muti.hdmi_audio_enable = hdmi_audio_enable;
	audio_func_muti.hdmi_set_audio_para = hdmi_set_audio_para;
	audio_set_muti_hdmi_func(&audio_func_muti);
#endif
#endif
	memset(&disp_func, 0, sizeof(struct disp_device_func));
	disp_func.enable = hdmi_enable;
	disp_func.disable = hdmi_disable;
	disp_func.set_mode = hdmi_set_display_mode;
	disp_func.mode_support = hdmi_mode_support;
	disp_func.get_HPD_status = hdmi_get_HPD_status;
	disp_func.get_input_csc = hdmi_get_input_csc;
	disp_func.get_video_timing_info = hdmi_get_video_timming_info;
	disp_func.suspend = hdmi_suspend;
	disp_func.resume = hdmi_resume;
	disp_set_hdmi_func(&disp_func);

	return 0;

err_thread:
	kfree(run_sem);
err_sem:
	hdmi_power_disable(hdmi_power);
err_power:
	hdmi_clk_disable();
err_clk_enable:
err_clk_get:
	iounmap((char __iomem *)reg_base);
err_iomap:
	return -1;
}

s32 hdmi_exit(void)
{
	if (hdmi_used) {
		hdmi_core_exit();

		kfree(run_sem);

		run_sem = NULL;
		if (HDMI_task) {
			kthread_stop(HDMI_task);
			HDMI_task = NULL;
		}

		if (hdmi_cec_support && cec_task) {
			kthread_stop(cec_task);
			cec_task = NULL;
		}

		if ((hdmi_power_used == 1) && (power_enable_count != 0))
			hdmi_power_disable(hdmi_power);

		if (hdmi_io_regulator_used && io_regulator_enable_count)
			hdmi_power_disable(hdmi_io_regulator);
		if (clk_enable_count != 0) {
			hdmi_clk_disable();
			clk_enable_count--;
		}
	}

	return 0;
}

#ifndef CONFIG_OF
static struct resource hdmi_resource[1] = {

	[0] = {
		.start = 0x01c16000,
		.end   = 0x01c165ff,
		.flags = IORESOURCE_MEM,
	},
};

static struct platform_device hdmi_device = {

	.name		   = "hdmi",
	.id				= -1,
	.num_resources  = ARRAY_SIZE(hdmi_resource),
	.resource		= hdmi_resource,
	.dev			= {}
};
#else
static const struct of_device_id sunxi_hdmi_match[] = {
	{ .compatible = "allwinner,sunxi-hdmi", },
	{},
};
#endif

static ssize_t hdmi_debug_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "debug=%s\n", hdmi_print?"on" : "off");
}

static ssize_t hdmi_debug_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	if (count < 1)
		return -EINVAL;

	if (strncasecmp(buf, "on", 2) == 0 || strncasecmp(buf, "1", 1) == 0)
		hdmi_print = 1;
	else if (strncasecmp(buf, "off", 3) == 0 ||
			strncasecmp(buf, "0", 1) == 0)
		hdmi_print = 0;
	else
		return -EINVAL;

	return count;
}

#if defined(HDMI_ENABLE_DUMP_WRITE)

static ssize_t hdmi_dump_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{

	static long long val;
	long long reg, num, i = 0;
	unsigned char value_r[128];
	s32 ret = -1;

	ret = kstrtoll(buf, 0, &val);
	reg = (val >> 8);
	num = val & 0xff;
	pr_alert("\n");
	pr_alert("read:start add:0x%llx,count:0x%llx\n", reg, num);
	do {
		bsp_hdmi_read(reg, &value_r[i]);
		pr_alert("0x%llx: 0x%04x ", reg, value_r[i]);
		reg += 1;
		i++;
		if (i == num)
			pr_alert("\n");
		if (i % 4 == 0)
			pr_alert("\n");
	} while (i < num);

	return count;
	return count;
}

static ssize_t hdmi_write_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	static long long val;
	long long reg;
	long long value_w;
	s32 ret = -1;

	ret = kstrtoll(buf, 0, &val);
	if (ret != 0) {
		pr_alert("convert string fail!\n");
		return 0;
	}

	reg = (val >> 16);
	value_w =  val & 0xFFFF;
	bsp_hdmi_write(reg, value_w);
	pr_alert("write 0x%llx to reg:0x%llx\n", value_w, reg);
	return count;
}

static ssize_t hdmi_dump_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	pr_alert("echo reg|count > dump\n");
	pr_alert("eg read star address=0x0006,count 0x10:echo 0x610 > dump\n");
	pr_alert(
	    "eg read star address=0x2000,count 0x10:echo 0x200010 > dump\n");

	return 0;
}

static ssize_t hdmi_write_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	pr_alert("echo reg|val > write\n");
	pr_alert(
	    "eg write value:0x13fe to address:0x0004 :echo 0x413fe > write\n");
	pr_alert(
	    "eg write value:0x6 to address:0x2000 :echo 0x20000006 > write\n");

	return 0;
}

#endif /*endif HDMI_ENABLE_DUMP_WRITE */
static DEVICE_ATTR(debug, S_IRUGO|S_IWUSR|S_IWGRP,
		hdmi_debug_show, hdmi_debug_store);

#if defined(HDMI_ENABLE_DUMP_WRITE)
static DEVICE_ATTR(dump, S_IRUGO|S_IWUSR|S_IWGRP,
		hdmi_dump_show, hdmi_dump_store);

static DEVICE_ATTR(write, S_IRUGO|S_IWUSR|S_IWGRP,
		hdmi_write_show, hdmi_write_store);
#endif

/* s32 hdmi_hpd_state(u32 state); */
static ssize_t hdmi_state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "nothing\n");
}

static ssize_t hdmi_state_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	if (count < 1)
		return -EINVAL;

	if (strncasecmp(buf, "1", 1) == 0)
		hdmi_hpd_state(1);
	else
		hdmi_hpd_state(0);

	return count;
}

static DEVICE_ATTR(state, S_IRUGO|S_IWUSR|S_IWGRP,
		hdmi_state_show, hdmi_state_store);

static ssize_t hdmi_rgb_only_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "rgb_only=%s\n", rgb_only?"on" : "off");
}

static ssize_t hdmi_rgb_only_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	if (count < 1)
		return -EINVAL;

	if (strncasecmp(buf, "on", 2) == 0 || strncasecmp(buf, "1", 1) == 0)
		rgb_only = 1;
	else if (strncasecmp(buf, "off", 3) == 0 ||
			strncasecmp(buf, "0", 1) == 0)
		rgb_only = 0;
	else
		return -EINVAL;

	return count;
}

static DEVICE_ATTR(rgb_only, S_IRUGO|S_IWUSR|S_IWGRP,
		hdmi_rgb_only_show, hdmi_rgb_only_store);


static ssize_t hdmi_hpd_mask_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%x\n", hdmi_hpd_mask);
}

static ssize_t hdmi_hpd_mask_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	int err;
	unsigned long val;

	if (count < 1)
		return -EINVAL;

	err = kstrtoul(buf, 16, &val);
	if (err) {
		pr_err("Invalid size\n");
		return err;
	}

	pr_info("val=0x%x\n", (u32)val);
	hdmi_hpd_mask = val;

	return count;
}

static DEVICE_ATTR(hpd_mask, S_IRUGO|S_IWUSR|S_IWGRP,
		hdmi_hpd_mask_show, hdmi_hpd_mask_store);

static ssize_t hdmi_edid_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	void *pedid = (void *)hdmi_edid_get_data();

	memcpy(buf, pedid, HDMI_EDID_LEN);
	return HDMI_EDID_LEN;
}

static ssize_t hdmi_edid_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	return count;
}

static DEVICE_ATTR(edid, S_IRUGO|S_IWUSR|S_IWGRP,
		hdmi_edid_show, hdmi_edid_store);

static ssize_t hdmi_hdcp_enable_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", hdmi_core_get_hdcp_enable());
}

static ssize_t hdmi_hdcp_enable_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	if (count < 1)
		return -EINVAL;

	if (strncasecmp(buf, "1", 1) == 0) {
		if (hdmi_core_get_hdcp_enable() != 1)
			hdmi_core_set_hdcp_enable(1);
	} else {
		if (hdmi_core_get_hdcp_enable() != 0)
			hdmi_core_set_hdcp_enable(0);
	}

	return count;
}

static DEVICE_ATTR(hdcp_enable, S_IRUGO|S_IWUSR|S_IWGRP,
		hdmi_hdcp_enable_show, hdmi_hdcp_enable_store);

static int hdmi_probe(struct platform_device *pdev)
{
	hdmi_inf("hdmi_probe call\n");
	memset(&ghdmi, 0, sizeof(hdmi_info_t));
	ghdmi.dev = &pdev->dev;
	hdmi_init(pdev);
	return 0;
}


static int hdmi_remove(struct platform_device *pdev)
{
	hdmi_inf("hdmi_remove call\n");
	hdmi_exit();
	return 0;
}

static struct platform_driver hdmi_driver = {

	.probe	  = hdmi_probe,
	.remove	 = hdmi_remove,
	.driver = {

		.name   = "hdmi",
		.owner  = THIS_MODULE,
		.of_match_table = sunxi_hdmi_match,
	},
};

static int hdmi_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int hdmi_release(struct inode *inode, struct file *file)
{
	return 0;
}


static ssize_t hdmi_read(struct file *file, char __user *buf,
			size_t count, loff_t *ppos)
{
	return -EINVAL;
}

static ssize_t hdmi_write(struct file *file, const char __user *buf,
			size_t count, loff_t *ppos)
{
	return -EINVAL;
}

static int hdmi_mmap(struct file *file, struct vm_area_struct *vma)
{
	return 0;
}

static long hdmi_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return 0;
}


static const struct file_operations hdmi_fops = {

	.owner		= THIS_MODULE,
	.open		= hdmi_open,
	.release	= hdmi_release,
	.write	  = hdmi_write,
	.read		= hdmi_read,
	.unlocked_ioctl	= hdmi_ioctl,
	.mmap	   = hdmi_mmap,
};

static struct attribute *hdmi_attributes[] = {

	&dev_attr_debug.attr,
	&dev_attr_state.attr,
	&dev_attr_rgb_only.attr,
	&dev_attr_hpd_mask.attr,
	&dev_attr_edid.attr,
	&dev_attr_hdcp_enable.attr,
#if defined(HDMI_ENABLE_DUMP_WRITE)
	&dev_attr_dump.attr,
	&dev_attr_write.attr,
#endif
	NULL
};

static struct attribute_group hdmi_attribute_group = {
	.name = "attr",
	.attrs = hdmi_attributes
};

static int __init hdmi_module_init(void)
{
	int ret = 0, err;

	alloc_chrdev_region(&devid, 0, 1, "hdmi");
	my_cdev = cdev_alloc();
	cdev_init(my_cdev, &hdmi_fops);
	my_cdev->owner = THIS_MODULE;
	err = cdev_add(my_cdev, devid, 1);
	if (err) {
		hdmi_wrn("cdev_add fail.\n");
		return -1;
	}

	hdmi_class = class_create(THIS_MODULE, "hdmi");
	if (IS_ERR(hdmi_class)) {
		hdmi_wrn("class_create fail\n");
		return -1;
	}

	ghdmi.dev = device_create(hdmi_class, NULL, devid, NULL, "hdmi");

	ret = sysfs_create_group(&ghdmi.dev->kobj, &hdmi_attribute_group);

#ifndef CONFIG_OF
	ret = platform_device_register(&hdmi_device);
#endif
	if (ret == 0)
		ret = platform_driver_register(&hdmi_driver);

	hdmi_inf("hdmi_module_init\n");
	return ret;
}

static void __exit hdmi_module_exit(void)
{
	hdmi_inf("hdmi_module_exit\n");
	platform_driver_unregister(&hdmi_driver);
#ifndef CONFIG_OF
	platform_device_unregister(&hdmi_device);
#endif
	class_destroy(hdmi_class);
	cdev_del(my_cdev);
}

late_initcall(hdmi_module_init);
module_exit(hdmi_module_exit);

MODULE_AUTHOR("tyle");
MODULE_DESCRIPTION("hdmi driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:hdmi");
