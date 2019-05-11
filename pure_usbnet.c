/*
 * CDC Ethernet based networking peripherals
 * Copyright (C) 2003-2005 by David Brownell
 * Copyright (C) 2006 by Ole Andre Vadla Ravnas (ActiveSync)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

// #define	DEBUG			// error path messages, extra info
// #define	VERBOSE			// more; success messages

#include <linux/module.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/workqueue.h>
#include <linux/mii.h>
#include <linux/usb.h>
#include <linux/usb/cdc.h>
#include <linux/usb/usbnet.h>


#if defined(CONFIG_USB_NET_RNDIS_HOST) || defined(CONFIG_USB_NET_RNDIS_HOST_MODULE)

static int is_rndis(struct usb_interface_descriptor *desc)
{
	return (desc->bInterfaceClass == USB_CLASS_COMM &&
		desc->bInterfaceSubClass == 2 &&
		desc->bInterfaceProtocol == 0xff);
}

static int is_activesync(struct usb_interface_descriptor *desc)
{
	return (desc->bInterfaceClass == USB_CLASS_MISC &&
		desc->bInterfaceSubClass == 1 &&
		desc->bInterfaceProtocol == 1);
}

static int is_wireless_rndis(struct usb_interface_descriptor *desc)
{
	return (desc->bInterfaceClass == USB_CLASS_WIRELESS_CONTROLLER &&
		desc->bInterfaceSubClass == 1 &&
		desc->bInterfaceProtocol == 3);
}

#else

#define is_rndis(desc)		0
#define is_activesync(desc)	0
#define is_wireless_rndis(desc)	0

#endif

static const u8 mbm_guid[16] = {
	0xa3, 0x17, 0xa8, 0x8b, 0x04, 0x5e, 0x4f, 0x01,
	0xa6, 0x07, 0xc0, 0xff, 0xcb, 0x7e, 0x39, 0x2a,
};

/*
 * probes control interface, claims data interface, collects the bulk
 * endpoints, activates data interface (if needed), maybe sets MTU.
 * all pure cdc, except for certain firmware workarounds, and knowing
 * that rndis uses one different rule.
 */
int pure_generic_cdc_bind(struct usbnet *dev, struct usb_interface *intf)
{
	u8				*buf = intf->cur_altsetting->extra;
	int				len = intf->cur_altsetting->extralen;
	struct usb_interface_descriptor	*d;
	struct cdc_state		*info = (void *) &dev->data;
	int				status;
	int				rndis;
	bool				android_rndis_quirk = false;
	struct usb_driver		*driver = driver_of(intf);
	struct usb_cdc_mdlm_desc	*desc = NULL;
	struct usb_cdc_mdlm_detail_desc *detail = NULL;

	if (sizeof dev->data < sizeof *info)
		return -EDOM;

	/* expect strict spec conformance for the descriptors, but
	 * cope with firmware which stores them in the wrong place
	 */
	if (len == 0 && dev->udev->actconfig->extralen) {
		/* Motorola SB4100 (and others: Brad Hards says it's
		 * from a Broadcom design) put CDC descriptors here
		 */
		buf = dev->udev->actconfig->extra;
		len = dev->udev->actconfig->extralen;
		dev_dbg(&intf->dev, "CDC descriptors on config\n");
	}

	/* Maybe CDC descriptors are after the endpoint?  This bug has
	 * been seen on some 2Wire Inc RNDIS-ish products.
	 */
	if (len == 0) {
		struct usb_host_endpoint	*hep;

		hep = intf->cur_altsetting->endpoint;
		if (hep) {
			buf = hep->extra;
			len = hep->extralen;
		}
		if (len)
			dev_dbg(&intf->dev,
				"CDC descriptors on endpoint\n");
	}

	/* this assumes that if there's a non-RNDIS vendor variant
	 * of cdc-acm, it'll fail RNDIS requests cleanly.
	 */
	rndis = (is_rndis(&intf->cur_altsetting->desc) ||
		 is_activesync(&intf->cur_altsetting->desc) ||
		 is_wireless_rndis(&intf->cur_altsetting->desc));

	memset(info, 0, sizeof *info);
	info->control = intf;
	while (len > 3) {
		if (buf [1] != USB_DT_CS_INTERFACE)
			goto next_desc;

		/* use bDescriptorSubType to identify the CDC descriptors.
		 * We expect devices with CDC header and union descriptors.
		 * For CDC Ethernet we need the ethernet descriptor.
		 * For RNDIS, ignore two (pointless) CDC modem descriptors
		 * in favor of a complicated OID-based RPC scheme doing what
		 * CDC Ethernet achieves with a simple descriptor.
		 */
		switch (buf [2]) {
		case USB_CDC_HEADER_TYPE:
			if (info->header) {
				dev_dbg(&intf->dev, "extra CDC header\n");
				goto bad_desc;
			}
			info->header = (void *) buf;
			if (info->header->bLength != sizeof *info->header) {
				dev_dbg(&intf->dev, "CDC header len %u\n",
					info->header->bLength);
				goto bad_desc;
			}
			break;
		case USB_CDC_ACM_TYPE:
			/* paranoia:  disambiguate a "real" vendor-specific
			 * modem interface from an RNDIS non-modem.
			 */
			if (rndis) {
				struct usb_cdc_acm_descriptor *acm;

				acm = (void *) buf;
				if (acm->bmCapabilities) {
					dev_dbg(&intf->dev,
						"ACM capabilities %02x, "
						"not really RNDIS?\n",
						acm->bmCapabilities);
					goto bad_desc;
				}
			}
			break;
		case USB_CDC_UNION_TYPE:
			if (info->u) {
				dev_dbg(&intf->dev, "extra CDC union\n");
				goto bad_desc;
			}
			info->u = (void *) buf;
			if (info->u->bLength != sizeof *info->u) {
				dev_dbg(&intf->dev, "CDC union len %u\n",
					info->u->bLength);
				goto bad_desc;
			}

			/* we need a master/control interface (what we're
			 * probed with) and a slave/data interface; union
			 * descriptors sort this all out.
			 */
			info->control = usb_ifnum_to_if(dev->udev,
						info->u->bMasterInterface0);
			info->data = usb_ifnum_to_if(dev->udev,
						info->u->bSlaveInterface0);
			if (!info->control || !info->data) {
				dev_dbg(&intf->dev,
					"master #%u/%p slave #%u/%p\n",
					info->u->bMasterInterface0,
					info->control,
					info->u->bSlaveInterface0,
					info->data);
				/* fall back to hard-wiring for RNDIS */
				if (rndis) {
					android_rndis_quirk = true;
					goto next_desc;
				}
				goto bad_desc;
			}
			if (info->control != intf) {
				dev_dbg(&intf->dev, "bogus CDC Union\n");
				/* Ambit USB Cable Modem (and maybe others)
				 * interchanges master and slave interface.
				 */
				if (info->data == intf) {
					info->data = info->control;
					info->control = intf;
				} else
					goto bad_desc;
			}

			/* a data interface altsetting does the real i/o */
			d = &info->data->cur_altsetting->desc;
			if (d->bInterfaceClass != USB_CLASS_CDC_DATA) {
				dev_dbg(&intf->dev, "slave class %u\n",
					d->bInterfaceClass);
				goto bad_desc;
			}
			break;
		case USB_CDC_ETHERNET_TYPE:
			if (info->ether) {
				dev_dbg(&intf->dev, "extra CDC ether\n");
				goto bad_desc;
			}
			info->ether = (void *) buf;
			if (info->ether->bLength != sizeof *info->ether) {
				dev_dbg(&intf->dev, "CDC ether len %u\n",
					info->ether->bLength);
				goto bad_desc;
			}
			dev->hard_mtu = le16_to_cpu(
						info->ether->wMaxSegmentSize);
			/* because of Zaurus, we may be ignoring the host
			 * side link address we were given.
			 */
			break;
		case USB_CDC_MDLM_TYPE:
			if (desc) {
				dev_dbg(&intf->dev, "extra MDLM descriptor\n");
				goto bad_desc;
			}

			desc = (void *)buf;

			if (desc->bLength != sizeof(*desc))
				goto bad_desc;

			if (memcmp(&desc->bGUID, mbm_guid, 16))
				goto bad_desc;
			break;
		case USB_CDC_MDLM_DETAIL_TYPE:
			if (detail) {
				dev_dbg(&intf->dev, "extra MDLM detail descriptor\n");
				goto bad_desc;
			}

			detail = (void *)buf;

			if (detail->bGuidDescriptorType == 0) {
				if (detail->bLength < (sizeof(*detail) + 1))
					goto bad_desc;
			} else
				goto bad_desc;
			break;
		}
next_desc:
		len -= buf [0];	/* bLength */
		buf += buf [0];
	}

	/* Microsoft ActiveSync based and some regular RNDIS devices lack the
	 * CDC descriptors, so we'll hard-wire the interfaces and not check
	 * for descriptors.
	 *
	 * Some Android RNDIS devices have a CDC Union descriptor pointing
	 * to non-existing interfaces.  Ignore that and attempt the same
	 * hard-wired 0 and 1 interfaces.
	 */
	if (rndis && (!info->u || android_rndis_quirk)) {
		info->control = usb_ifnum_to_if(dev->udev, 0);
		info->data = usb_ifnum_to_if(dev->udev, 1);
		if (!info->control || !info->data || info->control != intf) {
			dev_dbg(&intf->dev,
				"rndis: master #0/%p slave #1/%p\n",
				info->control,
				info->data);
			goto bad_desc;
		}

	} else if (!info->header || !info->u || (!rndis && !info->ether)) {
		dev_dbg(&intf->dev, "missing cdc %s%s%sdescriptor\n",
			info->header ? "" : "header ",
			info->u ? "" : "union ",
			info->ether ? "" : "ether ");
		goto bad_desc;
	}

	/* claim data interface and set it up ... with side effects.
	 * network traffic can't flow until an altsetting is enabled.
	 */
	status = usb_driver_claim_interface(driver, info->data, dev);
	if (status < 0)
		return status;
	status = usbnet_get_endpoints(dev, info->data);
	if (status < 0) {
		/* ensure immediate exit from usbnet_disconnect */
		usb_set_intfdata(info->data, NULL);
		usb_driver_release_interface(driver, info->data);
		return status;
	}

	/* status endpoint: optional for CDC Ethernet, not RNDIS (or ACM) */
	dev->status = NULL;
	if (info->control->cur_altsetting->desc.bNumEndpoints == 1) {
		struct usb_endpoint_descriptor	*desc;

		dev->status = &info->control->cur_altsetting->endpoint [0];
		desc = &dev->status->desc;
		if (!usb_endpoint_is_int_in(desc) ||
		    (le16_to_cpu(desc->wMaxPacketSize)
		     < sizeof(struct usb_cdc_notification)) ||
		    !desc->bInterval) {
			dev_dbg(&intf->dev, "bad notification endpoint\n");
			dev->status = NULL;
		}
	}
	if (rndis && !dev->status) {
		dev_dbg(&intf->dev, "missing RNDIS status endpoint\n");
		usb_set_intfdata(info->data, NULL);
		usb_driver_release_interface(driver, info->data);
		return -ENODEV;
	}
	return 0;

bad_desc:
	dev_info(&dev->udev->dev, "bad CDC descriptors\n");
	return -ENODEV;
}

void pure_unbind(struct usbnet *dev, struct usb_interface *intf)
{
	struct cdc_state		*info = (void *) &dev->data;
	struct usb_driver		*driver = driver_of(intf);

	/* disconnect master --> disconnect slave */
	if (intf == info->control && info->data) {
		/* ensure immediate exit from usbnet_disconnect */
		usb_set_intfdata(info->data, NULL);
		usb_driver_release_interface(driver, info->data);
		info->data = NULL;
	}

	/* and vice versa (just in case) */
	else if (intf == info->data && info->control) {
		/* ensure immediate exit from usbnet_disconnect */
		usb_set_intfdata(info->control, NULL);
		usb_driver_release_interface(driver, info->control);
		info->control = NULL;
	}
}

/*-------------------------------------------------------------------------
 *
 * Communications Device Class, Ethernet Control model
 *
 * Takes two interfaces.  The DATA interface is inactive till an altsetting
 * is selected.  Configuration data includes class descriptors.  There's
 * an optional status endpoint on the control interface.
 *
 * This should interop with whatever the 2.4 "CDCEther.c" driver
 * (by Brad Hards) talked with, with more functionality.
 *
 *-------------------------------------------------------------------------*/

static void dumpspeed(struct usbnet *dev, __le32 *speeds)
{
	netif_info(dev, timer, dev->net,
		   "link speeds: %u kbps up, %u kbps down\n",
		   __le32_to_cpu(speeds[0]) / 1000,
		   __le32_to_cpu(speeds[1]) / 1000);
}

void pure_status(struct usbnet *dev, struct urb *urb)
{
	struct usb_cdc_notification	*event;

	if (urb->actual_length < sizeof *event)
		return;

	/* SPEED_CHANGE can get split into two 8-byte packets */
	if (test_and_clear_bit(EVENT_STS_SPLIT, &dev->flags)) {
		dumpspeed(dev, (__le32 *) urb->transfer_buffer);
		return;
	}

	event = urb->transfer_buffer;
	switch (event->bNotificationType) {
	case USB_CDC_NOTIFY_NETWORK_CONNECTION:
		netif_dbg(dev, timer, dev->net, "CDC: carrier %s\n",
			  event->wValue ? "on" : "off");
		usbnet_link_change(dev, !!event->wValue, 0);
		break;
	case USB_CDC_NOTIFY_SPEED_CHANGE:	/* tx/rx rates */
		netif_dbg(dev, timer, dev->net, "CDC: speed change (len %d)\n",
			  urb->actual_length);
		if (urb->actual_length != (sizeof *event + 8))
			set_bit(EVENT_STS_SPLIT, &dev->flags);
		else
			dumpspeed(dev, (__le32 *) &event[1]);
		break;
	/* USB_CDC_NOTIFY_RESPONSE_AVAILABLE can happen too (e.g. RNDIS),
	 * but there are no standard formats for the response data.
	 */
	default:
		netdev_err(dev->net, "CDC: unexpected notification %02x!\n",
			   event->bNotificationType);
		break;
	}
}

int pure_bind(struct usbnet *dev, struct usb_interface *intf)
{
	int				status;
	struct cdc_state		*info = (void *) &dev->data;

	BUILD_BUG_ON((sizeof(((struct usbnet *)0)->data)
			< sizeof(struct cdc_state)));

	status = pure_generic_cdc_bind(dev, intf);
	if (status < 0)
		return status;

	status = usbnet_get_ethernet_addr(dev, info->ether->iMACAddress);
	if (status < 0) {
		usb_set_intfdata(info->data, NULL);
		usb_driver_release_interface(driver_of(intf), info->data);
		return status;
	}

	/* FIXME cdc-ether has some multicast code too, though it complains
	 * in routine cases.  info->ether describes the multicast support.
	 * Implement that here, manipulating the cdc filter as needed.
	 */
	return 0;
}

static const struct driver_info	cdc_info = {
	.description =	"CDC Ethernet Device",
	.flags =	FLAG_ETHER | FLAG_POINTTOPOINT,
	// .check_connect = cdc_check_connect,
	.bind =		pure_bind,
	.unbind =	pure_unbind,
	.status =	pure_status,
	.manage_power =	usbnet_manage_power,
};


/*-------------------------------------------------------------------------*/

#define TI_VENDOR_ID	        0x0451

static const struct usb_device_id	products [] = {
{
        /* PURE 6LoWPAN node */
        USB_DEVICE(TI_VENDOR_ID, 0x1000),
        .driver_info = (unsigned long)&cdc_info,
}, 
        { },
};



MODULE_DEVICE_TABLE(usb, products);

static struct usb_driver cdc_6lowpan_driver = {
	.name =		"cdc_6lowpan_ether",
	.id_table =	products,
	.probe =	usbnet_probe,
	.disconnect =	usbnet_disconnect,
	.suspend =	usbnet_suspend,
	.resume =	usbnet_resume,
	.reset_resume =	usbnet_resume,
	.supports_autosuspend = 1,
	.disable_hub_initiated_lpm = 1,
};

module_usb_driver(cdc_6lowpan_driver);

MODULE_AUTHOR("Lanpinguo");
MODULE_DESCRIPTION("USB CDC 6LoWPAN devices");
MODULE_LICENSE("GPL");
