/*
 * Copyright (c) 2019 Google LLC. All rights reserved.
 * Use of this source code is governed by a BSD-style
 * license that can be found in the LICENSE file.
 */

#include <sys/types.h>

#include <sys/ioctl.h>
#include <dev/usb/usb.h>
#include <dev/usb/usbhid.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <usbhid.h>
#include <poll.h>

#include "fido.h"

#define MAX_UHID	64
#define MAX_REPORT_LEN	(sizeof(((struct usb_ctl_report *)(NULL))->ucr_data))

struct hid_openbsd {
	int fd;
	size_t report_in_len;
	size_t report_out_len;
};

int
fido_hid_manifest(fido_dev_info_t *devlist, size_t ilen, size_t *olen)
{
	size_t i;
	char path[64];
	int is_fido, fd;
	struct usb_device_info udi;
	report_desc_t rdesc = NULL;
	hid_data_t hdata = NULL;
	hid_item_t hitem;
	fido_dev_info_t *di;

	if (ilen == 0)
		return (FIDO_OK); /* nothing to do */

	if (devlist == NULL || olen == NULL)
		return (FIDO_ERR_INVALID_ARGUMENT);

	for (i = *olen = 0; i < MAX_UHID && *olen < ilen; i++) {
		snprintf(path, sizeof(path), "/dev/uhid%zu", i);
		if ((fd = open(path, O_RDWR)) == -1) {
			if (errno != ENOENT && errno != ENXIO) {
				fido_log_debug("%s: open %s: %s", __func__,
				    path, strerror(errno));
			}
			continue;
		}
		memset(&udi, 0, sizeof(udi));
		if (ioctl(fd, USB_GET_DEVICEINFO, &udi) != 0) {
			fido_log_debug("%s: get device info %s: %s", __func__,
			    path, strerror(errno));
			close(fd);
			continue;
		}
		if ((rdesc = hid_get_report_desc(fd)) == NULL) {
			fido_log_debug("%s: failed to get report descriptor: %s",
			    __func__, path);
			close(fd);
			continue;
		}
		if ((hdata = hid_start_parse(rdesc,
		    1<<hid_collection, -1)) == NULL) {
			fido_log_debug("%s: failed to parse report descriptor: %s",
			    __func__, path);
			hid_dispose_report_desc(rdesc);
			close(fd);
			continue;
		}
		is_fido = 0;
		for (is_fido = 0; !is_fido;) {
			memset(&hitem, 0, sizeof(hitem));
			if (hid_get_item(hdata, &hitem) <= 0)
				break;
			if ((hitem._usage_page & 0xFFFF0000) == 0xf1d00000)
				is_fido = 1;
		}
		hid_end_parse(hdata);
		hid_dispose_report_desc(rdesc);
		close(fd);

		if (!is_fido)
			continue;

		fido_log_debug("%s: %s: bus = 0x%02x, addr = 0x%02x",
		    __func__, path, udi.udi_bus, udi.udi_addr);
		fido_log_debug("%s: %s: vendor = \"%s\", product = \"%s\"",
		    __func__, path, udi.udi_vendor, udi.udi_product);
		fido_log_debug("%s: %s: productNo = 0x%04x, vendorNo = 0x%04x, "
		    "releaseNo = 0x%04x", __func__, path, udi.udi_productNo,
		    udi.udi_vendorNo, udi.udi_releaseNo);

		di = &devlist[*olen];
		memset(di, 0, sizeof(*di));
		di->io = (fido_dev_io_t) {
			fido_hid_open,
			fido_hid_close,
			fido_hid_read,
			fido_hid_write,
			NULL,
			NULL,
		};
		if ((di->path = strdup(path)) == NULL ||
		    (di->manufacturer = strdup(udi.udi_vendor)) == NULL ||
		    (di->product = strdup(udi.udi_product)) == NULL) {
			free(di->path);
			free(di->manufacturer);
			free(di->product);
			explicit_bzero(di, sizeof(*di));
			return FIDO_ERR_INTERNAL;
		}
		di->vendor_id = udi.udi_vendorNo;
		di->product_id = udi.udi_productNo;
		(*olen)++;
	}

	return FIDO_OK;
}

/*
 * Workaround for OpenBSD <=6.6-current (as of 201910) bug that loses
 * sync of DATA0/DATA1 sequence bit across uhid open/close.
 * Send pings until we get a response - early pings with incorrect
 * sequence bits will be ignored as duplicate packets by the device.
 */
static int
terrible_ping_kludge(struct hid_openbsd *ctx)
{
	u_char data[256];
	int i, n;
	struct pollfd pfd;

	if (sizeof(data) < ctx->report_out_len + 1)
		return -1;
	for (i = 0; i < 4; i++) {
		memset(data, 0, sizeof(data));
		/* broadcast channel ID */
		data[1] = 0xff;
		data[2] = 0xff;
		data[3] = 0xff;
		data[4] = 0xff;
		/* Ping command */
		data[5] = 0x81;
		/* One byte ping only, Vasili */
		data[6] = 0;
		data[7] = 1;
		fido_log_debug("%s: send ping %d", __func__, i);
		if (fido_hid_write(ctx, data, ctx->report_out_len + 1) == -1)
			return -1;
		fido_log_debug("%s: wait reply", __func__);
		memset(&pfd, 0, sizeof(pfd));
		pfd.fd = ctx->fd;
		pfd.events = POLLIN;
		if ((n = poll(&pfd, 1, 100)) == -1) {
			fido_log_debug("%s: poll: %s", __func__, strerror(errno));
			return -1;
		} else if (n == 0) {
			fido_log_debug("%s: timed out", __func__);
			continue;
		}
		if (fido_hid_read(ctx, data, ctx->report_out_len, 250) == -1)
			return -1;
		/*
		 * Ping isn't always supported on the broadcast channel,
		 * so we might get an error, but we don't care - we're
		 * synched now.
		 */
		fido_log_debug("%s: got reply", __func__);
		fido_log_xxd(data, ctx->report_out_len);
		return 0;
	}
	fido_log_debug("%s: no response", __func__);
	return -1;
}

void *
fido_hid_open(const char *path)
{
	struct hid_openbsd *ret = NULL;
	report_desc_t rdesc = NULL;
	int len, usb_report_id = 0;

	if ((ret = calloc(1, sizeof(*ret))) == NULL ||
	    (ret->fd = open(path, O_RDWR)) < 0) {
		free(ret);
		return (NULL);
	}
	if (ioctl(ret->fd, USB_GET_REPORT_ID, &usb_report_id) != 0) {
		fido_log_debug("%s: failed to get report ID: %s", __func__,
		    strerror(errno));
		goto fail;
	}
	if ((rdesc = hid_get_report_desc(ret->fd)) == NULL) {
		fido_log_debug("%s: failed to get report descriptor", __func__);
		goto fail;
	}
	if ((len = hid_report_size(rdesc, hid_input, usb_report_id)) <= 0 ||
	    (size_t)len > MAX_REPORT_LEN) {
		fido_log_debug("%s: bad input report size %d", __func__, len);
		goto fail;
	}
	ret->report_in_len = (size_t)len;
	if ((len = hid_report_size(rdesc, hid_output, usb_report_id)) <= 0 ||
	    (size_t)len > MAX_REPORT_LEN) {
		fido_log_debug("%s: bad output report size %d", __func__, len);
 fail:
		hid_dispose_report_desc(rdesc);
		close(ret->fd);
		free(ret);
		return NULL;
	}	
	ret->report_out_len = (size_t)len;
	hid_dispose_report_desc(rdesc);
	fido_log_debug("%s: USB report ID %d, inlen = %zu outlen = %zu",
	    __func__, usb_report_id, ret->report_in_len, ret->report_out_len);

	/*
	 * OpenBSD (as of 201910) has a bug that causes it to lose
	 * track of the DATA0/DATA1 sequence toggle across uhid device
	 * open and close. This is a terrible hack to work around it.
	 */
	if (terrible_ping_kludge(ret) != 0) {
		fido_hid_close(ret);
		return NULL;
	}

	return (ret);
}

void
fido_hid_close(void *handle)
{
	struct hid_openbsd *ctx = (struct hid_openbsd *)handle;

	close(ctx->fd);
	free(ctx);
}

int
fido_hid_read(void *handle, unsigned char *buf, size_t len, int ms)
{
	struct hid_openbsd *ctx = (struct hid_openbsd *)handle;
	ssize_t r;

	(void)ms; /* XXX */

	if (len != ctx->report_in_len) {
		fido_log_debug("%s: invalid len: got %zu, want %zu", __func__,
		    len, ctx->report_in_len);
		return (-1);
	}
	if ((r = read(ctx->fd, buf, len)) == -1 || (size_t)r != len) {
		fido_log_debug("%s: read: %s", __func__, strerror(errno));
		return (-1);
	}
	return ((int)len);
}

int
fido_hid_write(void *handle, const unsigned char *buf, size_t len)
{
	struct hid_openbsd *ctx = (struct hid_openbsd *)handle;
	ssize_t r;

	if (len != ctx->report_out_len + 1) {
		fido_log_debug("%s: invalid len: got %zu, want %zu", __func__,
		    len, ctx->report_out_len);
		return (-1);
	}
	if ((r = write(ctx->fd, buf + 1, len - 1)) == -1 ||
	    (size_t)r != len - 1) {
		fido_log_debug("%s: write: %s", __func__, strerror(errno));
		return (-1);
	}
	return ((int)len);
}
