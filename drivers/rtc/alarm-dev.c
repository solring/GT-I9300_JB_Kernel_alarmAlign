/* drivers/rtc/alarm-dev.c
 *
 * Copyright (C) 2007-2009 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <asm/mach/time.h>
#include <linux/android_alarm.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/sysdev.h>
#include <linux/uaccess.h>
#include <linux/wakelock.h>
/* solring20140620: align alarm*/
#include <linux/proc_fs.h>

#define ANDROID_ALARM_PRINT_INFO (1U << 0)
#define ANDROID_ALARM_PRINT_IO (1U << 1)
#define ANDROID_ALARM_PRINT_INT (1U << 2)

/* solring20140620: align alarm*/
#define PROC_FNAME  "aligned-alarm"
#define DEFAULT_PERIOD 300
#define ALARM_DEBUG 1
#define DEBUG_PRINT(fmt, ...) \
            do { if (ALARM_DEBUG) printk(fmt, __VA_ARGS__); } while (0)

static struct proc_dir_entry *proc_fd;
static __kernel_time_t align_period;

static int debug_mask = ANDROID_ALARM_PRINT_INFO;
module_param_named(debug_mask, debug_mask, int, S_IRUGO | S_IWUSR | S_IWGRP);

#define pr_alarm(debug_level_mask, args...) \
	do { \
		if (debug_mask & ANDROID_ALARM_PRINT_##debug_level_mask) { \
			pr_info(args); \
		} \
	} while (0)

#define ANDROID_ALARM_WAKEUP_MASK ( \
	ANDROID_ALARM_RTC_WAKEUP_MASK | \
	ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP_MASK)

/* support old usespace code */
#define ANDROID_ALARM_SET_OLD               _IOW('a', 2, time_t) /* set alarm */
#define ANDROID_ALARM_SET_AND_WAIT_OLD      _IOW('a', 3, time_t)

static int alarm_opened;
static DEFINE_SPINLOCK(alarm_slock);
static struct wake_lock alarm_wake_lock;
static DECLARE_WAIT_QUEUE_HEAD(alarm_wait_queue);
static uint32_t alarm_pending;
static uint32_t alarm_enabled;
static uint32_t wait_pending;

static struct alarm alarms[ANDROID_ALARM_TYPE_COUNT];

static int alarm_read_proc(char *buffer, char **buffer_location,
	      off_t offset, int buffer_length, int *eof, void *data)
{
	int ret;
	
	printk(KERN_INFO "align alarm: proc file read (/proc/%s)\n", PROC_FNAME);
	
	if (offset > 0) {
		/* we have finished to read, return 0 */
		ret  = 0;
	} else {
		/* fill the buffer, return the buffer size */
		ret = sprintf(buffer, "%ld\n", align_period);
	}

	return ret;
}

static int alarm_write_proc(struct file *file, const char __user *buffer, unsigned long count, void *data)
{
	char    *buf;
    int     res;
    
    //printk("count=%d\n", count);
    buf = kmalloc(count+1, GFP_KERNEL);
    if ( copy_from_user(buf, (char*)buffer , count) ) {
        kfree(buf);
        return -EFAULT;
    }
    buf[count] = 0;
    printk("align alarm: proc writen, string=%s\n", buf);
    
    res = sscanf(buf, "%ld", &align_period);
    if(res >= 1) printk("align alarm: get new period failed.");
    
    kfree(buf);
    return count;
}

static long alarm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int rv = 0;
	unsigned long flags;
	struct timespec new_alarm_time;
	struct timespec new_rtc_time;
	struct timespec tmp_time;
	struct timespec mycurrent;
	struct timespec rawboott;
	enum android_alarm_type alarm_type = ANDROID_ALARM_IOCTL_TO_TYPE(cmd);
	uint32_t alarm_type_mask = 1U << alarm_type;
#if defined(CONFIG_RTC_ALARM_BOOT)
	char bootalarm_data[14];
#elif defined(CONFIG_RTC_POWER_OFF)
	char pwroffalarm_data[14];
#endif
    /* solring20140620: align alarm*/
    
    __kernel_time_t next_up;

	if (alarm_type >= ANDROID_ALARM_TYPE_COUNT)
		return -EINVAL;

	if (ANDROID_ALARM_BASE_CMD(cmd) != ANDROID_ALARM_GET_TIME(0)) {
		if ((file->f_flags & O_ACCMODE) == O_RDONLY)
			return -EPERM;
		if (file->private_data == NULL &&
		    cmd != ANDROID_ALARM_SET_RTC) {
			spin_lock_irqsave(&alarm_slock, flags);
			if (alarm_opened) {
				spin_unlock_irqrestore(&alarm_slock, flags);
				return -EBUSY;
			}
			alarm_opened = 1;
			file->private_data = (void *)1;
			spin_unlock_irqrestore(&alarm_slock, flags);
		}
	}

	switch (ANDROID_ALARM_BASE_CMD(cmd)) {
	case ANDROID_ALARM_CLEAR(0):
		spin_lock_irqsave(&alarm_slock, flags);
		pr_alarm(IO, "alarm %d clear\n", alarm_type);
		alarm_try_to_cancel(&alarms[alarm_type]);
		if (alarm_pending) {
			alarm_pending &= ~alarm_type_mask;
			if (!alarm_pending && !wait_pending)
				wake_unlock(&alarm_wake_lock);
		}
		alarm_enabled &= ~alarm_type_mask;
		spin_unlock_irqrestore(&alarm_slock, flags);
		break;

	case ANDROID_ALARM_SET_OLD:
	case ANDROID_ALARM_SET_AND_WAIT_OLD:
		if (get_user(new_alarm_time.tv_sec, (int __user *)arg)) {
			rv = -EFAULT;
			goto err1;
		}
		new_alarm_time.tv_nsec = 0;
		goto from_old_alarm_set;

	case ANDROID_ALARM_SET_AND_WAIT(0):
	case ANDROID_ALARM_SET(0):
		if (copy_from_user(&new_alarm_time, (void __user *)arg,
		    sizeof(new_alarm_time))) {
			rv = -EFAULT;
			goto err1;
		}
from_old_alarm_set:
        
        /* solring 20130620: align all the alarms */
        if(ALARM_DEBUG) printk("AlignAlarm: origin new_alarm_time: %ld.%09ld\n", new_alarm_time.tv_sec, new_alarm_time.tv_nsec);
        
        if(align_period == 0) align_period = DEFAULT_PERIOD;
        getnstimeofday(&mycurrent);
        //mycurrent = current_kernel_time();
        rawboott = ktime_to_timespec(alarm_get_elapsed_realtime());
        //getrawmonotonic(&rawboott);
        

        if(alarm_type==ANDROID_ALARM_RTC_WAKEUP){
            printk("AlignAlarm: alarm_type: ANDROID_ALARM_RTC_WAKEUP\n");
            next_up = mycurrent.tv_sec + align_period - (mycurrent.tv_sec % align_period);
            printk("AlignAlarm: current time: %ld.%09ld, Next up time: %ld\n", mycurrent.tv_sec, mycurrent.tv_nsec, next_up);
            
            if(new_alarm_time.tv_sec > mycurrent.tv_sec && new_alarm_time.tv_sec < next_up) {
                printk("AlignAlarm: align to next_up\n");
                new_alarm_time.tv_sec = next_up;
                new_alarm_time.tv_nsec = 0;
            }
        }else if(alarm_type==ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP){
            printk("AlignAlarm: alarm_type: ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP\n");
            next_up = rawboott.tv_sec + align_period - (mycurrent.tv_sec % align_period); // align to the same point as RTC
            printk("AlignAlarm: current time: %ld.%09ld, elapsed next up time: %ld\n", rawboott.tv_sec, rawboott.tv_nsec, next_up);
            
            if(new_alarm_time.tv_sec > rawboott.tv_sec && new_alarm_time.tv_sec < next_up) {
                printk("AlignAlarm: align to next_up\n");
                new_alarm_time.tv_sec = next_up;
                new_alarm_time.tv_nsec = 0;
            }
        }
        
        

		spin_lock_irqsave(&alarm_slock, flags);
		pr_alarm(IO, "alarm %d set %ld.%09ld\n", alarm_type,
			new_alarm_time.tv_sec, new_alarm_time.tv_nsec);
		//printk("--- alarm %d set %ld.%09ld ---\n", alarm_type, new_alarm_time.tv_sec, new_alarm_time.tv_nsec);
		alarm_enabled |= alarm_type_mask;
		alarm_start_range(&alarms[alarm_type],
			timespec_to_ktime(new_alarm_time),
			timespec_to_ktime(new_alarm_time));
		spin_unlock_irqrestore(&alarm_slock, flags);
		if (ANDROID_ALARM_BASE_CMD(cmd) != ANDROID_ALARM_SET_AND_WAIT(0)
		    && cmd != ANDROID_ALARM_SET_AND_WAIT_OLD)
			break;
		/* fall though */
	case ANDROID_ALARM_WAIT:
		spin_lock_irqsave(&alarm_slock, flags);
		pr_alarm(IO, "alarm wait\n");
		if (!alarm_pending && wait_pending) {
			wake_unlock(&alarm_wake_lock);
			wait_pending = 0;
		}
		spin_unlock_irqrestore(&alarm_slock, flags);
		rv = wait_event_interruptible(alarm_wait_queue, alarm_pending);
		if (rv)
			goto err1;
		spin_lock_irqsave(&alarm_slock, flags);
		rv = alarm_pending;
		wait_pending = 1;
		alarm_pending = 0;
		spin_unlock_irqrestore(&alarm_slock, flags);
		break;
	case ANDROID_ALARM_SET_RTC:
		if (copy_from_user(&new_rtc_time, (void __user *)arg,
		    sizeof(new_rtc_time))) {
			rv = -EFAULT;
			goto err1;
		}
		rv = alarm_set_rtc(new_rtc_time);
		spin_lock_irqsave(&alarm_slock, flags);
		alarm_pending |= ANDROID_ALARM_TIME_CHANGE_MASK;
		wake_up(&alarm_wait_queue);
		spin_unlock_irqrestore(&alarm_slock, flags);
		if (rv < 0)
			goto err1;
		break;
#if defined(CONFIG_RTC_ALARM_BOOT)
	case ANDROID_ALARM_SET_ALARM_BOOT:
		if (copy_from_user(bootalarm_data, (void __user *)arg, 14)) {
			rv = -EFAULT;
			goto err1;
		}
		rv = alarm_set_alarm_boot(bootalarm_data);
		break;
#elif defined(CONFIG_RTC_POWER_OFF)
	case ANDROID_ALARM_SET_ALARM_POWEROFF:
		if (copy_from_user(pwroffalarm_data, (void __user *)arg, 14)) {
			rv = -EFAULT;
			goto err1;
		}
		rv = alarm_set_alarm_poweroff(pwroffalarm_data);
		break;
#endif
	case ANDROID_ALARM_GET_TIME(0):
		switch (alarm_type) {
		case ANDROID_ALARM_RTC_WAKEUP:
		case ANDROID_ALARM_RTC:
			getnstimeofday(&tmp_time);
			break;
		case ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP:
		case ANDROID_ALARM_ELAPSED_REALTIME:
			tmp_time =
				ktime_to_timespec(alarm_get_elapsed_realtime());
			break;
		case ANDROID_ALARM_TYPE_COUNT:
		case ANDROID_ALARM_SYSTEMTIME:
			ktime_get_ts(&tmp_time);
			break;
		}
		if (copy_to_user((void __user *)arg, &tmp_time,
		    sizeof(tmp_time))) {
			rv = -EFAULT;
			goto err1;
		}
		break;

	default:
		rv = -EINVAL;
		goto err1;
	}
err1:
	return rv;
}

static int alarm_open(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}

static int alarm_release(struct inode *inode, struct file *file)
{
	int i;
	unsigned long flags;

	spin_lock_irqsave(&alarm_slock, flags);
	if (file->private_data != 0) {
		for (i = 0; i < ANDROID_ALARM_TYPE_COUNT; i++) {
			uint32_t alarm_type_mask = 1U << i;
			if (alarm_enabled & alarm_type_mask) {
				pr_alarm(INFO, "alarm_release: clear alarm, "
					"pending %d\n",
					!!(alarm_pending & alarm_type_mask));
				alarm_enabled &= ~alarm_type_mask;
			}
			spin_unlock_irqrestore(&alarm_slock, flags);
			alarm_cancel(&alarms[i]);
			spin_lock_irqsave(&alarm_slock, flags);
		}
		if (alarm_pending | wait_pending) {
			if (alarm_pending)
				pr_alarm(INFO, "alarm_release: clear "
					"pending alarms %x\n", alarm_pending);
			wake_unlock(&alarm_wake_lock);
			wait_pending = 0;
			alarm_pending = 0;
		}
		alarm_opened = 0;
	}
	spin_unlock_irqrestore(&alarm_slock, flags);
	return 0;
}

static void alarm_triggered(struct alarm *alarm)
{
	unsigned long flags;
	uint32_t alarm_type_mask = 1U << alarm->type;

	pr_alarm(INT, "alarm_triggered type %d\n", alarm->type);
	spin_lock_irqsave(&alarm_slock, flags);
	if (alarm_enabled & alarm_type_mask) {
		wake_lock_timeout(&alarm_wake_lock, 5 * HZ);
		alarm_enabled &= ~alarm_type_mask;
		alarm_pending |= alarm_type_mask;
		wake_up(&alarm_wait_queue);
	}
	spin_unlock_irqrestore(&alarm_slock, flags);
}

static const struct file_operations alarm_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = alarm_ioctl,
	.open = alarm_open,
	.release = alarm_release,
};

static struct miscdevice alarm_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "alarm",
	.fops = &alarm_fops,
};

static int __init alarm_dev_init(void)
{
	int err;
	int i;

	err = misc_register(&alarm_device);
	if (err)
		return err;

	for (i = 0; i < ANDROID_ALARM_TYPE_COUNT; i++)
		alarm_init(&alarms[i], i, alarm_triggered);
	wake_lock_init(&alarm_wake_lock, WAKE_LOCK_SUSPEND, "alarm");
    
    /* solring20140620: align alarm*/
    proc_fd = create_proc_entry(PROC_FNAME, 0, NULL);
    if (proc_fd == NULL) {
		remove_proc_entry(PROC_FNAME, NULL);
		printk(KERN_ALERT "Error: Could not initialize /proc/%s\n",
		       PROC_FNAME);
		return -ENOMEM;
	}
	proc_fd->read_proc = alarm_read_proc;
	proc_fd->write_proc = alarm_write_proc;
	//proc_fd->mode 	 = S_IFREG | S_IRUGO;
	//proc_fd->uid 	 = 0;
	//proc_fd->gid 	 = 0;
	//proc_fd->size 	 = 37;
	printk(KERN_INFO "/proc/%s created\n", PROC_FNAME);
	
	align_period = DEFAULT_PERIOD;
    
	return 0;
}

static void  __exit alarm_dev_exit(void)
{
	misc_deregister(&alarm_device);
	wake_lock_destroy(&alarm_wake_lock);
	/* solring20140620: align alarm*/
	remove_proc_entry(PROC_FNAME, NULL);
}

module_init(alarm_dev_init);
module_exit(alarm_dev_exit);

