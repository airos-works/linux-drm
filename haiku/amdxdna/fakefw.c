/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Lab aid: a stand-in for the NPU of a Ryzen AI 300 ("haiku_selftest 3" in
	the settings), for running amdxdna's own code for the hardware in a
	virtual machine - starting and stopping, power down and up, the mailbox
	channels and their interrupts, contexts and commands - thousands of
	times, to find what a count that runs out, a leak or a race in the port
	would do on the real thing.

	What is real: a PCI function with MSI-X of the virtual machine that no
	Haiku driver has (QEMU's megasas-gen2 by default, "fake_pci"), with the
	IDs of the NPU of Krackan Point put into its pci_dev. Its configuration
	space, the PCI bus manager's MSI-X setup and Haiku's interrupt vectors
	and handlers are used as on hardware. What is not: the four BARs the
	driver maps are memory (lx_pcim_iomap_hook), and a thread plays SMU, PSP
	and firmware in it. It looks at the registers every "fake_poll_us"
	microseconds:

	- SMU: a command is taken when the driver sets the interrupt register;
	  power on and off are remembered, everything else is acknowledged.
	- PSP: "validate" sums up the firmware image at the physical address it
	  is given and compares the sum with the first one (the image must stay
	  where and what it was across power cycles), "start" makes the firmware
	  alive: the description of the management channel appears in the SRAM.
	- firmware: messages are taken from the channels' ring buffers and
	  answered as the driver expects for the protocol of npu_7.sbin (7.x),
	  the answer followed by the channel's interrupt: an interrupt on the
	  vector Haiku allotted to the MSI-X entry, sent by the local APIC to
	  the CPU the thread runs on. Commands are answered "fake_exec_us" later
	  and compute nothing. Now and then an interrupt is sent for a channel
	  that was just closed, as a device may.

	The stand-in complains in the log about what a firmware could not live
	with: power cut with contexts alive, a start without power, messages
	it cannot read. */

#include <lx/haiku.h>

#include <drm/drm_device.h>
#include <drm/gpu_scheduler.h>
#include <linux/kthread.h>
#include <linux/pci.h>

#include "aie2_msg_priv.h"
#include "aie2_pci.h"
#include "amdxdna_pci_drv.h"

#include "fakefw.h"


static unsigned int fake_pci = 0x10000079;	/* vendor, device: megasas-gen2 */
module_param(fake_pci, uint, 0444);
static unsigned int fake_poll_us = 100;
module_param(fake_poll_us, uint, 0644);
static unsigned int fake_exec_us = 300;
module_param(fake_exec_us, uint, 0644);
/* one in this many closed channels gets a late interrupt (0: none) */
static unsigned int fake_stray = 4;
module_param(fake_stray, uint, 0644);

#define FW_CHANNELS		15		/* 0: management; the others: contexts */
#define FW_RING			0x1000u
#define FW_PENDING		8
#define FW_TOMBSTONE	0xdeadfaceu
#define FW_MAGIC		0x55504e5fu		/* MGMT_MBOX_MAGIC */
#define FW_INFO_OFFSET	0x1e100u		/* in the SRAM */
#define FW_REGS_OFFSET	0x20000u		/* in the mailbox BAR */

/* a channel's registers, from FW_REGS_OFFSET + 0x20 * channel */
#define FW_REG_X2I_HEAD	0x00
#define FW_REG_X2I_TAIL	0x04
#define FW_REG_I2X_HEAD	0x08
#define FW_REG_INTR		0x0c	/* the driver takes it to follow the head */
#define FW_REG_I2X_TAIL	0x10

static const size_t fw_bar_size[PCI_STD_NUM_BARS] = {
	[0] = 0x40000,		/* registers and mailbox */
	[2] = 0x80000,		/* SRAM */
	[4] = 0x10000,		/* PSP */
	[5] = 0x10000,		/* SMU */
};

struct fw_header {
	u32 total_size;
	u32 sz_ver;
	u32 id;
	u32 opcode;
};

/* aie2_pci.c's struct mgmt_mbox_chann_info */
struct fw_channel_info {
	u32 x2i_tail;
	u32 x2i_head;
	u32 x2i_buf;
	u32 x2i_buf_sz;
	u32 i2x_tail;
	u32 i2x_head;
	u32 i2x_buf;
	u32 i2x_buf_sz;
	u32 magic;
	u32 msi_id;
	u32 prot_major;
	u32 prot_minor;
	u32 rsvd[4];
};

struct fw_pending {
	bool used;
	u32 id;
	u32 opcode;
	u32 size;
	u64 due;
};

struct fw_channel {
	bool active;
	u32 x2i_head;
	u32 i2x_tail;
	struct fw_pending pending[FW_PENDING];
};

static struct {
	struct pci_dev *pdev;
	unsigned short vendor, device;
	u8 revision;
	struct resource resource[PCI_STD_NUM_BARS];
	u8 *bar[PCI_STD_NUM_BARS];
	struct task_struct *thread;
	void __iomem *apic;

	bool powered;
	bool alive;
	struct fw_channel channel[FW_CHANNELS];
	bool have_sum;
	u32 firmware_sum;
	u64 firmware_address;
	u32 stray_msix;
	u64 stray_due;
	unsigned int closed;

	unsigned long power_ons, starts, messages, commands, interrupts, strays;
	unsigned long complaints;
} fw;


#define fw_complain(format, ...) \
	do { \
		fw.complaints++; \
		pr_err("amdxdna fake npu: " format "\n", ##__VA_ARGS__); \
	} while (0)


/* #pragma mark - registers */


static const struct amdxdna_dev_priv *
fw_priv(void)
{
	return dev_npu6_info.dev_priv;
}


static u32 *
fw_reg(const struct aie_bar_off_pair *pair)
{
	return (u32 *)(fw.bar[pair->bar_idx] + pair->offset);
}


static u32 *
fw_psp_reg(int index)
{
	return fw_reg(&fw_priv()->psp_regs_off[index]);
}


static u32 *
fw_smu_reg(int index)
{
	return fw_reg(&fw_priv()->smu_regs_off[index]);
}


static u32 *
fw_channel_reg(int channel, u32 which)
{
	return (u32 *)(fw.bar[dev_npu6_info.mbox_bar] + FW_REGS_OFFSET
		+ 0x20 * channel + which);
}


static u8 *
fw_ring(int channel, bool i2x)
{
	return fw.bar[dev_npu6_info.sram_bar] + 2 * FW_RING * channel
		+ (i2x ? FW_RING : 0);
}


/* #pragma mark - interrupts */


static inline u64
fw_rdmsr(u32 msr)
{
	u32 low, high;
	asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
	return ((u64)high << 32) | low;
}


static inline void
fw_wrmsr(u32 msr, u64 value)
{
	asm volatile("wrmsr" : : "c"(msr), "a"((u32)value),
		"d"((u32)(value >> 32)));
}


/*	What the device's MSI-X message does: an interrupt on the vector Haiku
	allotted to the entry (the first one's, plus the entry's number; the
	interrupt vector is 32 more), here from the local APIC to its own CPU. */
static void
fw_raise(u32 msix)
{
	u32 vector;

	if (!fw.pdev->msix_enabled || msix >= fw.pdev->lx_irq_count)
		return;
	vector = fw.pdev->irq + msix + 32;
	fw.interrupts++;
	if (fw_rdmsr(0x1b) & BIT(10)) {
		/* x2APIC: the self IPI register */
		fw_wrmsr(0x83f, vector);
	} else if (fw.apic != NULL) {
		/* the interrupt command register: fixed, to self */
		writel(0x40000 | vector, fw.apic + 0x300);
	}
}


/* #pragma mark - firmware */


static void
fw_channel_reset(int index)
{
	struct fw_channel *channel = &fw.channel[index];
	memset(channel, 0, sizeof(*channel));
	memset(fw_channel_reg(index, 0), 0, 0x20);
}


static void
fw_channels_off(const char *why)
{
	int i;
	for (i = 1; i < FW_CHANNELS; i++) {
		if (fw.channel[i].active)
			fw_complain("%s with context %d alive", why, i);
	}
	for (i = 0; i < FW_CHANNELS; i++)
		fw.channel[i].active = false;
}


/* the firmware starts: it describes its management channel in the SRAM */
static void
fw_boot(void)
{
	const struct amdxdna_dev_priv *priv = fw_priv();
	struct fw_channel_info info = {};
	u32 regs = priv->mbox_dev_addr + FW_REGS_OFFSET;
	int i;

	for (i = 0; i < FW_CHANNELS; i++)
		fw_channel_reset(i);
	fw.channel[0].active = true;

	info.x2i_head = regs + FW_REG_X2I_HEAD;
	info.x2i_tail = regs + FW_REG_X2I_TAIL;
	info.x2i_buf = priv->sram_dev_addr;
	info.x2i_buf_sz = FW_RING;
	info.i2x_head = regs + FW_REG_I2X_HEAD;
	info.i2x_tail = regs + FW_REG_I2X_TAIL;
	info.i2x_buf = priv->sram_dev_addr + FW_RING;
	info.i2x_buf_sz = FW_RING;
	info.magic = FW_MAGIC;
	info.msi_id = 0;
	info.prot_major = 7;
	info.prot_minor = 2;
	memcpy(fw.bar[dev_npu6_info.sram_bar] + FW_INFO_OFFSET, &info,
		sizeof(info));
	WRITE_ONCE(*fw_psp_reg(PSP_PWAITMODE_REG), 0);
	fw.alive = true;
	fw.starts++;
	/* last: what the driver waits for */
	WRITE_ONCE(*fw_reg(&priv->sram_offs[FW_ALIVE_OFF]),
		priv->sram_dev_addr + FW_INFO_OFFSET);
}


/* an answer into the channel's ring to the host, and its interrupt */
static void
fw_answer(int index, u32 id, u32 opcode, const void *payload, u32 size)
{
	struct fw_channel *channel = &fw.channel[index];
	struct fw_header header = {
		.total_size = size,
		.sz_ver = size | (1 << 16),
		.id = id,
		.opcode = opcode,
	};
	u8 *ring = fw_ring(index, true);
	u32 tail = channel->i2x_tail;
	u32 total = sizeof(header) + size;

	if (tail + total > FW_RING) {
		/* from the start again, once the host has read all there is; a
		 * tombstone tells it so */
		int tries = 2000;
		while (READ_ONCE(*fw_channel_reg(index, FW_REG_I2X_HEAD)) != tail
			&& --tries > 0)
			lx_haiku_snooze(50);
		if (tries == 0) {
			/* nobody reads (the channel was closed): start over anyway */
		}
		if (tail + sizeof(u32) <= FW_RING)
			*(u32 *)(ring + tail) = FW_TOMBSTONE;
		tail = 0;
	}
	memcpy(ring + tail, &header, sizeof(header));
	memcpy(ring + tail + sizeof(header), payload, size);
	channel->i2x_tail = tail + total;
	wmb();
	WRITE_ONCE(*fw_channel_reg(index, FW_REG_I2X_TAIL), channel->i2x_tail);
	WRITE_ONCE(*fw_channel_reg(index, FW_REG_INTR), 1);
	fw_raise(index);
}


static void
fw_defer(int index, u32 id, u32 opcode, u32 size)
{
	struct fw_channel *channel = &fw.channel[index];
	u32 status[3] = {};
	int i;

	for (i = 0; i < FW_PENDING; i++) {
		if (channel->pending[i].used)
			continue;
		channel->pending[i].used = true;
		channel->pending[i].id = id;
		channel->pending[i].opcode = opcode;
		channel->pending[i].size = size;
		channel->pending[i].due = ktime_get_ns()
			+ (u64)READ_ONCE(fake_exec_us) * 1000;
		return;
	}
	/* more in flight than the driver ever sends: answer at once */
	fw_answer(index, id, opcode, status, size);
}


static void
fw_flush(int index)
{
	struct fw_channel *channel = &fw.channel[index];
	u32 status[3] = {};
	u64 now = ktime_get_ns();
	int i;

	for (i = 0; i < FW_PENDING; i++) {
		if (!channel->pending[i].used || channel->pending[i].due > now)
			continue;
		channel->pending[i].used = false;
		fw_answer(index, channel->pending[i].id, channel->pending[i].opcode,
			status, channel->pending[i].size);
	}
}


static void
fw_message(int index, const struct fw_header *header, const u8 *payload)
{
	const struct amdxdna_dev_priv *priv = fw_priv();
	union {
		u32 words[32];
		struct create_ctx_resp create;
		struct aie_tile_info_resp tiles;
		struct aie_version_info_resp version;
		struct firmware_version_resp firmware;
		struct get_dev_revision_resp revision;
		struct protocol_version_resp protocol;
	} answer;
	u32 size = sizeof(u32);		/* a status, 0 */
	bool suspend = false;
	int i;

	memset(&answer, 0, sizeof(answer));
	fw.messages++;

	if (index != 0) {
		switch (header->opcode) {
			case MSG_OP_CHAIN_EXEC_BUFFER_CF:
			case MSG_OP_CHAIN_EXEC_DPU:
			case MSG_OP_CHAIN_EXEC_NPU:
				fw.commands++;
				fw_defer(index, header->id, header->opcode,
					sizeof(struct cmd_chain_resp));
				return;
			case MSG_OP_EXECUTE_BUFFER_CF:
			case MSG_OP_EXEC_DPU:
				fw.commands++;
				fw_defer(index, header->id, header->opcode, sizeof(u32));
				return;
		}
		fw_answer(index, header->id, header->opcode, &answer, size);
		return;
	}

	switch (header->opcode) {
		case MSG_OP_CREATE_CONTEXT:
		{
			u32 regs, rings;
			for (i = 1; i < FW_CHANNELS && fw.channel[i].active; i++)
				;
			if (i == FW_CHANNELS) {
				answer.create.status = AIE2_STATUS_MGMT_ERT_NOAVAIL;
				break;
			}
			fw_channel_reset(i);
			fw.channel[i].active = true;
			regs = priv->mbox_dev_addr + FW_REGS_OFFSET + 0x20 * i;
			rings = priv->sram_dev_addr + 2 * FW_RING * i;
			answer.create.context_id = i;
			answer.create.msix_id = i;
			answer.create.num_cq_pairs_allocated = 1;
			answer.create.cq_pair[0].x2i_q.head_addr = regs + FW_REG_X2I_HEAD;
			answer.create.cq_pair[0].x2i_q.tail_addr = regs + FW_REG_X2I_TAIL;
			answer.create.cq_pair[0].x2i_q.buf_addr = rings;
			answer.create.cq_pair[0].x2i_q.buf_size = FW_RING;
			answer.create.cq_pair[0].i2x_q.head_addr = regs + FW_REG_I2X_HEAD;
			answer.create.cq_pair[0].i2x_q.tail_addr = regs + FW_REG_I2X_TAIL;
			answer.create.cq_pair[0].i2x_q.buf_addr = rings + FW_RING;
			answer.create.cq_pair[0].i2x_q.buf_size = FW_RING;
			size = sizeof(answer.create);
			break;
		}
		case MSG_OP_DESTROY_CONTEXT:
		{
			u32 id = ((const struct destroy_ctx_req *)payload)->context_id;
			if (id == 0 || id >= FW_CHANNELS || !fw.channel[id].active) {
				fw_complain("context %u destroyed, which is not there", id);
				answer.words[0] = AIE2_STATUS_INVALID_PARAM;
				break;
			}
			fw.channel[id].active = false;
			/* a device may still have an interrupt on its way */
			if (READ_ONCE(fake_stray) != 0
				&& ++fw.closed % READ_ONCE(fake_stray) == 0) {
				fw.stray_msix = id;
				fw.stray_due = ktime_get_ns() + 200 * 1000;
			}
			break;
		}
		case MSG_OP_SUSPEND:
			for (i = 1; i < FW_CHANNELS; i++) {
				if (fw.channel[i].active)
					fw_complain("suspended with context %d alive", i);
			}
			suspend = true;
			break;
		case MSG_OP_RESUME:
			WRITE_ONCE(*fw_psp_reg(PSP_PWAITMODE_REG), 0);
			break;
		case MSG_OP_QUERY_AIE_TILE_INFO:
			/* as the Ryzen AI 5 340 answers */
			answer.tiles.info.size = 0x100000;
			answer.tiles.info.major = 1;
			answer.tiles.info.minor = 1;
			answer.tiles.info.cols = 8;
			answer.tiles.info.rows = 6;
			answer.tiles.info.core_rows = 4;
			answer.tiles.info.mem_rows = 1;
			answer.tiles.info.shim_rows = 1;
			answer.tiles.info.core_row_start = 2;
			answer.tiles.info.mem_row_start = 1;
			answer.tiles.info.shim_row_start = 0;
			answer.tiles.info.core_dma_channels = 2;
			answer.tiles.info.mem_dma_channels = 6;
			answer.tiles.info.shim_dma_channels = 2;
			answer.tiles.info.core_locks = 16;
			answer.tiles.info.mem_locks = 64;
			answer.tiles.info.shim_locks = 16;
			size = sizeof(answer.tiles);
			break;
		case MSG_OP_QUERY_AIE_VERSION:
			answer.version.major = 1;
			answer.version.minor = 1;
			size = sizeof(answer.version);
			break;
		case MSG_OP_GET_FIRMWARE_VERSION:
			/* no firmware has this version */
			answer.firmware.major = 0;
			answer.firmware.minor = 0;
			answer.firmware.sub = 0;
			answer.firmware.build = 1;
			size = sizeof(answer.firmware);
			break;
		case MSG_OP_GET_DEV_REVISION:
			answer.revision.rev = AIE2_DEV_REVISION_KRK1;
			size = sizeof(answer.revision);
			break;
		case MSG_OP_GET_PROTOCOL_VERSION:
			answer.protocol.major = 7;
			answer.protocol.minor = 2;
			size = sizeof(answer.protocol);
			break;
		case MSG_OP_GET_RUNTIME_CONFIG:
			size = sizeof(struct get_runtime_cfg_resp);
			break;
		case MSG_OP_GET_APP_HEALTH:
			size = sizeof(struct get_app_health_resp);
			break;
		case MSG_OP_QUERY_COL_STATUS:
			size = sizeof(struct aie_column_info_resp);
			break;
		case MSG_OP_GET_TELEMETRY:
			size = sizeof(struct get_telemetry_resp);
			break;
		case MSG_OP_REGISTER_ASYNC_EVENT_MSG:
			/* answered when there is an error to report: never */
			return;
	}
	fw_answer(0, header->id, header->opcode, &answer, size);
	if (suspend)
		WRITE_ONCE(*fw_psp_reg(PSP_PWAITMODE_REG), 1);
}


/* the messages the host has put into a channel's ring */
static void
fw_channel_poll(int index)
{
	struct fw_channel *channel = &fw.channel[index];
	const u8 *ring = fw_ring(index, false);
	u32 tail = READ_ONCE(*fw_channel_reg(index, FW_REG_X2I_TAIL));

	while (channel->x2i_head != tail) {
		struct fw_header header;
		u32 head = channel->x2i_head;

		if (tail > FW_RING || (tail & 3) != 0 || head > FW_RING - sizeof(u32)) {
			fw_complain("channel %d: tail %#x, head %#x", index, tail, head);
			channel->active = false;
			return;
		}
		if (*(const u32 *)(ring + head) == FW_TOMBSTONE) {
			channel->x2i_head = 0;
			WRITE_ONCE(*fw_channel_reg(index, FW_REG_X2I_HEAD), 0);
			continue;
		}
		memcpy(&header, ring + head, sizeof(header));
		if (header.total_size > FW_RING - sizeof(header) - head
			|| (header.total_size & 3) != 0
			|| (header.sz_ver & 0x7ff) != header.total_size) {
			fw_complain("channel %d: message of %u bytes (%#x) at %#x", index,
				header.total_size, header.sz_ver, head);
			channel->active = false;
			return;
		}
		fw_message(index, &header, ring + head + sizeof(header));
		channel->x2i_head = head + sizeof(header) + header.total_size;
		WRITE_ONCE(*fw_channel_reg(index, FW_REG_X2I_HEAD), channel->x2i_head);
		if (!fw.alive || !channel->active)
			return;
	}
	fw_flush(index);
}


/* #pragma mark - PSP and SMU */


static u32
fw_sum(const u8 *data, size_t size)
{
	u32 sum = 0;
	size_t i;
	for (i = 0; i < size; i++)
		sum = sum * 31 + data[i];
	return sum;
}


static void
fw_psp_poll(void)
{
	u32 *interrupt = fw_psp_reg(PSP_INTR_REG);
	u32 command, answer = 0;
	u64 address;
	u32 size, sum;

	if (READ_ONCE(*interrupt) != 1)
		return;
	command = READ_ONCE(*fw_psp_reg(PSP_CMD_REG));
	address = READ_ONCE(*fw_psp_reg(PSP_ARG0_REG))
		| ((u64)READ_ONCE(*fw_psp_reg(PSP_ARG1_REG)) << 32);
	size = READ_ONCE(*fw_psp_reg(PSP_ARG2_REG)) & 0xffffff;

	switch (command) {
		case 1:		/* validate */
			if (address == 0 || (address & 0xffff) != 0 || size == 0) {
				fw_complain("PSP: firmware at %#llx, %u bytes", address, size);
				answer = 0xffff0001;
				break;
			}
			sum = fw_sum(phys_to_virt(address), size);
			if (!fw.have_sum) {
				fw.have_sum = true;
				fw.firmware_sum = sum;
				fw.firmware_address = address;
			} else if (sum != fw.firmware_sum
				|| address != fw.firmware_address) {
				fw_complain("PSP: the firmware image changed (%#llx sum %#x, "
					"was %#llx sum %#x)", address, sum, fw.firmware_address,
					fw.firmware_sum);
				answer = 0xffff0001;
			}
			break;
		case 2:		/* start */
			if (!fw.powered) {
				fw_complain("PSP: start without power");
				answer = 0xffff0007;
				break;
			}
			if (fw.alive)
				fw_complain("PSP: start of a firmware that runs");
			fw_boot();
			break;
		case 3:		/* release */
			fw_channels_off("PSP: firmware released");
			fw.alive = false;
			break;
		case 4:		/* validate a certificate */
			break;
		default:
			fw_complain("PSP: command %#x", command);
			answer = 0xffff0002;
			break;
	}
	WRITE_ONCE(*fw_psp_reg(PSP_RESP_REG), answer);
	WRITE_ONCE(*fw_psp_reg(PSP_STATUS_REG), command | BIT(31));
	WRITE_ONCE(*interrupt, 0);
}


static void
fw_smu_poll(void)
{
	u32 *interrupt = fw_smu_reg(SMU_INTR_REG);
	u32 command, argument, answer = 1;

	if (READ_ONCE(*interrupt) != 1)
		return;
	command = READ_ONCE(*fw_smu_reg(SMU_CMD_REG));
	argument = READ_ONCE(*fw_smu_reg(SMU_ARG_REG));
	switch (command) {
		case 3:		/* power on */
			if (!fw.powered)
				fw.power_ons++;
			fw.powered = true;
			break;
		case 4:		/* power off */
			if (fw.alive)
				fw_channels_off("power off");
			fw.alive = false;
			fw.powered = false;
			break;
		case 5: case 6: case 7: case 8:		/* clocks, levels */
			break;
		default:
			fw_complain("SMU: command %u", command);
			answer = 0xff;
			break;
	}
	WRITE_ONCE(*fw_smu_reg(SMU_OUT_REG), argument);
	WRITE_ONCE(*fw_smu_reg(SMU_RESP_REG), answer);
	WRITE_ONCE(*interrupt, 0);
}


static int
fw_thread(void *unused)
{
	while (!kthread_should_stop()) {
		int i;

		fw_smu_poll();
		fw_psp_poll();
		if (fw.alive) {
			for (i = 0; i < FW_CHANNELS; i++) {
				if (fw.channel[i].active)
					fw_channel_poll(i);
			}
		}
		if (fw.stray_due != 0 && ktime_get_ns() >= fw.stray_due) {
			fw.stray_due = 0;
			fw.strays++;
			fw_raise(fw.stray_msix);
		}
		lx_haiku_snooze(max(READ_ONCE(fake_poll_us), 20u));
	}
	return 0;
}


/* #pragma mark - the device */


static void __iomem *
fw_iomap(struct pci_dev *pdev, int bar)
{
	if (pdev != fw.pdev || bar < 0 || bar >= PCI_STD_NUM_BARS)
		return NULL;
	return fw.bar[bar];
}


int
lx_fakefw_stats(char *buffer, int size)
{
	if (fw.pdev == NULL)
		return 0;
	return scnprintf(buffer, size, " fake npu: power on %lu, firmware starts "
		"%lu, messages %lu, commands %lu, interrupts %lu (late %lu), "
		"complaints %lu", fw.power_ons, fw.starts, fw.messages, fw.commands,
		fw.interrupts, fw.strays, fw.complaints);
}


/*	Dresses a PCI function of the virtual machine up as the NPU and starts
	the thread that plays its other half. */
struct pci_dev *
lx_fakefw_start(void)
{
	struct pci_dev *pdev;
	int i;

	if (fw.pdev != NULL)
		return fw.pdev;
	pdev = pci_get_device(fake_pci >> 16, fake_pci & 0xffff, NULL);
	if (pdev == NULL) {
		pr_err("amdxdna fake npu: no PCI device %04x:%04x to stand in (QEMU: "
			"-device megasas-gen2)\n", fake_pci >> 16, fake_pci & 0xffff);
		return NULL;
	}
	if (pci_msix_vec_count(pdev) < 2) {
		pr_err("amdxdna fake npu: %s has no MSI-X\n", pci_name(pdev));
		return NULL;
	}

	memset(&fw, 0, sizeof(fw));
	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		if (fw_bar_size[i] == 0)
			continue;
		fw.bar[i] = vzalloc(fw_bar_size[i]);
		if (fw.bar[i] == NULL)
			goto fail;
	}
	fw.vendor = pdev->vendor;
	fw.device = pdev->device;
	fw.revision = pdev->revision;
	memcpy(fw.resource, pdev->resource, sizeof(fw.resource));
	fw.pdev = pdev;

	/* the Ryzen AI 5 340's NPU; its BARs are this memory */
	pdev->vendor = PCI_VENDOR_ID_AMD;
	pdev->device = 0x17f0;
	pdev->revision = 0x20;
	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		if (fw.bar[i] == NULL)
			continue;
		pdev->resource[i].start = (resource_size_t)(uintptr_t)fw.bar[i];
		pdev->resource[i].end = pdev->resource[i].start + fw_bar_size[i] - 1;
		pdev->resource[i].flags = IORESOURCE_MEM;
	}
	WRITE_ONCE(*fw_psp_reg(PSP_STATUS_REG), BIT(31));	/* ready */
	if ((fw_rdmsr(0x1b) & BIT(10)) == 0)
		fw.apic = ioremap(fw_rdmsr(0x1b) & ~0xfffULL, PAGE_SIZE);

	lx_pcim_iomap_hook = fw_iomap;
	lx_emulated_device = 1;
	fw.thread = kthread_run(fw_thread, NULL, "amdxdna fake npu");
	if (IS_ERR(fw.thread)) {
		fw.thread = NULL;
		lx_fakefw_stop();
		return NULL;
	}
	pr_info("amdxdna fake npu: %s (%04x:%04x, %d MSI-X vectors) stands in "
		"for the NPU\n", pci_name(pdev), fw.vendor, fw.device,
		pci_msix_vec_count(pdev));
	return pdev;

fail:
	for (i = 0; i < PCI_STD_NUM_BARS; i++)
		vfree(fw.bar[i]);
	memset(&fw, 0, sizeof(fw));
	return NULL;
}


void
lx_fakefw_stop(void)
{
	int i;

	if (fw.pdev == NULL)
		return;
	if (fw.thread != NULL)
		kthread_stop(fw.thread);
	lx_pcim_iomap_hook = NULL;
	lx_emulated_device = 0;
	fw.pdev->vendor = fw.vendor;
	fw.pdev->device = fw.device;
	fw.pdev->revision = fw.revision;
	memcpy(fw.pdev->resource, fw.resource, sizeof(fw.resource));
	if (fw.apic != NULL)
		iounmap(fw.apic);
	for (i = 0; i < PCI_STD_NUM_BARS; i++)
		vfree(fw.bar[i]);
	memset(&fw, 0, sizeof(fw));
}
