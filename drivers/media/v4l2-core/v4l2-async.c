/*
 * V4L2 asynchronous subdevice registration API
 *
 * Copyright (C) 2012-2013, Guennadi Liakhovetski <g.liakhovetski@gmx.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/types.h>

#include <media/v4l2-async.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

static bool match_i2c(struct v4l2_subdev *sd, struct v4l2_async_subdev *asd)
{
#if IS_ENABLED(CONFIG_I2C)
	struct i2c_client *client = i2c_verify_client(sd->dev);
	return client &&
		asd->match.i2c.adapter_id == client->adapter->nr &&
		asd->match.i2c.address == client->addr;
#else
	return false;
#endif
}

static bool match_devname(struct v4l2_subdev *sd,
			  struct v4l2_async_subdev *asd)
{
	return !strcmp(asd->match.device_name.name, dev_name(sd->dev));
}

static bool match_of(struct v4l2_subdev *sd, struct v4l2_async_subdev *asd)
{
	return sd->of_node == asd->match.of.node;
}

static bool match_custom(struct v4l2_subdev *sd, struct v4l2_async_subdev *asd)
{
	if (!asd->match.custom.match)
		/* Match always */
		return true;

	return asd->match.custom.match(sd->dev, asd);
}

static LIST_HEAD(subdev_list);
static LIST_HEAD(notifier_list);

static struct v4l2_async_subdev *v4l2_async_belongs(struct v4l2_async_notifier *notifier,
						    struct v4l2_subdev *sd)
{
	bool (*match)(struct v4l2_subdev *, struct v4l2_async_subdev *);
	struct v4l2_async_subdev *asd;

	list_for_each_entry(asd, &notifier->waiting, list) {
		/* bus_type has been verified valid before */
		switch (asd->match_type) {
		case V4L2_ASYNC_MATCH_CUSTOM:
			match = match_custom;
			break;
		case V4L2_ASYNC_MATCH_DEVNAME:
			match = match_devname;
			break;
		case V4L2_ASYNC_MATCH_I2C:
			match = match_i2c;
			break;
		case V4L2_ASYNC_MATCH_OF:
			match = match_of;
			break;
		default:
			/* Cannot happen, unless someone breaks us */
			WARN_ON(true);
			return NULL;
		}

		/* match cannot be NULL here */
		if (match(sd, asd))
			return asd;
	}

	return NULL;
}

static int v4l2_async_test_notify(struct v4l2_async_notifier *notifier,
				  struct v4l2_subdev *sd,
				  struct v4l2_async_subdev *asd)
{
	int ret;

	/* Remove from the waiting list */
	list_del(&asd->list);
	sd->asd = asd;
	sd->notifier = notifier;

	if (notifier->bound) {
		ret = notifier->bound(notifier, sd, asd);
		if (ret < 0) {
			dev_warn(notifier->v4l2_dev->dev, "subdev bound failed\n");
			return ret;
		}
	}
	/* Move from the global subdevice list to notifier's done */
	list_move(&sd->async_list, &notifier->done);

	ret = v4l2_device_register_subdev(notifier->v4l2_dev, sd);
	if (ret < 0) {
		dev_warn(notifier->v4l2_dev->dev, "subdev register failed\n");
		if (notifier->unbind)
			notifier->unbind(notifier, sd, asd);
		return ret;
	}

	if (list_empty(&notifier->waiting) && notifier->complete)
		return notifier->complete(notifier);

	return 0;
}

static void v4l2_async_cleanup(struct v4l2_subdev *sd)
{
	v4l2_device_unregister_subdev(sd);
	/* Subdevice driver will reprobe and put the subdev back onto the list */
	list_del_init(&sd->async_list);
	sd->asd = NULL;
	sd->dev = NULL;
}

int v4l2_async_notifier_register(struct v4l2_device *v4l2_dev,
				 struct v4l2_async_notifier *notifier)
{
	struct v4l2_subdev *sd, *tmp;
	struct v4l2_async_subdev *asd;
	int ret = 0, i;

	if (!notifier->num_subdevs || notifier->num_subdevs > V4L2_MAX_SUBDEVS)
		return -EINVAL;

	notifier->v4l2_dev = v4l2_dev;
	INIT_LIST_HEAD(&notifier->waiting);
	INIT_LIST_HEAD(&notifier->done);
	mutex_init(&notifier->lock);

	for (i = 0; i < notifier->num_subdevs; i++) {
		asd = notifier->subdevs[i];

		switch (asd->match_type) {
		case V4L2_ASYNC_MATCH_CUSTOM:
		case V4L2_ASYNC_MATCH_DEVNAME:
		case V4L2_ASYNC_MATCH_I2C:
		case V4L2_ASYNC_MATCH_OF:
			break;
		default:
			dev_err(notifier->v4l2_dev ? notifier->v4l2_dev->dev : NULL,
				"Invalid match type %u on %p\n",
				asd->match_type, asd);
			return -EINVAL;
		}
		list_add_tail(&asd->list, &notifier->waiting);
	}

	/* Keep also completed notifiers on the list */
	list_add(&notifier->list, &notifier_list);
	mutex_lock(&notifier->lock);

	list_for_each_entry_safe(sd, tmp, &subdev_list, async_list) {
		asd = v4l2_async_belongs(notifier, sd);
		if (!asd)
			continue;

		ret = v4l2_async_test_notify(notifier, sd, asd);
		if (ret < 0)
			break;
	}
	mutex_unlock(&notifier->lock);

	return ret;
}
EXPORT_SYMBOL(v4l2_async_notifier_register);

void v4l2_async_notifier_unregister(struct v4l2_async_notifier *notifier)
{
	struct v4l2_subdev *sd, *tmp;
	unsigned int notif_n_subdev = notifier->num_subdevs;
	unsigned int n_subdev = min(notif_n_subdev, V4L2_MAX_SUBDEVS);
	struct device **dev;
	int i = 0;

	if (!notifier->v4l2_dev)
		return;

	dev = kmalloc(n_subdev * sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		dev_err(notifier->v4l2_dev->dev,
			"Failed to allocate device cache!\n");
	}

	mutex_lock(&notifier->lock);

	list_del(&notifier->list);

	list_for_each_entry_safe(sd, tmp, &notifier->done, async_list) {
		struct device *d;

		d = get_device(sd->dev);

		v4l2_async_cleanup(sd);

		/* If we handled USB devices, we'd have to lock the parent too */
		device_release_driver(d);

		if (notifier->unbind)
			notifier->unbind(notifier, sd, sd->asd);

		/*
		 * Store device at the device cache, in order to call
		 * put_device() on the final step
		 */
		if (dev)
			dev[i++] = d;
		else
			put_device(d);
	}

	mutex_unlock(&notifier->lock);

	/*
	 * Call device_attach() to reprobe devices
	 *
	 * NOTE: If dev allocation fails, i is 0, and the whole loop won't be
	 * executed.
	 */
	while (i--) {
		struct device *d = dev[i];

		if (d && device_attach(d) < 0) {
			const char *name = "(none)";
			int lock = device_trylock(d);

			if (lock && d->driver)
				name = d->driver->name;
			dev_err(d, "Failed to re-probe to %s\n", name);
			if (lock)
				device_unlock(d);
		}
		put_device(d);
	}
	kfree(dev);

	mutex_destroy(&notifier->lock);
	notifier->v4l2_dev = NULL;

	/*
	 * Don't care about the waiting list, it is initialised and populated
	 * upon notifier registration.
	 */
}
EXPORT_SYMBOL(v4l2_async_notifier_unregister);

int v4l2_async_register_subdev(struct v4l2_subdev *sd)
{
	struct v4l2_async_notifier *notifier;
	struct v4l2_async_notifier *tmp;

	/*
	 * No reference taken. The reference is held by the device
	 * (struct v4l2_subdev.dev), and async sub-device does not
	 * exist independently of the device at any point of time.
	 */
	if (!sd->of_node && sd->dev)
		sd->of_node = sd->dev->of_node;

	INIT_LIST_HEAD(&sd->async_list);

	list_for_each_entry_safe(notifier, tmp, &notifier_list, list) {
		struct v4l2_async_subdev *asd;

		/* TODO: FIXME: if this is called by ->bound() we will also iterate over the locked notifier */
		mutex_lock_nested(&notifier->lock, SINGLE_DEPTH_NESTING);
		asd = v4l2_async_belongs(notifier, sd);
		if (asd) {
			int ret = v4l2_async_test_notify(notifier, sd, asd);
			mutex_unlock(&notifier->lock);
			return ret;
		}
		mutex_unlock(&notifier->lock);
	}

	/* None matched, wait for hot-plugging */
	list_add(&sd->async_list, &subdev_list);

	return 0;
}
EXPORT_SYMBOL(v4l2_async_register_subdev);

void v4l2_async_unregister_subdev(struct v4l2_subdev *sd)
{
	struct v4l2_async_notifier *notifier = sd->notifier;

	if (!sd->asd) {
		if (!list_empty(&sd->async_list))
			v4l2_async_cleanup(sd);
		return;
	}

	mutex_lock_nested(&notifier->lock, SINGLE_DEPTH_NESTING);

	list_add(&sd->asd->list, &notifier->waiting);

	v4l2_async_cleanup(sd);

	if (notifier->unbind)
		notifier->unbind(notifier, sd, sd->asd);

	mutex_unlock(&notifier->lock);
}
EXPORT_SYMBOL(v4l2_async_unregister_subdev);
