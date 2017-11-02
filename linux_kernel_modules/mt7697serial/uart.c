/*
 * uart.c: Serial driver for MT7697 serial controller
 *
 * Copyright (C) 2017 Sierra Wireless.
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2.  This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/platform_device.h>
#include <asm/uaccess.h>
#include "uart.h"

static int mt7697_uart_rx_poll(struct mt7697_uart_info* uart_info)
{
	struct poll_wqueues table;
	int mask;
	int ret = 0;

        poll_initwait(&table);

	while (1) {
		if (!uart_info->fd_hndl->f_op->poll) {
			dev_err(uart_info->dev, 
				"%s(): no poll function\n", __func__);
			ret = -EINVAL;
			goto cleanup;
		}

        	mask = uart_info->fd_hndl->f_op->poll(uart_info->fd_hndl, &table.pt); 
        	if (mask & (POLLRDNORM | POLLRDBAND | POLLIN |POLLHUP | POLLERR)) {
			dev_dbg(uart_info->dev, "%s(): Rx data\n", __func__);
                	break;
        	}

		if (signal_pending(current)) {
			dev_warn(uart_info->dev, 
				"%s(): signal_pending()\n", __func__);
			ret = -EINTR;
			goto cleanup;
		}

		if (!schedule_timeout_interruptible(MAX_SCHEDULE_TIMEOUT)) {
			dev_err(uart_info->dev, 
				"%s(): schedule_timeout_interruptible() failed\n", 
				__func__);
			ret = -ETIMEDOUT;
			goto cleanup;
		}
	}

cleanup:
	poll_freewait(&table);
	return ret;
}

static void mt7697_uart_rx_work(struct work_struct *rx_work)
{
	struct mt7697_uart_info* uart_info = container_of(rx_work, 
		struct mt7697_uart_info, rx_work);
	size_t ret;
	
	while (1) {
		ret = mt7697_uart_rx_poll(uart_info);
		if (ret) {
			dev_err(uart_info->dev, 
				"%s(): mt7697_uart_rx_poll() failed(%d)\n", 
				__func__, ret);
			goto cleanup;
		}

		if (uart_info->close) {
			dev_warn(uart_info->dev, "%s(): closed\n", __func__);
			goto cleanup;
		}

		ret = mt7697_uart_read(uart_info, (u32*)&uart_info->rsp, 
			LEN_TO_WORD(sizeof(struct mt7697_rsp_hdr)));
		if (ret != LEN_TO_WORD(sizeof(struct mt7697_rsp_hdr))) {
			if (ret) {
				dev_err(uart_info->dev, 
					"%s(): mt7697_uart_read() failed(%d != %d)\n", 
					__func__, ret, 
					LEN_TO_WORD(sizeof(struct mt7697_rsp_hdr)));
			}
			else {
				dev_warn(uart_info->dev, "%s(): closed\n", __func__);
			}

       			goto cleanup;
    		}

		if (uart_info->rsp.result < 0) {
			dev_warn(uart_info->dev, 
				"%s(): cmd(%u) result(%d)\n", 
				__func__, uart_info->rsp.cmd.type, 
				uart_info->rsp.result);
		}

		WARN_ON(!uart_info->rx_fcn);			
		ret = uart_info->rx_fcn((const struct mt7697_rsp_hdr*)&uart_info->rsp, 
			                uart_info->rx_hndl);
		if (ret < 0) {
			dev_err(uart_info->dev, 
				"%s(): rx_fcn() failed(%d)\n", 
				__func__, ret);
    		}
	}

cleanup:
	dev_warn(uart_info->dev, "%s(): task ended\n", __func__);
	uart_info->close = 0;
	wake_up_interruptible(&uart_info->close_wq);
	return;
}

void* mt7697_uart_open(rx_hndlr rx_fcn, void* rx_hndl)
{
	struct device *dev;
	struct platform_device *uart;
	struct mt7697_uart_info *uart_info;
	void *ret = NULL;

	pr_info("%s(): find UART device('%s')\n", __func__, MT7697_UART_DRVNAME);
	dev = bus_find_device_by_name(&platform_bus_type, NULL, MT7697_UART_DRVNAME);
	if (!dev) {
		pr_err("%s(): '%s' bus_find_device_by_name() failed\n", 
			__func__, MT7697_UART_DRVNAME);
		goto cleanup;
	}

	uart = container_of(dev, struct platform_device, dev);
	if (!uart) {
		pr_err("%s(): get UART device failed\n", __func__);
		goto cleanup;
	}

	uart_info = platform_get_drvdata(uart);
	if (!uart_info) {
		dev_err(&uart->dev, "%s(): platform_get_drvdata() failed\n", 
			__func__);
		goto cleanup;
	}

	WARN_ON(uart_info->fd_hndl);
	dev_err(uart_info->dev, "%s(): open serial device '%s'\n", 
		__func__, uart_info->dev_file);
	uart_info->fd_hndl = filp_open((const char __user*)uart_info->dev_file, O_RDWR, 0600);
	if (IS_ERR(uart_info->fd_hndl)) {
		dev_err(uart_info->dev, "%s(): filp_open() '%s' failed\n", 
			__func__, MT7697_UART_DEVICE);
		uart_info->fd_hndl = MT7697_UART_INVALID_FD;
		goto cleanup;
	}

	dev_dbg(uart_info->dev, "%s(): fd_hndl(%p)\n", 
		__func__, uart_info->fd_hndl);

	uart_info->rx_fcn = rx_fcn;
	uart_info->rx_hndl = rx_hndl;
	schedule_work(&uart_info->rx_work);
	ret = uart_info;

cleanup:
	if (!ret && uart_info->fd_hndl) 
		fput(uart_info->fd_hndl);

	return ret;
}

EXPORT_SYMBOL(mt7697_uart_open);

int mt7697_uart_close(void *arg)
{
	struct mt7697_uart_info *uart_info = arg;
	int ret = 0;

	dev_dbg(uart_info->dev, "%s(): fd_hndl(%p)\n", 
		__func__, uart_info->fd_hndl);
	WARN_ON (!uart_info->fd_hndl);

	uart_info->close = 1;
	ret = filp_close(uart_info->fd_hndl, 0);
	if (ret < 0) {
		dev_err(uart_info->dev, "%s(): filp_close() failed(%d)\n", 
			__func__, ret);
		goto cleanup;
	}

	wait_event_interruptible(uart_info->close_wq, !uart_info->close);
	cancel_work_sync(&uart_info->rx_work);
	uart_info->fd_hndl = MT7697_UART_INVALID_FD;

cleanup:
	return ret;
}

EXPORT_SYMBOL(mt7697_uart_close);

size_t mt7697_uart_read(void *arg, u32 *buf, size_t len)
{
	mm_segment_t oldfs;
	struct mt7697_uart_info *uart_info = arg;
	u8* ptr = (u8*)buf;
	loff_t offset = 0;
	size_t ret = 0;
	unsigned long count = len * sizeof(u32);
	unsigned long end = count;
	int err;

	oldfs = get_fs();
	set_fs(get_ds());

	if (!uart_info->fd_hndl) {
		dev_err(uart_info->dev, "%s(): device closed\n", __func__);
		goto cleanup;
	}

	dev_dbg(uart_info->dev, "%s(): len(%u)\n", __func__, len * sizeof(u32));
	while (offset < end) {
		err = kernel_read(uart_info->fd_hndl, offset, ptr, count);
		dev_dbg(uart_info->dev, "%s(): read(%d)\n", __func__, err);
		if (err < 0) {
			dev_err(uart_info->dev, "%s(): kernel_read() failed(%d)\n", 
				__func__, err);
			goto cleanup;
		}
		else if (!err) {
			dev_warn(uart_info->dev, "%s(): CLOSED\n", __func__);
			goto cleanup;
		}

		count -= err;
		ptr += err;
		offset += err;
	}

	ret = len;

cleanup:
	dev_dbg(uart_info->dev, "%s(): return(%u)\n", __func__, ret);
	set_fs(oldfs);
	return ret;
}

EXPORT_SYMBOL(mt7697_uart_read);

size_t mt7697_uart_write(void *arg, const u32 *buf, size_t len)
{
	mm_segment_t oldfs;
	loff_t pos = 0;
	size_t num_write;
	size_t end = len * sizeof(u32);
	size_t left = end;
	size_t ret = 0;
	struct mt7697_uart_info *uart_info = arg;
	u8* ptr = (u8*)buf;

	oldfs = get_fs();
	set_fs(get_ds());

	if (!uart_info->fd_hndl) {
		dev_err(uart_info->dev, "%s(): device closed\n", __func__);
		goto cleanup;
	}

	dev_dbg(uart_info->dev, "%s(): len(%u)\n", __func__, len);
	while (pos < end) {
		num_write = kernel_write(uart_info->fd_hndl, ptr, left, pos);
		dev_dbg(uart_info->dev, "%s(): written(%u)\n", __func__, num_write);
		if (!num_write) {
			dev_dbg(uart_info->dev, "%s(): WRITE no data\n", __func__);
			goto cleanup;
		}

		ptr += num_write;
		pos += num_write;
		left -= num_write;
	}

	ret = LEN_TO_WORD(pos);

cleanup:
	dev_dbg(uart_info->dev, "%s(): return(%u)\n", __func__, ret);
	set_fs(oldfs);
	return ret;
}

EXPORT_SYMBOL(mt7697_uart_write);

static int mt7697_uart_probe(struct platform_device *pdev)
{
	struct mt7697_uart_info* uart_info;
        int ret;

        dev_info(&pdev->dev, "%s(): init\n", __func__);

	uart_info = kzalloc(sizeof(struct mt7697_uart_info), GFP_KERNEL);
	if (!uart_info) {
		pr_err("%s(): kzalloc() failed\n", __func__);
		ret = -ENOMEM;
		goto cleanup;
	}

	memset(uart_info, 0, sizeof(struct mt7697_uart_info));
        uart_info->pdev = pdev;
	uart_info->dev = &pdev->dev;
	uart_info->fd_hndl = MT7697_UART_INVALID_FD;
	uart_info->dev_file = MT7697_UART_DEVICE;

	init_waitqueue_head(&uart_info->close_wq);
	INIT_WORK(&uart_info->rx_work, mt7697_uart_rx_work);

	platform_set_drvdata(pdev, uart_info);

	dev_info(&pdev->dev, "%s(): '%s' initialized\n", __func__, MT7697_UART_DRVNAME);
	return 0;

cleanup:
	if (ret) {
		if (uart_info) kfree(uart_info);
		platform_set_drvdata(pdev, NULL);
	}

	return ret;
}

static int mt7697_uart_remove(struct platform_device *pdev)
{
	struct mt7697_uart_info *uart_info = platform_get_drvdata(pdev);
	long ret;

	dev_info(&pdev->dev, "%s(): exit\n", __func__);

	ret = mt7697_uart_close(uart_info);
	if (ret < 0) {
		dev_err(&pdev->dev, "%s(): mt7697_uart_close() failed(%ld)\n", 
			__func__, ret);
	}

	kfree(uart_info);
	return ret;
}

static void mt7697_uart_release(struct device *dev)
{
	pr_info(MT7697_UART_DRVNAME" %s(): released.\n", __func__);
}

static struct platform_device mt7697_uart_platform_device = {
    	.name		= MT7697_UART_DRVNAME,
    	.id		= PLATFORM_DEVID_NONE,
    	.dev 		= {
		.release	= mt7697_uart_release,
	},
};

static struct platform_driver mt7697_uart_platform_driver = {
	.driver = {
		.name = MT7697_UART_DRVNAME,
		.owner = THIS_MODULE,
	},

	.probe = mt7697_uart_probe,
	.remove	= mt7697_uart_remove,
};

static int __init mt7697_uart_init(void)
{
	int ret;

	pr_info(MT7697_UART_DRVNAME" init\n");
	ret = platform_device_register(&mt7697_uart_platform_device);
	if (ret) {
		pr_err(MT7697_UART_DRVNAME" %s(): platform_device_register() failed(%d)\n", 
			__func__, ret);
		goto cleanup;
	}

	ret = platform_driver_register(&mt7697_uart_platform_driver);
	if (ret) {
		pr_err(MT7697_UART_DRVNAME" %s(): platform_driver_register() failed(%d)\n", 
			__func__, ret);
		platform_device_del(&mt7697_uart_platform_device);
		goto cleanup;
	}

cleanup:
	return ret;
}

static void __exit mt7697_uart_exit(void)
{
	platform_driver_unregister(&mt7697_uart_platform_driver);
	platform_device_unregister(&mt7697_uart_platform_device);
	pr_info(MT7697_UART_DRVNAME" exit\n");
}

module_init(mt7697_uart_init);
module_exit(mt7697_uart_exit);

MODULE_DESCRIPTION("MT7697 uart interface");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sierra Wireless");


