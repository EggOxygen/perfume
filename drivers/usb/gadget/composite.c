/*
 * composite.c - infrastructure for Composite USB Gadgets
 *
 * Copyright (C) 2006-2008 David Brownell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */


#include <linux/kallsyms.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/utsname.h>

#include "gadget_chips.h"
#include <linux/usb/composite.h>
#include <linux/usb/msm_hsusb.h>
#include <asm/unaligned.h>

#include "u_os_desc.h"
#define SSUSB_GADGET_VBUS_DRAW 900 
#define SSUSB_GADGET_VBUS_DRAW_UNITS 8
#define HSUSB_GADGET_VBUS_DRAW_UNITS 2

#define MAC_FIRST_DT_LENGTH  18
#define WIN_LINUX_FIRST_DT1_LENGTH 8
#define WIN_LINUX_FIRST_DT2_LENGTH 64

static bool enable_l1_for_hs;
module_param(enable_l1_for_hs, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(enable_l1_for_hs, "Enable support for L1 LPM for HS devices");

struct usb_os_string {
	__u8	bLength;
	__u8	bDescriptorType;
	__u8	qwSignature[OS_STRING_QW_SIGN_LEN];
	__u8	bMS_VendorCode;
	__u8	bPad;
} __packed;


enum {
	OS_NOT_YET,
	OS_MAC,
	OS_LINUX,
	OS_WINDOWS,
};

static int os_type;

static struct usb_gadget_strings **get_containers_gs(
		struct usb_gadget_string_container *uc)
{
	return (struct usb_gadget_strings **)uc->stash;
}

static struct usb_descriptor_header**
next_ep_desc(struct usb_descriptor_header **t)
{
	for (; *t; t++) {
		if ((*t)->bDescriptorType == USB_DT_ENDPOINT)
			return t;
	}
	return NULL;
}

#define for_each_ep_desc(start, ep_desc) \
	for (ep_desc = next_ep_desc(start); \
	      ep_desc; ep_desc = next_ep_desc(ep_desc+1))

int config_ep_by_speed(struct usb_gadget *g,
			struct usb_function *f,
			struct usb_ep *_ep)
{
	struct usb_composite_dev	*cdev = get_gadget_data(g);
	struct usb_endpoint_descriptor *chosen_desc = NULL;
	struct usb_descriptor_header **speed_desc = NULL;

	struct usb_ss_ep_comp_descriptor *comp_desc = NULL;
	int want_comp_desc = 0;

	struct usb_descriptor_header **d_spd; 

	if (!g || !f || !_ep)
		return -EIO;

	
	switch (g->speed) {
	case USB_SPEED_SUPER:
		if (gadget_is_superspeed(g)) {
			speed_desc = f->ss_descriptors;
			want_comp_desc = 1;
			break;
		}
		
	case USB_SPEED_HIGH:
		if (gadget_is_dualspeed(g)) {
			speed_desc = f->hs_descriptors;
			break;
		}
		
	default:
		speed_desc = f->fs_descriptors;
	}
	
	for_each_ep_desc(speed_desc, d_spd) {
		chosen_desc = (struct usb_endpoint_descriptor *)*d_spd;
		if (chosen_desc->bEndpointAddress == _ep->address)
			goto ep_found;
	}
	return -EIO;

ep_found:
	
	_ep->maxpacket = usb_endpoint_maxp(chosen_desc);
	_ep->desc = chosen_desc;
	_ep->comp_desc = NULL;
	_ep->maxburst = 0;
	_ep->mult = 0;
	if (!want_comp_desc)
		return 0;

	comp_desc = (struct usb_ss_ep_comp_descriptor *)*(++d_spd);
	if (!comp_desc ||
	    (comp_desc->bDescriptorType != USB_DT_SS_ENDPOINT_COMP))
		return -EIO;
	_ep->comp_desc = comp_desc;
	if (g->speed == USB_SPEED_SUPER) {
		switch (usb_endpoint_type(_ep->desc)) {
		case USB_ENDPOINT_XFER_ISOC:
			
			_ep->mult = comp_desc->bmAttributes & 0x3;
		case USB_ENDPOINT_XFER_BULK:
		case USB_ENDPOINT_XFER_INT:
			_ep->maxburst = comp_desc->bMaxBurst + 1;
			break;
		default:
			if (comp_desc->bMaxBurst != 0)
				ERROR(cdev, "ep0 bMaxBurst must be 0\n");
			_ep->maxburst = 1;
			break;
		}
	}
	return 0;
}
EXPORT_SYMBOL_GPL(config_ep_by_speed);

int usb_add_function(struct usb_configuration *config,
		struct usb_function *function)
{
	int	value = -EINVAL;

	DBG(config->cdev, "adding '%s'/%p to config '%s'/%p\n",
			function->name, function,
			config->label, config);

	if (!function->set_alt || !function->disable)
		goto done;

	function->config = config;
	function->intf_id = -EINVAL;
	list_add_tail(&function->list, &config->functions);

	
	if (function->bind) {
		value = function->bind(config, function);
		if (value < 0) {
			list_del(&function->list);
			function->config = NULL;
		}
	} else
		value = 0;

	if (!config->fullspeed && function->fs_descriptors)
		config->fullspeed = true;
	if (!config->highspeed && function->hs_descriptors)
		config->highspeed = true;
	if (!config->superspeed && function->ss_descriptors)
		config->superspeed = true;

done:
	if (value)
		DBG(config->cdev, "adding '%s'/%p --> %d\n",
				function->name, function, value);
	return value;
}
EXPORT_SYMBOL_GPL(usb_add_function);

void usb_remove_function(struct usb_configuration *c, struct usb_function *f)
{
	if (f->disable)
		f->disable(f);

	bitmap_zero(f->endpoints, 32);
	list_del(&f->list);
	if (f->unbind)
		f->unbind(c, f);
}
EXPORT_SYMBOL_GPL(usb_remove_function);

int usb_function_deactivate(struct usb_function *function)
{
	struct usb_composite_dev	*cdev = function->config->cdev;
	unsigned long			flags;
	int				status = 0;

	spin_lock_irqsave(&cdev->lock, flags);

	if (cdev->deactivations == 0)
		status = usb_gadget_disconnect(cdev->gadget);
	if (status == 0)
		cdev->deactivations++;

	spin_unlock_irqrestore(&cdev->lock, flags);
	return status;
}
EXPORT_SYMBOL_GPL(usb_function_deactivate);

int usb_function_activate(struct usb_function *function)
{
	struct usb_composite_dev	*cdev = function->config->cdev;
	unsigned long			flags;
	int				status = 0;

	spin_lock_irqsave(&cdev->lock, flags);

	if (WARN_ON(cdev->deactivations == 0))
		status = -EINVAL;
	else {
		cdev->deactivations--;
		if (cdev->deactivations == 0)
			status = usb_gadget_connect(cdev->gadget);
	}

	spin_unlock_irqrestore(&cdev->lock, flags);
	return status;
}
EXPORT_SYMBOL_GPL(usb_function_activate);

int usb_interface_id(struct usb_configuration *config,
		struct usb_function *function)
{
	unsigned id = config->next_interface_id;

	if (id < MAX_CONFIG_INTERFACES) {
		config->interface[id] = function;
		if (function->intf_id < 0)
			function->intf_id = id;
		config->next_interface_id = id + 1;
		return id;
	}
	return -ENODEV;
}
EXPORT_SYMBOL_GPL(usb_interface_id);

static int usb_func_wakeup_int(struct usb_function *func)
{
	int ret;
	struct usb_gadget *gadget;

	if (!func || !func->config || !func->config->cdev ||
		!func->config->cdev->gadget)
		return -EINVAL;

	pr_debug("%s - %s function wakeup\n",
		__func__, func->name ? func->name : "");

	gadget = func->config->cdev->gadget;
	if ((gadget->speed != USB_SPEED_SUPER) || !func->func_wakeup_allowed) {
		DBG(func->config->cdev,
			"Function Wakeup is not possible. speed=%u, func_wakeup_allowed=%u\n",
			gadget->speed,
			func->func_wakeup_allowed);

		return -ENOTSUPP;
	}

	ret = usb_gadget_func_wakeup(gadget, func->intf_id);

	return ret;
}

int usb_func_wakeup(struct usb_function *func)
{
	int ret;
	unsigned long flags;

	pr_debug("%s function wakeup\n",
		func->name ? func->name : "");

	spin_lock_irqsave(&func->config->cdev->lock, flags);
	ret = usb_func_wakeup_int(func);
	if (ret == -EAGAIN) {
		DBG(func->config->cdev,
			"Function wakeup for %s could not complete due to suspend state. Delayed until after bus resume.\n",
			func->name ? func->name : "");
		ret = 0;
	} else if (ret < 0 && ret != -ENOTSUPP) {
		ERROR(func->config->cdev,
			"Failed to wake function %s from suspend state. ret=%d. Canceling USB request.\n",
			func->name ? func->name : "", ret);
	}

	spin_unlock_irqrestore(&func->config->cdev->lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(usb_func_wakeup);

int usb_func_ep_queue(struct usb_function *func, struct usb_ep *ep,
			       struct usb_request *req, gfp_t gfp_flags)
{
	int ret;
	struct usb_gadget *gadget;

	if (!func || !func->config || !func->config->cdev ||
			!func->config->cdev->gadget || !ep || !req) {
		ret = -EINVAL;
		goto done;
	}

	pr_debug("Function %s queueing new data into ep %u\n",
		func->name ? func->name : "", ep->address);

	gadget = func->config->cdev->gadget;

	if (func->func_is_suspended && func->func_wakeup_allowed) {
		ret = usb_gadget_func_wakeup(gadget, func->intf_id);
		if (ret == -EAGAIN) {
			pr_debug("bus suspended func wakeup for %s delayed until bus resume.\n",
				func->name ? func->name : "");
		} else if (ret < 0 && ret != -ENOTSUPP) {
			pr_err("Failed to wake function %s from suspend state. ret=%d.\n",
				func->name ? func->name : "", ret);
		}
		goto done;
	}

	if (func->func_is_suspended && !func->func_wakeup_allowed) {
		ret = -ENOTSUPP;
		goto done;
	}

	ret = usb_ep_queue(ep, req, gfp_flags);
done:
	return ret;
}
EXPORT_SYMBOL_GPL(usb_func_ep_queue);

static u8 encode_bMaxPower(enum usb_device_speed speed,
		struct usb_configuration *c)
{
	unsigned val = CONFIG_USB_GADGET_VBUS_DRAW;

	switch (speed) {
	case USB_SPEED_SUPER:
		
		val = SSUSB_GADGET_VBUS_DRAW;
		return (u8)(val / SSUSB_GADGET_VBUS_DRAW_UNITS);
	default:
		return DIV_ROUND_UP(val, HSUSB_GADGET_VBUS_DRAW_UNITS);
	}
}

extern struct usb_descriptor_header *ss_mtp_descs[];
extern struct usb_descriptor_header *hs_mtp_descs[];
extern struct usb_descriptor_header *fs_mtp_descs[];

static int config_buf(struct usb_configuration *config,
		enum usb_device_speed speed, void *buf, u8 type)
{
	struct usb_config_descriptor	*c = buf;
	void				*next = buf + USB_DT_CONFIG_SIZE;
	int				len;
	struct usb_function		*f;
	int				status;

	len = USB_COMP_EP0_BUFSIZ - USB_DT_CONFIG_SIZE;
	
	c = buf;
	c->bLength = USB_DT_CONFIG_SIZE;
	c->bDescriptorType = type;
	/* wTotalLength is written later */
	c->bNumInterfaces = config->next_interface_id;
	c->bConfigurationValue = config->bConfigurationValue;
	c->iConfiguration = config->iConfiguration;
	c->bmAttributes = USB_CONFIG_ATT_ONE | config->bmAttributes;
	c->bMaxPower = encode_bMaxPower(speed, config);

	
	if (config->descriptors) {
		status = usb_descriptor_fillbuf(next, len,
				config->descriptors);
		if (status < 0)
			return status;
		len -= status;
		next += status;
	}

	
	list_for_each_entry(f, &config->functions, list) {
		struct usb_descriptor_header **descriptors;

		switch (speed) {
		case USB_SPEED_SUPER:
			descriptors = f->ss_descriptors;
			if (!strcmp("mtp", f->name) && (os_type == OS_MAC))
				descriptors = ss_mtp_descs;
			break;
		case USB_SPEED_HIGH:
			descriptors = f->hs_descriptors;
			if (!strcmp("mtp", f->name) && (os_type == OS_MAC))
				descriptors = hs_mtp_descs;
			break;
		default:
			descriptors = f->fs_descriptors;
			if (!strcmp("mtp", f->name) && (os_type == OS_MAC))
				descriptors = fs_mtp_descs;
		}

		if (!descriptors)
			continue;
		status = usb_descriptor_fillbuf(next, len,
			(const struct usb_descriptor_header **) descriptors);
		if (status < 0)
			return status;
		len -= status;
		next += status;
	}

	len = next - buf;
	c->wTotalLength = cpu_to_le16(len);
	return len;
}

static int config_desc(struct usb_composite_dev *cdev, unsigned w_value)
{
	struct usb_gadget		*gadget = cdev->gadget;
	struct usb_configuration	*c;
	struct list_head		*pos;
	u8				type = w_value >> 8;
	enum usb_device_speed		speed = USB_SPEED_UNKNOWN;

	if (gadget->speed == USB_SPEED_SUPER)
		speed = gadget->speed;
	else if (gadget_is_dualspeed(gadget)) {
		int	hs = 0;
		if (gadget->speed == USB_SPEED_HIGH)
			hs = 1;
		if (type == USB_DT_OTHER_SPEED_CONFIG)
			hs = !hs;
		if (hs)
			speed = USB_SPEED_HIGH;

	}

	
	w_value &= 0xff;

	pos = &cdev->configs;
	c = cdev->os_desc_config;
	if (c)
		goto check_config;

	while ((pos = pos->next) !=  &cdev->configs) {
		c = list_entry(pos, typeof(*c), list);

		
		if (c == cdev->os_desc_config)
			continue;

check_config:
		
		switch (speed) {
		case USB_SPEED_SUPER:
			if (!c->superspeed)
				continue;
			break;
		case USB_SPEED_HIGH:
			if (!c->highspeed)
				continue;
			break;
		default:
			if (!c->fullspeed)
				continue;
		}

		if (w_value == 0)
			return config_buf(c, speed, cdev->req->buf, type);
		w_value--;
	}
	return -EINVAL;
}

static int count_configs(struct usb_composite_dev *cdev, unsigned type)
{
	struct usb_gadget		*gadget = cdev->gadget;
	struct usb_configuration	*c;
	unsigned			count = 0;
	int				hs = 0;
	int				ss = 0;

	if (gadget_is_dualspeed(gadget)) {
		if (gadget->speed == USB_SPEED_HIGH)
			hs = 1;
		if (gadget->speed == USB_SPEED_SUPER)
			ss = 1;
		if (type == USB_DT_DEVICE_QUALIFIER)
			hs = !hs;
	}
	list_for_each_entry(c, &cdev->configs, list) {
		
		if (ss) {
			if (!c->superspeed)
				continue;
		} else if (hs) {
			if (!c->highspeed)
				continue;
		} else {
			if (!c->fullspeed)
				continue;
		}
		count++;
	}
	return count;
}

static int bos_desc(struct usb_composite_dev *cdev)
{
	struct usb_ext_cap_descriptor	*usb_ext;
	struct usb_ss_cap_descriptor	*ss_cap;
	struct usb_dcd_config_params	dcd_config_params;
	struct usb_bos_descriptor	*bos = cdev->req->buf;

	bos->bLength = USB_DT_BOS_SIZE;
	bos->bDescriptorType = USB_DT_BOS;

	bos->wTotalLength = cpu_to_le16(USB_DT_BOS_SIZE);
	bos->bNumDeviceCaps = 0;

	usb_ext = cdev->req->buf + le16_to_cpu(bos->wTotalLength);
	bos->bNumDeviceCaps++;
	le16_add_cpu(&bos->wTotalLength, USB_DT_USB_EXT_CAP_SIZE);
	usb_ext->bLength = USB_DT_USB_EXT_CAP_SIZE;
	usb_ext->bDescriptorType = USB_DT_DEVICE_CAPABILITY;
	usb_ext->bDevCapabilityType = USB_CAP_TYPE_EXT;
	usb_ext->bmAttributes = cpu_to_le32(USB_LPM_SUPPORT);

	if (gadget_is_superspeed(cdev->gadget)) {
		ss_cap = cdev->req->buf + le16_to_cpu(bos->wTotalLength);
		bos->bNumDeviceCaps++;
		le16_add_cpu(&bos->wTotalLength, USB_DT_USB_SS_CAP_SIZE);
		ss_cap->bLength = USB_DT_USB_SS_CAP_SIZE;
		ss_cap->bDescriptorType = USB_DT_DEVICE_CAPABILITY;
		ss_cap->bDevCapabilityType = USB_SS_CAP_TYPE;
		ss_cap->bmAttributes = 0; 
		ss_cap->wSpeedSupported = cpu_to_le16(USB_LOW_SPEED_OPERATION |
					USB_FULL_SPEED_OPERATION |
					USB_HIGH_SPEED_OPERATION |
					USB_5GBPS_OPERATION);
		ss_cap->bFunctionalitySupport = USB_LOW_SPEED_OPERATION;

		
		if (cdev->gadget->ops->get_config_params)
			cdev->gadget->ops->get_config_params
				(&dcd_config_params);
		else {
			dcd_config_params.bU1devExitLat =
				USB_DEFAULT_U1_DEV_EXIT_LAT;
			dcd_config_params.bU2DevExitLat =
				cpu_to_le16(USB_DEFAULT_U2_DEV_EXIT_LAT);
		}
		ss_cap->bU1devExitLat = dcd_config_params.bU1devExitLat;
		ss_cap->bU2DevExitLat = dcd_config_params.bU2DevExitLat;
	}

	return le16_to_cpu(bos->wTotalLength);
}

static void device_qual(struct usb_composite_dev *cdev)
{
	struct usb_qualifier_descriptor	*qual = cdev->req->buf;

	qual->bLength = sizeof(*qual);
	qual->bDescriptorType = USB_DT_DEVICE_QUALIFIER;
	
	qual->bcdUSB = cdev->desc.bcdUSB;
	qual->bDeviceClass = cdev->desc.bDeviceClass;
	qual->bDeviceSubClass = cdev->desc.bDeviceSubClass;
	qual->bDeviceProtocol = cdev->desc.bDeviceProtocol;
	
	qual->bMaxPacketSize0 = cdev->gadget->ep0->maxpacket;
	qual->bNumConfigurations = count_configs(cdev, USB_DT_DEVICE_QUALIFIER);
	qual->bRESERVED = 0;
}


static void reset_config(struct usb_composite_dev *cdev)
{
	struct usb_function		*f;

	DBG(cdev, "reset config\n");

	if (!cdev->config) {
		pr_err("%s:cdev->config is already NULL\n", __func__);
		return;
	}

	list_for_each_entry(f, &cdev->config->functions, list) {
		if (f->disable)
			f->disable(f);

		
		f->func_is_suspended = false;
		f->func_wakeup_allowed = false;
		f->func_wakeup_pending = false;

		bitmap_zero(f->endpoints, 32);
	}
	cdev->config = NULL;
	cdev->delayed_status = 0;
}

static int set_config(struct usb_composite_dev *cdev,
		const struct usb_ctrlrequest *ctrl, unsigned number)
{
	struct usb_gadget	*gadget = cdev->gadget;
	struct usb_configuration *c = NULL;
	int			result = -EINVAL;
	unsigned		power = gadget_is_otg(gadget) ? 8 : 100;
	int			tmp;

	if (cdev->config && (cdev->config->bConfigurationValue == number)) {
		DBG(cdev, "already in the same config with value %d\n",
				number);
		return 0;
	}

	if (number) {
		list_for_each_entry(c, &cdev->configs, list) {
			if (c->bConfigurationValue == number) {
				if (cdev->config)
					reset_config(cdev);
				result = 0;
				break;
			}
		}
		if (result < 0)
			goto done;
	} else { 
		if (cdev->config)
			reset_config(cdev);
		result = 0;
	}

	INFO(cdev, "%s config #%d: %s\n",
	     usb_speed_string(gadget->speed),
	     number, c ? c->label : "unconfigured");

	if (!c)
		goto done;

	usb_gadget_set_state(gadget, USB_STATE_CONFIGURED);
	cdev->config = c;
	c->num_ineps_used = 0;
	c->num_outeps_used = 0;

	
	for (tmp = 0; tmp < MAX_CONFIG_INTERFACES; tmp++) {
		struct usb_function	*f = c->interface[tmp];
		struct usb_descriptor_header **descriptors;

		if (!f)
			break;

		switch (gadget->speed) {
		case USB_SPEED_SUPER:
			if (!f->ss_descriptors) {
				pr_err("%s(): No SS desc for function:%s\n",
							__func__, f->name);
				usb_gadget_set_state(gadget, USB_STATE_ADDRESS);
				return -EINVAL;
			}
			descriptors = f->ss_descriptors;
			break;
		case USB_SPEED_HIGH:
			descriptors = f->hs_descriptors;
			break;
		default:
			descriptors = f->fs_descriptors;
		}

		for (; *descriptors; ++descriptors) {
			struct usb_endpoint_descriptor *ep;
			int addr;

			if ((*descriptors)->bDescriptorType != USB_DT_ENDPOINT)
				continue;

			ep = (struct usb_endpoint_descriptor *)*descriptors;
			addr = ((ep->bEndpointAddress & 0x80) >> 3)
			     |  (ep->bEndpointAddress & 0x0f);
			set_bit(addr, f->endpoints);
			if (usb_endpoint_dir_in(ep))
				c->num_ineps_used++;
			else
				c->num_outeps_used++;
		}

		result = f->set_alt(f, tmp, 0);
		if (result < 0) {
			DBG(cdev, "interface %d (%s/%p) alt 0 --> %d\n",
					tmp, f->name, f, result);

			reset_config(cdev);
			goto done;
		}

		if (result == USB_GADGET_DELAYED_STATUS) {
			DBG(cdev,
			 "%s: interface %d (%s) requested delayed status\n",
					__func__, tmp, f->name);
			cdev->delayed_status++;
			DBG(cdev, "delayed_status count %d\n",
					cdev->delayed_status);
		}
	}

	
	if (gadget->speed == USB_SPEED_SUPER)
		power = SSUSB_GADGET_VBUS_DRAW;
	else
		power = CONFIG_USB_GADGET_VBUS_DRAW;

done:
	usb_gadget_vbus_draw(gadget, power);
	if (result >= 0 && cdev->delayed_status)
		result = USB_GADGET_DELAYED_STATUS;
	return result;
}

int usb_add_config_only(struct usb_composite_dev *cdev,
		struct usb_configuration *config)
{
	struct usb_configuration *c;

	if (!config->bConfigurationValue)
		return -EINVAL;

	
	list_for_each_entry(c, &cdev->configs, list) {
		if (c->bConfigurationValue == config->bConfigurationValue)
			return -EBUSY;
	}

	config->cdev = cdev;
	list_add_tail(&config->list, &cdev->configs);

	INIT_LIST_HEAD(&config->functions);
	config->next_interface_id = 0;
	memset(config->interface, 0, sizeof(config->interface));

	return 0;
}
EXPORT_SYMBOL_GPL(usb_add_config_only);

int usb_add_config(struct usb_composite_dev *cdev,
		struct usb_configuration *config,
		int (*bind)(struct usb_configuration *))
{
	int				status = -EINVAL;

	if (!bind)
		goto done;

	DBG(cdev, "adding config #%u '%s'/%p\n",
			config->bConfigurationValue,
			config->label, config);

	status = usb_add_config_only(cdev, config);
	if (status)
		goto done;

	status = bind(config);
	if (status < 0) {
		while (!list_empty(&config->functions)) {
			struct usb_function		*f;

			f = list_first_entry(&config->functions,
					struct usb_function, list);
			list_del(&f->list);
			if (f->unbind) {
				DBG(cdev, "unbind function '%s'/%p\n",
					f->name, f);
				f->unbind(config, f);
				
			}
		}
		list_del(&config->list);
		config->cdev = NULL;
	} else {
		unsigned	i;

		DBG(cdev, "cfg %d/%p speeds:%s%s%s\n",
			config->bConfigurationValue, config,
			config->superspeed ? " super" : "",
			config->highspeed ? " high" : "",
			config->fullspeed
				? (gadget_is_dualspeed(cdev->gadget)
					? " full"
					: " full/low")
				: "");

		for (i = 0; i < MAX_CONFIG_INTERFACES; i++) {
			struct usb_function	*f = config->interface[i];

			if (!f)
				continue;
			DBG(cdev, "  interface %d = %s/%p\n",
				i, f->name, f);
		}
	}

	usb_ep_autoconfig_reset(cdev->gadget);

done:
	if (status)
		DBG(cdev, "added config '%s'/%u --> %d\n", config->label,
				config->bConfigurationValue, status);
	return status;
}
EXPORT_SYMBOL_GPL(usb_add_config);

static void unbind_config(struct usb_composite_dev *cdev,
			      struct usb_configuration *config)
{
	while (!list_empty(&config->functions)) {
		struct usb_function		*f;

		f = list_first_entry(&config->functions,
				struct usb_function, list);
		list_del(&f->list);
		if (f->unbind) {
			DBG(cdev, "unbind function '%s'/%p\n", f->name, f);
			f->unbind(config, f);
			
		}
	}
	if (config->unbind) {
		DBG(cdev, "unbind config '%s'/%p\n", config->label, config);
		config->unbind(config);
			
	}
}

void usb_remove_config(struct usb_composite_dev *cdev,
		      struct usb_configuration *config)
{
	unsigned long flags;

	spin_lock_irqsave(&cdev->lock, flags);

	if (WARN_ON(!config->cdev)) {
		spin_unlock_irqrestore(&cdev->lock, flags);
		return;
	}

	if (cdev->config == config) {
		if (!gadget_is_dwc3(cdev->gadget) && !cdev->suspended) {
			spin_unlock_irqrestore(&cdev->lock, flags);
			msm_do_bam_disable_enable(CI_CTRL);
			spin_lock_irqsave(&cdev->lock, flags);
		}
		reset_config(cdev);
	}

	list_del(&config->list);

	spin_unlock_irqrestore(&cdev->lock, flags);

	os_type = OS_NOT_YET;
#ifdef CONFIG_HTC_USB_DEBUG_FLAG
	printk("[USB]%s unbind+\n",__func__);
#endif

	unbind_config(cdev, config);
}



static void collect_langs(struct usb_gadget_strings **sp, __le16 *buf)
{
	const struct usb_gadget_strings	*s;
	__le16				language;
	__le16				*tmp;

	while (*sp) {
		s = *sp;
		language = cpu_to_le16(s->language);
		for (tmp = buf; *tmp && tmp < &buf[126]; tmp++) {
			if (*tmp == language)
				goto repeat;
		}
		*tmp++ = language;
repeat:
		sp++;
	}
}

static int lookup_string(
	struct usb_gadget_strings	**sp,
	void				*buf,
	u16				language,
	int				id
)
{
	struct usb_gadget_strings	*s;
	int				value;

	while (*sp) {
		s = *sp++;
		if (s->language != language)
			continue;
		value = usb_gadget_get_string(s, id, buf);
		if (value > 0)
			return value;
	}
	return -EINVAL;
}

static int get_string(struct usb_composite_dev *cdev,
		void *buf, u16 language, int id)
{
	struct usb_composite_driver	*composite = cdev->driver;
	struct usb_gadget_string_container *uc;
	struct usb_configuration	*c;
	struct usb_function		*f;
	int				len;


	
	if (id == 0) {
		struct usb_string_descriptor	*s = buf;
		struct usb_gadget_strings	**sp;

		memset(s, 0, 256);
		s->bDescriptorType = USB_DT_STRING;

		sp = composite->strings;
		if (sp)
			collect_langs(sp, s->wData);

		list_for_each_entry(c, &cdev->configs, list) {
			sp = c->strings;
			if (sp)
				collect_langs(sp, s->wData);

			list_for_each_entry(f, &c->functions, list) {
				sp = f->strings;
				if (sp)
					collect_langs(sp, s->wData);
			}
		}
		list_for_each_entry(uc, &cdev->gstrings, list) {
			struct usb_gadget_strings **sp;

			sp = get_containers_gs(uc);
			collect_langs(sp, s->wData);
		}

		for (len = 0; len <= 126 && s->wData[len]; len++)
			continue;
		if (!len)
			return -EINVAL;

		s->bLength = 2 * (len + 1);
		return s->bLength;
	}

	if (cdev->use_os_string && language == 0 && id == OS_STRING_IDX) {
		struct usb_os_string *b = buf;
		b->bLength = sizeof(*b);
		b->bDescriptorType = USB_DT_STRING;
		compiletime_assert(
			sizeof(b->qwSignature) == sizeof(cdev->qw_sign),
			"qwSignature size must be equal to qw_sign");
		memcpy(&b->qwSignature, cdev->qw_sign, sizeof(b->qwSignature));
		b->bMS_VendorCode = cdev->b_vendor_code;
		b->bPad = 0;
		return sizeof(*b);
	}

	list_for_each_entry(uc, &cdev->gstrings, list) {
		struct usb_gadget_strings **sp;

		sp = get_containers_gs(uc);
		len = lookup_string(sp, buf, language, id);
		if (len > 0)
			return len;
	}

	if (composite->strings) {
		len = lookup_string(composite->strings, buf, language, id);
		if (len > 0)
			return len;
	}
	list_for_each_entry(c, &cdev->configs, list) {
		if (c->strings) {
			len = lookup_string(c->strings, buf, language, id);
			if (len > 0)
				return len;
		}
		list_for_each_entry(f, &c->functions, list) {
			if (!f->strings)
				continue;
			len = lookup_string(f->strings, buf, language, id);
			if (len > 0)
				return len;
		}
	}
	return -EINVAL;
}

int usb_string_id(struct usb_composite_dev *cdev)
{
	if (cdev->next_string_id < 254) {
		
		cdev->next_string_id++;
		return cdev->next_string_id;
	}
	return -ENODEV;
}
EXPORT_SYMBOL_GPL(usb_string_id);

int usb_string_ids_tab(struct usb_composite_dev *cdev, struct usb_string *str)
{
	int next = cdev->next_string_id;

	for (; str->s; ++str) {
		if (unlikely(next >= 254))
			return -ENODEV;
		str->id = ++next;
	}

	cdev->next_string_id = next;

	return 0;
}
EXPORT_SYMBOL_GPL(usb_string_ids_tab);

static struct usb_gadget_string_container *copy_gadget_strings(
		struct usb_gadget_strings **sp, unsigned n_gstrings,
		unsigned n_strings)
{
	struct usb_gadget_string_container *uc;
	struct usb_gadget_strings **gs_array;
	struct usb_gadget_strings *gs;
	struct usb_string *s;
	unsigned mem;
	unsigned n_gs;
	unsigned n_s;
	void *stash;

	mem = sizeof(*uc);
	mem += sizeof(void *) * (n_gstrings + 1);
	mem += sizeof(struct usb_gadget_strings) * n_gstrings;
	mem += sizeof(struct usb_string) * (n_strings + 1) * (n_gstrings);
	uc = kmalloc(mem, GFP_KERNEL);
	if (!uc)
		return ERR_PTR(-ENOMEM);
	gs_array = get_containers_gs(uc);
	stash = uc->stash;
	stash += sizeof(void *) * (n_gstrings + 1);
	for (n_gs = 0; n_gs < n_gstrings; n_gs++) {
		struct usb_string *org_s;

		gs_array[n_gs] = stash;
		gs = gs_array[n_gs];
		stash += sizeof(struct usb_gadget_strings);
		gs->language = sp[n_gs]->language;
		gs->strings = stash;
		org_s = sp[n_gs]->strings;

		for (n_s = 0; n_s < n_strings; n_s++) {
			s = stash;
			stash += sizeof(struct usb_string);
			if (org_s->s)
				s->s = org_s->s;
			else
				s->s = "";
			org_s++;
		}
		s = stash;
		s->s = NULL;
		stash += sizeof(struct usb_string);

	}
	gs_array[n_gs] = NULL;
	return uc;
}

static struct usb_gadget_string_container *find_gadget_strings(
		struct usb_composite_dev *cdev,
		struct usb_gadget_strings **sp, unsigned n_strings)
{
	struct usb_gadget_string_container *uc;

	list_for_each_entry(uc, &cdev->gstrings, list) {
		struct usb_gadget_strings **org_gs;
		struct usb_string *org_s, *s;

		org_gs = get_containers_gs(uc);

		if (sp[0]->language != org_gs[0]->language)
			continue;

		org_s = org_gs[0]->strings;
		s = sp[0]->strings;

		if (s->s == org_s->s)
			return uc;
	}

	return NULL;
}

struct usb_string *usb_gstrings_attach(struct usb_composite_dev *cdev,
		struct usb_gadget_strings **sp, unsigned n_strings)
{
	struct usb_gadget_string_container *uc;
	struct usb_gadget_strings **n_gs;
	unsigned n_gstrings = 0;
	unsigned i;
	int ret;

	for (i = 0; sp[i]; i++)
		n_gstrings++;

	if (!n_gstrings)
		return ERR_PTR(-EINVAL);

	
	uc = find_gadget_strings(cdev, sp, n_strings);
	if (uc) {
		n_gs = get_containers_gs(uc);
		return n_gs[0]->strings;
	}

	uc = copy_gadget_strings(sp, n_gstrings, n_strings);
	if (IS_ERR(uc))
		return ERR_CAST(uc);

	n_gs = get_containers_gs(uc);
	ret = usb_string_ids_tab(cdev, n_gs[0]->strings);
	if (ret)
		goto err;

	for (i = 1; i < n_gstrings; i++) {
		struct usb_string *m_s;
		struct usb_string *s;
		unsigned n;

		m_s = n_gs[0]->strings;
		s = n_gs[i]->strings;
		for (n = 0; n < n_strings; n++) {
			s->id = m_s->id;
			s++;
			m_s++;
		}
	}
	list_add_tail(&uc->list, &cdev->gstrings);
	return n_gs[0]->strings;
err:
	kfree(uc);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(usb_gstrings_attach);

int usb_string_ids_n(struct usb_composite_dev *c, unsigned n)
{
	unsigned next = c->next_string_id;
	if (unlikely(n > 254 || (unsigned)next + n > 254))
		return -ENODEV;
	c->next_string_id += n;
	return next + 1;
}
EXPORT_SYMBOL_GPL(usb_string_ids_n);


static void composite_setup_complete(struct usb_ep *ep, struct usb_request *req)
{
	if (req->status || req->actual != req->length)
		DBG((struct usb_composite_dev *) ep->driver_data,
				"setup complete --> %d, %d/%d\n",
				req->status, req->actual, req->length);
}

/*
 * Copyright (C) 2015 HTC, Inc.
 * Author: HTC USB Team
 * The following two functions are used for get OS_type for HTC device.
 * In tranditional design,the length of request USB_DT_CONFIG will
 * different. MAC is 4, Windows is 255, Linux is 9. But for MAC 10.11,
 * both MAC/LINUX is 9. Therefore, the design check_MAC_or_LINUX is use
 * for distinguish OS_type. This function will store the first length
 * of USB_DT_STRING and USB_DT_DEVICE and base on the value to get os
 * type.
 * 1. The length of first USB_DT_STRING is 2 and USB_DT_DEVICE is 18
 * for MAC.
 * 2. The length of first USB_DT_DEVICE is 8/64 for Windows/Linux.
 */
static void check_MAC_or_LINUX(int first_dt_length, int first_string_length)
{
	switch (first_dt_length)
	{
		case MAC_FIRST_DT_LENGTH:
			if (first_string_length == 2)
				os_type = OS_MAC;
			break;
		case WIN_LINUX_FIRST_DT1_LENGTH:
		case WIN_LINUX_FIRST_DT2_LENGTH:
			os_type = OS_LINUX;
			break;
		default:
			break;
	}

	if (os_type == OS_LINUX)
		pr_info("%s: Re detect as OS_LINUX \n", __func__);
	else if (os_type == OS_MAC)
		pr_info("%s: Re detect as OS_MAC \n", __func__);
	else
		pr_info("unknown os type\n");
}

static void get_os_type(int length)
{
	if (length == 4) {
		pr_info("%s: OS_MAC\n", __func__);
		os_type = OS_MAC;
	} else if (length == 255) {
		pr_info("%s: OS_WINDOWS\n", __func__);
		os_type = OS_WINDOWS;
	} else if (length == 9 && os_type != OS_WINDOWS && os_type !=OS_MAC) {
		check_MAC_or_LINUX(first_dt_w_length,first_string_w_length);
	}
}

static int count_ext_compat(struct usb_configuration *c)
{
	int i, res;

	res = 0;
	for (i = 0; i < c->next_interface_id; ++i) {
		struct usb_function *f;
		int j;

		f = c->interface[i];
		for (j = 0; j < f->os_desc_n; ++j) {
			struct usb_os_desc *d;

			if (i != f->os_desc_table[j].if_id)
				continue;
			d = f->os_desc_table[j].os_desc;
			if (d && d->ext_compat_id)
				++res;
		}
	}
	BUG_ON(res > 255);
	return res;
}

static void fill_ext_compat(struct usb_configuration *c, u8 *buf)
{
	int i, count;

	count = 16;
	for (i = 0; i < c->next_interface_id; ++i) {
		struct usb_function *f;
		int j;

		f = c->interface[i];
		for (j = 0; j < f->os_desc_n; ++j) {
			struct usb_os_desc *d;

			if (i != f->os_desc_table[j].if_id)
				continue;
			d = f->os_desc_table[j].os_desc;
			if (d && d->ext_compat_id) {
				*buf++ = i;
				*buf++ = 0x01;
				memcpy(buf, d->ext_compat_id, 16);
				buf += 22;
			} else {
				++buf;
				*buf = 0x01;
				buf += 23;
			}
			count += 24;
			if (count >= 4096)
				return;
		}
	}
}

static int count_ext_prop(struct usb_configuration *c, int interface)
{
	struct usb_function *f;
	int j;

	f = c->interface[interface];
	for (j = 0; j < f->os_desc_n; ++j) {
		struct usb_os_desc *d;

		if (interface != f->os_desc_table[j].if_id)
			continue;
		d = f->os_desc_table[j].os_desc;
		if (d && d->ext_compat_id)
			return d->ext_prop_count;
	}
	return 0;
}

static int len_ext_prop(struct usb_configuration *c, int interface)
{
	struct usb_function *f;
	struct usb_os_desc *d;
	int j, res;

	res = 10; 
	f = c->interface[interface];
	for (j = 0; j < f->os_desc_n; ++j) {
		if (interface != f->os_desc_table[j].if_id)
			continue;
		d = f->os_desc_table[j].os_desc;
		if (d)
			return min(res + d->ext_prop_len, 4096);
	}
	return res;
}

static int fill_ext_prop(struct usb_configuration *c, int interface, u8 *buf)
{
	struct usb_function *f;
	struct usb_os_desc *d;
	struct usb_os_desc_ext_prop *ext_prop;
	int j, count, n, ret;
	u8 *start = buf;

	f = c->interface[interface];
	for (j = 0; j < f->os_desc_n; ++j) {
		if (interface != f->os_desc_table[j].if_id)
			continue;
		d = f->os_desc_table[j].os_desc;
		if (d)
			list_for_each_entry(ext_prop, &d->ext_prop, entry) {
				
				n = buf - start;
				if (n >= 4086)
					return 0;

				count = ext_prop->data_len +
					ext_prop->name_len + 14;
				if (count > 4086 - n)
					return -EINVAL;
				usb_ext_prop_put_size(buf, count);
				usb_ext_prop_put_type(buf, ext_prop->type);
				ret = usb_ext_prop_put_name(buf, ext_prop->name,
							    ext_prop->name_len);
				if (ret < 0)
					return ret;
				switch (ext_prop->type) {
				case USB_EXT_PROP_UNICODE:
				case USB_EXT_PROP_UNICODE_ENV:
				case USB_EXT_PROP_UNICODE_LINK:
					usb_ext_prop_put_unicode(buf, ret,
							 ext_prop->data,
							 ext_prop->data_len);
					break;
				case USB_EXT_PROP_BINARY:
					usb_ext_prop_put_binary(buf, ret,
							ext_prop->data,
							ext_prop->data_len);
					break;
				case USB_EXT_PROP_LE32:
					
				case USB_EXT_PROP_BE32:
					
				default:
					return -EINVAL;
				}
				buf += count;
			}
	}

	return 0;
}

int
composite_setup(struct usb_gadget *gadget, const struct usb_ctrlrequest *ctrl)
{
	struct usb_composite_dev	*cdev = get_gadget_data(gadget);
	struct usb_request		*req = cdev->req;
	int				value = -EOPNOTSUPP;
	int				status = 0;
	u16				w_index = le16_to_cpu(ctrl->wIndex);
	u8				intf = w_index & 0xFF;
	u16				w_value = le16_to_cpu(ctrl->wValue);
	u16				w_length = le16_to_cpu(ctrl->wLength);
	struct usb_function		*f = NULL;
	u8				endp;


	if (w_length > USB_COMP_EP0_BUFSIZ)
		return value;

	req->zero = 0;
	req->complete = composite_setup_complete;
	req->length = 0;
	gadget->ep0->driver_data = cdev;

	switch (ctrl->bRequest) {

	
	case USB_REQ_GET_DESCRIPTOR:
		if (ctrl->bRequestType != USB_DIR_IN)
			goto unknown;
		switch (w_value >> 8) {

		case USB_DT_DEVICE:
			if (first_dt_w_length == 0) {
				first_dt_w_length = w_length;
				printk("[USB] first_dt_w_length = %d \n",first_dt_w_length);
			}
			cdev->desc.bNumConfigurations =
				count_configs(cdev, USB_DT_DEVICE);
			if (cdev->desc.bNumConfigurations == 0) {
				pr_err("%s:config is not active. send stall\n",
								__func__);
				break;
			}

			cdev->desc.bMaxPacketSize0 =
				cdev->gadget->ep0->maxpacket;
			if (gadget_is_superspeed(gadget)) {
				if (gadget->speed >= USB_SPEED_SUPER) {
					cdev->desc.bcdUSB = cpu_to_le16(0x0300);
					cdev->desc.bMaxPacketSize0 = 9;
				} else if (gadget->l1_supported ||
						enable_l1_for_hs) {
					cdev->desc.bcdUSB = cpu_to_le16(0x0210);
					DBG(cdev,
					"Config HS device with LPM(L1)\n");
				} else {
					cdev->desc.bcdUSB = cpu_to_le16(0x0200);
				}
			} else if (gadget->l1_supported) {
				cdev->desc.bcdUSB = cpu_to_le16(0x0210);
				DBG(cdev, "Config HS device with LPM(L1)\n");
			}

			value = min(w_length, (u16) sizeof cdev->desc);
			memcpy(req->buf, &cdev->desc, value);
			break;
		case USB_DT_DEVICE_QUALIFIER:
			if (!gadget_is_dualspeed(gadget) ||
			    gadget->speed >= USB_SPEED_SUPER)
				break;
			spin_lock(&cdev->lock);
			device_qual(cdev);
			spin_unlock(&cdev->lock);
			value = min_t(int, w_length,
				sizeof(struct usb_qualifier_descriptor));
			break;
		case USB_DT_OTHER_SPEED_CONFIG:
			if (!gadget_is_dualspeed(gadget) ||
			    gadget->speed >= USB_SPEED_SUPER)
				break;
			
		case USB_DT_CONFIG:
			get_os_type(w_length);
			spin_lock(&cdev->lock);
			value = config_desc(cdev, w_value);
			spin_unlock(&cdev->lock);
			if (value >= 0)
				value = min(w_length, (u16) value);
			break;
		case USB_DT_STRING:
			if (first_string_w_length == 0) {
				first_string_w_length = w_length;
				printk("[USB] first_string_w_length = %d \n",first_string_w_length);
			}
			spin_lock(&cdev->lock);
			value = get_string(cdev, req->buf,
					w_index, w_value & 0xff);
			spin_unlock(&cdev->lock);
			if (value >= 0)
				value = min(w_length, (u16) value);
			break;
		case USB_DT_BOS:
			if ((gadget_is_superspeed(gadget) &&
				(gadget->speed >= USB_SPEED_SUPER))
				 || gadget->l1_supported) {
				value = bos_desc(cdev);
				value = min(w_length, (u16) value);
			}
			break;
		}
		break;

	
	case USB_REQ_SET_CONFIGURATION:
		if (ctrl->bRequestType != 0)
			goto unknown;
		if (gadget_is_otg(gadget)) {
			if (gadget->a_hnp_support)
				DBG(cdev, "HNP available\n");
			else if (gadget->a_alt_hnp_support)
				DBG(cdev, "HNP on another port\n");
			else
				VDBG(cdev, "HNP inactive\n");
		}
		spin_lock(&cdev->lock);
		value = set_config(cdev, ctrl, w_value);
		spin_unlock(&cdev->lock);
		break;
	case USB_REQ_GET_CONFIGURATION:
		if (ctrl->bRequestType != USB_DIR_IN)
			goto unknown;
		if (cdev->config)
			*(u8 *)req->buf = cdev->config->bConfigurationValue;
		else
			*(u8 *)req->buf = 0;
		value = min(w_length, (u16) 1);
		break;

	case USB_REQ_SET_INTERFACE:
		if (ctrl->bRequestType != USB_RECIP_INTERFACE)
			goto unknown;
		if (!cdev->config || intf >= MAX_CONFIG_INTERFACES)
			break;
		f = cdev->config->interface[intf];
		if (!f)
			break;
		if (w_value && !f->set_alt)
			break;
		if (!w_value && cdev->config && !f->get_alt) {
			value = 0;
			break;
		}
		value = f->set_alt(f, w_index, w_value);
		if (value == USB_GADGET_DELAYED_STATUS) {
			DBG(cdev,
			 "%s: interface %d (%s) requested delayed status\n",
					__func__, intf, f->name);
			cdev->delayed_status++;
			DBG(cdev, "delayed_status count %d\n",
					cdev->delayed_status);
		}
		break;
	case USB_REQ_GET_INTERFACE:
		if (ctrl->bRequestType != (USB_DIR_IN|USB_RECIP_INTERFACE))
			goto unknown;
		if (!cdev->config || intf >= MAX_CONFIG_INTERFACES)
			break;
		f = cdev->config->interface[intf];
		if (!f)
			break;
		
		value = f->get_alt ? f->get_alt(f, w_index) : 0;
		if (value < 0)
			break;
		*((u8 *)req->buf) = value;
		value = min(w_length, (u16) 1);
		break;

	case USB_REQ_GET_STATUS:
		if (!gadget_is_superspeed(gadget))
			goto unknown;
		if (ctrl->bRequestType != (USB_DIR_IN | USB_RECIP_INTERFACE))
			goto unknown;
		value = 2;	
		put_unaligned_le16(0, req->buf);
		if (!cdev->config || intf >= MAX_CONFIG_INTERFACES)
			break;
		f = cdev->config->interface[intf];
		if (!f)
			break;
		status = f->get_status ? f->get_status(f) : 0;
		if (status < 0)
			break;
		put_unaligned_le16(status & 0x0000ffff, req->buf);
		break;
	case USB_REQ_CLEAR_FEATURE:
	case USB_REQ_SET_FEATURE:
		if (!gadget_is_superspeed(gadget))
			goto unknown;
		if (ctrl->bRequestType != (USB_DIR_OUT | USB_RECIP_INTERFACE))
			goto unknown;
		switch (w_value) {
		case USB_INTRF_FUNC_SUSPEND:
			if (!cdev->config || intf >= MAX_CONFIG_INTERFACES)
				break;
			f = cdev->config->interface[intf];
			if (!f)
				break;
			value = 0;
			if (f->func_suspend) {
				const u8 suspend_opt = w_index >> 8;

				value = f->func_suspend(f, suspend_opt);
				DBG(cdev, "%s function: FUNCTION_SUSPEND(%u)",
					f->name ? f->name : "", suspend_opt);
			}
			if (value < 0) {
				ERROR(cdev,
				      "func_suspend() returned error %d\n",
				      value);
				value = 0;
			}
			break;
		}
		break;
	default:
unknown:
		if (cdev->use_os_string && cdev->os_desc_config &&
		    (ctrl->bRequest & USB_TYPE_VENDOR) &&
		    ctrl->bRequest == cdev->b_vendor_code) {
			struct usb_request		*req;
			struct usb_configuration	*os_desc_cfg;
			u8				*buf;
			int				interface;
			int				count = 0;

			req = cdev->os_desc_req;
			req->complete = composite_setup_complete;
			buf = req->buf;
			os_desc_cfg = cdev->os_desc_config;
			memset(buf, 0, w_length);
			buf[5] = 0x01;
			switch (ctrl->bRequestType & USB_RECIP_MASK) {
			case USB_RECIP_DEVICE:
				if (w_index != 0x4 || (w_value >> 8))
					break;
				buf[6] = w_index;
				if (w_length == 0x10) {
					
					count = count_ext_compat(os_desc_cfg);
					buf[8] = count;
					count *= 24; 
					count += 16; 
					put_unaligned_le32(count, buf);
					value = w_length;
				} else {
					
					count = count_ext_compat(os_desc_cfg);
					buf[8] = count;
					count *= 24; 
					count += 16; 
					put_unaligned_le32(count, buf);
					buf += 16;
					fill_ext_compat(os_desc_cfg, buf);
					value = w_length;
				}
				break;
			case USB_RECIP_INTERFACE:
				if (w_index != 0x5 || (w_value >> 8))
					break;
				interface = w_value & 0xFF;
				buf[6] = w_index;
				if (w_length == 0x0A) {
					count = count_ext_prop(os_desc_cfg,
						interface);
					put_unaligned_le16(count, buf + 8);
					count = len_ext_prop(os_desc_cfg,
						interface);
					put_unaligned_le32(count, buf);

					value = w_length;
				} else {
					count = count_ext_prop(os_desc_cfg,
						interface);
					put_unaligned_le16(count, buf + 8);
					count = len_ext_prop(os_desc_cfg,
						interface);
					put_unaligned_le32(count, buf);
					buf += 10;
					value = fill_ext_prop(os_desc_cfg,
							      interface, buf);
					if (value < 0)
						return value;

					value = w_length;
				}
				break;
			}
			req->length = value;
			req->zero = value < w_length;
			value = usb_ep_queue(gadget->ep0, req, GFP_ATOMIC);
			if (value < 0) {
				DBG(cdev, "ep_queue --> %d\n", value);
				req->status = 0;
				composite_setup_complete(gadget->ep0, req);
			}
			return value;
		}

		VDBG(cdev,
			"non-core control req%02x.%02x v%04x i%04x l%d\n",
			ctrl->bRequestType, ctrl->bRequest,
			w_value, w_index, w_length);

		switch (ctrl->bRequestType & USB_RECIP_MASK) {
		case USB_RECIP_INTERFACE:
			if (!cdev->config || intf >= MAX_CONFIG_INTERFACES)
				break;
			f = cdev->config->interface[intf];
			break;

		case USB_RECIP_ENDPOINT:
			endp = ((w_index & 0x80) >> 3) | (w_index & 0x0f);
			list_for_each_entry(f, &cdev->config->functions, list) {
				if (test_bit(endp, f->endpoints))
					break;
			}
			if (&f->list == &cdev->config->functions)
				f = NULL;
			break;
		}

		if (f && f->setup)
			value = f->setup(f, ctrl);
		else {
			struct usb_configuration	*c;

			c = cdev->config;
			if (!c)
				goto done;

			
			if (c->setup) {
				value = c->setup(c, ctrl);
				goto done;
			}

			
			if (!list_is_singular(&c->functions))
				goto done;
			f = list_first_entry(&c->functions, struct usb_function,
					     list);
			if (f->setup)
				value = f->setup(f, ctrl);
		}
		if (value == USB_GADGET_DELAYED_STATUS) {
			DBG(cdev,
			 "%s: interface %d (%s) requested delayed status\n",
					__func__, intf, f->name);
			cdev->delayed_status++;
			DBG(cdev, "delayed_status count %d\n",
					cdev->delayed_status);
		}

		goto done;
	}

	
	if (value >= 0 && value != USB_GADGET_DELAYED_STATUS) {
		req->length = value;
		req->zero = value < w_length;
		value = usb_ep_queue(gadget->ep0, req, GFP_ATOMIC);
		if (value < 0) {
			DBG(cdev, "ep_queue --> %d\n", value);
			req->status = 0;
			composite_setup_complete(gadget->ep0, req);
		}
	} else if (value == USB_GADGET_DELAYED_STATUS && w_length != 0) {
		WARN(cdev,
			"%s: Delayed status not supported for w_length != 0",
			__func__);
	}

done:
	
	return value;
}

void composite_disconnect(struct usb_gadget *gadget)
{
	struct usb_composite_dev	*cdev = get_gadget_data(gadget);
	unsigned long			flags;

	if (cdev == NULL) {
		WARN(1, "%s: Calling disconnect on a Gadget that is \
			 not connected\n", __func__);
		return;
	}

	spin_lock_irqsave(&cdev->lock, flags);
	if (cdev->config) {
		if (!gadget_is_dwc3(gadget) && !cdev->suspended) {
			spin_unlock_irqrestore(&cdev->lock, flags);
			msm_do_bam_disable_enable(CI_CTRL);
			spin_lock_irqsave(&cdev->lock, flags);
		}
		reset_config(cdev);
	}
	if (cdev->driver->disconnect) {
		cdev->driver->disconnect(cdev);
		os_type = OS_NOT_YET;
	}
	if (cdev->delayed_status != 0) {
		INFO(cdev, "delayed status mismatch..resetting\n");
		cdev->delayed_status = 0;
	}
	spin_unlock_irqrestore(&cdev->lock, flags);
}

void composite_mute_disconnect(struct usb_gadget *gadget)
{
	struct usb_composite_dev	*cdev = get_gadget_data(gadget);
	unsigned long				flags;

	spin_lock_irqsave(&cdev->lock, flags);
	if (cdev->config)
		reset_config(cdev);

	if (cdev->delayed_status != 0) {
		INFO(cdev, "delayed status mismatch..resetting\n");
		cdev->delayed_status = 0;
	}
	spin_unlock_irqrestore(&cdev->lock, flags);
}

static ssize_t suspended_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct usb_gadget *gadget = dev_to_usb_gadget(dev);
	struct usb_composite_dev *cdev = get_gadget_data(gadget);

	return sprintf(buf, "%d\n", cdev->suspended);
}
static DEVICE_ATTR_RO(suspended);

static void __composite_unbind(struct usb_gadget *gadget, bool unbind_driver)
{
	struct usb_composite_dev	*cdev = get_gadget_data(gadget);

	WARN_ON(cdev->config);

	while (!list_empty(&cdev->configs)) {
		struct usb_configuration	*c;
		c = list_first_entry(&cdev->configs,
				struct usb_configuration, list);
		list_del(&c->list);
		unbind_config(cdev, c);
	}
	if (cdev->driver->unbind && unbind_driver)
		cdev->driver->unbind(cdev);

	composite_dev_cleanup(cdev);

	kfree(cdev->def_manufacturer);
	kfree(cdev);
	set_gadget_data(gadget, NULL);
}

static void composite_unbind(struct usb_gadget *gadget)
{
	__composite_unbind(gadget, true);
}

static void update_unchanged_dev_desc(struct usb_device_descriptor *new,
		const struct usb_device_descriptor *old)
{
	__le16 idVendor;
	__le16 idProduct;
	__le16 bcdDevice;
	u8 iSerialNumber;
	u8 iManufacturer;
	u8 iProduct;

	idVendor = new->idVendor;
	idProduct = new->idProduct;
	bcdDevice = new->bcdDevice;
	iSerialNumber = new->iSerialNumber;
	iManufacturer = new->iManufacturer;
	iProduct = new->iProduct;

	*new = *old;
	if (idVendor)
		new->idVendor = idVendor;
	if (idProduct)
		new->idProduct = idProduct;
	if (bcdDevice)
		new->bcdDevice = bcdDevice;
	else
		new->bcdDevice = cpu_to_le16(get_default_bcdDevice());
	if (iSerialNumber)
		new->iSerialNumber = iSerialNumber;
	if (iManufacturer)
		new->iManufacturer = iManufacturer;
	if (iProduct)
		new->iProduct = iProduct;
}

int composite_dev_prepare(struct usb_composite_driver *composite,
		struct usb_composite_dev *cdev)
{
	struct usb_gadget *gadget = cdev->gadget;
	int ret = -ENOMEM;

	
	cdev->req = usb_ep_alloc_request(gadget->ep0, GFP_KERNEL);
	if (!cdev->req)
		return -ENOMEM;

	cdev->req->buf = kmalloc(USB_COMP_EP0_BUFSIZ +
				(gadget->extra_buf_alloc), GFP_KERNEL);
	if (!cdev->req->buf)
		goto fail;

	ret = device_create_file(&gadget->dev, &dev_attr_suspended);
	if (ret)
		goto fail_dev;

	cdev->req->complete = composite_setup_complete;
	gadget->ep0->driver_data = cdev;

	cdev->driver = composite;

	if (CONFIG_USB_GADGET_VBUS_DRAW <= USB_SELF_POWER_VBUS_MAX_DRAW)
		usb_gadget_set_selfpowered(gadget);

	usb_ep_autoconfig_reset(gadget);
	return 0;
fail_dev:
	kfree(cdev->req->buf);
fail:
	usb_ep_free_request(gadget->ep0, cdev->req);
	cdev->req = NULL;
	return ret;
}

int composite_os_desc_req_prepare(struct usb_composite_dev *cdev,
				  struct usb_ep *ep0)
{
	int ret = 0;

	cdev->os_desc_req = usb_ep_alloc_request(ep0, GFP_KERNEL);
	if (!cdev->os_desc_req) {
		ret = PTR_ERR(cdev->os_desc_req);
		goto end;
	}

	
	cdev->os_desc_req->buf = kmalloc(4096, GFP_KERNEL);
	if (!cdev->os_desc_req->buf) {
		ret = PTR_ERR(cdev->os_desc_req->buf);
		kfree(cdev->os_desc_req);
		goto end;
	}
	cdev->os_desc_req->complete = composite_setup_complete;
end:
	return ret;
}

void composite_dev_cleanup(struct usb_composite_dev *cdev)
{
	struct usb_gadget_string_container *uc, *tmp;

	list_for_each_entry_safe(uc, tmp, &cdev->gstrings, list) {
		list_del(&uc->list);
		kfree(uc);
	}
	if (cdev->os_desc_req) {
		kfree(cdev->os_desc_req->buf);
		usb_ep_free_request(cdev->gadget->ep0, cdev->os_desc_req);
	}
	if (cdev->req) {
		kfree(cdev->req->buf);
		usb_ep_free_request(cdev->gadget->ep0, cdev->req);
	}
	cdev->next_string_id = 0;
	device_remove_file(&cdev->gadget->dev, &dev_attr_suspended);
}

static int composite_bind(struct usb_gadget *gadget,
		struct usb_gadget_driver *gdriver)
{
	struct usb_composite_dev	*cdev;
	struct usb_composite_driver	*composite = to_cdriver(gdriver);
	int				status = -ENOMEM;

	cdev = kzalloc(sizeof *cdev, GFP_KERNEL);
	if (!cdev)
		return status;

	spin_lock_init(&cdev->lock);
	cdev->gadget = gadget;
	set_gadget_data(gadget, cdev);
	INIT_LIST_HEAD(&cdev->configs);
	INIT_LIST_HEAD(&cdev->gstrings);

	status = composite_dev_prepare(composite, cdev);
	if (status)
		goto fail;

	status = composite->bind(cdev);
	if (status < 0)
		goto fail;

	if (cdev->use_os_string) {
		status = composite_os_desc_req_prepare(cdev, gadget->ep0);
		if (status)
			goto fail;
	}

	update_unchanged_dev_desc(&cdev->desc, composite->dev);

	
	if (composite->needs_serial && !cdev->desc.iSerialNumber)
		WARNING(cdev, "userspace failed to provide iSerialNumber\n");

	INFO(cdev, "%s ready\n", composite->name);
	return 0;

fail:
	__composite_unbind(gadget, false);
	return status;
}


static void
composite_suspend(struct usb_gadget *gadget)
{
	struct usb_composite_dev	*cdev = get_gadget_data(gadget);
	struct usb_function		*f;
	unsigned long			flags;

	DBG(cdev, "suspend\n");
	spin_lock_irqsave(&cdev->lock, flags);
	if (cdev->config) {
		list_for_each_entry(f, &cdev->config->functions, list) {
			if (f->suspend)
				f->suspend(f);
		}
	}
	if (cdev->driver->suspend)
		cdev->driver->suspend(cdev);

	cdev->suspended = 1;
	spin_unlock_irqrestore(&cdev->lock, flags);

	usb_gadget_vbus_draw(gadget, 2);
}

static void
composite_resume(struct usb_gadget *gadget)
{
	struct usb_composite_dev	*cdev = get_gadget_data(gadget);
	struct usb_function		*f;
	u16				maxpower;
	int ret;
	unsigned long			flags;

	DBG(cdev, "resume\n");
	if (cdev->driver->resume)
		cdev->driver->resume(cdev);

	spin_lock_irqsave(&cdev->lock, flags);
	if (cdev->config) {
		list_for_each_entry(f, &cdev->config->functions, list) {
			ret = usb_func_wakeup_int(f);
			if (ret) {
				if (ret == -EAGAIN) {
					ERROR(f->config->cdev,
						"Function wakeup for %s could not complete due to suspend state.\n",
						f->name ? f->name : "");
					break;
				} else if (ret != -ENOTSUPP) {
					ERROR(f->config->cdev,
						"Failed to wake function %s from suspend state. ret=%d. Canceling USB request.\n",
						f->name ? f->name : "",
						ret);
				}
			}

			if (f->resume)
				f->resume(f);
		}

		maxpower = cdev->config->MaxPower;

		usb_gadget_vbus_draw(gadget, maxpower ?
			maxpower : CONFIG_USB_GADGET_VBUS_DRAW);
	}

	spin_unlock_irqrestore(&cdev->lock, flags);
	cdev->suspended = 0;
}


static const struct usb_gadget_driver composite_driver_template = {
	.bind		= composite_bind,
	.unbind		= composite_unbind,

	.setup		= composite_setup,
	.reset		= composite_disconnect,
	.disconnect	= composite_disconnect,

	.mute_disconnect = composite_mute_disconnect,   

	.suspend	= composite_suspend,
	.resume		= composite_resume,

	.driver	= {
		.owner		= THIS_MODULE,
	},
};

int usb_composite_probe(struct usb_composite_driver *driver)
{
	struct usb_gadget_driver *gadget_driver;
	u8 core_id;

	if (!driver || !driver->dev || !driver->bind)
		return -EINVAL;

	if (!driver->name)
		driver->name = "composite";

	core_id = driver->gadget_driver.usb_core_id;
	driver->gadget_driver = composite_driver_template;
	gadget_driver = &driver->gadget_driver;

	gadget_driver->function =  (char *) driver->name;
	gadget_driver->driver.name = driver->name;
	gadget_driver->max_speed = driver->max_speed;

	if (core_id)
		gadget_driver->usb_core_id = core_id;

	pr_debug("%s(): gadget_driver->usb_core_id:%d\n", __func__,
					gadget_driver->usb_core_id);
	return usb_gadget_probe_driver(gadget_driver);
}
EXPORT_SYMBOL_GPL(usb_composite_probe);

void usb_composite_unregister(struct usb_composite_driver *driver)
{
	usb_gadget_unregister_driver(&driver->gadget_driver);
}
EXPORT_SYMBOL_GPL(usb_composite_unregister);

void usb_composite_setup_continue(struct usb_composite_dev *cdev)
{
	int			value;
	struct usb_request	*req = cdev->req;
	unsigned long		flags;

	DBG(cdev, "%s\n", __func__);
	spin_lock_irqsave(&cdev->lock, flags);

	if (cdev->delayed_status == 0) {
		WARN(cdev, "%s: Unexpected call\n", __func__);

	} else if (--cdev->delayed_status == 0) {
		DBG(cdev, "%s: Completing delayed status\n", __func__);
		req->length = 0;
		value = usb_ep_queue(cdev->gadget->ep0, req, GFP_ATOMIC);
		if (value < 0) {
			DBG(cdev, "ep_queue --> %d\n", value);
			req->status = 0;
			composite_setup_complete(cdev->gadget->ep0, req);
		}
	}

	spin_unlock_irqrestore(&cdev->lock, flags);
}
EXPORT_SYMBOL_GPL(usb_composite_setup_continue);

static char *composite_default_mfr(struct usb_gadget *gadget)
{
	char *mfr;
	int len;

	len = snprintf(NULL, 0, "%s %s with %s", init_utsname()->sysname,
			init_utsname()->release, gadget->name);
	len++;
	mfr = kmalloc(len, GFP_KERNEL);
	if (!mfr)
		return NULL;
	snprintf(mfr, len, "%s %s with %s", init_utsname()->sysname,
			init_utsname()->release, gadget->name);
	return mfr;
}

void usb_composite_overwrite_options(struct usb_composite_dev *cdev,
		struct usb_composite_overwrite *covr)
{
	struct usb_device_descriptor	*desc = &cdev->desc;
	struct usb_gadget_strings	*gstr = cdev->driver->strings[0];
	struct usb_string		*dev_str = gstr->strings;

	if (covr->idVendor)
		desc->idVendor = cpu_to_le16(covr->idVendor);

	if (covr->idProduct)
		desc->idProduct = cpu_to_le16(covr->idProduct);

	if (covr->bcdDevice)
		desc->bcdDevice = cpu_to_le16(covr->bcdDevice);

	if (covr->serial_number) {
		desc->iSerialNumber = dev_str[USB_GADGET_SERIAL_IDX].id;
		dev_str[USB_GADGET_SERIAL_IDX].s = covr->serial_number;
	}
	if (covr->manufacturer) {
		desc->iManufacturer = dev_str[USB_GADGET_MANUFACTURER_IDX].id;
		dev_str[USB_GADGET_MANUFACTURER_IDX].s = covr->manufacturer;

	} else if (!strlen(dev_str[USB_GADGET_MANUFACTURER_IDX].s)) {
		desc->iManufacturer = dev_str[USB_GADGET_MANUFACTURER_IDX].id;
		cdev->def_manufacturer = composite_default_mfr(cdev->gadget);
		dev_str[USB_GADGET_MANUFACTURER_IDX].s = cdev->def_manufacturer;
	}

	if (covr->product) {
		desc->iProduct = dev_str[USB_GADGET_PRODUCT_IDX].id;
		dev_str[USB_GADGET_PRODUCT_IDX].s = covr->product;
	}
}
EXPORT_SYMBOL_GPL(usb_composite_overwrite_options);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Brownell");
