/************************************************************************************
 ** File: - SDM660.LA.1.0\android\vendor\oppo_app\fingerprints_hal\drivers\goodix_fp\gf_spi.c
 ** VENDOR_EDIT
 ** Copyright (C), 2008-2017, OPPO Mobile Comm Corp., Ltd
 **
 ** Description:
 **      goodix fingerprint kernel device driver
 **
 ** Version: 1.0
 ** Date created: 16:20:11,12/07/2017
 ** Author: Ziqing.guo@Prd.BaseDrv
 ** TAG: BSP.Fingerprint.Basic
 **
 ** --------------------------- Revision History: --------------------------------
 **  <author>        <data>          <desc>
 **  Ziqing.guo      2017/07/12      create the file for goodix 5288
 **  Ziqing.guo      2017/08/29      fix the problem of failure after restarting fingerprintd
 **  Ziqing.guo      2017/08/31      add goodix 3268,5288
 **  Ziqing.guo      2017/09/11      add gf_cmd_wakelock
 **  Ran.Chen        2017/11/30      add vreg_step for goodix_fp
 **  Ran.Chen        2017/12/07      remove power_off in release for Power supply timing
 **  Ran.Chen        2018/01/29      modify for fp_id, Code refactoring
 **  Ran.Chen        2018/11/27      remove define MSM_DRM_ONSCREENFINGERPRINT_EVENT
 **  Ran.Chen        2018/12/15      modify for power off in ftm mode (for SDM855)
 **  Ran.Chen        2019/03/17      remove power off in ftm mode (for SDM855)
 **  Bangxiong.Wu    2019/05/10      add for SM7150 (MSM_19031 MSM_19331)
 **  Ran.Chen        2019/05/09      add for GF_IOC_CLEAN_TOUCH_FLAG
 ************************************************************************************/
#define pr_fmt(fmt)    KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/input.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/compat.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/of_gpio.h>
#include <linux/timer.h>
#include <linux/notifier.h>
#include <linux/msm_drm_notify.h>
#include <linux/pm_qos.h>
#include <linux/cpufreq.h>
#include "gf_spi.h"
#include "../oppo_fp_common/oppo_fp_common.h"
#if defined(CONFIG_OPPO_FINGERPRINT_GOODIX_SPI)
#include <linux/spi/spi.h>
#include <linux/spi/spidev.h>
#elif defined(CONFIG_OPPO_FINGERPRINT_GOODIX_PLATFORM)
#include <linux/platform_device.h>
#endif
#include <soc/oppo/boot_mode.h>

#if ((defined CONFIG_MSM_855) || (defined CONFIG_MSM_7150))
#include <linux/uaccess.h>
#endif

#define VER_MAJOR   1
#define VER_MINOR   2
#define PATCH_LEVEL 9

#define WAKELOCK_HOLD_TIME 500 /* in ms */
#define SENDCMD_WAKELOCK_HOLD_TIME 1000 /* in ms */

#define GF_SPIDEV_NAME      "goodix,goodix_fp"
/*device name after register in charater*/
#define GF_DEV_NAME         "goodix_fp"

#define GF_CHRDEV_NAME      "goodix_fp_spi"
#define GF_CLASS_NAME       "goodix_fp"

#define GF_MAX_DEVS         32	/* ... up to 256 */

static struct fp_underscreen_info fp_tpinfo;
static unsigned int lasttouchmode = 0;

static int gf_dev_major;

static DECLARE_BITMAP(minors, GF_MAX_DEVS);
static LIST_HEAD(device_list);
static DEFINE_MUTEX(device_list_lock);
static struct wakeup_source fp_wakelock;
static struct wakeup_source gf_cmd_wakelock;
static struct gf_dev gf;


static void gf_enable_irq(struct gf_dev *gf_dev)
{
    if (!gf_dev->irq_enabled) {
        enable_irq(gf_dev->irq);
        gf_dev->irq_enabled = 1;
    } else {
        pr_warn("IRQ has been enabled.\n");
    }
}

static void gf_disable_irq(struct gf_dev *gf_dev)
{
    if (gf_dev->irq_enabled) {
        gf_dev->irq_enabled = 0;
        disable_irq(gf_dev->irq);
    } else {
        pr_warn("IRQ has been disabled.\n");
    }
}

#ifdef CONFIG_OPPO_FINGERPRINT_GOODIX_CLK_CTRL
static long spi_clk_max_rate(struct clk *clk, unsigned long rate)
{
    long lowest_available, nearest_low, step_size, cur;
    long step_direction = -1;
    long guess = rate;
    int max_steps = 10;

    cur = clk_round_rate(clk, rate);
    if (cur == rate)
        return rate;

    /* if we got here then: cur > rate */
    lowest_available = clk_round_rate(clk, 0);
    if (lowest_available > rate)
        return -EINVAL;

    step_size = (rate - lowest_available) >> 1;
    nearest_low = lowest_available;

    while (max_steps-- && step_size) {
        guess += step_size * step_direction;
        cur = clk_round_rate(clk, guess);

        if ((cur < rate) && (cur > nearest_low))
            nearest_low = cur;

        /*
         * if we stepped too far, then start stepping in the other
         * direction with half the step size
         */
        if (((cur > rate) && (step_direction > 0))
                || ((cur < rate) && (step_direction < 0))) {
            step_direction = -step_direction;
            step_size >>= 1;
        }
    }
    return nearest_low;
}

static void spi_clock_set(struct gf_dev *gf_dev, int speed)
{
    long rate;
    int rc;

    rate = spi_clk_max_rate(gf_dev->core_clk, speed);
    if (rate < 0) {
        pr_info("%s: no match found for requested clock frequency:%d",
                __func__, speed);
        return;
    }

    rc = clk_set_rate(gf_dev->core_clk, rate);
}

static int gfspi_ioctl_clk_init(struct gf_dev *data)
{
    data->clk_enabled = 0;
    data->core_clk = clk_get(data->dev, "core_clk");
    if (IS_ERR_OR_NULL(data->core_clk)) {
        pr_err("%s: fail to get core_clk\n", __func__);
        return -EPERM;
    }
    data->iface_clk = clk_get(data->dev, "iface_clk");
    if (IS_ERR_OR_NULL(data->iface_clk)) {
        pr_err("%s: fail to get iface_clk\n", __func__);
        clk_put(data->core_clk);
        data->core_clk = NULL;
        return -ENOENT;
    }
    return 0;
}

static int gfspi_ioctl_clk_enable(struct gf_dev *data)
{
    int err;

    if (data->clk_enabled)
        return 0;

    err = clk_prepare_enable(data->core_clk);
    if (err) {
        pr_debug("%s: fail to enable core_clk\n", __func__);
        return -EPERM;
    }

    err = clk_prepare_enable(data->iface_clk);
    if (err) {
        pr_debug("%s: fail to enable iface_clk\n", __func__);
        clk_disable_unprepare(data->core_clk);
        return -ENOENT;
    }

    data->clk_enabled = 1;

    return 0;
}

static int gfspi_ioctl_clk_disable(struct gf_dev *data)
{
    if (!data->clk_enabled)
        return 0;

    clk_disable_unprepare(data->core_clk);
    clk_disable_unprepare(data->iface_clk);
    data->clk_enabled = 0;

    return 0;
}

static int gfspi_ioctl_clk_uninit(struct gf_dev *data)
{
    if (data->clk_enabled)
        gfspi_ioctl_clk_disable(data);

    if (!IS_ERR_OR_NULL(data->core_clk)) {
        clk_put(data->core_clk);
        data->core_clk = NULL;
    }

    if (!IS_ERR_OR_NULL(data->iface_clk)) {
        clk_put(data->iface_clk);
        data->iface_clk = NULL;
    }

    return 0;
}
#endif

static irqreturn_t gf_irq(int irq, void *handle)
{
    char msg = GF_NET_EVENT_IRQ;
    __pm_wakeup_event(&fp_wakelock, msecs_to_jiffies(WAKELOCK_HOLD_TIME));
    gf_sendnlmsg(&msg);
#if defined (GF_FASYNC)
    struct gf_dev *gf_dev = &gf;
    if (gf_dev->async) {
        kill_fasync(&gf_dev->async, SIGIO, POLL_IN);
    }
#endif

    return IRQ_HANDLED;
}

static int irq_setup(struct gf_dev *gf_dev)
{
    int status;

    gf_dev->irq = gpio_to_irq(gf_dev->irq_gpio);
    status = request_threaded_irq(gf_dev->irq, NULL, gf_irq,
            IRQF_TRIGGER_RISING | IRQF_ONESHOT, "gf", gf_dev);

    if (status) {
        pr_err("failed to request IRQ:%d\n", gf_dev->irq);
        return status;
    }
    enable_irq_wake(gf_dev->irq);
    gf_dev->irq_enabled = 1;

    return status;
}

static void irq_cleanup(struct gf_dev *gf_dev)
{
    gf_dev->irq_enabled = 0;
    disable_irq(gf_dev->irq);
    disable_irq_wake(gf_dev->irq);
    free_irq(gf_dev->irq, gf_dev);//need modify
}

static long gf_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct gf_dev *gf_dev = &gf;
    int retval = 0;
    u8 netlink_route = NETLINK_TEST;
    struct gf_ioc_chip_info info;

    if (_IOC_TYPE(cmd) != GF_IOC_MAGIC) {
        return -ENODEV;
    }

    if (_IOC_DIR(cmd) & _IOC_READ) {
        retval = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
    } else if (_IOC_DIR(cmd) & _IOC_WRITE) {
        retval = !access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));
    }
    if (retval) {
        return -EFAULT;
    }

    if (gf_dev->device_available == 0) {
        switch (cmd) {
            case GF_IOC_ENABLE_POWER:
                pr_debug("power cmd\n");
                break;
            case GF_IOC_DISABLE_POWER:
                pr_debug("power cmd\n");
                break;
            default:
                pr_debug("Sensor is power off currently. \n");
                return -ENODEV;
        }
    }

    switch (cmd) {
        case GF_IOC_INIT:
            if (copy_to_user((void __user *)arg, (void *)&netlink_route, sizeof(u8)))
                retval = -EFAULT;
            break;
        case GF_IOC_DISABLE_IRQ:
            gf_disable_irq(gf_dev);
            break;
        case GF_IOC_ENABLE_IRQ:
            gf_enable_irq(gf_dev);
            break;
        case GF_IOC_RESET:
            gf_hw_reset(gf_dev, 10);
            break;
        case GF_IOC_ENABLE_SPI_CLK:
#ifdef CONFIG_OPPO_FINGERPRINT_GOODIX_CLK_CTRL
            gfspi_ioctl_clk_enable(gf_dev);
#endif
            break;
        case GF_IOC_DISABLE_SPI_CLK:
#ifdef CONFIG_OPPO_FINGERPRINT_GOODIX_CLK_CTRL
            gfspi_ioctl_clk_disable(gf_dev);
#endif
            break;
        case GF_IOC_ENABLE_POWER:
            if (gf_dev->device_available == 1)
                break;
            gf_set_power(gf_dev, true);
            gf_dev->device_available = 1;
            break;
        case GF_IOC_DISABLE_POWER:
            if (gf_dev->device_available == 0)
                break;
            gf_set_power(gf_dev, false);
            gf_dev->device_available = 0;
            break;
        case GF_IOC_REMOVE:
            irq_cleanup(gf_dev);
            gf_cleanup(gf_dev);
            break;
        case GF_IOC_CHIP_INFO:
            if (copy_from_user(&info, (struct gf_ioc_chip_info *)arg, sizeof(struct gf_ioc_chip_info))) {
                retval = -EFAULT;
            } else {
                pr_info("vendor_id : 0x%x\n", info.vendor_id);
                pr_info("mode : 0x%x\n", info.mode);
                pr_info("operation: 0x%x\n", info.operation);
            }
            break;
        case GF_IOC_WAKELOCK_TIMEOUT_ENABLE:
            __pm_wakeup_event(&gf_cmd_wakelock, msecs_to_jiffies(SENDCMD_WAKELOCK_HOLD_TIME));
            break;
        case GF_IOC_WAKELOCK_TIMEOUT_DISABLE:
            __pm_relax(&gf_cmd_wakelock);
            break;
        case GF_IOC_CLEAN_TOUCH_FLAG:
            lasttouchmode = 0;
            break;
        default:
            pr_warn("unsupport cmd:0x%x\n", cmd);
            break;
    }

    return retval;
}

#ifdef CONFIG_COMPAT
static long gf_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    return gf_ioctl(filp, cmd, (unsigned long)compat_ptr(arg));
}
#endif /*CONFIG_COMPAT*/


static int gf_open(struct inode *inode, struct file *filp)
{
    struct gf_dev *gf_dev = &gf;
    int status = -ENXIO;

    mutex_lock(&device_list_lock);

    list_for_each_entry(gf_dev, &device_list, device_entry) {
        if (gf_dev->devt == inode->i_rdev) {
            status = 0;
            break;
        }
    }

    if (status == 0) {
        gf_dev->users++;
        filp->private_data = gf_dev;
        nonseekable_open(inode, filp);
        if (gf_dev->users == 1) {
            status = gf_parse_dts(gf_dev);
            if (status)
                return status;

            status = irq_setup(gf_dev);
            if (status)
                gf_cleanup(gf_dev);
        }
        
    } else {
        pr_info("No device for minor %d\n", iminor(inode));
    }

    mutex_unlock(&device_list_lock);
    return status;
}

#ifdef GF_FASYNC
static int gf_fasync(int fd, struct file *filp, int mode)
{
    struct gf_dev *gf_dev = filp->private_data;
    int ret;

    ret = fasync_helper(fd, filp, mode, &gf_dev->async);
    return ret;
}
#endif

static int gf_release(struct inode *inode, struct file *filp)
{
    struct gf_dev *gf_dev = &gf;
    int status = 0;

    mutex_lock(&device_list_lock);
    gf_dev = filp->private_data;
    filp->private_data = NULL;

    /*last close?? */
    gf_dev->users--;
    if (!gf_dev->users) {
        irq_cleanup(gf_dev);
        gf_cleanup(gf_dev);
        /*power off the sensor*/
        gf_dev->device_available = 0;
    }
    mutex_unlock(&device_list_lock);
    return status;
}

static const struct file_operations gf_fops = {
    .owner = THIS_MODULE,
    /* REVISIT switch to aio primitives, so that userspace
     * gets more complete API coverage.  It'll simplify things
     * too, except for the locking.
     */
    .unlocked_ioctl = gf_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = gf_compat_ioctl,
#endif /*CONFIG_COMPAT*/
    .open = gf_open,
    .release = gf_release,
#ifdef GF_FASYNC
    .fasync = gf_fasync,
#endif
};

static int goodix_fb_state_chg_callback(struct notifier_block *nb,
        unsigned long val, void *data)
{
    struct gf_dev *gf_dev;
    struct msm_drm_notifier *evdata = data;
    unsigned int blank;
    char msg = 0;
    int retval = 0;

    gf_dev = container_of(nb, struct gf_dev, notifier);

    if (val == MSM_DRM_ONSCREENFINGERPRINT_EVENT) {
        uint8_t op_mode = 0x0;
        op_mode = *(uint8_t *)evdata->data;

        switch (op_mode) {
            case 1:
                msg = GF_NET_EVENT_UI_READY;
                gf_sendnlmsg(&msg);
                break;
        }
        return retval;
    }

    if (evdata && evdata->data && val == MSM_DRM_EARLY_EVENT_BLANK && gf_dev) {
        blank = *(int *)(evdata->data);
        switch (blank) {
            case MSM_DRM_BLANK_POWERDOWN:
                if (gf_dev->device_available == 1) {
                    gf_dev->fb_black = 1;
                    msg = GF_NET_EVENT_FB_BLACK;
                    gf_sendnlmsg(&msg);
#if defined (GF_FASYNC)
                    if (gf_dev->async) {
                        kill_fasync(&gf_dev->async, SIGIO, POLL_IN);
                    }
#endif
                }
                break;
            case MSM_DRM_BLANK_UNBLANK:
                if (gf_dev->device_available == 1) {
                    gf_dev->fb_black = 0;
                    msg = GF_NET_EVENT_FB_UNBLACK;
                    gf_sendnlmsg(&msg);
#if defined (GF_FASYNC)
                    if (gf_dev->async) {
                        kill_fasync(&gf_dev->async, SIGIO, POLL_IN);
                    }
#endif
                }
                break;
            default:
                pr_debug("%s defalut\n", __func__);
                break;
        }
    }
    return NOTIFY_OK;
}

static struct notifier_block goodix_noti_block = {
    .notifier_call = goodix_fb_state_chg_callback,
};

int gf_opticalfp_irq_handler(struct fp_underscreen_info* tp_info)
{
    char msg = 0;
    fp_tpinfo = *tp_info;
    if(tp_info->touch_state== lasttouchmode){
        return IRQ_HANDLED;
    }
    __pm_wakeup_event(&fp_wakelock, msecs_to_jiffies(WAKELOCK_HOLD_TIME));
    if (1 == tp_info->touch_state) {
        msg = GF_NET_EVENT_TP_TOUCHDOWN;
        gf_sendnlmsg(&msg);
        lasttouchmode = tp_info->touch_state;
    } else {
        msg = GF_NET_EVENT_TP_TOUCHUP;
        gf_sendnlmsg(&msg);
        lasttouchmode = tp_info->touch_state;
    }

    return IRQ_HANDLED;
}


static struct class *gf_class;
#if defined(CONFIG_OPPO_FINGERPRINT_GOODIX_SPI)
static int gf_probe(struct spi_device *dev)
#elif defined(CONFIG_OPPO_FINGERPRINT_GOODIX_PLATFORM)
static int gf_probe(struct platform_device *dev)
#endif
{
    struct gf_dev *gf_dev = &gf;
    int status = -EINVAL;
    unsigned long minor;
    int boot_mode = 0;
    /* Initialize the driver data */
    INIT_LIST_HEAD(&gf_dev->device_entry);
    gf_dev->dev = &dev->dev;
    gf_dev->irq_gpio = -EINVAL;
    gf_dev->reset_gpio = -EINVAL;
    gf_dev->pwr_gpio = -EINVAL;
    gf_dev->device_available = 0;
    gf_dev->fb_black = 0;

    if((FP_GOODIX_3268 != get_fpsensor_type())
            && (FP_GOODIX_5288 != get_fpsensor_type())
            && (FP_GOODIX_5228 != get_fpsensor_type())
            && (FP_GOODIX_OPTICAL_95 != get_fpsensor_type())){
        pr_err("%s, found not goodix sensor\n", __func__);
        status = -EINVAL;
        goto error_hw;
    }

    /* If we can allocate a minor number, hook up this device.
     * Reusing minors is fine so long as udev or mdev is working.
     */
    mutex_lock(&device_list_lock);
    minor = find_first_zero_bit(minors, GF_MAX_DEVS);
    if (minor < GF_MAX_DEVS) {
        struct device *dev;

        gf_dev->devt = MKDEV(gf_dev_major, minor);
        dev = device_create(gf_class, gf_dev->dev, gf_dev->devt,
                gf_dev, GF_DEV_NAME);
        status = IS_ERR(dev) ? PTR_ERR(dev) : 0;
    } else {
        dev_dbg(gf_dev->dev, "no minor number available!\n");
        status = -ENODEV;
        mutex_unlock(&device_list_lock);
        goto error_hw;
    }

    if (status == 0) {
        set_bit(minor, minors);
        list_add(&gf_dev->device_entry, &device_list);
    } else {
        gf_dev->devt = 0;
    }
    mutex_unlock(&device_list_lock);

#ifdef CONFIG_OPPO_FINGERPRINT_GOODIX_CLK_CTRL
    /* Enable spi clock */
    if (gfspi_ioctl_clk_init(gf_dev))
        goto gfspi_probe_clk_init_failed;

    if (gfspi_ioctl_clk_enable(gf_dev))
        goto gfspi_probe_clk_enable_failed;

    spi_clock_set(gf_dev, 1000000);
#endif

    gf_dev->notifier = goodix_noti_block;
#if defined(CONFIG_DRM_MSM)
    status = msm_drm_register_client(&gf_dev->notifier);
    if (status == -1) {
        return status;
    }
#endif
    wakeup_source_init(&fp_wakelock, "fp_wakelock");
    wakeup_source_init(&gf_cmd_wakelock, "gf_cmd_wakelock");
    pr_debug("version V%d.%d.%02d\n", VER_MAJOR, VER_MINOR, PATCH_LEVEL);

    return status;

#ifdef CONFIG_OPPO_FINGERPRINT_GOODIX_CLK_CTRL
gfspi_probe_clk_enable_failed:
    gfspi_ioctl_clk_uninit(gf_dev);
gfspi_probe_clk_init_failed:
#endif

error_hw:
    gf_dev->device_available = 0;
    boot_mode = get_boot_mode();
    if (MSM_BOOT_MODE__FACTORY == boot_mode)
        gf_set_power(gf_dev, false);

    return status;
}

#if defined(CONFIG_OPPO_FINGERPRINT_GOODIX_SPI)
static int gf_remove(struct spi_device *dev)
#elif defined(CONFIG_OPPO_FINGERPRINT_GOODIX_PLATFORM)
static int gf_remove(struct platform_device *dev)
#endif
{
    struct gf_dev *gf_dev = dev_get_drvdata(&dev->dev);
    wakeup_source_trash(&fp_wakelock);
    wakeup_source_trash(&gf_cmd_wakelock);

    msm_drm_unregister_client(&gf_dev->notifier);
    if (gf_dev->input)
        input_unregister_device(gf_dev->input);
    input_free_device(gf_dev->input);

    /* prevent new opens */
    mutex_lock(&device_list_lock);
    list_del(&gf_dev->device_entry);
    device_destroy(gf_class, gf_dev->devt);
    clear_bit(MINOR(gf_dev->devt), minors);
    mutex_unlock(&device_list_lock);

    return 0;
}

static struct of_device_id gx_match_table[] = {
    { .compatible = GF_SPIDEV_NAME },
    {},
};

#if defined(CONFIG_OPPO_FINGERPRINT_GOODIX_SPI)
static struct spi_driver gf_driver = {
#elif defined(CONFIG_OPPO_FINGERPRINT_GOODIX_PLATFORM)
static struct platform_driver gf_driver = {
#endif
    .driver = {
        .name = GF_DEV_NAME,
        .owner = THIS_MODULE,
        .of_match_table = gx_match_table,
        .probe_type = PROBE_PREFER_ASYNCHRONOUS,
    },
    .probe = gf_probe,
    .remove = gf_remove,
};

static int __init gf_init(void)
{
    int rc;

    /* Allocate chardev region and assign major number */
    BUILD_BUG_ON(GF_MAX_DEVS > 256);
    rc = __register_chrdev(gf_dev_major, 0, GF_MAX_DEVS, GF_CHRDEV_NAME,
                   &gf_fops);
    if (rc < 0) {
        pr_warn("failed to register char device\n");
        return rc;
    }
    gf_dev_major = rc;

    /* Create class */
    gf_class = class_create(THIS_MODULE, GF_CLASS_NAME);
    if (IS_ERR(gf_class)) {
        pr_warn("failed to create class.\n");
        rc = PTR_ERR(gf_class);
        goto error_class;
    }
#if defined(CONFIG_OPPO_FINGERPRINT_GOODIX_PLATFORM)
    rc = platform_driver_register(&gf_driver);
#elif defined(CONFIG_OPPO_FINGERPRINT_GOODIX_SPI)
    rc = spi_register_driver(&gf_driver);
#endif
    if (rc < 0) {
        pr_warn("failed to register driver\n");
        goto error_register;
    }

    rc = gf_netlink_init();
    if (rc < 0) {
        pr_warn("failed to initialize netlink\n");
        goto error_netlink;
    }
    pr_debug("initialization successfully done\n");
    return 0;

error_netlink:
#if defined(CONFIG_OPPO_FINGERPRINT_GOODIX_PLATFORM)
        platform_driver_unregister(&gf_driver);
#elif defined(CONFIG_OPPO_FINGERPRINT_GOODIX_SPI)
        spi_unregister_driver(&gf_driver);
#endif
error_register:
    class_destroy(gf_class);
error_class:
    unregister_chrdev(gf_dev_major, GF_CHRDEV_NAME);
    return rc;
}

static void __exit gf_exit(void)
{
    gf_netlink_exit();
#if defined(CONFIG_OPPO_FINGERPRINT_GOODIX_PLATFORM)
    platform_driver_unregister(&gf_driver);
#elif defined(CONFIG_OPPO_FINGERPRINT_GOODIX_SPI)
    spi_unregister_driver(&gf_driver);
#endif
    class_destroy(gf_class);
    unregister_chrdev(gf_dev_major, gf_driver.driver.name);
}

EXPORT_SYMBOL(gf_opticalfp_irq_handler);
late_initcall(gf_init);
module_exit(gf_exit);

MODULE_AUTHOR("Jiangtao Yi, <yijiangtao@goodix.com>");
MODULE_AUTHOR("Jandy Gou, <gouqingsong@goodix.com>");
MODULE_DESCRIPTION("goodix fingerprint sensor device driver");
MODULE_LICENSE("GPL");
