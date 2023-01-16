// SPDX-License-Identifier: GPL-2.0
/* Marvell Octeon EP (EndPoint) VF Ethernet Driver
 *
 * Copyright (C) 2020 Marvell.
 *
 */
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include "octep_vf_config.h"
#include "octep_vf_main.h"

int octep_vf_setup_mbox(struct octep_vf_device *oct)
{
	int ring = 0;

	oct->mbox = vzalloc(sizeof(*oct->mbox));
	if (!oct->mbox)
		return -1;

	mutex_init(&oct->mbox->lock);

	oct->hw_ops.setup_mbox_regs(oct, ring);
	INIT_WORK(&oct->mbox->wk.work, octep_vf_mbox_work);
	oct->mbox->wk.ctxptr = oct;

	dev_info(&oct->pdev->dev, "setup vf mbox successfully\n");
	return 0;
}

void octep_vf_delete_mbox(struct octep_vf_device *oct)
{
	if (oct->mbox) {
		if (work_pending(&oct->mbox->wk.work))
			cancel_work_sync(&oct->mbox->wk.work);

		mutex_destroy(&oct->mbox->lock);
		vfree(oct->mbox);
		oct->mbox = NULL;
		dev_info(&oct->pdev->dev, "Deleted vf mbox successfully\n");
	}
}

int octep_vf_mbox_version_check(struct octep_vf_device *oct)
{
	union octep_pfvf_mbox_word cmd;
	union octep_pfvf_mbox_word rsp;
	int ret;

	cmd.u64 = 0;
	cmd.s_version.opcode = OCTEP_PFVF_MBOX_CMD_VERSION;
	cmd.s_version.version = OCTEP_VF_MBOX_VERSION;
	ret = octep_vf_mbox_send_cmd(oct, cmd, &rsp);
	if (!ret)
		return 0;
	if (ret == OCTEP_PFVF_MBOX_CMD_STATUS_NACK) {
		dev_err(&oct->pdev->dev,
			"VF Mbox version:%llu is not compatible with PF\n",
			(u64)cmd.s_version.version);
		dev_err(&oct->pdev->dev,
			"Unload VF driver and load compatibale VF driver version\n");
	}
	return ret;
}

void octep_vf_mbox_work(struct work_struct *work)
{
	struct octep_vf_mbox_wk *wk = container_of(work, struct octep_vf_mbox_wk, work);
	struct octep_vf_iface_link_info *link_info;
	struct octep_vf_device *oct = NULL;
	struct octep_vf_mbox *mbox = NULL;
	u64 pf_vf_data;

	oct = (struct octep_vf_device *)wk->ctxptr;
	link_info = &oct->link_info;
	mbox = oct->mbox;
	pf_vf_data = readq(mbox->mbox_read_reg);
	if (unlikely(pf_vf_data == 0xFFFFFFFFFFFFFFFFU))
		return;
	if (!(pf_vf_data & OCTEP_PFVF_LINK_STATUS_DOWN))
		link_info->oper_up = OCTEP_PFVF_LINK_STATUS_DOWN;
	else if (pf_vf_data & OCTEP_PFVF_LINK_STATUS_UP)
		link_info->oper_up = OCTEP_PFVF_LINK_STATUS_UP;
}

static int __octep_vf_mbox_send_cmd(struct octep_vf_device *oct,
				    union octep_pfvf_mbox_word cmd,
				    union octep_pfvf_mbox_word *rsp)
{
	struct octep_vf_mbox *mbox = oct->mbox;
	u64 reg_val = 0ull;
	int count = 0;

	if (!mbox)
		return OCTEP_PFVF_MBOX_CMD_STATUS_NOT_SETUP;

	cmd.s.type = OCTEP_PFVF_MBOX_TYPE_CMD;
	writeq(cmd.u64, mbox->mbox_write_reg);

	/* No response for notification messages */
	if (!rsp)
		return 0;

	for (count = 0; count < OCTEP_PFVF_MBOX_TIMEOUT_WAIT_COUNT; count++) {
		usleep_range(1000, 1500);
		reg_val = readq(mbox->mbox_write_reg);
		if (unlikely(reg_val == 0xFFFFFFFFFFFFFFFFU)) {
			dev_err(&oct->pdev->dev, "mbox send command err\n");
			return OCTEP_PFVF_MBOX_CMD_STATUS_ERR;
		}
		if (reg_val != cmd.u64) {
			rsp->u64 = reg_val;
			break;
		}
	}
	if (count == OCTEP_PFVF_MBOX_TIMEOUT_WAIT_COUNT) {
		dev_err(&oct->pdev->dev, "mbox send command timed out\n");
		return OCTEP_PFVF_MBOX_CMD_STATUS_TIMEDOUT;
	}
	if (rsp->s.type != OCTEP_PFVF_MBOX_TYPE_RSP_ACK) {
		dev_err(&oct->pdev->dev, "mbox_send: Received NACK\n");
		return OCTEP_PFVF_MBOX_CMD_STATUS_NACK;
	}
	rsp->u64 = reg_val;
	return 0;
}

int octep_vf_mbox_send_cmd(struct octep_vf_device *oct, union octep_pfvf_mbox_word cmd,
			   union octep_pfvf_mbox_word *rsp)
{
	struct octep_vf_mbox *mbox = oct->mbox;
	int ret;

	if (!mbox)
		return OCTEP_PFVF_MBOX_CMD_STATUS_NOT_SETUP;

	mutex_lock(&mbox->lock);
	ret = __octep_vf_mbox_send_cmd(oct, cmd, rsp);
	mutex_unlock(&mbox->lock);
	return ret;
}

int octep_vf_mbox_bulk_read(struct octep_vf_device *oct, enum octep_pfvf_mbox_opcode opcode,
			    u8 *data, int *size)
{
	struct octep_vf_mbox *mbox = oct->mbox;
	union octep_pfvf_mbox_word cmd;
	union octep_pfvf_mbox_word rsp;
	int data_len = 0, tmp_len = 0;
	int read_cnt, i = 0, ret;

	if (!mbox)
		return OCTEP_PFVF_MBOX_CMD_STATUS_NOT_SETUP;

	mutex_lock(&mbox->lock);
	cmd.u64 = 0;
	cmd.s_data.opcode = opcode;
	cmd.s_data.frag = 0;
	/* Send cmd to read data from PF */
	ret = __octep_vf_mbox_send_cmd(oct, cmd, &rsp);
	if (ret) {
		dev_err(&oct->pdev->dev, "send mbox cmd fail for data request\n");
		mutex_unlock(&mbox->lock);
		return ret;
	}
	/*  PF sends the data length of requested CMD
	 *  in  ACK
	 */
	data_len = *((int32_t *)rsp.s_data.data);
	tmp_len = data_len;
	cmd.u64 = 0;
	rsp.u64 = 0;
	cmd.s_data.opcode = opcode;
	cmd.s_data.frag = 1;
	while (data_len) {
		ret = __octep_vf_mbox_send_cmd(oct, cmd, &rsp);
		if (ret) {
			dev_err(&oct->pdev->dev, "send mbox cmd fail for data request\n");
			mutex_unlock(&mbox->lock);
			mbox->mbox_data.data_index = 0;
			memset(mbox->mbox_data.recv_data, 0, OCTEP_PFVF_MBOX_MAX_DATA_BUF_SIZE);
			return ret;
		}
		if (data_len > OCTEP_PFVF_MBOX_MAX_DATA_SIZE) {
			data_len -= OCTEP_PFVF_MBOX_MAX_DATA_SIZE;
			read_cnt = OCTEP_PFVF_MBOX_MAX_DATA_SIZE;
		} else {
			read_cnt = data_len;
			data_len = 0;
		}
		for (i = 0; i < read_cnt; i++) {
			mbox->mbox_data.recv_data[mbox->mbox_data.data_index] =
				rsp.s_data.data[i];
			mbox->mbox_data.data_index++;
		}
		cmd.u64 = 0;
		rsp.u64 = 0;
		cmd.s_data.opcode = opcode;
		cmd.s_data.frag = 1;
	}
	memcpy(data, mbox->mbox_data.recv_data, tmp_len);
	*size = tmp_len;
	mbox->mbox_data.data_index = 0;
	memset(mbox->mbox_data.recv_data, 0, OCTEP_PFVF_MBOX_MAX_DATA_BUF_SIZE);
	mutex_unlock(&mbox->lock);
	return 0;
}

int octep_vf_mbox_set_mtu(struct octep_vf_device *oct, int mtu)
{
	int frame_size = mtu + ETH_HLEN + ETH_FCS_LEN;
	union octep_pfvf_mbox_word cmd;
	union octep_pfvf_mbox_word rsp;
	int ret = 0;

	if (mtu < ETH_MIN_MTU || frame_size > ETH_MAX_MTU) {
		dev_err(&oct->pdev->dev,
			"Failed to set MTU to %d MIN MTU:%d MAX MTU:%d\n",
			mtu, ETH_MIN_MTU, ETH_MAX_MTU);
		return -EINVAL;
	}

	cmd.u64 = 0;
	cmd.s_set_mtu.opcode = OCTEP_PFVF_MBOX_CMD_SET_MTU;
	cmd.s_set_mtu.mtu = mtu;

	ret = octep_vf_mbox_send_cmd(oct, cmd, &rsp);
	if (ret) {
		dev_err(&oct->pdev->dev, "Mbox send failed; err=%d\n", ret);
		return ret;
	}
	if (rsp.s_set_mtu.type != OCTEP_PFVF_MBOX_TYPE_RSP_ACK) {
		dev_err(&oct->pdev->dev, "Received Mbox NACK from PF for MTU:%d\n", mtu);
		return -EINVAL;
	}

	return 0;
}

int octep_vf_mbox_set_mac_addr(struct octep_vf_device *oct, char *mac_addr)
{
	union octep_pfvf_mbox_word cmd;
	union octep_pfvf_mbox_word rsp;
	int i, ret;

	cmd.u64 = 0;
	cmd.s_set_mac.opcode = OCTEP_PFVF_MBOX_CMD_SET_MAC_ADDR;
	for (i = 0; i < ETH_ALEN; i++)
		cmd.s_set_mac.mac_addr[i] = mac_addr[i];
	ret = octep_vf_mbox_send_cmd(oct, cmd, &rsp);
	if (ret) {
		dev_err(&oct->pdev->dev, "Mbox send failed; err = %d\n", ret);
		return ret;
	}
	if (rsp.s_set_mac.type != OCTEP_PFVF_MBOX_TYPE_RSP_ACK) {
		dev_err(&oct->pdev->dev, "received NACK\n");
		return -EINVAL;
	}
	return 0;
}

int octep_vf_mbox_get_mac_addr(struct octep_vf_device *oct, char *mac_addr)
{
	union octep_pfvf_mbox_word cmd;
	union octep_pfvf_mbox_word rsp;
	int i, ret;

	cmd.u64 = 0;
	cmd.s_set_mac.opcode = OCTEP_PFVF_MBOX_CMD_GET_MAC_ADDR;
	ret = octep_vf_mbox_send_cmd(oct, cmd, &rsp);
	if (ret) {
		dev_err(&oct->pdev->dev, "get_mac: mbox send failed; err = %d\n", ret);
		return ret;
	}
	if (rsp.s_set_mac.type != OCTEP_PFVF_MBOX_TYPE_RSP_ACK) {
		dev_err(&oct->pdev->dev, "get_mac: received NACK\n");
		return -EINVAL;
	}
	for (i = 0; i < ETH_ALEN; i++)
		mac_addr[i] = rsp.s_set_mac.mac_addr[i];
	return 0;
}

int octep_vf_mbox_set_rx_state(struct octep_vf_device *oct, bool state)
{
	union octep_pfvf_mbox_word cmd;
	union octep_pfvf_mbox_word rsp;
	int ret;

	cmd.u64 = 0;
	cmd.s_link_state.opcode = OCTEP_PFVF_MBOX_CMD_SET_RX_STATE;
	cmd.s_link_state.state = state;
	ret = octep_vf_mbox_send_cmd(oct, cmd, &rsp);
	if (ret) {
		dev_err(&oct->pdev->dev, "Set Rx state via VF Mbox send failed\n");
		return ret;
	}
	if (rsp.s_link_state.type != OCTEP_PFVF_MBOX_TYPE_RSP_ACK) {
		dev_err(&oct->pdev->dev, "Set Rx state received NACK\n");
		return -EINVAL;
	}
	return 0;
}

int octep_vf_mbox_set_link_status(struct octep_vf_device *oct, bool status)
{
	union octep_pfvf_mbox_word cmd;
	union octep_pfvf_mbox_word rsp;
	int ret;

	cmd.u64 = 0;
	cmd.s_link_status.opcode = OCTEP_PFVF_MBOX_CMD_SET_LINK_STATUS;
	cmd.s_link_status.status = status;
	ret = octep_vf_mbox_send_cmd(oct, cmd, &rsp);
	if (ret) {
		dev_err(&oct->pdev->dev, "Set link status via VF Mbox send failed\n");
		return ret;
	}
	if (rsp.s_link_status.type != OCTEP_PFVF_MBOX_TYPE_RSP_ACK) {
		dev_err(&oct->pdev->dev, "Set link status received NACK\n");
		return -EINVAL;
	}
	return 0;
}

int octep_vf_mbox_get_link_status(struct octep_vf_device *oct, u8 *oper_up)
{
	union octep_pfvf_mbox_word cmd;
	union octep_pfvf_mbox_word rsp;
	int ret;

	cmd.u64 = 0;
	cmd.s_link_status.opcode = OCTEP_PFVF_MBOX_CMD_GET_LINK_STATUS;
	ret = octep_vf_mbox_send_cmd(oct, cmd, &rsp);
	if (ret) {
		dev_err(&oct->pdev->dev, "Get link status via VF Mbox send failed\n");
		return ret;
	}
	if (rsp.s_link_status.type != OCTEP_PFVF_MBOX_TYPE_RSP_ACK) {
		dev_err(&oct->pdev->dev, "Get link status received NACK\n");
		return -EINVAL;
	}
	*oper_up = rsp.s_link_status.status;
	return 0;
}

int octep_vf_mbox_dev_remove(struct octep_vf_device *oct)
{
	union octep_pfvf_mbox_word cmd;
	int ret;

	cmd.u64 = 0;
	cmd.s.opcode = OCTEP_PFVF_MBOX_CMD_DEV_REMOVE;
	ret = octep_vf_mbox_send_cmd(oct, cmd, NULL);
	return ret;
}
