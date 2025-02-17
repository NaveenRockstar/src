/*	$OpenBSD: virtio.c,v 1.98 2022/12/23 19:25:22 dv Exp $	*/

/*
 * Copyright (c) 2015 Mike Larkin <mlarkin@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>	/* PAGE_SIZE */
#include <sys/socket.h>

#include <machine/vmmvar.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcidevs.h>
#include <dev/pv/virtioreg.h>
#include <dev/pci/virtio_pcireg.h>
#include <dev/pv/vioblkreg.h>
#include <dev/pv/vioscsireg.h>

#include <net/if.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <netinet/ip.h>

#include <errno.h>
#include <event.h>
#include <poll.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atomicio.h"
#include "pci.h"
#include "vioscsi.h"
#include "virtio.h"
#include "vmd.h"
#include "vmm.h"

extern char *__progname;
struct viornd_dev viornd;
struct vioblk_dev *vioblk;
struct vionet_dev *vionet;
struct vioscsi_dev *vioscsi;
struct vmmci_dev vmmci;

int nr_vionet;
int nr_vioblk;

#define MAXPHYS	(64 * 1024)	/* max raw I/O transfer size */

#define VIRTIO_NET_F_MAC	(1<<5)

#define VMMCI_F_TIMESYNC	(1<<0)
#define VMMCI_F_ACK		(1<<1)
#define VMMCI_F_SYNCRTC		(1<<2)

#define RXQ	0
#define TXQ	1

const char *
vioblk_cmd_name(uint32_t type)
{
	switch (type) {
	case VIRTIO_BLK_T_IN: return "read";
	case VIRTIO_BLK_T_OUT: return "write";
	case VIRTIO_BLK_T_SCSI_CMD: return "scsi read";
	case VIRTIO_BLK_T_SCSI_CMD_OUT: return "scsi write";
	case VIRTIO_BLK_T_FLUSH: return "flush";
	case VIRTIO_BLK_T_FLUSH_OUT: return "flush out";
	case VIRTIO_BLK_T_GET_ID: return "get id";
	default: return "unknown";
	}
}

static const char *
virtio_reg_name(uint8_t reg)
{
	switch (reg) {
	case VIRTIO_CONFIG_DEVICE_FEATURES: return "device feature";
	case VIRTIO_CONFIG_GUEST_FEATURES: return "guest feature";
	case VIRTIO_CONFIG_QUEUE_PFN: return "queue address";
	case VIRTIO_CONFIG_QUEUE_SIZE: return "queue size";
	case VIRTIO_CONFIG_QUEUE_SELECT: return "queue select";
	case VIRTIO_CONFIG_QUEUE_NOTIFY: return "queue notify";
	case VIRTIO_CONFIG_DEVICE_STATUS: return "device status";
	case VIRTIO_CONFIG_ISR_STATUS: return "isr status";
	case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI: return "device config 0";
	case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 4: return "device config 1";
	case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 8: return "device config 2";
	case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 12: return "device config 3";
	case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 16: return "device config 4";
	default: return "unknown";
	}
}

uint32_t
vring_size(uint32_t vq_size)
{
	uint32_t allocsize1, allocsize2;

	/* allocsize1: descriptor table + avail ring + pad */
	allocsize1 = VIRTQUEUE_ALIGN(sizeof(struct vring_desc) * vq_size
	    + sizeof(uint16_t) * (2 + vq_size));
	/* allocsize2: used ring + pad */
	allocsize2 = VIRTQUEUE_ALIGN(sizeof(uint16_t) * 2
	    + sizeof(struct vring_used_elem) * vq_size);

	return allocsize1 + allocsize2;
}

/* Update queue select */
void
viornd_update_qs(void)
{
	struct virtio_vq_info *vq_info;

	/* Invalid queue? */
	if (viornd.cfg.queue_select > 0) {
		viornd.cfg.queue_size = 0;
		return;
	}

	vq_info = &viornd.vq[viornd.cfg.queue_select];

	/* Update queue pfn/size based on queue select */
	viornd.cfg.queue_pfn = vq_info->q_gpa >> 12;
	viornd.cfg.queue_size = vq_info->qs;
}

/* Update queue address */
void
viornd_update_qa(void)
{
	struct virtio_vq_info *vq_info;
	void *hva = NULL;

	/* Invalid queue? */
	if (viornd.cfg.queue_select > 0)
		return;

	vq_info = &viornd.vq[viornd.cfg.queue_select];
	vq_info->q_gpa = (uint64_t)viornd.cfg.queue_pfn * VIRTIO_PAGE_SIZE;

	hva = hvaddr_mem(vq_info->q_gpa, vring_size(VIORND_QUEUE_SIZE));
	if (hva == NULL)
		fatal("viornd_update_qa");
	vq_info->q_hva = hva;
}

int
viornd_notifyq(void)
{
	size_t sz;
	int dxx, ret;
	uint16_t aidx, uidx;
	char *vr, *rnd_data;
	struct vring_desc *desc;
	struct vring_avail *avail;
	struct vring_used *used;
	struct virtio_vq_info *vq_info;

	ret = 0;

	/* Invalid queue? */
	if (viornd.cfg.queue_notify > 0)
		return (0);

	vq_info = &viornd.vq[viornd.cfg.queue_notify];
	vr = vq_info->q_hva;
	if (vr == NULL)
		fatalx("%s: null vring", __func__);

	desc = (struct vring_desc *)(vr);
	avail = (struct vring_avail *)(vr + vq_info->vq_availoffset);
	used = (struct vring_used *)(vr + vq_info->vq_usedoffset);

	aidx = avail->idx & VIORND_QUEUE_MASK;
	uidx = used->idx & VIORND_QUEUE_MASK;

	dxx = avail->ring[aidx] & VIORND_QUEUE_MASK;

	sz = desc[dxx].len;
	if (sz > MAXPHYS)
		fatalx("viornd descriptor size too large (%zu)", sz);

	rnd_data = malloc(sz);

	if (rnd_data != NULL) {
		arc4random_buf(rnd_data, sz);
		if (write_mem(desc[dxx].addr, rnd_data, sz)) {
			log_warnx("viornd: can't write random data @ "
			    "0x%llx",
			    desc[dxx].addr);
		} else {
			/* ret == 1 -> interrupt needed */
			/* XXX check VIRTIO_F_NO_INTR */
			ret = 1;
			viornd.cfg.isr_status = 1;
			used->ring[uidx].id = dxx;
			used->ring[uidx].len = sz;
			__sync_synchronize();
			used->idx++;
		}
		free(rnd_data);
	} else
		fatal("memory allocation error for viornd data");

	return (ret);
}

int
virtio_rnd_io(int dir, uint16_t reg, uint32_t *data, uint8_t *intr,
    void *unused, uint8_t sz)
{
	*intr = 0xFF;

	if (dir == 0) {
		switch (reg) {
		case VIRTIO_CONFIG_DEVICE_FEATURES:
		case VIRTIO_CONFIG_QUEUE_SIZE:
		case VIRTIO_CONFIG_ISR_STATUS:
			log_warnx("%s: illegal write %x to %s",
			    __progname, *data, virtio_reg_name(reg));
			break;
		case VIRTIO_CONFIG_GUEST_FEATURES:
			viornd.cfg.guest_feature = *data;
			break;
		case VIRTIO_CONFIG_QUEUE_PFN:
			viornd.cfg.queue_pfn = *data;
			viornd_update_qa();
			break;
		case VIRTIO_CONFIG_QUEUE_SELECT:
			viornd.cfg.queue_select = *data;
			viornd_update_qs();
			break;
		case VIRTIO_CONFIG_QUEUE_NOTIFY:
			viornd.cfg.queue_notify = *data;
			if (viornd_notifyq())
				*intr = 1;
			break;
		case VIRTIO_CONFIG_DEVICE_STATUS:
			viornd.cfg.device_status = *data;
			break;
		}
	} else {
		switch (reg) {
		case VIRTIO_CONFIG_DEVICE_FEATURES:
			*data = viornd.cfg.device_feature;
			break;
		case VIRTIO_CONFIG_GUEST_FEATURES:
			*data = viornd.cfg.guest_feature;
			break;
		case VIRTIO_CONFIG_QUEUE_PFN:
			*data = viornd.cfg.queue_pfn;
			break;
		case VIRTIO_CONFIG_QUEUE_SIZE:
			*data = viornd.cfg.queue_size;
			break;
		case VIRTIO_CONFIG_QUEUE_SELECT:
			*data = viornd.cfg.queue_select;
			break;
		case VIRTIO_CONFIG_QUEUE_NOTIFY:
			*data = viornd.cfg.queue_notify;
			break;
		case VIRTIO_CONFIG_DEVICE_STATUS:
			*data = viornd.cfg.device_status;
			break;
		case VIRTIO_CONFIG_ISR_STATUS:
			*data = viornd.cfg.isr_status;
			viornd.cfg.isr_status = 0;
			vcpu_deassert_pic_irq(viornd.vm_id, 0, viornd.irq);
			break;
		}
	}
	return (0);
}

void
vioblk_update_qa(struct vioblk_dev *dev)
{
	struct virtio_vq_info *vq_info;
	void *hva = NULL;

	/* Invalid queue? */
	if (dev->cfg.queue_select > 0)
		return;

	vq_info = &dev->vq[dev->cfg.queue_select];
	vq_info->q_gpa = (uint64_t)dev->cfg.queue_pfn * VIRTIO_PAGE_SIZE;

	hva = hvaddr_mem(vq_info->q_gpa, vring_size(VIOBLK_QUEUE_SIZE));
	if (hva == NULL)
		fatal("vioblk_update_qa");
	vq_info->q_hva = hva;
}

void
vioblk_update_qs(struct vioblk_dev *dev)
{
	struct virtio_vq_info *vq_info;

	/* Invalid queue? */
	if (dev->cfg.queue_select > 0) {
		dev->cfg.queue_size = 0;
		return;
	}

	vq_info = &dev->vq[dev->cfg.queue_select];

	/* Update queue pfn/size based on queue select */
	dev->cfg.queue_pfn = vq_info->q_gpa >> 12;
	dev->cfg.queue_size = vq_info->qs;
}

static void
vioblk_free_info(struct ioinfo *info)
{
	if (!info)
		return;
	free(info->buf);
	free(info);
}

static struct ioinfo *
vioblk_start_read(struct vioblk_dev *dev, off_t sector, size_t sz)
{
	struct ioinfo *info;

	/* Limit to 64M for now */
	if (sz > (1 << 26)) {
		log_warnx("%s: read size exceeded 64M", __func__);
		return (NULL);
	}

	info = calloc(1, sizeof(*info));
	if (!info)
		goto nomem;
	info->buf = malloc(sz);
	if (info->buf == NULL)
		goto nomem;
	info->len = sz;
	info->offset = sector * VIRTIO_BLK_SECTOR_SIZE;
	info->file = &dev->file;

	return info;

nomem:
	free(info);
	log_warn("malloc error vioblk read");
	return (NULL);
}


static const uint8_t *
vioblk_finish_read(struct ioinfo *info)
{
	struct virtio_backing *file;

	file = info->file;
	if (file->pread(file->p, info->buf, info->len, info->offset) != info->len) {
		info->error = errno;
		log_warn("vioblk read error");
		return NULL;
	}

	return info->buf;
}

static struct ioinfo *
vioblk_start_write(struct vioblk_dev *dev, off_t sector,
    paddr_t addr, size_t len)
{
	struct ioinfo *info;

	/* Limit to 64M for now */
	if (len > (1 << 26)) {
		log_warnx("%s: write size exceeded 64M", __func__);
		return (NULL);
	}

	info = calloc(1, sizeof(*info));
	if (!info)
		goto nomem;

	info->buf = malloc(len);
	if (info->buf == NULL)
		goto nomem;
	info->len = len;
	info->offset = sector * VIRTIO_BLK_SECTOR_SIZE;
	info->file = &dev->file;

	if (read_mem(addr, info->buf, info->len)) {
		vioblk_free_info(info);
		return NULL;
	}

	return info;

nomem:
	free(info);
	log_warn("malloc error vioblk write");
	return (NULL);
}

static int
vioblk_finish_write(struct ioinfo *info)
{
	struct virtio_backing *file;

	file = info->file;
	if (file->pwrite(file->p, info->buf, info->len, info->offset) != info->len) {
		log_warn("vioblk write error");
		return EIO;
	}
	return 0;
}

/*
 * XXX in various cases, ds should be set to VIRTIO_BLK_S_IOERR, if we can
 */
int
vioblk_notifyq(struct vioblk_dev *dev)
{
	uint16_t idx, cmd_desc_idx, secdata_desc_idx, ds_desc_idx;
	uint8_t ds;
	int cnt;
	off_t secbias;
	char *vr;
	struct vring_desc *desc, *cmd_desc, *secdata_desc, *ds_desc;
	struct vring_avail *avail;
	struct vring_used *used;
	struct virtio_blk_req_hdr cmd;
	struct virtio_vq_info *vq_info;

	/* Invalid queue? */
	if (dev->cfg.queue_notify > 0)
		return (0);

	vq_info = &dev->vq[dev->cfg.queue_notify];
	vr = vq_info->q_hva;
	if (vr == NULL)
		fatalx("%s: null vring", __func__);

	/* Compute offsets in ring of descriptors, avail ring, and used ring */
	desc = (struct vring_desc *)(vr);
	avail = (struct vring_avail *)(vr + vq_info->vq_availoffset);
	used = (struct vring_used *)(vr + vq_info->vq_usedoffset);

	idx = vq_info->last_avail & VIOBLK_QUEUE_MASK;

	if ((avail->idx & VIOBLK_QUEUE_MASK) == idx) {
		log_debug("%s - nothing to do?", __func__);
		return (0);
	}

	while (idx != (avail->idx & VIOBLK_QUEUE_MASK)) {

		ds = VIRTIO_BLK_S_IOERR;
		cmd_desc_idx = avail->ring[idx] & VIOBLK_QUEUE_MASK;
		cmd_desc = &desc[cmd_desc_idx];

		if ((cmd_desc->flags & VRING_DESC_F_NEXT) == 0) {
			log_warnx("unchained vioblk cmd descriptor received "
			    "(idx %d)", cmd_desc_idx);
			goto out;
		}

		/* Read command from descriptor ring */
		if (cmd_desc->flags & VRING_DESC_F_WRITE) {
			log_warnx("vioblk: unexpected writable cmd descriptor "
			    "%d", cmd_desc_idx);
			goto out;
		}
		if (read_mem(cmd_desc->addr, &cmd, sizeof(cmd))) {
			log_warnx("vioblk: command read_mem error @ 0x%llx",
			    cmd_desc->addr);
			goto out;
		}

		switch (cmd.type) {
		case VIRTIO_BLK_T_IN:
			/* first descriptor */
			secdata_desc_idx = cmd_desc->next & VIOBLK_QUEUE_MASK;
			secdata_desc = &desc[secdata_desc_idx];

			if ((secdata_desc->flags & VRING_DESC_F_NEXT) == 0) {
				log_warnx("unchained vioblk data descriptor "
				    "received (idx %d)", cmd_desc_idx);
				goto out;
			}

			cnt = 0;
			secbias = 0;
			do {
				struct ioinfo *info;
				const uint8_t *secdata;

				if ((secdata_desc->flags & VRING_DESC_F_WRITE)
				    == 0) {
					log_warnx("vioblk: unwritable data "
					    "descriptor %d", secdata_desc_idx);
					goto out;
				}

				info = vioblk_start_read(dev,
				    cmd.sector + secbias, secdata_desc->len);

				if (info == NULL) {
					log_warnx("vioblk: can't start read");
					goto out;
				}

				/* read the data, use current data descriptor */
				secdata = vioblk_finish_read(info);
				if (secdata == NULL) {
					vioblk_free_info(info);
					log_warnx("vioblk: block read error, "
					    "sector %lld", cmd.sector);
					goto out;
				}

				if (write_mem(secdata_desc->addr, secdata,
					secdata_desc->len)) {
					log_warnx("can't write sector "
					    "data to gpa @ 0x%llx",
					    secdata_desc->addr);
					vioblk_free_info(info);
					goto out;
				}

				vioblk_free_info(info);

				secbias += (secdata_desc->len /
				    VIRTIO_BLK_SECTOR_SIZE);
				secdata_desc_idx = secdata_desc->next &
				    VIOBLK_QUEUE_MASK;
				secdata_desc = &desc[secdata_desc_idx];

				/* Guard against infinite chains */
				if (++cnt >= VIOBLK_QUEUE_SIZE) {
					log_warnx("%s: descriptor table "
					    "invalid", __func__);
					goto out;
				}
			} while (secdata_desc->flags & VRING_DESC_F_NEXT);

			ds_desc_idx = secdata_desc_idx;
			ds_desc = secdata_desc;

			ds = VIRTIO_BLK_S_OK;
			break;
		case VIRTIO_BLK_T_OUT:
			secdata_desc_idx = cmd_desc->next & VIOBLK_QUEUE_MASK;
			secdata_desc = &desc[secdata_desc_idx];

			if ((secdata_desc->flags & VRING_DESC_F_NEXT) == 0) {
				log_warnx("wr vioblk: unchained vioblk data "
				    "descriptor received (idx %d)",
				    cmd_desc_idx);
				goto out;
			}

			if (secdata_desc->len > dev->max_xfer) {
				log_warnx("%s: invalid read size %d requested",
				    __func__, secdata_desc->len);
				goto out;
			}

			cnt = 0;
			secbias = 0;
			do {
				struct ioinfo *info;

				if (secdata_desc->flags & VRING_DESC_F_WRITE) {
					log_warnx("wr vioblk: unexpected "
					    "writable data descriptor %d",
					    secdata_desc_idx);
					goto out;
				}

				info = vioblk_start_write(dev,
				    cmd.sector + secbias,
				    secdata_desc->addr, secdata_desc->len);

				if (info == NULL) {
					log_warnx("wr vioblk: can't read "
					    "sector data @ 0x%llx",
					    secdata_desc->addr);
					goto out;
				}

				if (vioblk_finish_write(info)) {
					log_warnx("wr vioblk: disk write "
					    "error");
					vioblk_free_info(info);
					goto out;
				}

				vioblk_free_info(info);

				secbias += secdata_desc->len /
				    VIRTIO_BLK_SECTOR_SIZE;

				secdata_desc_idx = secdata_desc->next &
				    VIOBLK_QUEUE_MASK;
				secdata_desc = &desc[secdata_desc_idx];

				/* Guard against infinite chains */
				if (++cnt >= VIOBLK_QUEUE_SIZE) {
					log_warnx("%s: descriptor table "
					    "invalid", __func__);
					goto out;
				}
			} while (secdata_desc->flags & VRING_DESC_F_NEXT);

			ds_desc_idx = secdata_desc_idx;
			ds_desc = secdata_desc;

			ds = VIRTIO_BLK_S_OK;
			break;
		case VIRTIO_BLK_T_FLUSH:
		case VIRTIO_BLK_T_FLUSH_OUT:
			ds_desc_idx = cmd_desc->next & VIOBLK_QUEUE_MASK;
			ds_desc = &desc[ds_desc_idx];

			ds = VIRTIO_BLK_S_UNSUPP;
			break;
		case VIRTIO_BLK_T_GET_ID:
			secdata_desc_idx = cmd_desc->next & VIOBLK_QUEUE_MASK;
			secdata_desc = &desc[secdata_desc_idx];

			/*
			 * We don't support this command yet. While it's not
			 * officially part of the virtio spec (will be in v1.2)
			 * there's no feature to negotiate. Linux drivers will
			 * often send this command regardless.
			 *
			 * When the command is received, it should appear as a
			 * chain of 3 descriptors, similar to the IN/OUT
			 * commands. The middle descriptor should have have a
			 * length of VIRTIO_BLK_ID_BYTES bytes.
			 */
			if ((secdata_desc->flags & VRING_DESC_F_NEXT) == 0) {
				log_warnx("id vioblk: unchained vioblk data "
				    "descriptor received (idx %d)",
				    cmd_desc_idx);
				goto out;
			}

			/* Skip the data descriptor. */
			ds_desc_idx = secdata_desc->next & VIOBLK_QUEUE_MASK;
			ds_desc = &desc[ds_desc_idx];

			ds = VIRTIO_BLK_S_UNSUPP;
			break;
		default:
			log_warnx("%s: unsupported command 0x%x", __func__,
			    cmd.type);
			ds_desc_idx = cmd_desc->next & VIOBLK_QUEUE_MASK;
			ds_desc = &desc[ds_desc_idx];

			ds = VIRTIO_BLK_S_UNSUPP;
			break;
		}

		if ((ds_desc->flags & VRING_DESC_F_WRITE) == 0) {
			log_warnx("%s: ds descriptor %d unwritable", __func__,
			    ds_desc_idx);
			goto out;
		}
		if (write_mem(ds_desc->addr, &ds, sizeof(ds))) {
			log_warnx("%s: can't write device status data @ 0x%llx",
			    __func__, ds_desc->addr);
			goto out;
		}

		dev->cfg.isr_status = 1;
		used->ring[used->idx & VIOBLK_QUEUE_MASK].id = cmd_desc_idx;
		used->ring[used->idx & VIOBLK_QUEUE_MASK].len = cmd_desc->len;
		__sync_synchronize();
		used->idx++;

		vq_info->last_avail = avail->idx & VIOBLK_QUEUE_MASK;
		idx = (idx + 1) & VIOBLK_QUEUE_MASK;
	}
out:
	return (1);
}

int
virtio_blk_io(int dir, uint16_t reg, uint32_t *data, uint8_t *intr,
    void *cookie, uint8_t sz)
{
	struct vioblk_dev *dev = (struct vioblk_dev *)cookie;

	*intr = 0xFF;


	if (dir == 0) {
		switch (reg) {
		case VIRTIO_CONFIG_DEVICE_FEATURES:
		case VIRTIO_CONFIG_QUEUE_SIZE:
		case VIRTIO_CONFIG_ISR_STATUS:
			log_warnx("%s: illegal write %x to %s",
			    __progname, *data, virtio_reg_name(reg));
			break;
		case VIRTIO_CONFIG_GUEST_FEATURES:
			dev->cfg.guest_feature = *data;
			break;
		case VIRTIO_CONFIG_QUEUE_PFN:
			dev->cfg.queue_pfn = *data;
			vioblk_update_qa(dev);
			break;
		case VIRTIO_CONFIG_QUEUE_SELECT:
			dev->cfg.queue_select = *data;
			vioblk_update_qs(dev);
			break;
		case VIRTIO_CONFIG_QUEUE_NOTIFY:
			dev->cfg.queue_notify = *data;
			if (vioblk_notifyq(dev))
				*intr = 1;
			break;
		case VIRTIO_CONFIG_DEVICE_STATUS:
			dev->cfg.device_status = *data;
			if (dev->cfg.device_status == 0) {
				log_debug("%s: device reset", __func__);
				dev->cfg.guest_feature = 0;
				dev->cfg.queue_pfn = 0;
				vioblk_update_qa(dev);
				dev->cfg.queue_size = 0;
				vioblk_update_qs(dev);
				dev->cfg.queue_select = 0;
				dev->cfg.queue_notify = 0;
				dev->cfg.isr_status = 0;
				dev->vq[0].last_avail = 0;
				vcpu_deassert_pic_irq(dev->vm_id, 0, dev->irq);
			}
			break;
		default:
			break;
		}
	} else {
		switch (reg) {
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI:
			switch (sz) {
			case 4:
				*data = (uint32_t)(dev->sz);
				break;
			case 2:
				*data &= 0xFFFF0000;
				*data |= (uint32_t)(dev->sz) & 0xFFFF;
				break;
			case 1:
				*data &= 0xFFFFFF00;
				*data |= (uint32_t)(dev->sz) & 0xFF;
				break;
			}
			/* XXX handle invalid sz */
			break;
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 1:
			if (sz == 1) {
				*data &= 0xFFFFFF00;
				*data |= (uint32_t)(dev->sz >> 8) & 0xFF;
			}
			/* XXX handle invalid sz */
			break;
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 2:
			if (sz == 1) {
				*data &= 0xFFFFFF00;
				*data |= (uint32_t)(dev->sz >> 16) & 0xFF;
			} else if (sz == 2) {
				*data &= 0xFFFF0000;
				*data |= (uint32_t)(dev->sz >> 16) & 0xFFFF;
			}
			/* XXX handle invalid sz */
			break;
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 3:
			if (sz == 1) {
				*data &= 0xFFFFFF00;
				*data |= (uint32_t)(dev->sz >> 24) & 0xFF;
			}
			/* XXX handle invalid sz */
			break;
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 4:
			switch (sz) {
			case 4:
				*data = (uint32_t)(dev->sz >> 32);
				break;
			case 2:
				*data &= 0xFFFF0000;
				*data |= (uint32_t)(dev->sz >> 32) & 0xFFFF;
				break;
			case 1:
				*data &= 0xFFFFFF00;
				*data |= (uint32_t)(dev->sz >> 32) & 0xFF;
				break;
			}
			/* XXX handle invalid sz */
			break;
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 5:
			if (sz == 1) {
				*data &= 0xFFFFFF00;
				*data |= (uint32_t)(dev->sz >> 40) & 0xFF;
			}
			/* XXX handle invalid sz */
			break;
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 6:
			if (sz == 1) {
				*data &= 0xFFFFFF00;
				*data |= (uint32_t)(dev->sz >> 48) & 0xFF;
			} else if (sz == 2) {
				*data &= 0xFFFF0000;
				*data |= (uint32_t)(dev->sz >> 48) & 0xFFFF;
			}
			/* XXX handle invalid sz */
			break;
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 7:
			if (sz == 1) {
				*data &= 0xFFFFFF00;
				*data |= (uint32_t)(dev->sz >> 56) & 0xFF;
			}
			/* XXX handle invalid sz */
			break;
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 8:
			switch (sz) {
			case 4:
				*data = (uint32_t)(dev->max_xfer);
				break;
			case 2:
				*data &= 0xFFFF0000;
				*data |= (uint32_t)(dev->max_xfer) & 0xFFFF;
				break;
			case 1:
				*data &= 0xFFFFFF00;
				*data |= (uint32_t)(dev->max_xfer) & 0xFF;
				break;
			}
			/* XXX handle invalid sz */
			break;
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 9:
			if (sz == 1) {
				*data &= 0xFFFFFF00;
				*data |= (uint32_t)(dev->max_xfer >> 8) & 0xFF;
			}
			/* XXX handle invalid sz */
			break;
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 10:
			if (sz == 1) {
				*data &= 0xFFFFFF00;
				*data |= (uint32_t)(dev->max_xfer >> 16) & 0xFF;
			} else if (sz == 2) {
				*data &= 0xFFFF0000;
				*data |= (uint32_t)(dev->max_xfer >> 16)
				    & 0xFFFF;
			}
			/* XXX handle invalid sz */
			break;
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 11:
			if (sz == 1) {
				*data &= 0xFFFFFF00;
				*data |= (uint32_t)(dev->max_xfer >> 24) & 0xFF;
			}
			/* XXX handle invalid sz */
			break;
		case VIRTIO_CONFIG_DEVICE_FEATURES:
			*data = dev->cfg.device_feature;
			break;
		case VIRTIO_CONFIG_GUEST_FEATURES:
			*data = dev->cfg.guest_feature;
			break;
		case VIRTIO_CONFIG_QUEUE_PFN:
			*data = dev->cfg.queue_pfn;
			break;
		case VIRTIO_CONFIG_QUEUE_SIZE:
			if (sz == 4)
				*data = dev->cfg.queue_size;
			else if (sz == 2) {
				*data &= 0xFFFF0000;
				*data |= (uint16_t)dev->cfg.queue_size;
			} else if (sz == 1) {
				*data &= 0xFFFFFF00;
				*data |= (uint8_t)dev->cfg.queue_size;
			}
			break;
		case VIRTIO_CONFIG_QUEUE_SELECT:
			*data = dev->cfg.queue_select;
			break;
		case VIRTIO_CONFIG_QUEUE_NOTIFY:
			*data = dev->cfg.queue_notify;
			break;
		case VIRTIO_CONFIG_DEVICE_STATUS:
			if (sz == 4)
				*data = dev->cfg.device_status;
			else if (sz == 2) {
				*data &= 0xFFFF0000;
				*data |= (uint16_t)dev->cfg.device_status;
			} else if (sz == 1) {
				*data &= 0xFFFFFF00;
				*data |= (uint8_t)dev->cfg.device_status;
			}
			break;
		case VIRTIO_CONFIG_ISR_STATUS:
			*data = dev->cfg.isr_status;
			dev->cfg.isr_status = 0;
			vcpu_deassert_pic_irq(dev->vm_id, 0, dev->irq);
			break;
		}
	}
	return (0);
}

int
virtio_net_io(int dir, uint16_t reg, uint32_t *data, uint8_t *intr,
    void *cookie, uint8_t sz)
{
	struct vionet_dev *dev = (struct vionet_dev *)cookie;

	*intr = 0xFF;
	mutex_lock(&dev->mutex);

	if (dir == 0) {
		switch (reg) {
		case VIRTIO_CONFIG_DEVICE_FEATURES:
		case VIRTIO_CONFIG_QUEUE_SIZE:
		case VIRTIO_CONFIG_ISR_STATUS:
			log_warnx("%s: illegal write %x to %s",
			    __progname, *data, virtio_reg_name(reg));
			break;
		case VIRTIO_CONFIG_GUEST_FEATURES:
			dev->cfg.guest_feature = *data;
			break;
		case VIRTIO_CONFIG_QUEUE_PFN:
			dev->cfg.queue_pfn = *data;
			vionet_update_qa(dev);
			break;
		case VIRTIO_CONFIG_QUEUE_SELECT:
			dev->cfg.queue_select = *data;
			vionet_update_qs(dev);
			break;
		case VIRTIO_CONFIG_QUEUE_NOTIFY:
			dev->cfg.queue_notify = *data;
			if (vionet_notifyq(dev))
				*intr = 1;
			break;
		case VIRTIO_CONFIG_DEVICE_STATUS:
			dev->cfg.device_status = *data;
			if (dev->cfg.device_status == 0) {
				log_debug("%s: device reset", __func__);
				dev->cfg.guest_feature = 0;
				dev->cfg.queue_pfn = 0;
				vionet_update_qa(dev);
				dev->cfg.queue_size = 0;
				vionet_update_qs(dev);
				dev->cfg.queue_select = 0;
				dev->cfg.queue_notify = 0;
				dev->cfg.isr_status = 0;
				dev->vq[RXQ].last_avail = 0;
				dev->vq[RXQ].notified_avail = 0;
				dev->vq[TXQ].last_avail = 0;
				dev->vq[TXQ].notified_avail = 0;
				vcpu_deassert_pic_irq(dev->vm_id, 0, dev->irq);
			}
			break;
		default:
			break;
		}
	} else {
		switch (reg) {
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI:
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 1:
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 2:
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 3:
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 4:
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 5:
			*data = dev->mac[reg -
			    VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI];
			break;
		case VIRTIO_CONFIG_DEVICE_FEATURES:
			*data = dev->cfg.device_feature;
			break;
		case VIRTIO_CONFIG_GUEST_FEATURES:
			*data = dev->cfg.guest_feature;
			break;
		case VIRTIO_CONFIG_QUEUE_PFN:
			*data = dev->cfg.queue_pfn;
			break;
		case VIRTIO_CONFIG_QUEUE_SIZE:
			*data = dev->cfg.queue_size;
			break;
		case VIRTIO_CONFIG_QUEUE_SELECT:
			*data = dev->cfg.queue_select;
			break;
		case VIRTIO_CONFIG_QUEUE_NOTIFY:
			*data = dev->cfg.queue_notify;
			break;
		case VIRTIO_CONFIG_DEVICE_STATUS:
			*data = dev->cfg.device_status;
			break;
		case VIRTIO_CONFIG_ISR_STATUS:
			*data = dev->cfg.isr_status;
			dev->cfg.isr_status = 0;
			vcpu_deassert_pic_irq(dev->vm_id, 0, dev->irq);
			break;
		}
	}

	mutex_unlock(&dev->mutex);
	return (0);
}

/*
 * Must be called with dev->mutex acquired.
 */
void
vionet_update_qa(struct vionet_dev *dev)
{
	struct virtio_vq_info *vq_info;
	void *hva = NULL;

	/* Invalid queue? */
	if (dev->cfg.queue_select > 1)
		return;

	vq_info = &dev->vq[dev->cfg.queue_select];
	vq_info->q_gpa = (uint64_t)dev->cfg.queue_pfn * VIRTIO_PAGE_SIZE;

	hva = hvaddr_mem(vq_info->q_gpa, vring_size(VIONET_QUEUE_SIZE));
	if (hva == NULL)
		fatal("vionet_update_qa");
	vq_info->q_hva = hva;
}

/*
 * Must be called with dev->mutex acquired.
 */
void
vionet_update_qs(struct vionet_dev *dev)
{
	struct virtio_vq_info *vq_info;

	/* Invalid queue? */
	if (dev->cfg.queue_select > 1) {
		dev->cfg.queue_size = 0;
		return;
	}

	vq_info = &dev->vq[dev->cfg.queue_select];

	/* Update queue pfn/size based on queue select */
	dev->cfg.queue_pfn = vq_info->q_gpa >> 12;
	dev->cfg.queue_size = vq_info->qs;
}

/*
 * vionet_enq_rx
 *
 * Take a given packet from the host-side tap and copy it into the guest's
 * buffers utilizing the rx virtio ring. If the packet length is invalid
 * (too small or too large) or if there are not enough buffers available,
 * the packet is dropped.
 *
 * Must be called with dev->mutex acquired.
 */
int
vionet_enq_rx(struct vionet_dev *dev, char *pkt, size_t sz, int *spc)
{
	uint16_t dxx, idx, hdr_desc_idx, chain_hdr_idx;
	char *vr = NULL;
	size_t bufsz = 0, off = 0, pkt_offset = 0, chunk_size = 0;
	size_t chain_len = 0;
	struct vring_desc *desc, *pkt_desc, *hdr_desc;
	struct vring_avail *avail;
	struct vring_used *used;
	struct virtio_vq_info *vq_info;
	struct virtio_net_hdr hdr;
	size_t hdr_sz;

	if (sz < VIONET_MIN_TXLEN || sz > VIONET_MAX_TXLEN) {
		log_warn("%s: invalid packet size", __func__);
		return (0);
	}

	hdr_sz = sizeof(hdr);

	if (!(dev->cfg.device_status & VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK))
		return (0);

	vq_info = &dev->vq[RXQ];
	vr = vq_info->q_hva;
	if (vr == NULL)
		fatalx("%s: null vring", __func__);

	/* Compute offsets in ring of descriptors, avail ring, and used ring */
	desc = (struct vring_desc *)(vr);
	avail = (struct vring_avail *)(vr + vq_info->vq_availoffset);
	used = (struct vring_used *)(vr + vq_info->vq_usedoffset);

	idx = vq_info->last_avail & VIONET_QUEUE_MASK;
	if ((vq_info->notified_avail & VIONET_QUEUE_MASK) == idx) {
		log_debug("%s: insufficient available buffer capacity, "
		    "dropping packet.", __func__);
		return (0);
	}

	hdr_desc_idx = avail->ring[idx] & VIONET_QUEUE_MASK;
	hdr_desc = &desc[hdr_desc_idx];

	dxx = hdr_desc_idx;
	chain_hdr_idx = dxx;
	chain_len = 0;

	/* Process the descriptor and walk any potential chain. */
	do {
		off = 0;
		pkt_desc = &desc[dxx];
		if (!(pkt_desc->flags & VRING_DESC_F_WRITE)) {
			log_warnx("%s: invalid descriptor, not writable",
			    __func__);
			return (0);
		}

		/* How much data do we get to write? */
		if (sz - bufsz > pkt_desc->len)
			chunk_size = pkt_desc->len;
		else
			chunk_size = sz - bufsz;

		if (chain_len == 0) {
			off = hdr_sz;
			if (chunk_size == pkt_desc->len)
				chunk_size -= off;
		}

		/* Write a chunk of data if we need to */
		if (chunk_size && write_mem(pkt_desc->addr + off,
			pkt + pkt_offset, chunk_size)) {
			log_warnx("%s: failed to write to buffer 0x%llx",
			    __func__, pkt_desc->addr);
			return (0);
		}

		chain_len += chunk_size + off;
		bufsz += chunk_size;
		pkt_offset += chunk_size;

		dxx = pkt_desc->next & VIONET_QUEUE_MASK;
	} while (bufsz < sz && pkt_desc->flags & VRING_DESC_F_NEXT);

	/* Move our marker in the ring...*/
	vq_info->last_avail = (vq_info->last_avail + 1) &
	    VIONET_QUEUE_MASK;

	/* Prepend the virtio net header in the first buffer. */
	memset(&hdr, 0, sizeof(hdr));
	hdr.hdr_len = hdr_sz;
	if (write_mem(hdr_desc->addr, &hdr, hdr_sz)) {
	    log_warnx("vionet: rx enq header write_mem error @ 0x%llx",
		hdr_desc->addr);
	    return (0);
	}

	/* Update the index field in the used ring. This must be done last. */
	dev->cfg.isr_status = 1;
	*spc = (vq_info->notified_avail - vq_info->last_avail)
	    & VIONET_QUEUE_MASK;

	/* Update the list of used buffers. */
	used->ring[used->idx & VIONET_QUEUE_MASK].id = chain_hdr_idx;
	used->ring[used->idx & VIONET_QUEUE_MASK].len = chain_len;
	__sync_synchronize();
	used->idx++;

	return (1);
}

/*
 * vionet_rx
 *
 * Enqueue data that was received on a tap file descriptor
 * to the vionet device queue.
 *
 * Must be called with dev->mutex acquired.
 */
static int
vionet_rx(struct vionet_dev *dev)
{
	char buf[PAGE_SIZE];
	int num_enq = 0, spc = 0;
	struct ether_header *eh;
	ssize_t sz;

	do {
		sz = read(dev->fd, buf, sizeof(buf));
		if (sz == -1) {
			/*
			 * If we get EAGAIN, No data is currently available.
			 * Do not treat this as an error.
			 */
			if (errno != EAGAIN)
				log_warn("unexpected read error on vionet "
				    "device");
		} else if (sz > 0) {
			eh = (struct ether_header *)buf;
			if (!dev->lockedmac ||
			    ETHER_IS_MULTICAST(eh->ether_dhost) ||
			    memcmp(eh->ether_dhost, dev->mac,
			    sizeof(eh->ether_dhost)) == 0)
				num_enq += vionet_enq_rx(dev, buf, sz, &spc);
		} else if (sz == 0) {
			log_debug("process_rx: no data");
			break;
		}
	} while (spc > 0 && sz > 0);

	return (num_enq);
}

/*
 * vionet_rx_event
 *
 * Called from the event handling thread when new data can be
 * received on the tap fd of a vionet device.
 */
static void
vionet_rx_event(int fd, short kind, void *arg)
{
	struct vionet_dev *dev = arg;

	mutex_lock(&dev->mutex);

	if (vionet_rx(dev) > 0) {
		/* XXX: vcpu_id */
		vcpu_assert_pic_irq(dev->vm_id, 0, dev->irq);
	}

	mutex_unlock(&dev->mutex);
}

/*
 * Must be called with dev->mutex acquired.
 */
void
vionet_notify_rx(struct vionet_dev *dev)
{
	char *vr;
	struct vring_avail *avail;
	struct virtio_vq_info *vq_info;

	vq_info = &dev->vq[RXQ];
	vr = vq_info->q_hva;
	if (vr == NULL)
		fatalx("%s: null vring", __func__);

	/* Compute offset into avail ring */
	avail = (struct vring_avail *)(vr + vq_info->vq_availoffset);
	vq_info->notified_avail = avail->idx - 1;
}

/*
 * Must be called with dev->mutex acquired.
 */
int
vionet_notifyq(struct vionet_dev *dev)
{
	int ret = 0;

	switch (dev->cfg.queue_notify) {
	case RXQ:
		vionet_notify_rx(dev);
		break;
	case TXQ:
		ret = vionet_notify_tx(dev);
		break;
	default:
		/*
		 * Catch the unimplemented queue ID 2 (control queue) as
		 * well as any bogus queue IDs.
		 */
		log_debug("%s: notify for unimplemented queue ID %d",
		    __func__, dev->cfg.queue_notify);
		break;
	}

	return (ret);
}

/*
 * Must be called with dev->mutex acquired.
 */
int
vionet_notify_tx(struct vionet_dev *dev)
{
	uint16_t idx, pkt_desc_idx, hdr_desc_idx, dxx, cnt;
	size_t pktsz, chunk_size = 0;
	ssize_t dhcpsz = 0;
	int num_enq, ofs, spc = 0;
	char *vr = NULL, *pkt = NULL, *dhcppkt = NULL;
	struct vring_desc *desc, *pkt_desc, *hdr_desc;
	struct vring_avail *avail;
	struct vring_used *used;
	struct virtio_vq_info *vq_info;
	struct ether_header *eh;

	vq_info = &dev->vq[TXQ];
	vr = vq_info->q_hva;
	if (vr == NULL)
		fatalx("%s: null vring", __func__);

	/* Compute offsets in ring of descriptors, avail ring, and used ring */
	desc = (struct vring_desc *)(vr);
	avail = (struct vring_avail *)(vr + vq_info->vq_availoffset);
	used = (struct vring_used *)(vr + vq_info->vq_usedoffset);

	num_enq = 0;

	idx = vq_info->last_avail & VIONET_QUEUE_MASK;

	if ((avail->idx & VIONET_QUEUE_MASK) == idx) {
		log_debug("%s - nothing to do?", __func__);
		return (0);
	}

	while ((avail->idx & VIONET_QUEUE_MASK) != idx) {
		hdr_desc_idx = avail->ring[idx] & VIONET_QUEUE_MASK;
		hdr_desc = &desc[hdr_desc_idx];
		pktsz = 0;

		cnt = 0;
		dxx = hdr_desc_idx;
		do {
			pktsz += desc[dxx].len;
			dxx = desc[dxx].next & VIONET_QUEUE_MASK;

			/*
			 * Virtio 1.0, cs04, section 2.4.5:
			 *  "The number of descriptors in the table is defined
			 *   by the queue size for this virtqueue: this is the
			 *   maximum possible descriptor chain length."
			 */
			if (++cnt >= VIONET_QUEUE_SIZE) {
				log_warnx("%s: descriptor table invalid",
				    __func__);
				goto out;
			}
		} while (desc[dxx].flags & VRING_DESC_F_NEXT);

		pktsz += desc[dxx].len;

		/* Remove virtio header descriptor len */
		pktsz -= hdr_desc->len;

		/* Drop packets violating device MTU-based limits */
		if (pktsz < VIONET_MIN_TXLEN || pktsz > VIONET_MAX_TXLEN) {
			log_warnx("%s: invalid packet size %lu", __func__,
			    pktsz);
			goto drop_packet;
		}
		pkt = malloc(pktsz);
		if (pkt == NULL) {
			log_warn("malloc error alloc packet buf");
			goto out;
		}

		ofs = 0;
		pkt_desc_idx = hdr_desc->next & VIONET_QUEUE_MASK;
		pkt_desc = &desc[pkt_desc_idx];

		while (pkt_desc->flags & VRING_DESC_F_NEXT) {
			/* must be not writable */
			if (pkt_desc->flags & VRING_DESC_F_WRITE) {
				log_warnx("unexpected writable tx desc "
				    "%d", pkt_desc_idx);
				goto out;
			}

			/* Check we don't read beyond allocated pktsz */
			if (pkt_desc->len > pktsz - ofs) {
				log_warnx("%s: descriptor len past pkt len",
				    __func__);
				chunk_size = pktsz - ofs;
			} else
				chunk_size = pkt_desc->len;

			/* Read packet from descriptor ring */
			if (read_mem(pkt_desc->addr, pkt + ofs, chunk_size)) {
				log_warnx("vionet: packet read_mem error "
				    "@ 0x%llx", pkt_desc->addr);
				goto out;
			}

			ofs += pkt_desc->len;
			pkt_desc_idx = pkt_desc->next & VIONET_QUEUE_MASK;
			pkt_desc = &desc[pkt_desc_idx];
		}

		/* Now handle tail descriptor - must be not writable */
		if (pkt_desc->flags & VRING_DESC_F_WRITE) {
			log_warnx("unexpected writable tx descriptor %d",
			    pkt_desc_idx);
			goto out;
		}

		/* Check we don't read beyond allocated pktsz */
		if (pkt_desc->len > pktsz - ofs) {
			log_warnx("%s: descriptor len past pkt len", __func__);
			chunk_size = pktsz - ofs - pkt_desc->len;
		} else
			chunk_size = pkt_desc->len;

		/* Read packet from descriptor ring */
		if (read_mem(pkt_desc->addr, pkt + ofs, chunk_size)) {
			log_warnx("vionet: packet read_mem error @ "
			    "0x%llx", pkt_desc->addr);
			goto out;
		}

		/* reject other source addresses */
		if (dev->lockedmac && pktsz >= ETHER_HDR_LEN &&
		    (eh = (struct ether_header *)pkt) &&
		    memcmp(eh->ether_shost, dev->mac,
		    sizeof(eh->ether_shost)) != 0)
			log_debug("vionet: wrong source address %s for vm %d",
			    ether_ntoa((struct ether_addr *)
			    eh->ether_shost), dev->vm_id);
		else if (dev->local &&
		    (dhcpsz = dhcp_request(dev, pkt, pktsz, &dhcppkt)) != -1) {
			log_debug("vionet: dhcp request,"
			    " local response size %zd", dhcpsz);

		/* XXX signed vs unsigned here, funky cast */
		} else if (write(dev->fd, pkt, pktsz) != (int)pktsz) {
			log_warnx("vionet: tx failed writing to tap: "
			    "%d", errno);
			goto out;
		}

	drop_packet:
		dev->cfg.isr_status = 1;
		used->ring[used->idx & VIONET_QUEUE_MASK].id = hdr_desc_idx;
		used->ring[used->idx & VIONET_QUEUE_MASK].len = hdr_desc->len;
		__sync_synchronize();
		used->idx++;

		vq_info->last_avail = avail->idx & VIONET_QUEUE_MASK;
		idx = (idx + 1) & VIONET_QUEUE_MASK;

		num_enq++;

		free(pkt);
		pkt = NULL;
	}

	if (dhcpsz > 0)
		vionet_enq_rx(dev, dhcppkt, dhcpsz, &spc);

out:
	free(pkt);
	free(dhcppkt);

	return (1);
}

int
vmmci_ctl(unsigned int cmd)
{
	struct timeval tv = { 0, 0 };

	if ((vmmci.cfg.device_status &
	    VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK) == 0)
		return (-1);

	if (cmd == vmmci.cmd)
		return (0);

	switch (cmd) {
	case VMMCI_NONE:
		break;
	case VMMCI_SHUTDOWN:
	case VMMCI_REBOOT:
		/* Update command */
		vmmci.cmd = cmd;

		/*
		 * vmm VMs do not support powerdown, send a reboot request
		 * instead and turn it off after the triple fault.
		 */
		if (cmd == VMMCI_SHUTDOWN)
			cmd = VMMCI_REBOOT;

		/* Trigger interrupt */
		vmmci.cfg.isr_status = VIRTIO_CONFIG_ISR_CONFIG_CHANGE;
		vcpu_assert_pic_irq(vmmci.vm_id, 0, vmmci.irq);

		/* Add ACK timeout */
		tv.tv_sec = VMMCI_TIMEOUT;
		evtimer_add(&vmmci.timeout, &tv);
		break;
	case VMMCI_SYNCRTC:
		if (vmmci.cfg.guest_feature & VMMCI_F_SYNCRTC) {
			/* RTC updated, request guest VM resync of its RTC */
			vmmci.cmd = cmd;

			vmmci.cfg.isr_status = VIRTIO_CONFIG_ISR_CONFIG_CHANGE;
			vcpu_assert_pic_irq(vmmci.vm_id, 0, vmmci.irq);
		} else {
			log_debug("%s: RTC sync skipped (guest does not "
			    "support RTC sync)\n", __func__);
		}
		break;
	default:
		fatalx("invalid vmmci command: %d", cmd);
	}

	return (0);
}

void
vmmci_ack(unsigned int cmd)
{
	struct timeval	 tv = { 0, 0 };

	switch (cmd) {
	case VMMCI_NONE:
		break;
	case VMMCI_SHUTDOWN:
		/*
		 * The shutdown was requested by the VM if we don't have
		 * a pending shutdown request.  In this case add a short
		 * timeout to give the VM a chance to reboot before the
		 * timer is expired.
		 */
		if (vmmci.cmd == 0) {
			log_debug("%s: vm %u requested shutdown", __func__,
			    vmmci.vm_id);
			tv.tv_sec = VMMCI_TIMEOUT;
			evtimer_add(&vmmci.timeout, &tv);
			return;
		}
		/* FALLTHROUGH */
	case VMMCI_REBOOT:
		/*
		 * If the VM acknowleged our shutdown request, give it
		 * enough time to shutdown or reboot gracefully.  This
		 * might take a considerable amount of time (running
		 * rc.shutdown on the VM), so increase the timeout before
		 * killing it forcefully.
		 */
		if (cmd == vmmci.cmd &&
		    evtimer_pending(&vmmci.timeout, NULL)) {
			log_debug("%s: vm %u acknowledged shutdown request",
			    __func__, vmmci.vm_id);
			tv.tv_sec = VMMCI_SHUTDOWN_TIMEOUT;
			evtimer_add(&vmmci.timeout, &tv);
		}
		break;
	case VMMCI_SYNCRTC:
		log_debug("%s: vm %u acknowledged RTC sync request",
		    __func__, vmmci.vm_id);
		vmmci.cmd = VMMCI_NONE;
		break;
	default:
		log_warnx("%s: illegal request %u", __func__, cmd);
		break;
	}
}

void
vmmci_timeout(int fd, short type, void *arg)
{
	log_debug("%s: vm %u shutdown", __progname, vmmci.vm_id);
	vm_shutdown(vmmci.cmd == VMMCI_REBOOT ? VMMCI_REBOOT : VMMCI_SHUTDOWN);
}

int
vmmci_io(int dir, uint16_t reg, uint32_t *data, uint8_t *intr,
    void *unused, uint8_t sz)
{
	*intr = 0xFF;

	if (dir == 0) {
		switch (reg) {
		case VIRTIO_CONFIG_DEVICE_FEATURES:
		case VIRTIO_CONFIG_QUEUE_SIZE:
		case VIRTIO_CONFIG_ISR_STATUS:
			log_warnx("%s: illegal write %x to %s",
			    __progname, *data, virtio_reg_name(reg));
			break;
		case VIRTIO_CONFIG_GUEST_FEATURES:
			vmmci.cfg.guest_feature = *data;
			break;
		case VIRTIO_CONFIG_QUEUE_PFN:
			vmmci.cfg.queue_pfn = *data;
			break;
		case VIRTIO_CONFIG_QUEUE_SELECT:
			vmmci.cfg.queue_select = *data;
			break;
		case VIRTIO_CONFIG_QUEUE_NOTIFY:
			vmmci.cfg.queue_notify = *data;
			break;
		case VIRTIO_CONFIG_DEVICE_STATUS:
			vmmci.cfg.device_status = *data;
			break;
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI:
			vmmci_ack(*data);
			break;
		}
	} else {
		switch (reg) {
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI:
			*data = vmmci.cmd;
			break;
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 4:
			/* Update time once when reading the first register */
			gettimeofday(&vmmci.time, NULL);
			*data = (uint64_t)vmmci.time.tv_sec;
			break;
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 8:
			*data = (uint64_t)vmmci.time.tv_sec << 32;
			break;
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 12:
			*data = (uint64_t)vmmci.time.tv_usec;
			break;
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 16:
			*data = (uint64_t)vmmci.time.tv_usec << 32;
			break;
		case VIRTIO_CONFIG_DEVICE_FEATURES:
			*data = vmmci.cfg.device_feature;
			break;
		case VIRTIO_CONFIG_GUEST_FEATURES:
			*data = vmmci.cfg.guest_feature;
			break;
		case VIRTIO_CONFIG_QUEUE_PFN:
			*data = vmmci.cfg.queue_pfn;
			break;
		case VIRTIO_CONFIG_QUEUE_SIZE:
			*data = vmmci.cfg.queue_size;
			break;
		case VIRTIO_CONFIG_QUEUE_SELECT:
			*data = vmmci.cfg.queue_select;
			break;
		case VIRTIO_CONFIG_QUEUE_NOTIFY:
			*data = vmmci.cfg.queue_notify;
			break;
		case VIRTIO_CONFIG_DEVICE_STATUS:
			*data = vmmci.cfg.device_status;
			break;
		case VIRTIO_CONFIG_ISR_STATUS:
			*data = vmmci.cfg.isr_status;
			vmmci.cfg.isr_status = 0;
			vcpu_deassert_pic_irq(vmmci.vm_id, 0, vmmci.irq);
			break;
		}
	}
	return (0);
}

int
virtio_get_base(int fd, char *path, size_t npath, int type, const char *dpath)
{
	switch (type) {
	case VMDF_RAW:
		return 0;
	case VMDF_QCOW2:
		return virtio_qcow2_get_base(fd, path, npath, dpath);
	}
	log_warnx("%s: invalid disk format", __func__);
	return -1;
}

/*
 * Initializes a struct virtio_backing using the list of fds.
 */
static int
virtio_init_disk(struct virtio_backing *file, off_t *sz,
    int *fd, size_t nfd, int type)
{
	/*
	 * probe disk types in order of preference, first one to work wins.
	 * TODO: provide a way of specifying the type and options.
	 */
	switch (type) {
	case VMDF_RAW:
		return virtio_raw_init(file, sz, fd, nfd);
	case VMDF_QCOW2:
		return virtio_qcow2_init(file, sz, fd, nfd);
	}
	log_warnx("%s: invalid disk format", __func__);
	return -1;
}

void
virtio_init(struct vmd_vm *vm, int child_cdrom,
    int child_disks[][VM_MAX_BASE_PER_DISK], int *child_taps)
{
	struct vmop_create_params *vmc = &vm->vm_params;
	struct vm_create_params *vcp = &vmc->vmc_params;
	uint8_t id;
	uint8_t i;
	int ret;

	/* Virtio entropy device */
	if (pci_add_device(&id, PCI_VENDOR_QUMRANET,
	    PCI_PRODUCT_QUMRANET_VIO_RNG, PCI_CLASS_SYSTEM,
	    PCI_SUBCLASS_SYSTEM_MISC,
	    PCI_VENDOR_OPENBSD,
	    PCI_PRODUCT_VIRTIO_ENTROPY, 1, NULL)) {
		log_warnx("%s: can't add PCI virtio rng device",
		    __progname);
		return;
	}

	if (pci_add_bar(id, PCI_MAPREG_TYPE_IO, virtio_rnd_io, NULL)) {
		log_warnx("%s: can't add bar for virtio rng device",
		    __progname);
		return;
	}

	memset(&viornd, 0, sizeof(viornd));
	viornd.vq[0].qs = VIORND_QUEUE_SIZE;
	viornd.vq[0].vq_availoffset = sizeof(struct vring_desc) *
	    VIORND_QUEUE_SIZE;
	viornd.vq[0].vq_usedoffset = VIRTQUEUE_ALIGN(
	    sizeof(struct vring_desc) * VIORND_QUEUE_SIZE
	    + sizeof(uint16_t) * (2 + VIORND_QUEUE_SIZE));
	viornd.pci_id = id;
	viornd.irq = pci_get_dev_irq(id);
	viornd.vm_id = vcp->vcp_id;

	if (vcp->vcp_nnics > 0) {
		vionet = calloc(vcp->vcp_nnics, sizeof(struct vionet_dev));
		if (vionet == NULL) {
			log_warn("%s: calloc failure allocating vionets",
			    __progname);
			return;
		}

		nr_vionet = vcp->vcp_nnics;
		/* Virtio network */
		for (i = 0; i < vcp->vcp_nnics; i++) {
			if (pci_add_device(&id, PCI_VENDOR_QUMRANET,
			    PCI_PRODUCT_QUMRANET_VIO_NET, PCI_CLASS_SYSTEM,
			    PCI_SUBCLASS_SYSTEM_MISC,
			    PCI_VENDOR_OPENBSD,
			    PCI_PRODUCT_VIRTIO_NETWORK, 1, NULL)) {
				log_warnx("%s: can't add PCI virtio net device",
				    __progname);
				return;
			}

			if (pci_add_bar(id, PCI_MAPREG_TYPE_IO, virtio_net_io,
			    &vionet[i])) {
				log_warnx("%s: can't add bar for virtio net "
				    "device", __progname);
				return;
			}

			ret = pthread_mutex_init(&vionet[i].mutex, NULL);
			if (ret) {
				errno = ret;
				log_warn("%s: could not initialize mutex "
				    "for vionet device", __progname);
				return;
			}

			vionet[i].vq[RXQ].qs = VIONET_QUEUE_SIZE;
			vionet[i].vq[RXQ].vq_availoffset =
			    sizeof(struct vring_desc) * VIONET_QUEUE_SIZE;
			vionet[i].vq[RXQ].vq_usedoffset = VIRTQUEUE_ALIGN(
			    sizeof(struct vring_desc) * VIONET_QUEUE_SIZE
			    + sizeof(uint16_t) * (2 + VIONET_QUEUE_SIZE));
			vionet[i].vq[RXQ].last_avail = 0;
			vionet[i].vq[RXQ].notified_avail = 0;

			vionet[i].vq[TXQ].qs = VIONET_QUEUE_SIZE;
			vionet[i].vq[TXQ].vq_availoffset =
			    sizeof(struct vring_desc) * VIONET_QUEUE_SIZE;
			vionet[i].vq[TXQ].vq_usedoffset = VIRTQUEUE_ALIGN(
			    sizeof(struct vring_desc) * VIONET_QUEUE_SIZE
			    + sizeof(uint16_t) * (2 + VIONET_QUEUE_SIZE));
			vionet[i].vq[TXQ].last_avail = 0;
			vionet[i].vq[TXQ].notified_avail = 0;
			vionet[i].fd = child_taps[i];
			vionet[i].vm_id = vcp->vcp_id;
			vionet[i].vm_vmid = vm->vm_vmid;
			vionet[i].irq = pci_get_dev_irq(id);

			event_set(&vionet[i].event, vionet[i].fd,
			    EV_READ | EV_PERSIST, vionet_rx_event, &vionet[i]);
			if (event_add(&vionet[i].event, NULL)) {
				log_warn("could not initialize vionet event "
				    "handler");
				return;
			}

			/* MAC address has been assigned by the parent */
			memcpy(&vionet[i].mac, &vcp->vcp_macs[i], 6);
			vionet[i].cfg.device_feature = VIRTIO_NET_F_MAC;

			vionet[i].lockedmac =
			    vmc->vmc_ifflags[i] & VMIFF_LOCKED ? 1 : 0;
			vionet[i].local =
			    vmc->vmc_ifflags[i] & VMIFF_LOCAL ? 1 : 0;
			if (i == 0 && vmc->vmc_bootdevice & VMBOOTDEV_NET)
				vionet[i].pxeboot = 1;
			vionet[i].idx = i;
			vionet[i].pci_id = id;

			log_debug("%s: vm \"%s\" vio%u lladdr %s%s%s%s",
			    __func__, vcp->vcp_name, i,
			    ether_ntoa((void *)vionet[i].mac),
			    vionet[i].lockedmac ? ", locked" : "",
			    vionet[i].local ? ", local" : "",
			    vionet[i].pxeboot ? ", pxeboot" : "");
		}
	}

	if (vcp->vcp_ndisks > 0) {
		nr_vioblk = vcp->vcp_ndisks;
		vioblk = calloc(vcp->vcp_ndisks, sizeof(struct vioblk_dev));
		if (vioblk == NULL) {
			log_warn("%s: calloc failure allocating vioblks",
			    __progname);
			return;
		}

		/* One virtio block device for each disk defined in vcp */
		for (i = 0; i < vcp->vcp_ndisks; i++) {
			if (pci_add_device(&id, PCI_VENDOR_QUMRANET,
			    PCI_PRODUCT_QUMRANET_VIO_BLOCK,
			    PCI_CLASS_MASS_STORAGE,
			    PCI_SUBCLASS_MASS_STORAGE_SCSI,
			    PCI_VENDOR_OPENBSD,
			    PCI_PRODUCT_VIRTIO_BLOCK, 1, NULL)) {
				log_warnx("%s: can't add PCI virtio block "
				    "device", __progname);
				return;
			}
			if (pci_add_bar(id, PCI_MAPREG_TYPE_IO, virtio_blk_io,
			    &vioblk[i])) {
				log_warnx("%s: can't add bar for virtio block "
				    "device", __progname);
				return;
			}
			vioblk[i].vq[0].qs = VIOBLK_QUEUE_SIZE;
			vioblk[i].vq[0].vq_availoffset =
			    sizeof(struct vring_desc) * VIOBLK_QUEUE_SIZE;
			vioblk[i].vq[0].vq_usedoffset = VIRTQUEUE_ALIGN(
			    sizeof(struct vring_desc) * VIOBLK_QUEUE_SIZE
			    + sizeof(uint16_t) * (2 + VIOBLK_QUEUE_SIZE));
			vioblk[i].vq[0].last_avail = 0;
			vioblk[i].cfg.device_feature = VIRTIO_BLK_F_SIZE_MAX;
			vioblk[i].max_xfer = 1048576;
			vioblk[i].pci_id = id;
			vioblk[i].vm_id = vcp->vcp_id;
			vioblk[i].irq = pci_get_dev_irq(id);
			if (virtio_init_disk(&vioblk[i].file, &vioblk[i].sz,
			    child_disks[i], vmc->vmc_diskbases[i],
			    vmc->vmc_disktypes[i]) == -1) {
				log_warnx("%s: unable to determine disk format",
				    __func__);
				return;
			}
			vioblk[i].sz /= 512;
		}
	}

	/* vioscsi cdrom */
	if (strlen(vcp->vcp_cdrom)) {
		vioscsi = calloc(1, sizeof(struct vioscsi_dev));
		if (vioscsi == NULL) {
			log_warn("%s: calloc failure allocating vioscsi",
			    __progname);
			return;
		}

		if (pci_add_device(&id, PCI_VENDOR_QUMRANET,
		    PCI_PRODUCT_QUMRANET_VIO_SCSI,
		    PCI_CLASS_MASS_STORAGE,
		    PCI_SUBCLASS_MASS_STORAGE_SCSI,
		    PCI_VENDOR_OPENBSD,
		    PCI_PRODUCT_VIRTIO_SCSI, 1, NULL)) {
			log_warnx("%s: can't add PCI vioscsi device",
			    __progname);
			return;
		}

		if (pci_add_bar(id, PCI_MAPREG_TYPE_IO, vioscsi_io, vioscsi)) {
			log_warnx("%s: can't add bar for vioscsi device",
			    __progname);
			return;
		}

		for ( i = 0; i < VIRTIO_MAX_QUEUES; i++) {
			vioscsi->vq[i].qs = VIOSCSI_QUEUE_SIZE;
			vioscsi->vq[i].vq_availoffset =
			    sizeof(struct vring_desc) * VIOSCSI_QUEUE_SIZE;
			vioscsi->vq[i].vq_usedoffset = VIRTQUEUE_ALIGN(
			    sizeof(struct vring_desc) * VIOSCSI_QUEUE_SIZE
			    + sizeof(uint16_t) * (2 + VIOSCSI_QUEUE_SIZE));
			vioscsi->vq[i].last_avail = 0;
		}
		if (virtio_init_disk(&vioscsi->file, &vioscsi->sz,
		    &child_cdrom, 1, VMDF_RAW) == -1) {
			log_warnx("%s: unable to determine iso format",
			    __func__);
			return;
		}
		vioscsi->locked = 0;
		vioscsi->lba = 0;
		vioscsi->n_blocks = vioscsi->sz >> 11; /* num of 2048 blocks in file */
		vioscsi->max_xfer = VIOSCSI_BLOCK_SIZE_CDROM;
		vioscsi->pci_id = id;
		vioscsi->vm_id = vcp->vcp_id;
		vioscsi->irq = pci_get_dev_irq(id);
	}

	/* virtio control device */
	if (pci_add_device(&id, PCI_VENDOR_OPENBSD,
	    PCI_PRODUCT_OPENBSD_CONTROL,
	    PCI_CLASS_COMMUNICATIONS,
	    PCI_SUBCLASS_COMMUNICATIONS_MISC,
	    PCI_VENDOR_OPENBSD,
	    PCI_PRODUCT_VIRTIO_VMMCI, 1, NULL)) {
		log_warnx("%s: can't add PCI vmm control device",
		    __progname);
		return;
	}

	if (pci_add_bar(id, PCI_MAPREG_TYPE_IO, vmmci_io, NULL)) {
		log_warnx("%s: can't add bar for vmm control device",
		    __progname);
		return;
	}

	memset(&vmmci, 0, sizeof(vmmci));
	vmmci.cfg.device_feature = VMMCI_F_TIMESYNC | VMMCI_F_ACK |
	    VMMCI_F_SYNCRTC;
	vmmci.vm_id = vcp->vcp_id;
	vmmci.irq = pci_get_dev_irq(id);
	vmmci.pci_id = id;

	evtimer_set(&vmmci.timeout, vmmci_timeout, NULL);
}

/*
 * vionet_set_hostmac
 *
 * Sets the hardware address for the host-side tap(4) on a vionet_dev.
 *
 * This should only be called from the event-loop thread
 *
 * vm: pointer to the current vmd_vm instance
 * idx: index into the array of vionet_dev's for the target vionet_dev
 * addr: ethernet address to set
 */
void
vionet_set_hostmac(struct vmd_vm *vm, unsigned int idx, uint8_t *addr)
{
	struct vmop_create_params *vmc = &vm->vm_params;
	struct vm_create_params	  *vcp = &vmc->vmc_params;
	struct vionet_dev	  *dev;

	if (idx > vcp->vcp_nnics)
		fatalx("vionet_set_hostmac");

	dev = &vionet[idx];
	memcpy(dev->hostmac, addr, sizeof(dev->hostmac));
}

void
virtio_shutdown(struct vmd_vm *vm)
{
	int i;

	/* ensure that our disks are synced */
	if (vioscsi != NULL)
		vioscsi->file.close(vioscsi->file.p, 0);

	for (i = 0; i < nr_vioblk; i++)
		vioblk[i].file.close(vioblk[i].file.p, 0);
}

int
vmmci_restore(int fd, uint32_t vm_id)
{
	log_debug("%s: receiving vmmci", __func__);
	if (atomicio(read, fd, &vmmci, sizeof(vmmci)) != sizeof(vmmci)) {
		log_warnx("%s: error reading vmmci from fd", __func__);
		return (-1);
	}

	if (pci_set_bar_fn(vmmci.pci_id, 0, vmmci_io, NULL)) {
		log_warnx("%s: can't set bar fn for vmm control device",
		    __progname);
		return (-1);
	}
	vmmci.vm_id = vm_id;
	vmmci.irq = pci_get_dev_irq(vmmci.pci_id);
	memset(&vmmci.timeout, 0, sizeof(struct event));
	evtimer_set(&vmmci.timeout, vmmci_timeout, NULL);
	return (0);
}

int
viornd_restore(int fd, struct vm_create_params *vcp)
{
	log_debug("%s: receiving viornd", __func__);
	if (atomicio(read, fd, &viornd, sizeof(viornd)) != sizeof(viornd)) {
		log_warnx("%s: error reading viornd from fd", __func__);
		return (-1);
	}
	if (pci_set_bar_fn(viornd.pci_id, 0, virtio_rnd_io, NULL)) {
		log_warnx("%s: can't set bar fn for virtio rng device",
		    __progname);
		return (-1);
	}
	viornd.vm_id = vcp->vcp_id;
	viornd.irq = pci_get_dev_irq(viornd.pci_id);

	return (0);
}

int
vionet_restore(int fd, struct vmd_vm *vm, int *child_taps)
{
	struct vmop_create_params *vmc = &vm->vm_params;
	struct vm_create_params *vcp = &vmc->vmc_params;
	uint8_t i;
	int ret;

	nr_vionet = vcp->vcp_nnics;
	if (vcp->vcp_nnics > 0) {
		vionet = calloc(vcp->vcp_nnics, sizeof(struct vionet_dev));
		if (vionet == NULL) {
			log_warn("%s: calloc failure allocating vionets",
			    __progname);
			return (-1);
		}
		log_debug("%s: receiving vionet", __func__);
		if (atomicio(read, fd, vionet,
		    vcp->vcp_nnics * sizeof(struct vionet_dev)) !=
		    vcp->vcp_nnics * sizeof(struct vionet_dev)) {
			log_warnx("%s: error reading vionet from fd",
			    __func__);
			return (-1);
		}

		/* Virtio network */
		for (i = 0; i < vcp->vcp_nnics; i++) {
			if (pci_set_bar_fn(vionet[i].pci_id, 0, virtio_net_io,
			    &vionet[i])) {
				log_warnx("%s: can't set bar fn for virtio net "
				    "device", __progname);
				return (-1);
			}

			memset(&vionet[i].mutex, 0, sizeof(pthread_mutex_t));
			ret = pthread_mutex_init(&vionet[i].mutex, NULL);

			if (ret) {
				errno = ret;
				log_warn("%s: could not initialize mutex "
				    "for vionet device", __progname);
				return (-1);
			}
			vionet[i].fd = child_taps[i];
			vionet[i].vm_id = vcp->vcp_id;
			vionet[i].vm_vmid = vm->vm_vmid;
			vionet[i].irq = pci_get_dev_irq(vionet[i].pci_id);

			memset(&vionet[i].event, 0, sizeof(struct event));
			event_set(&vionet[i].event, vionet[i].fd,
			    EV_READ | EV_PERSIST, vionet_rx_event, &vionet[i]);
		}
	}
	return (0);
}

int
vioblk_restore(int fd, struct vmop_create_params *vmc,
    int child_disks[][VM_MAX_BASE_PER_DISK])
{
	struct vm_create_params *vcp = &vmc->vmc_params;
	uint8_t i;

	nr_vioblk = vcp->vcp_ndisks;
	vioblk = calloc(vcp->vcp_ndisks, sizeof(struct vioblk_dev));
	if (vioblk == NULL) {
		log_warn("%s: calloc failure allocating vioblks", __progname);
		return (-1);
	}
	log_debug("%s: receiving vioblk", __func__);
	if (atomicio(read, fd, vioblk,
	    nr_vioblk * sizeof(struct vioblk_dev)) !=
	    nr_vioblk * sizeof(struct vioblk_dev)) {
		log_warnx("%s: error reading vioblk from fd", __func__);
		return (-1);
	}
	for (i = 0; i < vcp->vcp_ndisks; i++) {
		if (pci_set_bar_fn(vioblk[i].pci_id, 0, virtio_blk_io,
		    &vioblk[i])) {
			log_warnx("%s: can't set bar fn for virtio block "
			    "device", __progname);
			return (-1);
		}
		if (virtio_init_disk(&vioblk[i].file, &vioblk[i].sz,
		    child_disks[i], vmc->vmc_diskbases[i],
		    vmc->vmc_disktypes[i]) == -1)  {
			log_warnx("%s: unable to determine disk format",
			    __func__);
			return (-1);
		}
		vioblk[i].vm_id = vcp->vcp_id;
		vioblk[i].irq = pci_get_dev_irq(vioblk[i].pci_id);
	}
	return (0);
}

int
vioscsi_restore(int fd, struct vm_create_params *vcp, int child_cdrom)
{
	if (!strlen(vcp->vcp_cdrom))
		return (0);

	vioscsi = calloc(1, sizeof(struct vioscsi_dev));
	if (vioscsi == NULL) {
		log_warn("%s: calloc failure allocating vioscsi", __progname);
		return (-1);
	}

	log_debug("%s: receiving vioscsi", __func__);

	if (atomicio(read, fd, vioscsi, sizeof(struct vioscsi_dev)) !=
	    sizeof(struct vioscsi_dev)) {
		log_warnx("%s: error reading vioscsi from fd", __func__);
		return (-1);
	}

	if (pci_set_bar_fn(vioscsi->pci_id, 0, vioscsi_io, vioscsi)) {
		log_warnx("%s: can't set bar fn for vmm control device",
		    __progname);
		return (-1);
	}

	if (virtio_init_disk(&vioscsi->file, &vioscsi->sz, &child_cdrom, 1,
	    VMDF_RAW) == -1) {
		log_warnx("%s: unable to determine iso format", __func__);
		return (-1);
	}
	vioscsi->vm_id = vcp->vcp_id;
	vioscsi->irq = pci_get_dev_irq(vioscsi->pci_id);

	return (0);
}

int
virtio_restore(int fd, struct vmd_vm *vm, int child_cdrom,
    int child_disks[][VM_MAX_BASE_PER_DISK], int *child_taps)
{
	struct vmop_create_params *vmc = &vm->vm_params;
	struct vm_create_params *vcp = &vmc->vmc_params;
	int ret;

	if ((ret = viornd_restore(fd, vcp)) == -1)
		return ret;

	if ((ret = vioblk_restore(fd, vmc, child_disks)) == -1)
		return ret;

	if ((ret = vioscsi_restore(fd, vcp, child_cdrom)) == -1)
		return ret;

	if ((ret = vionet_restore(fd, vm, child_taps)) == -1)
		return ret;

	if ((ret = vmmci_restore(fd, vcp->vcp_id)) == -1)
		return ret;

	return (0);
}

int
viornd_dump(int fd)
{
	log_debug("%s: sending viornd", __func__);
	if (atomicio(vwrite, fd, &viornd, sizeof(viornd)) != sizeof(viornd)) {
		log_warnx("%s: error writing viornd to fd", __func__);
		return (-1);
	}
	return (0);
}

int
vmmci_dump(int fd)
{
	log_debug("%s: sending vmmci", __func__);
	if (atomicio(vwrite, fd, &vmmci, sizeof(vmmci)) != sizeof(vmmci)) {
		log_warnx("%s: error writing vmmci to fd", __func__);
		return (-1);
	}
	return (0);
}

int
vionet_dump(int fd)
{
	log_debug("%s: sending vionet", __func__);
	if (atomicio(vwrite, fd, vionet,
	    nr_vionet * sizeof(struct vionet_dev)) !=
	    nr_vionet * sizeof(struct vionet_dev)) {
		log_warnx("%s: error writing vionet to fd", __func__);
		return (-1);
	}
	return (0);
}

int
vioblk_dump(int fd)
{
	log_debug("%s: sending vioblk", __func__);
	if (atomicio(vwrite, fd, vioblk,
	    nr_vioblk * sizeof(struct vioblk_dev)) !=
	    nr_vioblk * sizeof(struct vioblk_dev)) {
		log_warnx("%s: error writing vioblk to fd", __func__);
		return (-1);
	}
	return (0);
}

int
vioscsi_dump(int fd)
{
	if (vioscsi == NULL)
		return (0);

	log_debug("%s: sending vioscsi", __func__);
	if (atomicio(vwrite, fd, vioscsi, sizeof(struct vioscsi_dev)) !=
	    sizeof(struct vioscsi_dev)) {
		log_warnx("%s: error writing vioscsi to fd", __func__);
		return (-1);
	}
	return (0);
}

int
virtio_dump(int fd)
{
	int ret;

	if ((ret = viornd_dump(fd)) == -1)
		return ret;

	if ((ret = vioblk_dump(fd)) == -1)
		return ret;

	if ((ret = vioscsi_dump(fd)) == -1)
		return ret;

	if ((ret = vionet_dump(fd)) == -1)
		return ret;

	if ((ret = vmmci_dump(fd)) == -1)
		return ret;

	return (0);
}

void
virtio_stop(struct vm_create_params *vcp)
{
	uint8_t i;
	for (i = 0; i < vcp->vcp_nnics; i++) {
		if (event_del(&vionet[i].event)) {
			log_warn("could not initialize vionet event "
			    "handler");
			return;
		}
	}
}

void
virtio_start(struct vm_create_params *vcp)
{
	uint8_t i;
	for (i = 0; i < vcp->vcp_nnics; i++) {
		if (event_add(&vionet[i].event, NULL)) {
			log_warn("could not initialize vionet event "
			    "handler");
			return;
		}
	}
}
