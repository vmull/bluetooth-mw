/*
 * bluetooth HCI worker.
 *
 * partially based on this:
 *   http://mobileim.googlecode.com/svn/MeeGo/meegoTraning/MeeGo_Traing_Doc_0119/meego_trn/meego-quality-assurance-mcts/mcts-blts/blts-bluetooth/src/pairing.c\
 * 
 * GPL license header is below:
 */

/*
   Copyright (C) 2000-2010, Nokia Corporation.
   Copyright (C) 2000-2002  Maxim Krasnyansky <maxk@qualcomm.com>
   Copyright (C) 2003-2007  Marcel Holtmann <marcel@holtmann.org>

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 2.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.

   Includes Bluetooth event interpretation from BlueZ HCIDump source.
*/

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <poll.h>
#include <stdint.h>

#include <bluetooth.h>
#include <hci.h>
#include <hci_lib.h>

#include "bt_ctx.h"


#define BLTS_DEBUG(s...) { printf("[HCI]: DEBUG: " s); }
#define BLTS_LOGGED_PERROR(s...) { fprintf(stderr, "[HCI]: ERROR: %s (%d - %s)\n", s, errno, strerror(errno)); }

#define EVENT_NUM 61
static const char *event_str[EVENT_NUM + 1] = {
	"Unknown",
	"Inquiry Complete",
	"Inquiry Result",
	"Connect Complete",
	"Connect Request",
	"Disconn Complete",
	"Auth Complete",
	"Remote Name Req Complete",
	"Encrypt Change",
	"Change Connection Link Key Complete",
	"Master Link Key Complete",
	"Read Remote Supported Features",
	"Read Remote Ver Info Complete",
	"QoS Setup Complete",
	"Command Complete",
	"Command Status",
	"Hardware Error",
	"Flush Occurred",
	"Role Change",
	"Number of Completed Packets",
	"Mode Change",
	"Return Link Keys",
	"PIN Code Request",
	"Link Key Request",
	"Link Key Notification",
	"Loopback Command",
	"Data Buffer Overflow",
	"Max Slots Change",
	"Read Clock Offset Complete",
	"Connection Packet Type Changed",
	"QoS Violation",
	"Page Scan Mode Change",
	"Page Scan Repetition Mode Change",
	"Flow Specification Complete",
	"Inquiry Result with RSSI",
	"Read Remote Extended Features",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Synchronous Connect Complete",
	"Synchronous Connect Changed",
	"Sniff Subrate",
	"Extended Inquiry Result",
	"Encryption Key Refresh Complete",
	"IO Capability Request",
	"IO Capability Response",
	"User Confirmation Request",
	"User Passkey Request",
	"Remote OOB Data Request",
	"Simple Pairing Complete",
	"Unknown",
	"Link Supervision Timeout Change",
	"Enhanced Flush Complete",
	"Unknown",
	"User Passkey Notification",
	"Keypress Notification",
	"Remote Host Supported Features Notification",
};

struct link_key_neg_reply_cp {
	bdaddr_t bdaddr;
} __attribute__((packed));
#define LINK_KEY_NEG_REPLY_CP_SIZE 6

/*
 * Wait for incoming hci traffic on dd according to filter, return message in
 * newly allocated *resp (size = resp_len). Timeout as in poll().
 * Return 0 on success or -errno.
 */
static int wait_for_events(int dd, struct hci_filter *filter,
			unsigned char **resp, int *resp_len, int timeout)
{
	unsigned char *buf;
	struct hci_filter of;
	socklen_t olen;
	int err, retries, len;

	if (!resp)
		return -EINVAL;

	buf = reinterpret_cast<unsigned char*>(malloc(HCI_MAX_EVENT_SIZE));
	if (!buf)
		return -ENOMEM;

	/* We'll likely want to save the previous filter */
	olen = sizeof(of);
	if (getsockopt(dd, SOL_HCI, HCI_FILTER, &of, &olen) < 0) {
		free(buf);
		return -1;
	}

	if (setsockopt(dd, SOL_HCI, HCI_FILTER, filter, sizeof(*filter)) < 0) {
		free(buf);
		return -1;
	}

	retries = 10;
	while (retries--) {

		if (timeout) {
			struct pollfd p;
			int n;

			p.fd = dd; p.events = POLLIN;
			while ((n = poll(&p, 1, timeout)) < 0) {
				if (errno == EAGAIN || errno == EINTR)
					continue;
				goto failed;
			}

			if (!n) {
				errno = ETIMEDOUT;
				goto failed;
			}

			timeout -= 10;
			if (timeout < 0) timeout = 0;

		}

		while ((len = read(dd, buf, HCI_MAX_EVENT_SIZE)) < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			goto failed;
		}

		goto done;
	}

	errno = ETIMEDOUT;

failed:
	free(buf);
	err = errno;
	setsockopt(dd, SOL_HCI, HCI_FILTER, &of, sizeof(of));
	errno = err;
	return -1;

done:
	*resp = buf;
	*resp_len = len;
	setsockopt(dd, SOL_HCI, HCI_FILTER, &of, sizeof(of));
	return 0;
}

static void set_default_event_filter(struct hci_filter *flt)
{
	hci_filter_clear(flt);
	hci_filter_set_ptype(HCI_EVENT_PKT, flt);
	hci_filter_all_events(flt);

}

	/*BLTS_DEBUG("Sending LINK_KEY_NEG_REPLY\n");
	if (hci_send_cmd(dd, OGF_LINK_CTL, OCF_LINK_KEY_NEG_REPLY, 6, &neg_reply) < 0) {
		BLTS_DEBUG("LINK_KEY_NEG_REPLY failed\n");
		ret = errno ? -errno : -1;
		goto done;
	}*/

/*
 * Wait until event occurs on dd or timeout (as in poll()) is reached.
 * If we were waiting for an incoming connection, also store the bdaddr to
 * globals.
 * Return 0 on event, -errno on fail (-ETIMEDOUT on timeout).
 */
static int event_loop(bt_ctx* ctx, int timeout)
{
	struct hci_filter nf;
	double t_start,t_end,t_diff;
	struct timeval tv;
	unsigned char *buf = 0;
	int buf_len, ret = 0;
	hci_event_hdr *hdr;

	int dd = ctx->hci_fd;

	set_default_event_filter(&nf);

	gettimeofday(&tv, 0);
	t_start = tv.tv_sec + 1E-6 * tv.tv_usec;
	t_end = t_start + 1E-3 * timeout;

	while (1) {
		if (buf) {
			free(buf);
			buf = 0;
		}
		if (wait_for_events(dd, &nf, &buf, &buf_len, timeout) < 0) {
			BLTS_LOGGED_PERROR("Failed waiting for event\n");
			ret = errno?-errno:-1;
			break;
		}
		gettimeofday(&tv, 0);
		t_diff = t_end - tv.tv_sec - 1E-6 * tv.tv_usec;
		if (t_diff <= 0) {
			BLTS_DEBUG("Timed out waiting for event\n");
			ret = -ETIMEDOUT;
			break;
		}

		timeout = t_diff * 1E3;

		hdr = reinterpret_cast<hci_event_hdr*>(buf + 1);
		BLTS_DEBUG("> Event 0x%.2x (\"%s\") (%lf s)\n",hdr->evt,
			event_str[hdr->evt], t_end - t_diff - t_start);

		/*
		 * 
		 */

		switch(hdr->evt) {
			case EVT_PIN_CODE_REQ: {
				pin_code_reply_cp pin_reply = {
					.bdaddr = ctx->remote_mac,
					.pin_len = 4
				};
				memmove(&pin_reply.pin_code, &ctx->pin_code, sizeof(pin_reply.pin_code));

				int z = hci_send_cmd(dd,
					OGF_LINK_CTL,
					OCF_PIN_CODE_REPLY,
					PIN_CODE_REPLY_CP_SIZE,
					&pin_reply);

				BLTS_DEBUG("Sending PIN '%s' ...\n", pin_reply.pin_code);

				if (z < 0) {
					BLTS_DEBUG("PIN_CODE_REPLY failed\n");
					return errno ? -errno : -1;
				}

				break;
			}
			case EVT_LINK_KEY_REQ: {
				link_key_neg_reply_cp neg_reply = {
					.bdaddr = ctx->remote_mac,
				};

				int z = hci_send_cmd(dd,
					OGF_LINK_CTL,
					OCF_LINK_KEY_NEG_REPLY,
					6,
					&neg_reply);

				BLTS_DEBUG("Sending LINK_KEY_NEG_REPLY ...\n");

				if (z < 0) {
					BLTS_DEBUG("LINK_KEY_NEG_REPLY failed\n");
					return errno ? -errno : -1;
				}

				break;
			}
		}

		if (hdr->evt == EVT_DISCONN_COMPLETE || hdr->evt == EVT_LINK_KEY_NOTIFY)
		{
			ret = 0;
			break;
		}
	}

	if (buf)
		free(buf);

	return ret;
}

#pragma GCC diagnostic ignored "-fpermissive"

void configure_adapter(int dd, int devnum) {
	struct hci_dev_req dr;
	struct hci_dev_info di;
	uint8_t lap[] = { 0x33,0x8b,0x9e };
	uint8_t scan_page;

	/* First, reset the adapter. We'll catch errors here on the devinfo call. */
	hci_send_cmd(dd, OGF_HOST_CTL, OCF_RESET, 0, 0);

	usleep(100000);
	ioctl(dd, HCIDEVDOWN, devnum);
	usleep(100000);

	/* LM options */
	memset(&dr, 0, sizeof(dr));

	dr.dev_id = devnum;
	dr.dev_opt = HCI_LM_ACCEPT | HCI_LM_MASTER |
		HCI_LM_AUTH | HCI_LM_ENCRYPT |
		HCI_LM_TRUSTED | HCI_LM_SECURE;
	ioctl(dd, HCISETLINKMODE, (unsigned long) &dr);

	/* Policy (should we disable role switch?) */
	dr.dev_opt = HCI_LP_RSWITCH | HCI_LP_SNIFF | HCI_LP_HOLD | HCI_LP_PARK;
	ioctl(dd, HCISETLINKPOL, (unsigned long) &dr);

	/* We can now bring the device up and continue with libbluetooth code */
	ioctl(dd, HCIDEVUP, devnum);

	if (hci_devinfo(devnum, &di) < 0) {
		BLTS_DEBUG("Adapter init failed.\n");
		return;
	}

	if (hci_test_bit(HCI_RAW, &di.flags)) {
		BLTS_DEBUG("Warning: adapter in raw mode.\n");
	}

	if (hci_write_local_name(dd, "car-controller", 1000) < 0)
		BLTS_DEBUG("Warning: failed to set local name\n");

	if (hci_write_class_of_dev(dd, 0x010204, 1000) < 0)
		BLTS_DEBUG("Warning: failed to set local device class\n");

	/* Clean stored keys if any */
	if (hci_delete_stored_link_key(dd, BDADDR_ANY, 1, 1000) < 0) {
		BLTS_DEBUG("Warning: failed to clear stored link keys\n");
	}
}

int wait_for_pairing(bt_ctx* ctx) {
	int ret = -1;

	if (!ctx)
		return -EINVAL;

	int timeout = (ctx->test_timeout).tv_sec * 1E3
		+ (ctx->test_timeout).tv_nsec * 1E-6;

	ret = event_loop(ctx, timeout);
	if (ret) {
		BLTS_DEBUG("Event loop exited with an error.\n");
		goto done;
	}

done:
	return ret;
}
