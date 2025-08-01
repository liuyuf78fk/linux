// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
// Copyright(c) 2015-17 Intel Corporation.

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/mod_devicetable.h>
#include <linux/pm_runtime.h>
#include <linux/soundwire/sdw_registers.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_type.h>
#include <linux/string_choices.h>
#include "bus.h"
#include "irq.h"
#include "sysfs_local.h"

static DEFINE_IDA(sdw_bus_ida);

static int sdw_get_id(struct sdw_bus *bus)
{
	int rc = ida_alloc(&sdw_bus_ida, GFP_KERNEL);

	if (rc < 0)
		return rc;

	bus->id = rc;

	if (bus->controller_id == -1)
		bus->controller_id = rc;

	return 0;
}

/**
 * sdw_bus_master_add() - add a bus Master instance
 * @bus: bus instance
 * @parent: parent device
 * @fwnode: firmware node handle
 *
 * Initializes the bus instance, read properties and create child
 * devices.
 */
int sdw_bus_master_add(struct sdw_bus *bus, struct device *parent,
		       struct fwnode_handle *fwnode)
{
	struct sdw_master_prop *prop = NULL;
	int ret;

	if (!parent) {
		pr_err("SoundWire parent device is not set\n");
		return -ENODEV;
	}

	ret = sdw_get_id(bus);
	if (ret < 0) {
		dev_err(parent, "Failed to get bus id\n");
		return ret;
	}

	ida_init(&bus->slave_ida);

	ret = sdw_master_device_add(bus, parent, fwnode);
	if (ret < 0) {
		dev_err(parent, "Failed to add master device at link %d\n",
			bus->link_id);
		return ret;
	}

	if (!bus->ops) {
		dev_err(bus->dev, "SoundWire Bus ops are not set\n");
		return -EINVAL;
	}

	if (!bus->compute_params) {
		dev_err(bus->dev,
			"Bandwidth allocation not configured, compute_params no set\n");
		return -EINVAL;
	}

	/*
	 * Give each bus_lock and msg_lock a unique key so that lockdep won't
	 * trigger a deadlock warning when the locks of several buses are
	 * grabbed during configuration of a multi-bus stream.
	 */
	lockdep_register_key(&bus->msg_lock_key);
	__mutex_init(&bus->msg_lock, "msg_lock", &bus->msg_lock_key);

	lockdep_register_key(&bus->bus_lock_key);
	__mutex_init(&bus->bus_lock, "bus_lock", &bus->bus_lock_key);

	INIT_LIST_HEAD(&bus->slaves);
	INIT_LIST_HEAD(&bus->m_rt_list);

	/*
	 * Initialize multi_link flag
	 */
	bus->multi_link = false;
	if (bus->ops->read_prop) {
		ret = bus->ops->read_prop(bus);
		if (ret < 0) {
			dev_err(bus->dev,
				"Bus read properties failed:%d\n", ret);
			return ret;
		}
	}

	sdw_bus_debugfs_init(bus);

	/*
	 * Device numbers in SoundWire are 0 through 15. Enumeration device
	 * number (0), Broadcast device number (15), Group numbers (12 and
	 * 13) and Master device number (14) are not used for assignment so
	 * mask these and other higher bits.
	 */

	/* Set higher order bits */
	*bus->assigned = ~GENMASK(SDW_BROADCAST_DEV_NUM, SDW_ENUM_DEV_NUM);

	/* Set enumeration device number and broadcast device number */
	set_bit(SDW_ENUM_DEV_NUM, bus->assigned);
	set_bit(SDW_BROADCAST_DEV_NUM, bus->assigned);

	/* Set group device numbers and master device number */
	set_bit(SDW_GROUP12_DEV_NUM, bus->assigned);
	set_bit(SDW_GROUP13_DEV_NUM, bus->assigned);
	set_bit(SDW_MASTER_DEV_NUM, bus->assigned);

	ret = sdw_irq_create(bus, fwnode);
	if (ret)
		return ret;

	/*
	 * SDW is an enumerable bus, but devices can be powered off. So,
	 * they won't be able to report as present.
	 *
	 * Create Slave devices based on Slaves described in
	 * the respective firmware (ACPI/DT)
	 */
	if (IS_ENABLED(CONFIG_ACPI) && ACPI_HANDLE(bus->dev))
		ret = sdw_acpi_find_slaves(bus);
	else if (IS_ENABLED(CONFIG_OF) && bus->dev->of_node)
		ret = sdw_of_find_slaves(bus);
	else
		ret = -ENOTSUPP; /* No ACPI/DT so error out */

	if (ret < 0) {
		dev_err(bus->dev, "Finding slaves failed:%d\n", ret);
		sdw_irq_delete(bus);
		return ret;
	}

	/*
	 * Initialize clock values based on Master properties. The max
	 * frequency is read from max_clk_freq property. Current assumption
	 * is that the bus will start at highest clock frequency when
	 * powered on.
	 *
	 * Default active bank will be 0 as out of reset the Slaves have
	 * to start with bank 0 (Table 40 of Spec)
	 */
	prop = &bus->prop;
	bus->params.max_dr_freq = prop->max_clk_freq * SDW_DOUBLE_RATE_FACTOR;
	bus->params.curr_dr_freq = bus->params.max_dr_freq;
	bus->params.curr_bank = SDW_BANK0;
	bus->params.next_bank = SDW_BANK1;

	return 0;
}
EXPORT_SYMBOL(sdw_bus_master_add);

static int sdw_delete_slave(struct device *dev, void *data)
{
	struct sdw_slave *slave = dev_to_sdw_dev(dev);
	struct sdw_bus *bus = slave->bus;

	pm_runtime_disable(dev);

	sdw_slave_debugfs_exit(slave);

	mutex_lock(&bus->bus_lock);

	if (slave->dev_num) { /* clear dev_num if assigned */
		clear_bit(slave->dev_num, bus->assigned);
		if (bus->ops && bus->ops->put_device_num)
			bus->ops->put_device_num(bus, slave);
	}
	list_del_init(&slave->node);
	mutex_unlock(&bus->bus_lock);

	device_unregister(dev);
	return 0;
}

/**
 * sdw_bus_master_delete() - delete the bus master instance
 * @bus: bus to be deleted
 *
 * Remove the instance, delete the child devices.
 */
void sdw_bus_master_delete(struct sdw_bus *bus)
{
	device_for_each_child(bus->dev, NULL, sdw_delete_slave);

	sdw_irq_delete(bus);

	sdw_master_device_del(bus);

	sdw_bus_debugfs_exit(bus);
	lockdep_unregister_key(&bus->bus_lock_key);
	lockdep_unregister_key(&bus->msg_lock_key);
	ida_free(&sdw_bus_ida, bus->id);
}
EXPORT_SYMBOL(sdw_bus_master_delete);

/*
 * SDW IO Calls
 */

static inline int find_response_code(enum sdw_command_response resp)
{
	switch (resp) {
	case SDW_CMD_OK:
		return 0;

	case SDW_CMD_IGNORED:
		return -ENODATA;

	case SDW_CMD_TIMEOUT:
		return -ETIMEDOUT;

	default:
		return -EIO;
	}
}

static inline int do_transfer(struct sdw_bus *bus, struct sdw_msg *msg)
{
	int retry = bus->prop.err_threshold;
	enum sdw_command_response resp;
	int ret = 0, i;

	for (i = 0; i <= retry; i++) {
		resp = bus->ops->xfer_msg(bus, msg);
		ret = find_response_code(resp);

		/* if cmd is ok or ignored return */
		if (ret == 0 || ret == -ENODATA)
			return ret;
	}

	return ret;
}

static inline int do_transfer_defer(struct sdw_bus *bus,
				    struct sdw_msg *msg)
{
	struct sdw_defer *defer = &bus->defer_msg;
	int retry = bus->prop.err_threshold;
	enum sdw_command_response resp;
	int ret = 0, i;

	defer->msg = msg;
	defer->length = msg->len;
	init_completion(&defer->complete);

	for (i = 0; i <= retry; i++) {
		resp = bus->ops->xfer_msg_defer(bus);
		ret = find_response_code(resp);
		/* if cmd is ok or ignored return */
		if (ret == 0 || ret == -ENODATA)
			return ret;
	}

	return ret;
}

static int sdw_transfer_unlocked(struct sdw_bus *bus, struct sdw_msg *msg)
{
	int ret;

	ret = do_transfer(bus, msg);
	if (ret != 0 && ret != -ENODATA)
		dev_err(bus->dev, "trf on Slave %d failed:%d %s addr %x count %d\n",
			msg->dev_num, ret,
			str_write_read(msg->flags & SDW_MSG_FLAG_WRITE),
			msg->addr, msg->len);

	return ret;
}

/**
 * sdw_transfer() - Synchronous transfer message to a SDW Slave device
 * @bus: SDW bus
 * @msg: SDW message to be xfered
 */
int sdw_transfer(struct sdw_bus *bus, struct sdw_msg *msg)
{
	int ret;

	mutex_lock(&bus->msg_lock);

	ret = sdw_transfer_unlocked(bus, msg);

	mutex_unlock(&bus->msg_lock);

	return ret;
}

/**
 * sdw_show_ping_status() - Direct report of PING status, to be used by Peripheral drivers
 * @bus: SDW bus
 * @sync_delay: Delay before reading status
 */
void sdw_show_ping_status(struct sdw_bus *bus, bool sync_delay)
{
	u32 status;

	if (!bus->ops->read_ping_status)
		return;

	/*
	 * wait for peripheral to sync if desired. 10-15ms should be more than
	 * enough in most cases.
	 */
	if (sync_delay)
		usleep_range(10000, 15000);

	mutex_lock(&bus->msg_lock);

	status = bus->ops->read_ping_status(bus);

	mutex_unlock(&bus->msg_lock);

	if (!status)
		dev_warn(bus->dev, "%s: no peripherals attached\n", __func__);
	else
		dev_dbg(bus->dev, "PING status: %#x\n", status);
}
EXPORT_SYMBOL(sdw_show_ping_status);

/**
 * sdw_transfer_defer() - Asynchronously transfer message to a SDW Slave device
 * @bus: SDW bus
 * @msg: SDW message to be xfered
 *
 * Caller needs to hold the msg_lock lock while calling this
 */
int sdw_transfer_defer(struct sdw_bus *bus, struct sdw_msg *msg)
{
	int ret;

	if (!bus->ops->xfer_msg_defer)
		return -ENOTSUPP;

	ret = do_transfer_defer(bus, msg);
	if (ret != 0 && ret != -ENODATA)
		dev_err(bus->dev, "Defer trf on Slave %d failed:%d\n",
			msg->dev_num, ret);

	return ret;
}

int sdw_fill_msg(struct sdw_msg *msg, struct sdw_slave *slave,
		 u32 addr, size_t count, u16 dev_num, u8 flags, u8 *buf)
{
	memset(msg, 0, sizeof(*msg));
	msg->addr = addr; /* addr is 16 bit and truncated here */
	msg->len = count;
	msg->dev_num = dev_num;
	msg->flags = flags;
	msg->buf = buf;

	if (addr < SDW_REG_NO_PAGE) /* no paging area */
		return 0;

	if (addr >= SDW_REG_MAX) { /* illegal addr */
		pr_err("SDW: Invalid address %x passed\n", addr);
		return -EINVAL;
	}

	if (addr < SDW_REG_OPTIONAL_PAGE) { /* 32k but no page */
		if (slave && !slave->prop.paging_support)
			return 0;
		/* no need for else as that will fall-through to paging */
	}

	/* paging mandatory */
	if (dev_num == SDW_ENUM_DEV_NUM || dev_num == SDW_BROADCAST_DEV_NUM) {
		pr_err("SDW: Invalid device for paging :%d\n", dev_num);
		return -EINVAL;
	}

	if (!slave) {
		pr_err("SDW: No slave for paging addr\n");
		return -EINVAL;
	}

	if (!slave->prop.paging_support) {
		dev_err(&slave->dev,
			"address %x needs paging but no support\n", addr);
		return -EINVAL;
	}

	msg->addr_page1 = FIELD_GET(SDW_SCP_ADDRPAGE1_MASK, addr);
	msg->addr_page2 = FIELD_GET(SDW_SCP_ADDRPAGE2_MASK, addr);
	msg->addr |= BIT(15);
	msg->page = true;

	return 0;
}

/*
 * Read/Write IO functions.
 */

static int sdw_ntransfer_no_pm(struct sdw_slave *slave, u32 addr, u8 flags,
			       size_t count, u8 *val)
{
	struct sdw_msg msg;
	size_t size;
	int ret;

	while (count) {
		// Only handle bytes up to next page boundary
		size = min_t(size_t, count, (SDW_REGADDR + 1) - (addr & SDW_REGADDR));

		ret = sdw_fill_msg(&msg, slave, addr, size, slave->dev_num, flags, val);
		if (ret < 0)
			return ret;

		ret = sdw_transfer(slave->bus, &msg);
		if (ret < 0 && !slave->is_mockup_device)
			return ret;

		addr += size;
		val += size;
		count -= size;
	}

	return 0;
}

/**
 * sdw_nread_no_pm() - Read "n" contiguous SDW Slave registers with no PM
 * @slave: SDW Slave
 * @addr: Register address
 * @count: length
 * @val: Buffer for values to be read
 *
 * Note that if the message crosses a page boundary each page will be
 * transferred under a separate invocation of the msg_lock.
 */
int sdw_nread_no_pm(struct sdw_slave *slave, u32 addr, size_t count, u8 *val)
{
	return sdw_ntransfer_no_pm(slave, addr, SDW_MSG_FLAG_READ, count, val);
}
EXPORT_SYMBOL(sdw_nread_no_pm);

/**
 * sdw_nwrite_no_pm() - Write "n" contiguous SDW Slave registers with no PM
 * @slave: SDW Slave
 * @addr: Register address
 * @count: length
 * @val: Buffer for values to be written
 *
 * Note that if the message crosses a page boundary each page will be
 * transferred under a separate invocation of the msg_lock.
 */
int sdw_nwrite_no_pm(struct sdw_slave *slave, u32 addr, size_t count, const u8 *val)
{
	return sdw_ntransfer_no_pm(slave, addr, SDW_MSG_FLAG_WRITE, count, (u8 *)val);
}
EXPORT_SYMBOL(sdw_nwrite_no_pm);

/**
 * sdw_write_no_pm() - Write a SDW Slave register with no PM
 * @slave: SDW Slave
 * @addr: Register address
 * @value: Register value
 */
int sdw_write_no_pm(struct sdw_slave *slave, u32 addr, u8 value)
{
	return sdw_nwrite_no_pm(slave, addr, 1, &value);
}
EXPORT_SYMBOL(sdw_write_no_pm);

static int
sdw_bread_no_pm(struct sdw_bus *bus, u16 dev_num, u32 addr)
{
	struct sdw_msg msg;
	u8 buf;
	int ret;

	ret = sdw_fill_msg(&msg, NULL, addr, 1, dev_num,
			   SDW_MSG_FLAG_READ, &buf);
	if (ret < 0)
		return ret;

	ret = sdw_transfer(bus, &msg);
	if (ret < 0)
		return ret;

	return buf;
}

static int
sdw_bwrite_no_pm(struct sdw_bus *bus, u16 dev_num, u32 addr, u8 value)
{
	struct sdw_msg msg;
	int ret;

	ret = sdw_fill_msg(&msg, NULL, addr, 1, dev_num,
			   SDW_MSG_FLAG_WRITE, &value);
	if (ret < 0)
		return ret;

	return sdw_transfer(bus, &msg);
}

int sdw_bread_no_pm_unlocked(struct sdw_bus *bus, u16 dev_num, u32 addr)
{
	struct sdw_msg msg;
	u8 buf;
	int ret;

	ret = sdw_fill_msg(&msg, NULL, addr, 1, dev_num,
			   SDW_MSG_FLAG_READ, &buf);
	if (ret < 0)
		return ret;

	ret = sdw_transfer_unlocked(bus, &msg);
	if (ret < 0)
		return ret;

	return buf;
}
EXPORT_SYMBOL(sdw_bread_no_pm_unlocked);

int sdw_bwrite_no_pm_unlocked(struct sdw_bus *bus, u16 dev_num, u32 addr, u8 value)
{
	struct sdw_msg msg;
	int ret;

	ret = sdw_fill_msg(&msg, NULL, addr, 1, dev_num,
			   SDW_MSG_FLAG_WRITE, &value);
	if (ret < 0)
		return ret;

	return sdw_transfer_unlocked(bus, &msg);
}
EXPORT_SYMBOL(sdw_bwrite_no_pm_unlocked);

/**
 * sdw_read_no_pm() - Read a SDW Slave register with no PM
 * @slave: SDW Slave
 * @addr: Register address
 */
int sdw_read_no_pm(struct sdw_slave *slave, u32 addr)
{
	u8 buf;
	int ret;

	ret = sdw_nread_no_pm(slave, addr, 1, &buf);
	if (ret < 0)
		return ret;
	else
		return buf;
}
EXPORT_SYMBOL(sdw_read_no_pm);

int sdw_update_no_pm(struct sdw_slave *slave, u32 addr, u8 mask, u8 val)
{
	int tmp;

	tmp = sdw_read_no_pm(slave, addr);
	if (tmp < 0)
		return tmp;

	tmp = (tmp & ~mask) | val;
	return sdw_write_no_pm(slave, addr, tmp);
}
EXPORT_SYMBOL(sdw_update_no_pm);

/* Read-Modify-Write Slave register */
int sdw_update(struct sdw_slave *slave, u32 addr, u8 mask, u8 val)
{
	int tmp;

	tmp = sdw_read(slave, addr);
	if (tmp < 0)
		return tmp;

	tmp = (tmp & ~mask) | val;
	return sdw_write(slave, addr, tmp);
}
EXPORT_SYMBOL(sdw_update);

/**
 * sdw_nread() - Read "n" contiguous SDW Slave registers
 * @slave: SDW Slave
 * @addr: Register address
 * @count: length
 * @val: Buffer for values to be read
 *
 * This version of the function will take a PM reference to the slave
 * device.
 * Note that if the message crosses a page boundary each page will be
 * transferred under a separate invocation of the msg_lock.
 */
int sdw_nread(struct sdw_slave *slave, u32 addr, size_t count, u8 *val)
{
	int ret;

	ret = pm_runtime_get_sync(&slave->dev);
	if (ret < 0 && ret != -EACCES) {
		pm_runtime_put_noidle(&slave->dev);
		return ret;
	}

	ret = sdw_nread_no_pm(slave, addr, count, val);

	pm_runtime_mark_last_busy(&slave->dev);
	pm_runtime_put(&slave->dev);

	return ret;
}
EXPORT_SYMBOL(sdw_nread);

/**
 * sdw_nwrite() - Write "n" contiguous SDW Slave registers
 * @slave: SDW Slave
 * @addr: Register address
 * @count: length
 * @val: Buffer for values to be written
 *
 * This version of the function will take a PM reference to the slave
 * device.
 * Note that if the message crosses a page boundary each page will be
 * transferred under a separate invocation of the msg_lock.
 */
int sdw_nwrite(struct sdw_slave *slave, u32 addr, size_t count, const u8 *val)
{
	int ret;

	ret = pm_runtime_get_sync(&slave->dev);
	if (ret < 0 && ret != -EACCES) {
		pm_runtime_put_noidle(&slave->dev);
		return ret;
	}

	ret = sdw_nwrite_no_pm(slave, addr, count, val);

	pm_runtime_mark_last_busy(&slave->dev);
	pm_runtime_put(&slave->dev);

	return ret;
}
EXPORT_SYMBOL(sdw_nwrite);

/**
 * sdw_read() - Read a SDW Slave register
 * @slave: SDW Slave
 * @addr: Register address
 *
 * This version of the function will take a PM reference to the slave
 * device.
 */
int sdw_read(struct sdw_slave *slave, u32 addr)
{
	u8 buf;
	int ret;

	ret = sdw_nread(slave, addr, 1, &buf);
	if (ret < 0)
		return ret;

	return buf;
}
EXPORT_SYMBOL(sdw_read);

/**
 * sdw_write() - Write a SDW Slave register
 * @slave: SDW Slave
 * @addr: Register address
 * @value: Register value
 *
 * This version of the function will take a PM reference to the slave
 * device.
 */
int sdw_write(struct sdw_slave *slave, u32 addr, u8 value)
{
	return sdw_nwrite(slave, addr, 1, &value);
}
EXPORT_SYMBOL(sdw_write);

/*
 * SDW alert handling
 */

/* called with bus_lock held */
static struct sdw_slave *sdw_get_slave(struct sdw_bus *bus, int i)
{
	struct sdw_slave *slave;

	list_for_each_entry(slave, &bus->slaves, node) {
		if (slave->dev_num == i)
			return slave;
	}

	return NULL;
}

int sdw_compare_devid(struct sdw_slave *slave, struct sdw_slave_id id)
{
	if (slave->id.mfg_id != id.mfg_id ||
	    slave->id.part_id != id.part_id ||
	    slave->id.class_id != id.class_id ||
	    (slave->id.unique_id != SDW_IGNORED_UNIQUE_ID &&
	     slave->id.unique_id != id.unique_id))
		return -ENODEV;

	return 0;
}
EXPORT_SYMBOL(sdw_compare_devid);

/* called with bus_lock held */
static int sdw_get_device_num(struct sdw_slave *slave)
{
	struct sdw_bus *bus = slave->bus;
	int bit;

	if (bus->ops && bus->ops->get_device_num) {
		bit = bus->ops->get_device_num(bus, slave);
		if (bit < 0)
			goto err;
	} else {
		bit = find_first_zero_bit(bus->assigned, SDW_MAX_DEVICES);
		if (bit == SDW_MAX_DEVICES) {
			bit = -ENODEV;
			goto err;
		}
	}

	/*
	 * Do not update dev_num in Slave data structure here,
	 * Update once program dev_num is successful
	 */
	set_bit(bit, bus->assigned);

err:
	return bit;
}

static int sdw_assign_device_num(struct sdw_slave *slave)
{
	struct sdw_bus *bus = slave->bus;
	struct device *dev = bus->dev;
	int ret;

	/* check first if device number is assigned, if so reuse that */
	if (!slave->dev_num) {
		if (!slave->dev_num_sticky) {
			int dev_num;

			mutex_lock(&slave->bus->bus_lock);
			dev_num = sdw_get_device_num(slave);
			mutex_unlock(&slave->bus->bus_lock);
			if (dev_num < 0) {
				dev_err(dev, "Get dev_num failed: %d\n", dev_num);
				return dev_num;
			}

			slave->dev_num_sticky = dev_num;
		} else {
			dev_dbg(dev, "Slave already registered, reusing dev_num: %d\n",
				slave->dev_num_sticky);
		}
	}

	/* Clear the slave->dev_num to transfer message on device 0 */
	slave->dev_num = 0;

	ret = sdw_write_no_pm(slave, SDW_SCP_DEVNUMBER, slave->dev_num_sticky);
	if (ret < 0) {
		dev_err(dev, "Program device_num %d failed: %d\n",
			slave->dev_num_sticky, ret);
		return ret;
	}

	/* After xfer of msg, restore dev_num */
	slave->dev_num = slave->dev_num_sticky;

	if (bus->ops && bus->ops->new_peripheral_assigned)
		bus->ops->new_peripheral_assigned(bus, slave, slave->dev_num);

	return 0;
}

void sdw_extract_slave_id(struct sdw_bus *bus,
			  u64 addr, struct sdw_slave_id *id)
{
	dev_dbg(bus->dev, "SDW Slave Addr: %llx\n", addr);

	id->sdw_version = SDW_VERSION(addr);
	id->unique_id = SDW_UNIQUE_ID(addr);
	id->mfg_id = SDW_MFG_ID(addr);
	id->part_id = SDW_PART_ID(addr);
	id->class_id = SDW_CLASS_ID(addr);

	dev_dbg(bus->dev,
		"SDW Slave class_id 0x%02x, mfg_id 0x%04x, part_id 0x%04x, unique_id 0x%x, version 0x%x\n",
		id->class_id, id->mfg_id, id->part_id, id->unique_id, id->sdw_version);
}
EXPORT_SYMBOL(sdw_extract_slave_id);

bool is_clock_scaling_supported_by_slave(struct sdw_slave *slave)
{
	/*
	 * Dynamic scaling is a defined by SDCA. However, some devices expose the class ID but
	 * can't support dynamic scaling. We might need a quirk to handle such devices.
	 */
	return slave->id.class_id;
}
EXPORT_SYMBOL(is_clock_scaling_supported_by_slave);

static int sdw_program_device_num(struct sdw_bus *bus, bool *programmed)
{
	u8 buf[SDW_NUM_DEV_ID_REGISTERS] = {0};
	struct sdw_slave *slave, *_s;
	struct sdw_slave_id id;
	struct sdw_msg msg;
	bool found;
	int count = 0, ret;
	u64 addr;

	*programmed = false;

	/* No Slave, so use raw xfer api */
	ret = sdw_fill_msg(&msg, NULL, SDW_SCP_DEVID_0,
			   SDW_NUM_DEV_ID_REGISTERS, 0, SDW_MSG_FLAG_READ, buf);
	if (ret < 0)
		return ret;

	do {
		ret = sdw_transfer(bus, &msg);
		if (ret == -ENODATA) { /* end of device id reads */
			dev_dbg(bus->dev, "No more devices to enumerate\n");
			ret = 0;
			break;
		}
		if (ret < 0) {
			dev_err(bus->dev, "DEVID read fail:%d\n", ret);
			break;
		}

		/*
		 * Construct the addr and extract. Cast the higher shift
		 * bits to avoid truncation due to size limit.
		 */
		addr = buf[5] | (buf[4] << 8) | (buf[3] << 16) |
			((u64)buf[2] << 24) | ((u64)buf[1] << 32) |
			((u64)buf[0] << 40);

		sdw_extract_slave_id(bus, addr, &id);

		found = false;
		/* Now compare with entries */
		list_for_each_entry_safe(slave, _s, &bus->slaves, node) {
			if (sdw_compare_devid(slave, id) == 0) {
				found = true;

				/*
				 * To prevent skipping state-machine stages don't
				 * program a device until we've seen it UNATTACH.
				 * Must return here because no other device on #0
				 * can be detected until this one has been
				 * assigned a device ID.
				 */
				if (slave->status != SDW_SLAVE_UNATTACHED)
					return 0;

				/*
				 * Assign a new dev_num to this Slave and
				 * not mark it present. It will be marked
				 * present after it reports ATTACHED on new
				 * dev_num
				 */
				ret = sdw_assign_device_num(slave);
				if (ret < 0) {
					dev_err(bus->dev,
						"Assign dev_num failed:%d\n",
						ret);
					return ret;
				}

				*programmed = true;

				break;
			}
		}

		if (!found) {
			/* TODO: Park this device in Group 13 */

			/*
			 * add Slave device even if there is no platform
			 * firmware description. There will be no driver probe
			 * but the user/integration will be able to see the
			 * device, enumeration status and device number in sysfs
			 */
			sdw_slave_add(bus, &id, NULL);

			dev_err(bus->dev, "Slave Entry not found\n");
		}

		count++;

		/*
		 * Check till error out or retry (count) exhausts.
		 * Device can drop off and rejoin during enumeration
		 * so count till twice the bound.
		 */

	} while (ret == 0 && count < (SDW_MAX_DEVICES * 2));

	return ret;
}

static void sdw_modify_slave_status(struct sdw_slave *slave,
				    enum sdw_slave_status status)
{
	struct sdw_bus *bus = slave->bus;

	mutex_lock(&bus->bus_lock);

	dev_vdbg(bus->dev,
		 "changing status slave %d status %d new status %d\n",
		 slave->dev_num, slave->status, status);

	if (status == SDW_SLAVE_UNATTACHED) {
		dev_dbg(&slave->dev,
			"initializing enumeration and init completion for Slave %d\n",
			slave->dev_num);

		reinit_completion(&slave->enumeration_complete);
		reinit_completion(&slave->initialization_complete);

	} else if ((status == SDW_SLAVE_ATTACHED) &&
		   (slave->status == SDW_SLAVE_UNATTACHED)) {
		dev_dbg(&slave->dev,
			"signaling enumeration completion for Slave %d\n",
			slave->dev_num);

		complete_all(&slave->enumeration_complete);
	}
	slave->status = status;
	mutex_unlock(&bus->bus_lock);
}

static int sdw_slave_clk_stop_callback(struct sdw_slave *slave,
				       enum sdw_clk_stop_mode mode,
				       enum sdw_clk_stop_type type)
{
	int ret = 0;

	mutex_lock(&slave->sdw_dev_lock);

	if (slave->probed)  {
		struct device *dev = &slave->dev;
		struct sdw_driver *drv = drv_to_sdw_driver(dev->driver);

		if (drv->ops && drv->ops->clk_stop)
			ret = drv->ops->clk_stop(slave, mode, type);
	}

	mutex_unlock(&slave->sdw_dev_lock);

	return ret;
}

static int sdw_slave_clk_stop_prepare(struct sdw_slave *slave,
				      enum sdw_clk_stop_mode mode,
				      bool prepare)
{
	bool wake_en;
	u32 val = 0;
	int ret;

	wake_en = slave->prop.wake_capable;

	if (prepare) {
		val = SDW_SCP_SYSTEMCTRL_CLK_STP_PREP;

		if (mode == SDW_CLK_STOP_MODE1)
			val |= SDW_SCP_SYSTEMCTRL_CLK_STP_MODE1;

		if (wake_en)
			val |= SDW_SCP_SYSTEMCTRL_WAKE_UP_EN;
	} else {
		ret = sdw_read_no_pm(slave, SDW_SCP_SYSTEMCTRL);
		if (ret < 0) {
			if (ret != -ENODATA)
				dev_err(&slave->dev, "SDW_SCP_SYSTEMCTRL read failed:%d\n", ret);
			return ret;
		}
		val = ret;
		val &= ~(SDW_SCP_SYSTEMCTRL_CLK_STP_PREP);
	}

	ret = sdw_write_no_pm(slave, SDW_SCP_SYSTEMCTRL, val);

	if (ret < 0 && ret != -ENODATA)
		dev_err(&slave->dev, "SDW_SCP_SYSTEMCTRL write failed:%d\n", ret);

	return ret;
}

static int sdw_bus_wait_for_clk_prep_deprep(struct sdw_bus *bus, u16 dev_num, bool prepare)
{
	int retry = bus->clk_stop_timeout;
	int val;

	do {
		val = sdw_bread_no_pm(bus, dev_num, SDW_SCP_STAT);
		if (val < 0) {
			if (val != -ENODATA)
				dev_err(bus->dev, "SDW_SCP_STAT bread failed:%d\n", val);
			return val;
		}
		val &= SDW_SCP_STAT_CLK_STP_NF;
		if (!val) {
			dev_dbg(bus->dev, "clock stop %s done slave:%d\n",
				prepare ? "prepare" : "deprepare",
				dev_num);
			return 0;
		}

		usleep_range(1000, 1500);
		retry--;
	} while (retry);

	dev_dbg(bus->dev, "clock stop %s did not complete for slave:%d\n",
		prepare ? "prepare" : "deprepare",
		dev_num);

	return -ETIMEDOUT;
}

/**
 * sdw_bus_prep_clk_stop: prepare Slave(s) for clock stop
 *
 * @bus: SDW bus instance
 *
 * Query Slave for clock stop mode and prepare for that mode.
 */
int sdw_bus_prep_clk_stop(struct sdw_bus *bus)
{
	bool simple_clk_stop = true;
	struct sdw_slave *slave;
	bool is_slave = false;
	int ret = 0;

	/*
	 * In order to save on transition time, prepare
	 * each Slave and then wait for all Slave(s) to be
	 * prepared for clock stop.
	 * If one of the Slave devices has lost sync and
	 * replies with Command Ignored/-ENODATA, we continue
	 * the loop
	 */
	list_for_each_entry(slave, &bus->slaves, node) {
		if (!slave->dev_num)
			continue;

		if (slave->status != SDW_SLAVE_ATTACHED &&
		    slave->status != SDW_SLAVE_ALERT)
			continue;

		/* Identify if Slave(s) are available on Bus */
		is_slave = true;

		ret = sdw_slave_clk_stop_callback(slave,
						  SDW_CLK_STOP_MODE0,
						  SDW_CLK_PRE_PREPARE);
		if (ret < 0 && ret != -ENODATA) {
			dev_err(&slave->dev, "clock stop pre-prepare cb failed:%d\n", ret);
			return ret;
		}

		/* Only prepare a Slave device if needed */
		if (!slave->prop.simple_clk_stop_capable) {
			simple_clk_stop = false;

			ret = sdw_slave_clk_stop_prepare(slave,
							 SDW_CLK_STOP_MODE0,
							 true);
			if (ret < 0 && ret != -ENODATA) {
				dev_err(&slave->dev, "clock stop prepare failed:%d\n", ret);
				return ret;
			}
		}
	}

	/* Skip remaining clock stop preparation if no Slave is attached */
	if (!is_slave)
		return 0;

	/*
	 * Don't wait for all Slaves to be ready if they follow the simple
	 * state machine
	 */
	if (!simple_clk_stop) {
		ret = sdw_bus_wait_for_clk_prep_deprep(bus,
						       SDW_BROADCAST_DEV_NUM, true);
		/*
		 * if there are no Slave devices present and the reply is
		 * Command_Ignored/-ENODATA, we don't need to continue with the
		 * flow and can just return here. The error code is not modified
		 * and its handling left as an exercise for the caller.
		 */
		if (ret < 0)
			return ret;
	}

	/* Inform slaves that prep is done */
	list_for_each_entry(slave, &bus->slaves, node) {
		if (!slave->dev_num)
			continue;

		if (slave->status != SDW_SLAVE_ATTACHED &&
		    slave->status != SDW_SLAVE_ALERT)
			continue;

		ret = sdw_slave_clk_stop_callback(slave,
						  SDW_CLK_STOP_MODE0,
						  SDW_CLK_POST_PREPARE);

		if (ret < 0 && ret != -ENODATA) {
			dev_err(&slave->dev, "clock stop post-prepare cb failed:%d\n", ret);
			return ret;
		}
	}

	return 0;
}
EXPORT_SYMBOL(sdw_bus_prep_clk_stop);

/**
 * sdw_bus_clk_stop: stop bus clock
 *
 * @bus: SDW bus instance
 *
 * After preparing the Slaves for clock stop, stop the clock by broadcasting
 * write to SCP_CTRL register.
 */
int sdw_bus_clk_stop(struct sdw_bus *bus)
{
	int ret;

	/*
	 * broadcast clock stop now, attached Slaves will ACK this,
	 * unattached will ignore
	 */
	ret = sdw_bwrite_no_pm(bus, SDW_BROADCAST_DEV_NUM,
			       SDW_SCP_CTRL, SDW_SCP_CTRL_CLK_STP_NOW);
	if (ret < 0) {
		if (ret != -ENODATA)
			dev_err(bus->dev, "ClockStopNow Broadcast msg failed %d\n", ret);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL(sdw_bus_clk_stop);

/**
 * sdw_bus_exit_clk_stop: Exit clock stop mode
 *
 * @bus: SDW bus instance
 *
 * This De-prepares the Slaves by exiting Clock Stop Mode 0. For the Slaves
 * exiting Clock Stop Mode 1, they will be de-prepared after they enumerate
 * back.
 */
int sdw_bus_exit_clk_stop(struct sdw_bus *bus)
{
	bool simple_clk_stop = true;
	struct sdw_slave *slave;
	bool is_slave = false;
	int ret;

	/*
	 * In order to save on transition time, de-prepare
	 * each Slave and then wait for all Slave(s) to be
	 * de-prepared after clock resume.
	 */
	list_for_each_entry(slave, &bus->slaves, node) {
		if (!slave->dev_num)
			continue;

		if (slave->status != SDW_SLAVE_ATTACHED &&
		    slave->status != SDW_SLAVE_ALERT)
			continue;

		/* Identify if Slave(s) are available on Bus */
		is_slave = true;

		ret = sdw_slave_clk_stop_callback(slave, SDW_CLK_STOP_MODE0,
						  SDW_CLK_PRE_DEPREPARE);
		if (ret < 0)
			dev_warn(&slave->dev, "clock stop pre-deprepare cb failed:%d\n", ret);

		/* Only de-prepare a Slave device if needed */
		if (!slave->prop.simple_clk_stop_capable) {
			simple_clk_stop = false;

			ret = sdw_slave_clk_stop_prepare(slave, SDW_CLK_STOP_MODE0,
							 false);

			if (ret < 0)
				dev_warn(&slave->dev, "clock stop deprepare failed:%d\n", ret);
		}
	}

	/* Skip remaining clock stop de-preparation if no Slave is attached */
	if (!is_slave)
		return 0;

	/*
	 * Don't wait for all Slaves to be ready if they follow the simple
	 * state machine
	 */
	if (!simple_clk_stop) {
		ret = sdw_bus_wait_for_clk_prep_deprep(bus, SDW_BROADCAST_DEV_NUM, false);
		if (ret < 0)
			dev_warn(bus->dev, "clock stop deprepare wait failed:%d\n", ret);
	}

	list_for_each_entry(slave, &bus->slaves, node) {
		if (!slave->dev_num)
			continue;

		if (slave->status != SDW_SLAVE_ATTACHED &&
		    slave->status != SDW_SLAVE_ALERT)
			continue;

		ret = sdw_slave_clk_stop_callback(slave, SDW_CLK_STOP_MODE0,
						  SDW_CLK_POST_DEPREPARE);
		if (ret < 0)
			dev_warn(&slave->dev, "clock stop post-deprepare cb failed:%d\n", ret);
	}

	return 0;
}
EXPORT_SYMBOL(sdw_bus_exit_clk_stop);

int sdw_configure_dpn_intr(struct sdw_slave *slave,
			   int port, bool enable, int mask)
{
	u32 addr;
	int ret;
	u8 val = 0;

	if (slave->bus->params.s_data_mode != SDW_PORT_DATA_MODE_NORMAL) {
		dev_dbg(&slave->dev, "TEST FAIL interrupt %s\n",
			str_on_off(enable));
		mask |= SDW_DPN_INT_TEST_FAIL;
	}

	addr = SDW_DPN_INTMASK(port);

	/* Set/Clear port ready interrupt mask */
	if (enable) {
		val |= mask;
		val |= SDW_DPN_INT_PORT_READY;
	} else {
		val &= ~(mask);
		val &= ~SDW_DPN_INT_PORT_READY;
	}

	ret = sdw_update_no_pm(slave, addr, (mask | SDW_DPN_INT_PORT_READY), val);
	if (ret < 0)
		dev_err(&slave->dev,
			"SDW_DPN_INTMASK write failed:%d\n", val);

	return ret;
}

int sdw_slave_get_scale_index(struct sdw_slave *slave, u8 *base)
{
	u32 mclk_freq = slave->bus->prop.mclk_freq;
	u32 curr_freq = slave->bus->params.curr_dr_freq >> 1;
	unsigned int scale;
	u8 scale_index;

	if (!mclk_freq) {
		dev_err(&slave->dev,
			"no bus MCLK, cannot set SDW_SCP_BUS_CLOCK_BASE\n");
		return -EINVAL;
	}

	/*
	 * map base frequency using Table 89 of SoundWire 1.2 spec.
	 * The order of the tests just follows the specification, this
	 * is not a selection between possible values or a search for
	 * the best value but just a mapping.  Only one case per platform
	 * is relevant.
	 * Some BIOS have inconsistent values for mclk_freq but a
	 * correct root so we force the mclk_freq to avoid variations.
	 */
	if (!(19200000 % mclk_freq)) {
		mclk_freq = 19200000;
		*base = SDW_SCP_BASE_CLOCK_19200000_HZ;
	} else if (!(22579200 % mclk_freq)) {
		mclk_freq = 22579200;
		*base = SDW_SCP_BASE_CLOCK_22579200_HZ;
	} else if (!(24576000 % mclk_freq)) {
		mclk_freq = 24576000;
		*base = SDW_SCP_BASE_CLOCK_24576000_HZ;
	} else if (!(32000000 % mclk_freq)) {
		mclk_freq = 32000000;
		*base = SDW_SCP_BASE_CLOCK_32000000_HZ;
	} else if (!(96000000 % mclk_freq)) {
		mclk_freq = 24000000;
		*base = SDW_SCP_BASE_CLOCK_24000000_HZ;
	} else {
		dev_err(&slave->dev,
			"Unsupported clock base, mclk %d\n",
			mclk_freq);
		return -EINVAL;
	}

	if (mclk_freq % curr_freq) {
		dev_err(&slave->dev,
			"mclk %d is not multiple of bus curr_freq %d\n",
			mclk_freq, curr_freq);
		return -EINVAL;
	}

	scale = mclk_freq / curr_freq;

	/*
	 * map scale to Table 90 of SoundWire 1.2 spec - and check
	 * that the scale is a power of two and maximum 64
	 */
	scale_index = ilog2(scale);

	if (BIT(scale_index) != scale || scale_index > 6) {
		dev_err(&slave->dev,
			"No match found for scale %d, bus mclk %d curr_freq %d\n",
			scale, mclk_freq, curr_freq);
		return -EINVAL;
	}
	scale_index++;

	dev_dbg(&slave->dev,
		"Configured bus base %d, scale %d, mclk %d, curr_freq %d\n",
		*base, scale_index, mclk_freq, curr_freq);

	return scale_index;
}
EXPORT_SYMBOL(sdw_slave_get_scale_index);

static int sdw_slave_set_frequency(struct sdw_slave *slave)
{
	int scale_index;
	u8 base;
	int ret;

	/*
	 * frequency base and scale registers are required for SDCA
	 * devices. They may also be used for 1.2+/non-SDCA devices.
	 * Driver can set the property directly, for now there's no
	 * DisCo property to discover support for the scaling registers
	 * from platform firmware.
	 */
	if (!slave->id.class_id && !slave->prop.clock_reg_supported)
		return 0;

	scale_index = sdw_slave_get_scale_index(slave, &base);
	if (scale_index < 0)
		return scale_index;

	ret = sdw_write_no_pm(slave, SDW_SCP_BUS_CLOCK_BASE, base);
	if (ret < 0) {
		dev_err(&slave->dev,
			"SDW_SCP_BUS_CLOCK_BASE write failed:%d\n", ret);
		return ret;
	}

	/* initialize scale for both banks */
	ret = sdw_write_no_pm(slave, SDW_SCP_BUSCLOCK_SCALE_B0, scale_index);
	if (ret < 0) {
		dev_err(&slave->dev,
			"SDW_SCP_BUSCLOCK_SCALE_B0 write failed:%d\n", ret);
		return ret;
	}
	ret = sdw_write_no_pm(slave, SDW_SCP_BUSCLOCK_SCALE_B1, scale_index);
	if (ret < 0)
		dev_err(&slave->dev,
			"SDW_SCP_BUSCLOCK_SCALE_B1 write failed:%d\n", ret);

	return ret;
}

static int sdw_initialize_slave(struct sdw_slave *slave)
{
	struct sdw_slave_prop *prop = &slave->prop;
	int status;
	int ret;
	u8 val;

	ret = sdw_slave_set_frequency(slave);
	if (ret < 0)
		return ret;

	if (slave->bus->prop.quirks & SDW_MASTER_QUIRKS_CLEAR_INITIAL_CLASH) {
		/* Clear bus clash interrupt before enabling interrupt mask */
		status = sdw_read_no_pm(slave, SDW_SCP_INT1);
		if (status < 0) {
			dev_err(&slave->dev,
				"SDW_SCP_INT1 (BUS_CLASH) read failed:%d\n", status);
			return status;
		}
		if (status & SDW_SCP_INT1_BUS_CLASH) {
			dev_warn(&slave->dev, "Bus clash detected before INT mask is enabled\n");
			ret = sdw_write_no_pm(slave, SDW_SCP_INT1, SDW_SCP_INT1_BUS_CLASH);
			if (ret < 0) {
				dev_err(&slave->dev,
					"SDW_SCP_INT1 (BUS_CLASH) write failed:%d\n", ret);
				return ret;
			}
		}
	}
	if ((slave->bus->prop.quirks & SDW_MASTER_QUIRKS_CLEAR_INITIAL_PARITY) &&
	    !(prop->quirks & SDW_SLAVE_QUIRKS_INVALID_INITIAL_PARITY)) {
		/* Clear parity interrupt before enabling interrupt mask */
		status = sdw_read_no_pm(slave, SDW_SCP_INT1);
		if (status < 0) {
			dev_err(&slave->dev,
				"SDW_SCP_INT1 (PARITY) read failed:%d\n", status);
			return status;
		}
		if (status & SDW_SCP_INT1_PARITY) {
			dev_warn(&slave->dev, "PARITY error detected before INT mask is enabled\n");
			ret = sdw_write_no_pm(slave, SDW_SCP_INT1, SDW_SCP_INT1_PARITY);
			if (ret < 0) {
				dev_err(&slave->dev,
					"SDW_SCP_INT1 (PARITY) write failed:%d\n", ret);
				return ret;
			}
		}
	}

	/*
	 * Set SCP_INT1_MASK register, typically bus clash and
	 * implementation-defined interrupt mask. The Parity detection
	 * may not always be correct on startup so its use is
	 * device-dependent, it might e.g. only be enabled in
	 * steady-state after a couple of frames.
	 */
	val = prop->scp_int1_mask;

	/* Enable SCP interrupts */
	ret = sdw_update_no_pm(slave, SDW_SCP_INTMASK1, val, val);
	if (ret < 0) {
		dev_err(&slave->dev,
			"SDW_SCP_INTMASK1 write failed:%d\n", ret);
		return ret;
	}

	/* No need to continue if DP0 is not present */
	if (!prop->dp0_prop)
		return 0;

	/* Enable DP0 interrupts */
	val = prop->dp0_prop->imp_def_interrupts;
	val |= SDW_DP0_INT_PORT_READY | SDW_DP0_INT_BRA_FAILURE;

	ret = sdw_update_no_pm(slave, SDW_DP0_INTMASK, val, val);
	if (ret < 0)
		dev_err(&slave->dev,
			"SDW_DP0_INTMASK read failed:%d\n", ret);
	return ret;
}

static int sdw_handle_dp0_interrupt(struct sdw_slave *slave, u8 *slave_status)
{
	u8 clear, impl_int_mask;
	int status, status2, ret, count = 0;

	status = sdw_read_no_pm(slave, SDW_DP0_INT);
	if (status < 0) {
		dev_err(&slave->dev,
			"SDW_DP0_INT read failed:%d\n", status);
		return status;
	}

	do {
		clear = status & ~(SDW_DP0_INTERRUPTS | SDW_DP0_SDCA_CASCADE);

		if (status & SDW_DP0_INT_TEST_FAIL) {
			dev_err(&slave->dev, "Test fail for port 0\n");
			clear |= SDW_DP0_INT_TEST_FAIL;
		}

		/*
		 * Assumption: PORT_READY interrupt will be received only for
		 * ports implementing Channel Prepare state machine (CP_SM)
		 */

		if (status & SDW_DP0_INT_PORT_READY) {
			complete(&slave->port_ready[0]);
			clear |= SDW_DP0_INT_PORT_READY;
		}

		if (status & SDW_DP0_INT_BRA_FAILURE) {
			dev_err(&slave->dev, "BRA failed\n");
			clear |= SDW_DP0_INT_BRA_FAILURE;
		}

		impl_int_mask = SDW_DP0_INT_IMPDEF1 |
			SDW_DP0_INT_IMPDEF2 | SDW_DP0_INT_IMPDEF3;

		if (status & impl_int_mask) {
			clear |= impl_int_mask;
			*slave_status = clear;
		}

		/* clear the interrupts but don't touch reserved and SDCA_CASCADE fields */
		ret = sdw_write_no_pm(slave, SDW_DP0_INT, clear);
		if (ret < 0) {
			dev_err(&slave->dev,
				"SDW_DP0_INT write failed:%d\n", ret);
			return ret;
		}

		/* Read DP0 interrupt again */
		status2 = sdw_read_no_pm(slave, SDW_DP0_INT);
		if (status2 < 0) {
			dev_err(&slave->dev,
				"SDW_DP0_INT read failed:%d\n", status2);
			return status2;
		}
		/* filter to limit loop to interrupts identified in the first status read */
		status &= status2;

		count++;

		/* we can get alerts while processing so keep retrying */
	} while ((status & SDW_DP0_INTERRUPTS) && (count < SDW_READ_INTR_CLEAR_RETRY));

	if (count == SDW_READ_INTR_CLEAR_RETRY)
		dev_warn(&slave->dev, "Reached MAX_RETRY on DP0 read\n");

	return ret;
}

static int sdw_handle_port_interrupt(struct sdw_slave *slave,
				     int port, u8 *slave_status)
{
	u8 clear, impl_int_mask;
	int status, status2, ret, count = 0;
	u32 addr;

	if (port == 0)
		return sdw_handle_dp0_interrupt(slave, slave_status);

	addr = SDW_DPN_INT(port);
	status = sdw_read_no_pm(slave, addr);
	if (status < 0) {
		dev_err(&slave->dev,
			"SDW_DPN_INT read failed:%d\n", status);

		return status;
	}

	do {
		clear = status & ~SDW_DPN_INTERRUPTS;

		if (status & SDW_DPN_INT_TEST_FAIL) {
			dev_err(&slave->dev, "Test fail for port:%d\n", port);
			clear |= SDW_DPN_INT_TEST_FAIL;
		}

		/*
		 * Assumption: PORT_READY interrupt will be received only
		 * for ports implementing CP_SM.
		 */
		if (status & SDW_DPN_INT_PORT_READY) {
			complete(&slave->port_ready[port]);
			clear |= SDW_DPN_INT_PORT_READY;
		}

		impl_int_mask = SDW_DPN_INT_IMPDEF1 |
			SDW_DPN_INT_IMPDEF2 | SDW_DPN_INT_IMPDEF3;

		if (status & impl_int_mask) {
			clear |= impl_int_mask;
			*slave_status = clear;
		}

		/* clear the interrupt but don't touch reserved fields */
		ret = sdw_write_no_pm(slave, addr, clear);
		if (ret < 0) {
			dev_err(&slave->dev,
				"SDW_DPN_INT write failed:%d\n", ret);
			return ret;
		}

		/* Read DPN interrupt again */
		status2 = sdw_read_no_pm(slave, addr);
		if (status2 < 0) {
			dev_err(&slave->dev,
				"SDW_DPN_INT read failed:%d\n", status2);
			return status2;
		}
		/* filter to limit loop to interrupts identified in the first status read */
		status &= status2;

		count++;

		/* we can get alerts while processing so keep retrying */
	} while ((status & SDW_DPN_INTERRUPTS) && (count < SDW_READ_INTR_CLEAR_RETRY));

	if (count == SDW_READ_INTR_CLEAR_RETRY)
		dev_warn(&slave->dev, "Reached MAX_RETRY on port read");

	return ret;
}

static int sdw_handle_slave_alerts(struct sdw_slave *slave)
{
	struct sdw_slave_intr_status slave_intr;
	u8 clear = 0, bit, port_status[15] = {0};
	int port_num, stat, ret, count = 0;
	unsigned long port;
	bool slave_notify;
	u8 sdca_cascade = 0;
	u8 buf, buf2[2];
	bool parity_check;
	bool parity_quirk;

	sdw_modify_slave_status(slave, SDW_SLAVE_ALERT);

	ret = pm_runtime_get_sync(&slave->dev);
	if (ret < 0 && ret != -EACCES) {
		dev_err(&slave->dev, "Failed to resume device: %d\n", ret);
		pm_runtime_put_noidle(&slave->dev);
		return ret;
	}

	/* Read Intstat 1, Intstat 2 and Intstat 3 registers */
	ret = sdw_read_no_pm(slave, SDW_SCP_INT1);
	if (ret < 0) {
		dev_err(&slave->dev,
			"SDW_SCP_INT1 read failed:%d\n", ret);
		goto io_err;
	}
	buf = ret;

	ret = sdw_nread_no_pm(slave, SDW_SCP_INTSTAT2, 2, buf2);
	if (ret < 0) {
		dev_err(&slave->dev,
			"SDW_SCP_INT2/3 read failed:%d\n", ret);
		goto io_err;
	}

	if (slave->id.class_id) {
		ret = sdw_read_no_pm(slave, SDW_DP0_INT);
		if (ret < 0) {
			dev_err(&slave->dev,
				"SDW_DP0_INT read failed:%d\n", ret);
			goto io_err;
		}
		sdca_cascade = ret & SDW_DP0_SDCA_CASCADE;
	}

	do {
		slave_notify = false;

		/*
		 * Check parity, bus clash and Slave (impl defined)
		 * interrupt
		 */
		if (buf & SDW_SCP_INT1_PARITY) {
			parity_check = slave->prop.scp_int1_mask & SDW_SCP_INT1_PARITY;
			parity_quirk = !slave->first_interrupt_done &&
				(slave->prop.quirks & SDW_SLAVE_QUIRKS_INVALID_INITIAL_PARITY);

			if (parity_check && !parity_quirk)
				dev_err(&slave->dev, "Parity error detected\n");
			clear |= SDW_SCP_INT1_PARITY;
		}

		if (buf & SDW_SCP_INT1_BUS_CLASH) {
			if (slave->prop.scp_int1_mask & SDW_SCP_INT1_BUS_CLASH)
				dev_err(&slave->dev, "Bus clash detected\n");
			clear |= SDW_SCP_INT1_BUS_CLASH;
		}

		/*
		 * When bus clash or parity errors are detected, such errors
		 * are unlikely to be recoverable errors.
		 * TODO: In such scenario, reset bus. Make this configurable
		 * via sysfs property with bus reset being the default.
		 */

		if (buf & SDW_SCP_INT1_IMPL_DEF) {
			if (slave->prop.scp_int1_mask & SDW_SCP_INT1_IMPL_DEF) {
				dev_dbg(&slave->dev, "Slave impl defined interrupt\n");
				slave_notify = true;
			}
			clear |= SDW_SCP_INT1_IMPL_DEF;
		}

		/* the SDCA interrupts are cleared in the codec driver .interrupt_callback() */
		if (sdca_cascade)
			slave_notify = true;

		/* Check port 0 - 3 interrupts */
		port = buf & SDW_SCP_INT1_PORT0_3;

		/* To get port number corresponding to bits, shift it */
		port = FIELD_GET(SDW_SCP_INT1_PORT0_3, port);
		for_each_set_bit(bit, &port, 8) {
			sdw_handle_port_interrupt(slave, bit,
						  &port_status[bit]);
		}

		/* Check if cascade 2 interrupt is present */
		if (buf & SDW_SCP_INT1_SCP2_CASCADE) {
			port = buf2[0] & SDW_SCP_INTSTAT2_PORT4_10;
			for_each_set_bit(bit, &port, 8) {
				/* scp2 ports start from 4 */
				port_num = bit + 4;
				sdw_handle_port_interrupt(slave,
						port_num,
						&port_status[port_num]);
			}
		}

		/* now check last cascade */
		if (buf2[0] & SDW_SCP_INTSTAT2_SCP3_CASCADE) {
			port = buf2[1] & SDW_SCP_INTSTAT3_PORT11_14;
			for_each_set_bit(bit, &port, 8) {
				/* scp3 ports start from 11 */
				port_num = bit + 11;
				sdw_handle_port_interrupt(slave,
						port_num,
						&port_status[port_num]);
			}
		}

		/* Update the Slave driver */
		if (slave_notify) {
			if (slave->prop.use_domain_irq && slave->irq)
				handle_nested_irq(slave->irq);

			mutex_lock(&slave->sdw_dev_lock);

			if (slave->probed) {
				struct device *dev = &slave->dev;
				struct sdw_driver *drv = drv_to_sdw_driver(dev->driver);

				if (drv->ops && drv->ops->interrupt_callback) {
					slave_intr.sdca_cascade = sdca_cascade;
					slave_intr.control_port = clear;
					memcpy(slave_intr.port, &port_status,
					       sizeof(slave_intr.port));

					drv->ops->interrupt_callback(slave, &slave_intr);
				}
			}

			mutex_unlock(&slave->sdw_dev_lock);
		}

		/* Ack interrupt */
		ret = sdw_write_no_pm(slave, SDW_SCP_INT1, clear);
		if (ret < 0) {
			dev_err(&slave->dev,
				"SDW_SCP_INT1 write failed:%d\n", ret);
			goto io_err;
		}

		/* at this point all initial interrupt sources were handled */
		slave->first_interrupt_done = true;

		/*
		 * Read status again to ensure no new interrupts arrived
		 * while servicing interrupts.
		 */
		ret = sdw_read_no_pm(slave, SDW_SCP_INT1);
		if (ret < 0) {
			dev_err(&slave->dev,
				"SDW_SCP_INT1 recheck read failed:%d\n", ret);
			goto io_err;
		}
		buf = ret;

		ret = sdw_nread_no_pm(slave, SDW_SCP_INTSTAT2, 2, buf2);
		if (ret < 0) {
			dev_err(&slave->dev,
				"SDW_SCP_INT2/3 recheck read failed:%d\n", ret);
			goto io_err;
		}

		if (slave->id.class_id) {
			ret = sdw_read_no_pm(slave, SDW_DP0_INT);
			if (ret < 0) {
				dev_err(&slave->dev,
					"SDW_DP0_INT recheck read failed:%d\n", ret);
				goto io_err;
			}
			sdca_cascade = ret & SDW_DP0_SDCA_CASCADE;
		}

		/*
		 * Make sure no interrupts are pending
		 */
		stat = buf || buf2[0] || buf2[1] || sdca_cascade;

		/*
		 * Exit loop if Slave is continuously in ALERT state even
		 * after servicing the interrupt multiple times.
		 */
		count++;

		/* we can get alerts while processing so keep retrying */
	} while (stat != 0 && count < SDW_READ_INTR_CLEAR_RETRY);

	if (count == SDW_READ_INTR_CLEAR_RETRY)
		dev_warn(&slave->dev, "Reached MAX_RETRY on alert read\n");

io_err:
	pm_runtime_mark_last_busy(&slave->dev);
	pm_runtime_put_autosuspend(&slave->dev);

	return ret;
}

static int sdw_update_slave_status(struct sdw_slave *slave,
				   enum sdw_slave_status status)
{
	int ret = 0;

	mutex_lock(&slave->sdw_dev_lock);

	if (slave->probed) {
		struct device *dev = &slave->dev;
		struct sdw_driver *drv = drv_to_sdw_driver(dev->driver);

		if (drv->ops && drv->ops->update_status)
			ret = drv->ops->update_status(slave, status);
	}

	mutex_unlock(&slave->sdw_dev_lock);

	return ret;
}

/**
 * sdw_handle_slave_status() - Handle Slave status
 * @bus: SDW bus instance
 * @status: Status for all Slave(s)
 */
int sdw_handle_slave_status(struct sdw_bus *bus,
			    enum sdw_slave_status status[])
{
	enum sdw_slave_status prev_status;
	struct sdw_slave *slave;
	bool attached_initializing, id_programmed;
	int i, ret = 0;

	/* first check if any Slaves fell off the bus */
	for (i = 1; i <= SDW_MAX_DEVICES; i++) {
		mutex_lock(&bus->bus_lock);
		if (test_bit(i, bus->assigned) == false) {
			mutex_unlock(&bus->bus_lock);
			continue;
		}
		mutex_unlock(&bus->bus_lock);

		slave = sdw_get_slave(bus, i);
		if (!slave)
			continue;

		if (status[i] == SDW_SLAVE_UNATTACHED &&
		    slave->status != SDW_SLAVE_UNATTACHED) {
			dev_warn(&slave->dev, "Slave %d state check1: UNATTACHED, status was %d\n",
				 i, slave->status);
			sdw_modify_slave_status(slave, SDW_SLAVE_UNATTACHED);

			/* Ensure driver knows that peripheral unattached */
			ret = sdw_update_slave_status(slave, status[i]);
			if (ret < 0)
				dev_warn(&slave->dev, "Update Slave status failed:%d\n", ret);
		}
	}

	if (status[0] == SDW_SLAVE_ATTACHED) {
		dev_dbg(bus->dev, "Slave attached, programming device number\n");

		/*
		 * Programming a device number will have side effects,
		 * so we deal with other devices at a later time.
		 * This relies on those devices reporting ATTACHED, which will
		 * trigger another call to this function. This will only
		 * happen if at least one device ID was programmed.
		 * Error returns from sdw_program_device_num() are currently
		 * ignored because there's no useful recovery that can be done.
		 * Returning the error here could result in the current status
		 * of other devices not being handled, because if no device IDs
		 * were programmed there's nothing to guarantee a status change
		 * to trigger another call to this function.
		 */
		sdw_program_device_num(bus, &id_programmed);
		if (id_programmed)
			return 0;
	}

	/* Continue to check other slave statuses */
	for (i = 1; i <= SDW_MAX_DEVICES; i++) {
		mutex_lock(&bus->bus_lock);
		if (test_bit(i, bus->assigned) == false) {
			mutex_unlock(&bus->bus_lock);
			continue;
		}
		mutex_unlock(&bus->bus_lock);

		slave = sdw_get_slave(bus, i);
		if (!slave)
			continue;

		attached_initializing = false;

		switch (status[i]) {
		case SDW_SLAVE_UNATTACHED:
			if (slave->status == SDW_SLAVE_UNATTACHED)
				break;

			dev_warn(&slave->dev, "Slave %d state check2: UNATTACHED, status was %d\n",
				 i, slave->status);

			sdw_modify_slave_status(slave, SDW_SLAVE_UNATTACHED);
			break;

		case SDW_SLAVE_ALERT:
			ret = sdw_handle_slave_alerts(slave);
			if (ret < 0)
				dev_err(&slave->dev,
					"Slave %d alert handling failed: %d\n",
					i, ret);
			break;

		case SDW_SLAVE_ATTACHED:
			if (slave->status == SDW_SLAVE_ATTACHED)
				break;

			prev_status = slave->status;
			sdw_modify_slave_status(slave, SDW_SLAVE_ATTACHED);

			if (prev_status == SDW_SLAVE_ALERT)
				break;

			attached_initializing = true;

			ret = sdw_initialize_slave(slave);
			if (ret < 0)
				dev_err(&slave->dev,
					"Slave %d initialization failed: %d\n",
					i, ret);

			break;

		default:
			dev_err(&slave->dev, "Invalid slave %d status:%d\n",
				i, status[i]);
			break;
		}

		ret = sdw_update_slave_status(slave, status[i]);
		if (ret < 0)
			dev_err(&slave->dev,
				"Update Slave status failed:%d\n", ret);
		if (attached_initializing) {
			dev_dbg(&slave->dev,
				"signaling initialization completion for Slave %d\n",
				slave->dev_num);

			complete_all(&slave->initialization_complete);

			/*
			 * If the manager became pm_runtime active, the peripherals will be
			 * restarted and attach, but their pm_runtime status may remain
			 * suspended. If the 'update_slave_status' callback initiates
			 * any sort of deferred processing, this processing would not be
			 * cancelled on pm_runtime suspend.
			 * To avoid such zombie states, we queue a request to resume.
			 * This would be a no-op in case the peripheral was being resumed
			 * by e.g. the ALSA/ASoC framework.
			 */
			pm_request_resume(&slave->dev);
		}
	}

	return ret;
}
EXPORT_SYMBOL(sdw_handle_slave_status);

void sdw_clear_slave_status(struct sdw_bus *bus, u32 request)
{
	struct sdw_slave *slave;
	int i;

	/* Check all non-zero devices */
	for (i = 1; i <= SDW_MAX_DEVICES; i++) {
		mutex_lock(&bus->bus_lock);
		if (test_bit(i, bus->assigned) == false) {
			mutex_unlock(&bus->bus_lock);
			continue;
		}
		mutex_unlock(&bus->bus_lock);

		slave = sdw_get_slave(bus, i);
		if (!slave)
			continue;

		if (slave->status != SDW_SLAVE_UNATTACHED) {
			sdw_modify_slave_status(slave, SDW_SLAVE_UNATTACHED);
			slave->first_interrupt_done = false;
			sdw_update_slave_status(slave, SDW_SLAVE_UNATTACHED);
		}

		/* keep track of request, used in pm_runtime resume */
		slave->unattach_request = request;
	}
}
EXPORT_SYMBOL(sdw_clear_slave_status);

int sdw_bpt_send_async(struct sdw_bus *bus, struct sdw_slave *slave, struct sdw_bpt_msg *msg)
{
	if (msg->len > SDW_BPT_MSG_MAX_BYTES) {
		dev_err(bus->dev, "Invalid BPT message length %d\n", msg->len);
		return -EINVAL;
	}

	/* check device is enumerated */
	if (slave->dev_num == SDW_ENUM_DEV_NUM ||
	    slave->dev_num > SDW_MAX_DEVICES) {
		dev_err(&slave->dev, "Invalid device number %d\n", slave->dev_num);
		return -ENODEV;
	}

	/* make sure all callbacks are defined */
	if (!bus->ops->bpt_send_async ||
	    !bus->ops->bpt_wait) {
		dev_err(bus->dev, "BPT callbacks not defined\n");
		return -EOPNOTSUPP;
	}

	return bus->ops->bpt_send_async(bus, slave, msg);
}
EXPORT_SYMBOL(sdw_bpt_send_async);

int sdw_bpt_wait(struct sdw_bus *bus, struct sdw_slave *slave, struct sdw_bpt_msg *msg)
{
	return bus->ops->bpt_wait(bus, slave, msg);
}
EXPORT_SYMBOL(sdw_bpt_wait);

int sdw_bpt_send_sync(struct sdw_bus *bus, struct sdw_slave *slave, struct sdw_bpt_msg *msg)
{
	int ret;

	ret = sdw_bpt_send_async(bus, slave, msg);
	if (ret < 0)
		return ret;

	return sdw_bpt_wait(bus, slave, msg);
}
EXPORT_SYMBOL(sdw_bpt_send_sync);
