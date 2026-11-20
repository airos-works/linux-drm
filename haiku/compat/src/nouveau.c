/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The parts of the compat layer only nouveau uses (lx/nouveau.h). */


/* #pragma mark - maple tree */

struct lx_mt_range {
	struct list_head link;
	unsigned long index;
	unsigned long last;
	void *entry;
};


/*	The first range that ends at or after \a index, or NULL. */
static struct lx_mt_range *
mt_first_from(struct maple_tree *mt, unsigned long index)
{
	struct lx_mt_range *range;
	list_for_each_entry(range, &mt->ma_ranges, link) {
		if (range->last >= index)
			return range;
	}
	return NULL;
}


void *
mas_walk(struct ma_state *mas)
{
	struct maple_tree *mt = mas->tree;
	struct lx_mt_range *range = mt_first_from(mt, mas->index);
	if (range != NULL && range->index <= mas->index) {
		mas->index = range->index;
		mas->last = range->last;
		return range->entry;
	}
	// the gap around index
	struct lx_mt_range *previous = NULL;
	if (range != NULL) {
		if (range->link.prev != &mt->ma_ranges)
			previous = list_entry(range->link.prev, struct lx_mt_range, link);
	} else if (!list_empty(&mt->ma_ranges))
		previous = list_last_entry(&mt->ma_ranges, struct lx_mt_range, link);
	mas->index = previous != NULL ? previous->last + 1 : 0;
	mas->last = range != NULL ? range->index - 1 : ULONG_MAX;
	return NULL;
}


void *
mas_find(struct ma_state *mas, unsigned long max)
{
	unsigned long from = mas->index;
	if (mas->started) {
		if (mas->last == ULONG_MAX)
			return NULL;
		from = mas->last + 1;
	}
	struct lx_mt_range *range = mt_first_from(mas->tree, from);
	if (range == NULL || range->index > max)
		return NULL;
	mas->started = 1;
	mas->index = range->index;
	mas->last = range->last;
	return range->entry;
}


void *
mas_erase(struct ma_state *mas)
{
	struct lx_mt_range *range = mt_first_from(mas->tree, mas->index);
	// iterating continues from the erased range's place
	mas->started = 0;
	if (range == NULL || range->index > mas->index)
		return NULL;
	void *entry = range->entry;
	mas->index = range->index;
	mas->last = range->last;
	list_del(&range->link);
	kfree(range);
	return entry;
}


/*	Stores \a entry for [index, last]. Ranges it overlaps are cut back or
	removed, as the maple tree does. */
int
mas_store_gfp(struct ma_state *mas, void *entry, gfp_t gfp)
{
	struct maple_tree *mt = mas->tree;
	unsigned long index = mas->index, last = mas->last;
	struct lx_mt_range *range, *next;
	struct lx_mt_range *added = NULL, *tail = NULL;
	if (entry != NULL) {
		added = kmalloc(sizeof(*added), gfp);
		if (added == NULL)
			return -ENOMEM;
		added->index = index;
		added->last = last;
		added->entry = entry;
	}
	list_for_each_entry_safe(range, next, &mt->ma_ranges, link) {
		if (range->last < index || range->index > last)
			continue;
		if (range->index < index && range->last > last) {
			// split: the part after the new range
			tail = kmalloc(sizeof(*tail), gfp);
			if (tail == NULL) {
				kfree(added);
				return -ENOMEM;
			}
			tail->index = last + 1;
			tail->last = range->last;
			tail->entry = range->entry;
			list_add(&tail->link, &range->link);
			range->last = index - 1;
		} else if (range->index < index) {
			range->last = index - 1;
		} else if (range->last > last) {
			range->index = last + 1;
		} else {
			list_del(&range->link);
			kfree(range);
		}
	}
	if (added != NULL) {
		struct list_head *before = &mt->ma_ranges;
		list_for_each_entry(range, &mt->ma_ranges, link) {
			if (range->index > last) {
				before = &range->link;
				break;
			}
		}
		list_add_tail(&added->link, before);
	}
	return 0;
}


void
__mt_destroy(struct maple_tree *mt)
{
	struct lx_mt_range *range, *next;
	list_for_each_entry_safe(range, next, &mt->ma_ranges, link) {
		list_del(&range->link);
		kfree(range);
	}
}


/* #pragma mark - I2C clients */

int
i2c_master_send(const struct i2c_client *client, const char *buf, int count)
{
	struct i2c_msg msg = { .addr = client->addr,
		.flags = client->flags & I2C_M_TEN, .len = count, .buf = (u8 *)buf };
	int ret = i2c_transfer(client->adapter, &msg, 1);
	return ret == 1 ? count : (ret < 0 ? ret : -EIO);
}


int
i2c_master_recv(const struct i2c_client *client, char *buf, int count)
{
	struct i2c_msg msg = { .addr = client->addr,
		.flags = (client->flags & I2C_M_TEN) | I2C_M_RD, .len = count,
		.buf = (u8 *)buf };
	int ret = i2c_transfer(client->adapter, &msg, 1);
	return ret == 1 ? count : (ret < 0 ? ret : -EIO);
}


/* #pragma mark - CRC */

u32
crc32_le(u32 crc, const void *data, size_t length)
{
	const u8 *bytes = data;
	while (length-- > 0) {
		crc ^= *bytes++;
		for (int bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320 & -(crc & 1));
	}
	return crc;
}
