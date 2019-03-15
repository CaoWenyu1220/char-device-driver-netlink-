/**
 * Training module device driver about netlink, timer, miscdev, etc.
 * Use with a user netlink damon.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/skbuff.h>
#include <linux/netlink.h>
#include <net/netlink.h>
#include <net/sock.h>
#include <asm/uaccess.h>/*for arm*/
#include <linux/wait.h>
#include <linux/semaphore.h>

static int debug_mask = 0x01;
module_param_named(debug_mask, debug_mask, int, S_IRUGO | S_IWUSR | S_IWGRP);
#define HACK_INFO(fmt, args...) \
	do { \
		if ( debug_mask & 0x01 ) \
			printk(KERN_INFO "HackInfo: [%s] " fmt, __func__, ##args); \
	} while (0)
#define HACK_DEBUG(fmt, args...) \
	do { \
		if ( debug_mask & 0x02 ) \
			printk(KERN_INFO "HackDebug: [%s] " fmt, __func__, ##args); \
	} while (0)
#define HACK_DUMP(fmt, args...) \
	do { \
		if ( debug_mask & 0x04 ) \
			printk(KERN_INFO "HackDump: [%s] " fmt, __func__, ##args); \
	} while (0)

#define TIMER_START		0
#define TIMER_STOP		1
#define TIMER_MODIFY	2

#define KTIME_BUF_MAX 8

// time struct
struct kernel_tm {
	unsigned long sec;
	unsigned long msecs;
};

struct training_device {
	/* NetLink */
	struct sock *sock_p;
	int nlk_pid;
	struct work_struct work;
	spinlock_t spin_lock;
	
	/* Timer */
	struct timer_list training_timer;
	int timer_interval_ms;
	int timer_initialize;
	int timer_count;

#define DATA_WORKING 0
#define DATA_READY 1
	/* Buffer */
	struct kernel_tm ktime_buf[KTIME_BUF_MAX];
	int buf_count;
	int ready;
	int dropped;

	/* Semaphore & Queue */
	wait_queue_head_t wait_r;
};

static int training_send_msg(const char *fmt, ...);
static struct training_device local_dev;

// get current time from kernel timer
static int get_kernel_time(struct kernel_tm *tm)
{
	unsigned long rem_nsec;
	u64 ts_nsec;

	ts_nsec		= local_clock();
	rem_nsec	= do_div(ts_nsec, 1000000000);
	tm->sec		= (unsigned long)ts_nsec;
	tm->msecs	= rem_nsec / 1000;

	return 0;
}

static void training_work_func(struct work_struct *work){
	struct training_device *dev = (struct training_device *)&local_dev;
	unsigned long flags;
	struct kernel_tm tm;

	spin_lock_irqsave(&dev->spin_lock, flags);
	tm.sec		= dev->ktime_buf[dev->buf_count].sec;
	tm.msecs	= dev->ktime_buf[dev->buf_count].msecs;
	dev->ready	= DATA_READY;
	//dev->buf_count ++;
	spin_unlock_irqrestore(&dev->spin_lock, flags);

	training_send_msg("Sec.%d, Msec.%d\n", tm.sec, tm.msecs);
}

static void timer_callback(unsigned long data){
	HACK_INFO("Enter\n");
	
	struct training_device *dev = (struct training_device *)&local_dev;
	unsigned long flags;
	struct kernel_tm tm;

	if ( dev->ready != DATA_READY ) {
		HACK_INFO("Is not ready, data dropped %d in all !!!\n", ++ dev->dropped);
		goto exit;
	}

	get_kernel_time(&tm);
	HACK_DEBUG("Now.[%5lu.%06lu], count.%d\n",
					dev->ktime_buf[dev->buf_count].sec,
					dev->ktime_buf[dev->buf_count].msecs,
					dev->timer_count);

	spin_lock_irqsave(&dev->spin_lock, flags);
	HACK_INFO("spin_lock_irqsave Enter\n");
	if ( dev->buf_count >= KTIME_BUF_MAX ) {
		dev->ready = DATA_WORKING;
	
		HACK_INFO("KTIME_BUF_MAX Enter\n");
		//dev->buf_count = 0;
		spin_unlock_irqrestore(&dev->spin_lock, flags);
		wake_up_interruptible(&dev->wait_r);
		if ( ! work_pending(&dev->work) )
		schedule_work(&dev->work);
		goto exit;
	}
	dev->ktime_buf[dev->buf_count].sec		= tm.sec;
	dev->ktime_buf[dev->buf_count].msecs	= tm.msecs;
	dev->timer_count ++;
	dev->buf_count++;
	dev->ready = DATA_READY;
	spin_unlock_irqrestore(&dev->spin_lock, flags);

exit:
	mod_timer(&dev->training_timer, jiffies + msecs_to_jiffies(dev->timer_interval_ms));
}

static int training_timer_command(struct training_device *dev, int cmd){
	HACK_DEBUG("Enter timer_cmd.%d\n", cmd);

	switch ( cmd ) {
		case TIMER_START:
			if ( ! dev->timer_initialize ) {
				HACK_INFO("Started.%d, cmd.%d\n", dev->timer_initialize, cmd);
				dev->timer_initialize ++;
				setup_timer(&dev->training_timer, timer_callback, (unsigned long)dev);
			}
		case TIMER_MODIFY:
			mod_timer(&dev->training_timer, jiffies + msecs_to_jiffies(dev->timer_interval_ms));
			break;
		case TIMER_STOP:
			if ( ! dev->timer_initialize ) {
				HACK_INFO("Stopped.%d, cmd.%d\n", dev->timer_initialize, cmd);
				return -1;
			}
			del_timer_sync(&dev->training_timer);
			dev->timer_initialize --;
			break;

		default:
			HACK_INFO("default timer_cmd.%d\n", cmd);
			break;
	}
	
	return 0;
}

static int training_timer_init(struct training_device *dev){
#define TIMER_DEMO_INTERVAL_MS 1000
	dev->timer_interval_ms	= TIMER_DEMO_INTERVAL_MS;
	dev->timer_count		= 0;
	dev->timer_initialize	= 0;
	return 0;
}

static void training_timer_deinit(struct training_device *dev){
	del_timer_sync(&dev->training_timer);
	dev->timer_initialize	= 0;
	dev->timer_count		= 0;
}

static int training_open(struct inode *inode, struct file *filp){
	HACK_INFO("Enter\n");
	
	/* Point private data of file at device struct. */
	struct training_device *dev = (struct training_device *)&local_dev;
	filp->private_data = dev;
	
	return 0;
}

// empty function
static int training_release(struct inode *inode, struct file *filp){
	HACK_INFO("Enter\n");
	return 0;
}

static ssize_t training_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos){
	HACK_DEBUG("Enter\n");
	unsigned long p = *f_pos;
	unsigned long flags;
	int ret = 0, n;
	struct training_device *dev = (struct training_device *)filp->private_data;

      if ( (dev->buf_count < 8) && (filp->f_flags & O_NONBLOCK) ) {
        	ret = -EAGAIN;
        	return ret;
      }

/*waiting for buffer no empty*/	
	if ( wait_event_interruptible(dev->wait_r, (dev->buf_count >= 8)) < 0) {
		HACK_DEBUG("wait_event_interruptible failed.!!!!!!!!!\n");
		return -ERESTARTSYS;
	}

	HACK_DEBUG("wait_event_interruptible buf_count=%d\n",dev->buf_count);

	spin_lock_irqsave(&dev->spin_lock, flags);
	n = sizeof(struct kernel_tm) * (dev->buf_count + 1);
	if (copy_to_user((struct kernel_tm *)buf, (void *)(dev->ktime_buf), n)){
		HACK_DEBUG("copy failed.!!!!!!!!!\n");
		ret = - EFAULT;
	}
	else{
		*f_pos += n;	
		ret = n;
		dev->buf_count = 0;
		printk(KERN_INFO"read %d from %ld\n",count,p);
	}
	spin_unlock_irqrestore(&dev->spin_lock, flags);
	HACK_DEBUG("Enter\n");
	return ret;
}


static unsigned int training_poll(struct file *filp, struct poll_table_struct *wait){
	HACK_DUMP("Enter\n");
	unsigned int mask = 0;
	unsigned long flags;
	struct training_device*dev = (struct training_device *)filp->private_data;

	poll_wait(filp, &dev->wait_r, wait);

	spin_lock_irqsave(&dev->spin_lock, flags);
	if ( dev->buf_count != 0 ) {
		mask |= POLLIN | POLLRDNORM;
	}
	spin_unlock_irqrestore(&dev->spin_lock, flags);
	return mask;
}


static struct file_operations training_fops = {
	.owner			= THIS_MODULE,
	.open			= training_open,
	.release		= training_release,
	.read			= training_read,
	.poll			= training_poll,

};

// misc device struct
static struct miscdevice training_misc_dev = {
#define TRAINING_MINOR	255
#define DEVICE_NAME	"t-netlink"
	.minor		= TRAINING_MINOR,
	.name		= DEVICE_NAME,
	.fops		= &training_fops,
};

// show function
static ssize_t state_dbg_show(struct device *pdev, struct device_attribute *attr,
			char *buf){
	struct training_device *dev = (struct training_device *)&local_dev;
	char *s = buf;

	s += sprintf(s, "timer_initialize.%d\n", dev->timer_initialize);
	s += sprintf(s, "timer_interval_ms.%d\n", dev->timer_interval_ms);
	s += sprintf(s, "timer_count.%d\n", dev->timer_count);
	s += sprintf(s, "buf_count.%d\n", dev->buf_count);
	s += sprintf(s, "ready.%d\n", dev->ready);
	s += sprintf(s, "dropped.%d\n", dev->dropped);

	return (s - buf);
}

// store function
static ssize_t state_dbg_store(struct device *pdev, struct device_attribute *attr,
			 const char *buf, size_t count){
	struct training_device *dev = (struct training_device *)&local_dev;
	int error = -EINVAL;

	if ( !strncmp(buf, "start", 5) ) {
		error = training_timer_command(dev, TIMER_START);
	} else if ( !strncmp(buf, "stop", 4) ) {
		error = training_timer_command(dev, TIMER_STOP);
	} else if ( !strncmp(buf, "reset", 5) ) {
		dev->buf_count = 0;
		error = training_timer_command(dev, TIMER_MODIFY);
	} else {
		HACK_INFO("Not supported !!!\n");
	}
	
	return (error ? error : count);
}

// device attribution
static DEVICE_ATTR(state, S_IRWXUGO, state_dbg_show, state_dbg_store);

// probe function
static int __devinit training_probe(struct platform_device *pdev)
{
	int ret;
	struct training_device *dev = (struct training_device *)&local_dev;

	HACK_INFO("Enter\n");

	if ( (ret = misc_register(&training_misc_dev)) != 0 ) {
		HACK_INFO("Failed misc_register\n");
		return ret;
	}

	if ( device_create_file(training_misc_dev.this_device, &dev_attr_state) < 0 )
		HACK_INFO("Failed device_create_file");

	init_waitqueue_head(&dev->wait_r);

	return 0;
}

static int __devexit training_remove(struct platform_device *dev){
	HACK_INFO("Enter\n");
	device_remove_file(training_misc_dev.this_device, &dev_attr_state);
	misc_deregister(&training_misc_dev);
	return 0;
}

// driver struct
static struct platform_driver training_driver = {
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= "t-netlink",
	},
	.probe		= training_probe,
	.remove		= training_remove,
};

// device struct
static struct platform_device training_device  = {
	.name		= "t-netlink",
	.id			= 0,
	.dev		= {
		.platform_data = NULL,
	},
};

static int training_misc_init(struct training_device *dev)
{
    HACK_INFO("Enter\n");

	dev->buf_count = 0;
    if ( platform_device_register(&training_device) < 0 )
		return -ENODEV;

    return platform_driver_register(&training_driver);
}

static void training_misc_deinit(struct training_device *dev)
{
	HACK_INFO("Enter\n");
	platform_driver_unregister(&training_driver);
	platform_device_unregister(&training_device);
}

static inline int training_skbuff_expand(struct sk_buff *skb, int extra)
{
	int oldtail = skb_tailroom(skb);
	int newtail = skb_tailroom(skb);

	HACK_INFO("newtail.0x%x, oldtail.0x%x\n", newtail, oldtail);
	if ( pskb_expand_head(skb, 0, extra, GFP_KERNEL) < 0 ) {
		HACK_INFO("Failed to pskb_expand_head\n");
		return 0;
	}

	skb->truesize += newtail - oldtail;
	return newtail;
}

static void training_send(struct sk_buff *skb)
{
    struct training_device *dev = (struct training_device *)&local_dev;

    HACK_DEBUG("Enter\n");

    if ( netlink_unicast(dev->sock_p, skb, dev->nlk_pid, 0) < 0 ) {
        HACK_INFO("Failed to netlink_unicast\n");
    }
}

static void training_recv(struct sk_buff *skb){
	struct training_device *dev = (struct training_device *)&local_dev;

    HACK_DEBUG("Enter\n");

	dev->nlk_pid = NETLINK_CB(skb).pid;
}

static int training_send_msg(const char *fmt, ...){
#define BUFF_SIZE 64
#define MAX_PAYLOAD 1024
	struct nlmsghdr *nlh;
	struct sk_buff *skb;
	char str[BUFF_SIZE];
	int len, avail;
	va_list args;

	HACK_DEBUG("Enter\n");

	va_start(args, fmt);
	len = vsnprintf(str, INT_MAX, fmt, args);
	va_end(args);
	if ( len < 0 ) {
		HACK_INFO("Failed to vsnprintf\n");
		return -1;
	}
	HACK_DEBUG("len.%d\n", len);

	if ( (skb = alloc_skb(NLMSG_SPACE(MAX_PAYLOAD), GFP_KERNEL)) < 0 ) {
		HACK_INFO("Failed to alloc_skb\n");
		return -1;
	}
	HACK_DEBUG("skb.%p\n", skb);

	if ( (avail = skb_tailroom(skb)) < MAX_PAYLOAD ) {
		HACK_INFO("avail.%d <<\n", avail);
		if ( (avail = training_skbuff_expand(skb, MAX_PAYLOAD)) == 0 ) {
			HACK_INFO("Failed to training_skbuff_expand\n");
			return -1;
		}
	}
	HACK_DEBUG("avail.%d\n", avail);

	if ( (nlh = nlmsg_put(skb, 0, 0, 0, MAX_PAYLOAD, 0)) == NULL ) {
		HACK_INFO("Failed to nlh\n");
		return -1;
	}

	NETLINK_CB(skb).pid = 0;
	memcpy(NLMSG_DATA(nlh), str, sizeof(str));
	HACK_DEBUG("memcpy\n");

	training_send(skb);

	return 0;
}

static int training_netlink_init(struct training_device *dev)
{
#define NETLINK_TRAINING 30
	HACK_INFO("Enter\n");

	if ( (dev->sock_p = netlink_kernel_create(&init_net,
												NETLINK_TRAINING,
												0,
												training_recv,
												NULL,
												THIS_MODULE)) == NULL ) {
		HACK_INFO("Cannot initialize netlink socket\n");
		return -1;
	}
	
	dev->sock_p->sk_sndtimeo = MAX_SCHEDULE_TIMEOUT;

	INIT_WORK(&dev->work, training_work_func);
	dev->ready = DATA_READY;

	return 0;
}

static void training_netlink_deinit(struct training_device *dev)
{
	HACK_INFO("Enter\n");
	cancel_work_sync(&dev->work);
}

// module init function
static int __devinit training_netlink_module_init(void)
{
	struct training_device *dev = (struct training_device *)&local_dev;

	spin_lock_init(&dev->spin_lock);

	if ( training_timer_init(dev) < 0 )
		return -1;

	if ( training_misc_init(dev) < 0 )
		return -1;

	if ( training_netlink_init(dev) < 0 )
		return -1;

	return 0;
}

// module exit function
static void __exit training_netlink_module_exit(void)
{
	struct training_device *dev = (struct training_device *)&local_dev;

	training_netlink_deinit(dev);
	training_misc_deinit(dev);
	training_timer_deinit(dev);
}

module_init(training_netlink_module_init);
module_exit(training_netlink_module_exit);

