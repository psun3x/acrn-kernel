// SPDX-License-Identifier: GPL-2.0+
//
// skl-virtio-be.c  --  Virtio BE service for SKL architecture
//
// Copyright (C) 2018 Intel Corporation.
//
// Authors: Furtak, Pawel <pawel.furtak@intel.com>
//          Janca, Grzegorz <grzegorz.janca@intel.com>
//
//  BE receives commands from FE drivers and forward them to appropriate
//  entity, such as DSP, PCM or sound card control. BE sends buffer position
//  updates to FE driver.

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/hw_random.h>
#include <linux/uio.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/device.h>
#include <sound/pcm_params.h>
#include <linux/vbs/vq.h>
#include <linux/vbs/vbs.h>
#include <linux/vhm/acrn_common.h>
#include <linux/vhm/acrn_vhm_ioreq.h>
#include <linux/vhm/acrn_vhm_mm.h>
#include <linux/vhm/vhm_vm_mngt.h>
#include <linux/spinlock.h>
#include "skl-virtio-be.h"
#include "../skl.h"
#include "../skl-sst-ipc.h"
#include "../skl-topology.h"
#include "../../common/sst-dsp-priv.h"
#include "skl-virtio.h"

static struct vbe_static_kctl_domain kctl_domain_map[] = {
		KCTL_DOMAIN_ITEM("BtHfp_ssp0_in pcm cfg", 0x1),
		KCTL_DOMAIN_ITEM("BtHfp_ssp0_out pcm cfg", 0x1),
		KCTL_DOMAIN_ITEM("Speaker Switch", 0x1),
};

struct vbe_substream_info *vbe_find_substream_info_by_pcm(
	const struct snd_skl_vbe_client *client, char *pcm_id, int direction)
{
	struct vbe_substream_info *info;

	list_for_each_entry(info, &client->substr_info_list, list) {
		if (info->direction == direction &&
			strncmp(info->pcm->id, pcm_id,
					ARRAY_SIZE(info->pcm->id)) == 0)
			return info;
	}
	return NULL;
}

struct vbe_substream_info *vbe_find_substream_info(
	struct snd_skl_vbe *vbe, struct snd_pcm_substream *substr)
{
	struct snd_skl_vbe_client *client;
	struct vbe_substream_info *info;

	list_for_each_entry(client, &vbe->client_list, list) {
		info = vbe_find_substream_info_by_pcm(client,
				substr->pcm->id, substr->stream);
		if (info)
			return info;
	}
	return NULL;
}

static struct vbe_substream_info *vbe_skl_find_substream_info(
	const struct skl *sdev, struct snd_pcm_substream *substr)
{
	struct snd_skl_vbe *vbe = skl_get_vbe(sdev);

	return vbe_find_substream_info(vbe, substr);
}

struct snd_soc_dapm_widget *vbe_skl_find_kcontrol_widget(
	const struct skl *sdev,	const struct snd_kcontrol *kcontrol)
{
	struct snd_soc_dapm_widget *w;
	int i;

	list_for_each_entry(w, &sdev->component->card->widgets, list) {
		for (i = 0; i < w->num_kcontrols; i++) {
			if (kcontrol == w->kcontrols[i])
				return w;
		}
	}

	return NULL;
}

struct skl_tplg_domain *vbe_skl_find_tplg_domain_by_name(
	struct skl *skl, char *domain_name)
{
	struct skl_tplg_domain *tplg_domain;

	list_for_each_entry(tplg_domain, &skl->skl_sst->tplg_domains, list) {
		if (strncmp(tplg_domain->domain_name, domain_name,
				ARRAY_SIZE(tplg_domain->domain_name)) == 0)
			return tplg_domain;
	}

	return NULL;
}

struct skl_tplg_domain *vbe_skl_find_tplg_domain_by_id(
	const struct skl *skl, u32 domain_id)
{
	struct skl_tplg_domain *tplg_domain;

	list_for_each_entry(tplg_domain, &skl->skl_sst->tplg_domains, list) {
		if (tplg_domain->domain_id == domain_id)
			return tplg_domain;
	}

	return NULL;
}

inline int vbe_skl_is_valid_pcm_id(const char *pcm_id)
{
	if (pcm_id == NULL || strlen(pcm_id) == 0 ||
		strcmp(pcm_id, "((null))") == 0)
		return -EINVAL;

	return 0;
}

static struct snd_soc_pcm_runtime *vbe_skl_find_rtd_by_pcm_id(
	struct skl *skl, const char *pcm_name)
{
	struct snd_soc_pcm_runtime *rtd;
	int ret = vbe_skl_is_valid_pcm_id(pcm_name);

	if (ret < 0)
		return NULL;

	if (unlikely(!skl || !skl->component || !skl->component->card))
		return NULL;

	list_for_each_entry(rtd, &skl->component->card->rtd_list, list) {
		if (strncmp(rtd->pcm->id, pcm_name,
				ARRAY_SIZE(rtd->pcm->id)) == 0)
			return rtd;
	}
	return NULL;
}

struct snd_pcm *vbe_skl_find_pcm_by_name(struct skl *skl, char *pcm_name)
{
	const struct snd_soc_pcm_runtime *rtd;

	if (!strlen(pcm_name))
		return NULL;

	rtd = vbe_skl_find_rtd_by_pcm_id(skl, pcm_name);

	return rtd ? rtd->pcm : NULL;
}

static bool vbe_skl_try_send(struct snd_skl_vbe *vbe,
		struct virtio_vq_info *vq, void *buff,
		unsigned int size)
{
	struct iovec iov;
	u16 idx;

	if (virtio_vq_has_descs(vq) &&
		(virtio_vq_getchain(vq, &idx, &iov, 1, NULL) > 0)) {
		if (iov.iov_len < size) {
			dev_err(vbe->dev, "iov len %lu, expecting len %u\n",
				iov.iov_len, size);
			virtio_vq_relchain(vq, idx, iov.iov_len);
		}
		memcpy(iov.iov_base, buff, size);
		virtio_vq_relchain(vq, idx, iov.iov_len);
		virtio_vq_endchains(vq, true);
		return true;
	}
	return false;
}


static void vbe_skl_send_or_enqueue(struct snd_skl_vbe *vbe,
		struct virtio_vq_info *vq,
		struct vfe_pending_msg *pen_msg)
{
	struct vfe_pending_msg *save_msg;

	if (vbe_skl_try_send(vbe, vq,
		(void *)&pen_msg->msg, pen_msg->sizeof_msg) == false) {
		save_msg = kzalloc(sizeof(*save_msg), GFP_KERNEL);
		*save_msg = *pen_msg;
		list_add_tail(&save_msg->list, &vbe->pending_msg_list);
	}
}

void vbe_stream_update(struct hdac_bus *bus, struct hdac_stream *hstr)
{
	struct skl *skl = bus_to_skl(bus);
	struct snd_skl_vbe *vbe = skl_get_vbe(skl);

	if (hstr->substream)
		skl_notify_stream_update(bus, hstr->substream);

	vbe->nops.hda_irq_ack(bus, hstr);
}

int vbe_send_kctl_msg(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol,
		struct vfe_kctl_result *result)
{
	struct vfe_pending_msg kctl_msg;
	struct snd_skl_vbe *vbe = &get_virtio_audio()->vbe;
	struct virtio_vq_info *vq = &vbe->vqs[SKL_VIRTIO_IPC_NOT_RX_VQ];

	kctl_msg.msg.posn.msg_type = VFE_MSG_KCTL_SET;
	strncpy(kctl_msg.msg.kctln.kcontrol.kcontrol_id, kcontrol->id.name,
			ARRAY_SIZE(kcontrol->id.name));

	kctl_msg.msg.kctln.kcontrol_value.value = *ucontrol;

	kctl_msg.sizeof_msg = sizeof(struct vfe_kctl_noti);

	vbe_skl_send_or_enqueue(vbe, vq, &kctl_msg);

	result->ret = 0;
	return 0;
}

void skl_notify_stream_update(struct hdac_bus *bus,
		struct snd_pcm_substream *substream)
{
	struct skl *skl = bus_to_skl(bus);
	struct vbe_substream_info *substr_info;
	struct snd_soc_pcm_runtime *rtd;
	struct snd_skl_vbe *vbe;


	substr_info = vbe_skl_find_substream_info(skl, substream);
	if (!substr_info || !substr_info->pos_desc)
		return;

	vbe = substr_info->vbe;

	rtd = substream->private_data;
	substr_info->pos_desc->hw_ptr = rtd->ops.pointer(substream);
	substr_info->pos_desc->be_irq_cnt++;

	/*sync pos_desc*/
	wmb();

	virtio_vq_interrupt(&vbe->dev_info,
		&vbe->vqs[SKL_VIRTIO_IPC_CMD_RX_VQ]);
}

int vbe_skl_allocate_runtime(struct snd_soc_card *card,
		struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd;
	struct snd_pcm_runtime *runtime;
	int size;

	runtime = kzalloc(sizeof(*runtime), GFP_KERNEL);
	if (!runtime)
		return -ENOMEM;

	size = PAGE_ALIGN(sizeof(struct snd_pcm_mmap_status));
	runtime->status = snd_malloc_pages(size, GFP_KERNEL);
	if (!runtime->status)
		goto alloc_free;

	memset((void *)runtime->status, 0, size);

	size = PAGE_ALIGN(sizeof(struct snd_pcm_mmap_control));
	runtime->control = snd_malloc_pages(size, GFP_KERNEL);
	if (!runtime->control) {
		snd_free_pages((void *)runtime->status,
			PAGE_ALIGN(sizeof(struct snd_pcm_mmap_status)));
		goto alloc_free;
	}
	memset((void *)runtime->control, 0, size);

	init_waitqueue_head(&runtime->sleep);
	init_waitqueue_head(&runtime->tsleep);
	runtime->status->state = SNDRV_PCM_STATE_OPEN;

	substream->runtime = runtime;

	list_for_each_entry(rtd, &card->rtd_list, list) {
		if (strcmp(rtd->pcm->id, substream->pcm->id) == 0) {
			substream->private_data = rtd;
			break;
		}
	}
	return 0;
alloc_free:
	kfree(runtime);
	return -ENOMEM;
}

void vbe_skl_initialize_substream_runtime(struct snd_pcm_runtime *runtime,
		struct snd_pcm_hw_params *params)
{
	int bits;
	int frames;

	runtime->access = params_access(params);
	runtime->format = params_format(params);
	runtime->subformat = params_subformat(params);
	runtime->channels = params_channels(params);
	runtime->rate = params_rate(params);
	runtime->period_size = params_period_size(params);
	runtime->periods = params_periods(params);
	runtime->buffer_size = params_buffer_size(params);
	runtime->info = params->info;
	runtime->rate_num = params->rate_num;
	runtime->rate_den = params->rate_den;
	runtime->no_period_wakeup =
		(params->info & SNDRV_PCM_INFO_NO_PERIOD_WAKEUP) &&
		(params->flags & SNDRV_PCM_HW_PARAMS_NO_PERIOD_WAKEUP);
	runtime->no_rewinds =
		(params->flags & SNDRV_PCM_HW_PARAMS_NO_REWINDS) ? 1 : 0;
	bits = snd_pcm_format_physical_width(runtime->format);
	runtime->sample_bits = bits;
	bits *= runtime->channels;
	runtime->frame_bits = bits;
	frames = 1;
	while (bits % 8 != 0) {
		bits *= 2;
		frames *= 2;
	}
	runtime->byte_align = bits / 8;
	runtime->min_align = frames;
	/* Default sw params */
	runtime->tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
	runtime->period_step = 1;
	runtime->control->avail_min = runtime->period_size;
	runtime->start_threshold = 1;
	runtime->stop_threshold = runtime->buffer_size;
	runtime->silence_threshold = 0;
	runtime->silence_size = 0;
	runtime->boundary = runtime->buffer_size << 4;
}

static int vbe_skl_prepare_dma(struct vbe_substream_info *substr_info,
	int vm_id, struct vfe_pcm_dma_conf *dma_conf)
{
	const struct snd_sg_buf *sg_buf;
	int cnt;
	u64 pcm_buffer_gpa = dma_conf->addr;
	u64 pcm_buffer_hpa = vhm_vm_gpa2hpa(vm_id, pcm_buffer_gpa);

	if (!pcm_buffer_hpa)
		return -EINVAL;

	sg_buf = snd_pcm_substream_sgbuf(substr_info->substream);
	if (!sg_buf)
		return -EINVAL;

	substr_info->native_dma_addr = sg_buf->table[0].addr;
	sg_buf->table[0].addr = pcm_buffer_hpa;
	pcm_buffer_hpa &= ~(u64)0xfff;
	for (cnt = 1; cnt < sg_buf->pages; cnt++) {
		pcm_buffer_hpa += PAGE_SIZE;
		sg_buf->table[cnt].addr = pcm_buffer_hpa;
	}

	substr_info->pos_desc = map_guest_phys(vm_id,
		dma_conf->stream_pos_addr,
		dma_conf->stream_pos_size);

	if (!substr_info->pos_desc) {
		pr_err("Failed to map guest stream description %p",
			(void *)dma_conf->stream_pos_addr);

		return -EINVAL;
	}

	return 0;
}

static int vbe_skl_assemble_params(struct vfe_pcm_hw_params *vfe_params,
		struct snd_pcm_hw_params *params)
{
	hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS)->min =
		vfe_params->channels;

	hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE)->min =
		vfe_params->rate;

	hw_param_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_BYTES)->min =
		vfe_params->host_period_bytes;

	hw_param_interval(params, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->min =
		vfe_params->buffer_size;

	hw_param_interval(params, SNDRV_PCM_HW_PARAM_BUFFER_BYTES)->min =
		vfe_params->buffer_bytes;

	hw_param_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min =
		vfe_params->period_size;

	hw_param_interval(params, SNDRV_PCM_HW_PARAM_PERIODS)->min =
		vfe_params->periods;

	params_set_format(params, vfe_params->frame_fmt);

	return 0;
}

static int vbe_skl_add_substream_info(struct snd_skl_vbe *vbe, int vm_id,
		struct snd_pcm_substream *substream)
{
	struct vbe_substream_info *substr_info;
	/*TODO: call vbe_client_find with proper client_id*/
	struct snd_skl_vbe_client *client = list_first_entry_or_null(
			&vbe->client_list, struct snd_skl_vbe_client, list);

	if (!client) {
		dev_err(vbe->dev,
			"Can not find active client [%d].\n", vm_id);
		return -EINVAL;
	}

	substr_info = kzalloc(sizeof(*substr_info), GFP_KERNEL);

	if (!substr_info)
		return -ENOMEM;

	substr_info->pcm = substream->pcm;
	substr_info->substream = substream;
	substr_info->direction = substream->stream;
	substr_info->vbe = vbe;

	list_add(&substr_info->list, &client->substr_info_list);
	return 0;
}

static int vbe_skl_pcm_get_domain_id(struct skl *sdev,
	const char *pcm_id, int direction, int *domain_id)
{
	struct snd_soc_pcm_runtime *rtd;
	struct skl_module_cfg *mconfig = NULL;

	if (unlikely(!domain_id))
		return -EINVAL;

	rtd = vbe_skl_find_rtd_by_pcm_id(sdev, pcm_id);
	if (!rtd)
		return -ENODEV;

	if (rtd->cpu_dai)
		mconfig = skl_tplg_fe_get_cpr_module(rtd->cpu_dai, direction);

	if (mconfig) {
		*domain_id = mconfig->domain_id;
		return 0;
	}

	return -EINVAL;
}

static int vbe_skl_pcm_check_permission(struct skl *sdev,
	int domain_id, const char *pcm_id, int direction)
{
	int pcm_domain_id;
	int ret = 0;

	ret = vbe_skl_pcm_get_domain_id(sdev, pcm_id,
			direction, &pcm_domain_id);
	if (ret < 0)
		return ret;

	if (domain_id != pcm_domain_id)
		return -EACCES;

	return ret;
}

static int vbe_skl_pcm_open(struct snd_skl_vbe *vbe, struct skl *sdev,
		int vm_id, struct vbe_ipc_msg *msg)
{
	struct snd_soc_pcm_runtime *rtd;
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	int ret;
	struct vfe_pcm_result *vbe_result = msg->rx_data;
	struct vfe_pcm_info *pcm_desc = &msg->header->desc.pcm;
	struct snd_pcm *pcm =
		vbe_skl_find_pcm_by_name(vbe->sdev, pcm_desc->pcm_id);
	int direction = pcm_desc->direction;

	if (!pcm) {
		dev_err(&sdev->pci->dev, "Can not find PCM [%s].\n",
			pcm_desc->pcm_id);
		ret = -ENODEV;
		goto ret_err;
	}

	ret = vbe_skl_pcm_check_permission(sdev,
		msg->header->domain_id, pcm_desc->pcm_id, direction);
	if (ret < 0)
		goto ret_err;

	substream = pcm->streams[direction].substream;
	runtime = substream->runtime;

	if (substream->ref_count > 0) {
		ret = -EBUSY;
		goto ret_err;
	}

	ret = vbe_skl_allocate_runtime(sdev->component->card, substream);
	if (ret < 0)
		goto ret_err;
	ret = vbe_skl_add_substream_info(vbe, vm_id, substream);
	if (ret < 0)
		goto ret_err;
	substream->ref_count++;  /* set it used */
	rtd = substream->private_data;
	ret = rtd->ops.open(substream);

ret_err:
	if (vbe_result)
		vbe_result->ret = ret;

	return ret;
}

static int vbe_skl_pcm_close(const struct skl *sdev, int vm_id,
		struct vbe_substream_info *substr_info,
		struct vbe_ipc_msg *msg)
{
	struct snd_soc_pcm_runtime *rtd;
	int ret, cnt;
	struct snd_pcm_substream *substream = substr_info->substream;
	struct vfe_pcm_result *vbe_result = msg->rx_data;
	struct snd_sg_buf *sg_buf;
	u64 native_addr = substr_info->native_dma_addr;

	if (snd_pcm_get_dma_buf(substream)) {
		sg_buf = snd_pcm_substream_sgbuf(substream);

		/* restore original dma pages */
		sg_buf->table[0].addr = native_addr;
		native_addr &= ~(u64)0xfff;
		for (cnt = 1; cnt < sg_buf->pages; cnt++) {
			native_addr += PAGE_SIZE;
			sg_buf->table[cnt].addr = native_addr;
		}
	}

	if (substr_info->pos_desc) {
		unmap_guest_phys(vm_id, (u64)substr_info->pos_desc);
		substr_info->pos_desc = NULL;
	}

	list_del(&substr_info->list);
	kfree(substr_info);

	substream->ref_count = 0;
	rtd = substream->private_data;
	ret = rtd->ops.close(substream);

	if (vbe_result)
		vbe_result->ret = ret;

	return ret;
}

static int vbe_skl_pcm_prepare(struct skl *sdev, int vm_id,
		struct vbe_substream_info *substr_info,
		struct vbe_ipc_msg *msg)
{
	const struct snd_soc_pcm_runtime *rtd;
	int ret;
	struct vfe_pcm_dma_conf *dma_params = msg->tx_data;
	struct vfe_pcm_result *vbe_result = msg->rx_data;
	struct snd_pcm_substream *substream = substr_info->substream;

	ret = vbe_skl_prepare_dma(substr_info, vm_id, dma_params);
	if (ret < 0)
		return ret;

	rtd = substream->private_data;
	ret = rtd->ops.prepare(substream);

	if (vbe_result)
		vbe_result->ret = ret;

	return ret;
}

void vbe_skl_pcm_close_all(struct snd_skl_vbe *vbe,
		struct snd_skl_vbe_client *client)
{
	struct vbe_substream_info *info, *tmp;
	struct vbe_ipc_msg msg;
	int ret;

	msg.rx_data = NULL;
	list_for_each_entry_safe(info, tmp, &client->substr_info_list, list) {
		ret = vbe_skl_pcm_close(vbe->sdev, 0, info, &msg);
		if (ret < 0)
			dev_err(vbe->dev,
				"Could not close PCM\n");
	}
}

struct snd_pcm_hw_params hw_params;

static int vbe_skl_pcm_hw_params(const struct skl *sdev, int vm_id,
		struct vbe_substream_info *substr_info,
		struct vbe_ipc_msg *msg)
{
	struct snd_soc_pcm_runtime *rtd;
	int ret;
	struct snd_pcm_substream *substream = substr_info->substream;
	//TODO: check if tx and rx data have expected size
	struct vfe_pcm_hw_params *hw_params_ipc = msg->tx_data;
	struct vfe_pcm_result *vbe_result = msg->rx_data;

	vbe_skl_assemble_params(hw_params_ipc, &hw_params);
	vbe_skl_initialize_substream_runtime(substream->runtime, &hw_params);

	rtd = substream->private_data;
	ret = rtd->ops.hw_params(substream, &hw_params);

	if (vbe_result)
		vbe_result->ret = ret;

	return ret;
}

static int vbe_skl_pcm_trigger(struct skl *sdev, int vm_id,
		struct vbe_substream_info *substr_info,
		struct vbe_ipc_msg *msg)
{
	struct snd_soc_pcm_runtime *rtd;
	struct snd_pcm_substream *substream = substr_info->substream;
	int cmd = *(int *)msg->tx_data;

	rtd = substream->private_data;
	return rtd->ops.trigger(substream, cmd);
}

static u32 vbe_skl_kcontrol_find_domain_id(const struct snd_kcontrol *kcontrol,
	struct skl_module_cfg *mconfig)
{
	struct skl_kctl_domain *domain;
	bool name_match = false;

	list_for_each_entry(domain, &mconfig->kctl_domains, list) {
		name_match = strncmp(domain->name, kcontrol->id.name,
			ARRAY_SIZE(domain->name)) == 0;
		if (name_match)
			return domain->domain_id;
	}

	return 0;
}

static u32 vbe_skl_get_static_domain_id(const struct snd_ctl_elem_id *ctl_id)
{
	u32 idx, num = ARRAY_SIZE(kctl_domain_map);
	u32 size = strnlen(ctl_id->name, sizeof(ctl_id->name));

	for (idx = 0; idx < num; ++idx) {
		if ((kctl_domain_map[idx].str_size == size) &&
			(strncmp(ctl_id->name,
				kctl_domain_map[idx].name, size) == 0))
			return kctl_domain_map[idx].domain_flag;
	}
	return 0;
}

static int vbe_skl_kcontrol_get_domain_id(const struct snd_kcontrol *kcontrol,
		u32 *domain_id)
{
	struct skl_module_cfg *mconfig;
	struct snd_soc_dapm_widget *w;
	void *priv = kcontrol->private_data;
	struct skl *sdev = get_virtio_audio()->skl;

	if (sdev == NULL)
		return -EINVAL;

	if (unlikely(!domain_id))
		return -EINVAL;

	*domain_id = 0;

	if (priv == sdev->component ||
		priv == sdev->component->card) {
		/* temporary solution for controls without widget */
		*domain_id = vbe_skl_get_static_domain_id(&kcontrol->id);
		return 0;
	}

	w = vbe_skl_find_kcontrol_widget(sdev, kcontrol);
	if (w) {
		mconfig = w->priv;
		*domain_id = vbe_skl_kcontrol_find_domain_id(kcontrol, mconfig);
	}

	return 0;
}

static struct kctl_ops vbe_kctl_ops = {
		.get_domain_id = vbe_skl_kcontrol_get_domain_id,
		.send_noti = vbe_send_kctl_msg,
};

static int vbe_skl_cfg_hda(struct skl *sdev, int vm_id,
		const struct vbe_ipc_msg *msg)
{
	struct hdac_bus *bus = &sdev->hbus;
	struct vfe_hda_cfg *hda_cfg = msg->rx_data;
	unsigned short gcap;

	if (!hda_cfg || msg->rx_size != sizeof(*hda_cfg))
		return -EINVAL;

	hda_cfg->resource_length = pci_resource_len(sdev->pci, 0);
	gcap = snd_hdac_chip_readw(bus, GCAP);

	hda_cfg->cp_streams = (gcap >> 8) & 0x0f;
	hda_cfg->pb_streams = (gcap >> 12) & 0x0f;

	hda_cfg->ppcap = bus->ppcap ? bus->ppcap - bus->remap_addr : 0;
	hda_cfg->spbcap = bus->spbcap ? bus->spbcap - bus->remap_addr : 0;
	hda_cfg->mlcap = bus->mlcap ? bus->mlcap - bus->remap_addr : 0;
	hda_cfg->gtscap = bus->gtscap ? bus->gtscap - bus->remap_addr : 0;
	hda_cfg->drsmcap = bus->drsmcap ? bus->drsmcap - bus->remap_addr : 0;

	return 0;
}

static const struct firmware *vbe_find_lib_fw(struct skl_sst *skl_sst,
		const char *name)
{
	int idx, ret;
	struct skl_lib_info *lib_info = skl_sst->lib_info;

	/* library indices start from 1 to N. 0 represents base FW */
	for (idx = 1; idx < skl_sst->lib_count; ++idx) {
		ret = strncmp(lib_info[idx].name, name,
				ARRAY_SIZE(lib_info[idx].name));
		if (ret == 0)
			return lib_info[idx].fw;
	}

	return NULL;
}

static const struct firmware *vbe_find_res_hndl(struct snd_skl_vbe *vbe,
		int type, const char *name)
{
	struct snd_skl_vbe_client *client;
	const struct firmware *fw;
	struct skl_sst *skl_sst = vbe->sdev->skl_sst;

	switch (type) {
	case VFE_TOPOLOGY_RES:
		client = list_first_entry_or_null(&vbe->client_list,
				struct snd_skl_vbe_client, list);
		fw = client->tplg;
		break;
	case VFE_FIRMWARE_RES:
		fw = skl_sst->dsp->fw;
		break;
	case VFE_LIBRARY_RES:
		fw = vbe_find_lib_fw(skl_sst, name);
		break;
	default:
		fw = NULL;
	}

	if (fw)
		return fw;

	dev_err(vbe->dev, "Unable to find resource [%d](%.*s)\n",
			type, SKL_LIB_NAME_LENGTH, name);
	return NULL;
}

static int vbe_skl_cfg_resource_info(struct snd_skl_vbe *vbe, int vm_id,
		const struct vbe_ipc_msg *msg)
{
	struct vfe_resource_info *res_info = msg->rx_data;
	const struct firmware *fw;

	if (!res_info || msg->rx_size != sizeof(*res_info))
		return -EINVAL;

	res_info->size = 0;

	fw = vbe_find_res_hndl(vbe, res_info->type, res_info->name);

	if (!fw)
		return -EBADF;

	res_info->size = fw->size;

	return 0;
}

static int vbe_skl_cfg_resource_desc(struct snd_skl_vbe *vbe, int vm_id,
		const struct vbe_ipc_msg *msg)
{
	u8 *fw_data;
	int ret = 0;
	const struct firmware *fw;
	struct vfe_resource_desc *res_desc = msg->rx_data;

	if (!res_desc || msg->rx_size != sizeof(*res_desc))
		return -EINVAL;

	fw = vbe_find_res_hndl(vbe, res_desc->type, res_desc->name);

	if (!fw) {
		ret = -EBADF;
		goto ret_val;
	}

	if (fw->size != res_desc->size) {
		ret = -EINVAL;
		goto ret_val;
	}

	fw_data = map_guest_phys(vm_id, res_desc->phys_addr,
			res_desc->size);
	memcpy(fw_data, fw->data, res_desc->size);
	unmap_guest_phys(vm_id, res_desc->phys_addr);

ret_val:
	res_desc->ret = ret;
	return ret;
}

static int vbe_skl_cfg_domain(struct snd_skl_vbe *vbe, int vm_id,
		const struct vbe_ipc_msg *msg)
{
	struct skl_tplg_domain *tplg_domain;
	struct vfe_domain_info *domain_info = msg->rx_data;
	int ret;
	struct snd_skl_vbe_client *client = list_first_entry_or_null(
			&vbe->client_list, struct snd_skl_vbe_client, list);

	if (!domain_info || msg->rx_size != sizeof(*domain_info))
		return -EINVAL;

	if (!client) {
		ret = -EINVAL;
		goto ret_val;
	}

	tplg_domain = vbe_skl_find_tplg_domain_by_name(vbe->sdev,
		msg->header->domain_name);
	if (!tplg_domain) {
		ret = -EACCES;
		goto ret_val;
	}

	domain_info->domain_id = tplg_domain->domain_id;
	ret = request_firmware(&client->tplg,
		tplg_domain->tplg_name, vbe->dev);

ret_val:
	domain_info->ret = ret;
	return domain_info->ret;
}

static int vbe_skl_msg_cfg_handle(struct snd_skl_vbe *vbe,
		struct skl *sdev, int vm_id, struct vbe_ipc_msg *msg)
{
	switch (msg->header->cmd) {
	case VFE_MSG_CFG_HDA:
		return vbe_skl_cfg_hda(sdev, vm_id, msg);
	case VFE_MSG_CFG_RES_INFO:
		return vbe_skl_cfg_resource_info(vbe, vm_id, msg);
	case VFE_MSG_CFG_RES_DESC:
		return vbe_skl_cfg_resource_desc(vbe, vm_id, msg);
	case VFE_MSG_CFG_DOMAIN:
		return vbe_skl_cfg_domain(vbe, vm_id, msg);
	default:
		dev_err(vbe->dev, "Unknown command %d for config get message.\n",
				msg->header->cmd);
		break;
	}

	return 0;
}

static int vbe_skl_msg_pcm_handle(struct snd_skl_vbe *vbe,
		struct skl *sdev, int vm_id, struct vbe_ipc_msg *msg)
{
	struct vbe_substream_info *substream_info;
	char *pcm_id;
	int direction;
	/* TODO: call vbe_client_find with proper client_id */
	struct snd_skl_vbe_client *client = list_first_entry_or_null(
			&vbe->client_list, struct snd_skl_vbe_client, list);


	if (!client) {
		dev_err(vbe->dev,
			"Can not find active client [%d].\n", vm_id);
		return -EINVAL;
	}

	if (msg->header->cmd == VFE_MSG_PCM_OPEN)
		return vbe_skl_pcm_open(vbe, sdev, vm_id, msg);

	pcm_id = msg->header->desc.pcm.pcm_id;
	direction = msg->header->desc.pcm.direction;
	substream_info = vbe_find_substream_info_by_pcm(client,
			pcm_id, direction);

	if (!substream_info) {
		dev_err(vbe->dev,
			"Can not find active substream [%s].\n", pcm_id);
		return -ENODEV;
	}

	switch (msg->header->cmd) {
	case VFE_MSG_PCM_CLOSE:
		return vbe_skl_pcm_close(sdev, vm_id, substream_info, msg);
	case VFE_MSG_PCM_PREPARE:
		return vbe_skl_pcm_prepare(sdev, vm_id, substream_info, msg);
	case VFE_MSG_PCM_HW_PARAMS:
		return vbe_skl_pcm_hw_params(sdev, vm_id, substream_info, msg);
	case VFE_MSG_PCM_TRIGGER:
		return vbe_skl_pcm_trigger(sdev, vm_id, substream_info, msg);
	default:
		dev_err(vbe->dev, "PCM stream notification %d not supported\n",
			msg->header->cmd);
	}

	return 0;
}

int vbe_skl_msg_kcontrol_handle(struct snd_skl_vbe *vbe,
		int vm_id, const struct vbe_ipc_msg *msg)
{
	const struct vfe_kctl_info *kctl_desc = &msg->header->desc.kcontrol;
	u32 domain_id = msg->header->domain_id;

	switch (msg->header->cmd) {
	case VFE_MSG_KCTL_SET:
		return kctl_ipc_handle(domain_id, kctl_desc,
				msg->tx_data, msg->rx_data);
	default:
		dev_err(vbe->dev, "Unknown command %d for kcontrol [%s].\n",
			msg->header->cmd, kctl_desc->kcontrol_id);
	break;
	}

	return 0;
}

static int vbe_skl_not_fwd(struct snd_skl_vbe *vbe,
	struct skl *sdev, int vm_id, void *ipc_bufs[SKL_VIRTIO_NOT_VQ_SZ],
	size_t ipc_lens[SKL_VIRTIO_NOT_VQ_SZ])
{
	struct vbe_ipc_msg msg;

	if (sizeof(struct vfe_msg_header) != ipc_lens[SKL_VIRTIO_MSG_HEADER]) {
		dev_err(vbe->dev, "Mismatch of IPC header size");
		return -EINVAL;
	}

	msg.header = ipc_bufs[SKL_VIRTIO_MSG_HEADER];
	msg.tx_data = ipc_bufs[SKL_VIRTIO_MSG_TX];
	msg.rx_data = ipc_bufs[SKL_VIRTIO_MSG_RX];

	msg.tx_size = ipc_lens[SKL_VIRTIO_MSG_TX];
	msg.rx_size = ipc_lens[SKL_VIRTIO_MSG_RX];

	switch (msg.header->cmd & VFE_MSG_TYPE_MASK) {
	case VFE_MSG_PCM:
		return vbe_skl_msg_pcm_handle(vbe, sdev, vm_id, &msg);
	case VFE_MSG_KCTL:
		return vbe_skl_msg_kcontrol_handle(vbe, vm_id, &msg);
	case VFE_MSG_CFG:
		return vbe_skl_msg_cfg_handle(vbe, sdev, vm_id, &msg);
	}

	return 0;
}

static int vbe_skl_ipc_fwd(const struct snd_skl_vbe *vbe,
		const struct skl *sdev, int vm_id,
		void *ipc_buf, void *reply_buf, size_t count, size_t *reply_sz)
{
	struct vfe_dsp_ipc_msg *ipc_data = ipc_buf;
	struct skl_sst *skl_sst = sdev->skl_sst;
	int ret;

	dev_dbg(vbe->dev, "IPC forward request. Header:0X%016llX tx_data:%p\n",
			ipc_data->header,
			ipc_data->data_size ? &ipc_data->data : NULL);
	dev_dbg(vbe->dev, "tx_size:%zu rx_data:%p rx_size:%zu\n",
			ipc_data->data_size,
			*reply_sz ? reply_buf : NULL,
			*reply_sz);

	/* Tx IPC and wait for response */
	ret = *reply_sz <= 0 ? 0 : sst_ipc_tx_message_wait(&skl_sst->ipc,
			ipc_data->header,
			ipc_data->data_size ? &ipc_data->data : NULL,
			ipc_data->data_size,
			*reply_sz ? reply_buf : NULL,
			reply_sz);

	if (ret < 0) {
		dev_dbg(vbe->dev, "IPC reply error:%d\n", ret);
		return ret;
	}
	if (*reply_sz > 0) {
		print_hex_dump(KERN_DEBUG, "IPC response:", DUMP_PREFIX_OFFSET,
			8, 4, (char *)reply_buf, *reply_sz, false);
	}

	return 0;
}

static int vbe_skl_virtio_vq_handle(struct snd_skl_vbe *vbe,
	struct virtio_vq_info *vq, u16 *idx, struct iovec *iov,
	void *reply_buf[], size_t *reply_len, int vq_id, int vq_size)
{
	int i;
	struct device *dev = vbe->sdev->skl_sst->dev;
	int ret = virtio_vq_getchain(vq, idx, iov, vq_size, NULL);

	if (ret != vq_size) {
		dev_err(dev, "notification buffers not paired, expected:%d, got:%d",
			vq_size, ret);
		if (ret < 0) {
			virtio_vq_endchains(vq, true);
			return ret;
		}
		for (i = 0; i <= ret; i++)
			virtio_vq_relchain(vq, *idx + i, iov[i].iov_len);

		virtio_vq_endchains(vq, true);
		return ret;
	}
	for (i = 0; i < ret; i++) {
		reply_len[i] = iov[vq_id+i].iov_len;
		reply_buf[i] = iov[vq_id+i].iov_base;
	}
	return 0;
}

static void vbe_handle_irq_queue(struct snd_skl_vbe *vbe, int vq_idx)
{
	u16 idx;
	struct iovec iov;
	struct virtio_vq_info *vq = &vbe->vqs[vq_idx];

	if (virtio_vq_has_descs(vq) &&
		(virtio_vq_getchain(vq, &idx, &iov, 1, NULL) > 0)) {

		virtio_vq_relchain(vq, idx, iov.iov_len);
		virtio_vq_endchains(vq, true);
	}
}

static void vbe_skl_ipc_fe_not_get(struct snd_skl_vbe *vbe, int vq_idx)
{
	int ret;
	u16 idx;
	struct iovec iov[SKL_VIRTIO_NOT_VQ_SZ];
	void *reply_buf[SKL_VIRTIO_NOT_VQ_SZ];
	size_t reply_len[SKL_VIRTIO_NOT_VQ_SZ];
	struct virtio_vq_info *vq = &vbe->vqs[vq_idx];
	struct device *dev = vbe->sdev->skl_sst->dev;
	int vm_id = vbe->vmid;

	memset(iov, 0, sizeof(iov));

	/* while there are mesages in virtio queue */
	while (virtio_vq_has_descs(vq)) {
		ret = vbe_skl_virtio_vq_handle(vbe, vq, &idx, iov,
			reply_buf, reply_len,
			SKL_VIRTIO_IPC_MSG, SKL_VIRTIO_NOT_VQ_SZ);
		if (ret) {
			dev_err(dev, "Failed to handle virtio message");
			return;
		}

		ret = vbe_skl_not_fwd(vbe, vbe->sdev, vm_id,
				reply_buf, reply_len);
		if (ret < 0)
			dev_err(dev, "submit guest ipc command fail\n");

		virtio_vq_relchain(vq, idx + SKL_VIRTIO_MSG_HEADER,
			reply_len[SKL_VIRTIO_MSG_HEADER]);
	}
	virtio_vq_endchains(vq, true);
}

static void vbe_skl_ipc_fe_cmd_get(struct snd_skl_vbe *vbe, int vq_idx)
{
	u16 idx;
	int ret;
	struct iovec iov[SKL_VIRTIO_IPC_VQ_SZ];
	void *reply_buf[SKL_VIRTIO_IPC_VQ_SZ];
	size_t reply_len[SKL_VIRTIO_IPC_VQ_SZ];
	struct virtio_vq_info *vq = &vbe->vqs[vq_idx];
	struct device *dev = vbe->sdev->skl_sst->dev;
	int vm_id = vbe->vmid;

	memset(iov, 0, sizeof(iov));

	/* while there are mesages in virtio queue */
	while (virtio_vq_has_descs(vq)) {
		ret = vbe_skl_virtio_vq_handle(vbe, vq, &idx, iov,
				reply_buf, reply_len,
				SKL_VIRTIO_IPC_MSG, SKL_VIRTIO_IPC_VQ_SZ);
		if (ret) {
			dev_err(dev, "Failed to handle virtio message");
			return;
		}

		/* send IPC to HW */
		ret = vbe_skl_ipc_fwd(vbe, vbe->sdev, vm_id, reply_buf[0],
				reply_buf[1], reply_len[0], &reply_len[1]);
		if (ret < 0)
			dev_err(dev, "submit guest ipc command fail\n");

		virtio_vq_relchain(vq, idx, reply_len[0]);
	}

	/* BE has finished the operations, now let's kick back */
	virtio_vq_endchains(vq, false);
}

/* IPC notification reply from FE to DSP */
static void vbe_skl_ipc_fe_not_reply_get(struct snd_skl_vbe *vbe, int vq_idx)
{
	struct virtio_vq_info *vq;
	struct vfe_pending_msg *entry;
	bool sent;

	while (!list_empty(&vbe->pending_msg_list)) {
		vq = &vbe->vqs[vq_idx];
		entry = list_first_entry(&vbe->pending_msg_list,
				struct vfe_pending_msg, list);

		sent = vbe_skl_try_send(vbe, vq,
				(void *)&entry->msg, entry->sizeof_msg);

		if (sent == true) {
			list_del(&entry->list);
			kfree(entry);
		} else {
			/* break and handle in next kick */
			break;
		}
	}
}

void vbe_skl_handle_kick(struct snd_skl_vbe *vbe, int vq_idx)
{
	dev_dbg(vbe->dev, "vq_idx %d\n", vq_idx);

	switch (vq_idx) {
	case SKL_VIRTIO_IPC_CMD_TX_VQ:
		/* IPC command from FE to DSP */
		vbe_skl_ipc_fe_cmd_get(vbe, vq_idx);
		break;
	case SKL_VIRTIO_IPC_CMD_RX_VQ:
		/* IPC command reply from DSP to FE - NOT kick */
		vbe_handle_irq_queue(vbe, vq_idx);
		break;
	case SKL_VIRTIO_IPC_NOT_TX_VQ:
		schedule_work(&vbe->not_tx_handler_work);
		break;
	case SKL_VIRTIO_IPC_NOT_RX_VQ:
		/* IPC notification from DSP to FE - NOT kick */
		vbe_skl_ipc_fe_not_reply_get(vbe, vq_idx);
		break;
	default:
		dev_err(vbe->dev, "idx %d is invalid\n", vq_idx);
		break;
	}
}

static void not_tx_handler(struct work_struct *work)
{
	struct snd_skl_vbe *vbe =
		container_of(work, struct snd_skl_vbe, not_tx_handler_work);

	vbe_skl_ipc_fe_not_get(vbe, SKL_VIRTIO_IPC_NOT_TX_VQ);
}

int vbe_skl_attach(struct snd_skl_vbe *vbe, struct skl *skl)
{
	static bool kctl_init;

	if (!kctl_init) {

		if (unlikely(!skl || !skl->component || !skl->component->card))
			return -EINVAL;

		kctl_init_proxy(vbe->dev, &vbe_kctl_ops);
		kctl_notify_machine_ready(skl->component->card);

		INIT_WORK(&vbe->not_tx_handler_work, not_tx_handler);
		kctl_init = true;
	}

	return 0;
}

int vbe_skl_detach(struct snd_skl_vbe *vbe, struct skl *skl)
{
	/* TODO: Notify FE, close all streams opened by FE and delete all
	 * pending messages
	 */

	cancel_work_sync(&vbe->not_tx_handler_work);

	return 0;
}

void vbe_skl_bind(struct snd_skl_vbe *vbe, struct skl *skl)
{
	vbe->sdev = skl;
	vbe->nops.request_tplg = skl->skl_sst->request_tplg;
	vbe->nops.hda_irq_ack = skl->skl_sst->hda_irq_ack;
	skl->skl_sst->hda_irq_ack = vbe_stream_update;
}

void vbe_skl_unbind(struct snd_skl_vbe *vbe, struct skl *skl)
{
	if (!vbe->sdev)
		return;

	skl->skl_sst->request_tplg = vbe->nops.request_tplg;
	skl->skl_sst->hda_irq_ack = vbe->nops.hda_irq_ack;
	vbe->sdev = NULL;
}
