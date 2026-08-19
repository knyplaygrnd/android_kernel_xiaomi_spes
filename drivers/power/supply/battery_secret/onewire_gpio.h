/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * onewire_gpio - 1-Wire bus provider/consumer interface
 *
 * Copyright (c) 2016 Xiaomi Inc.
 *
 */
#ifndef __ONEWIRE_GPIO_H__
#define __ONEWIRE_GPIO_H__

#include <linux/kref.h>
#include <linux/list.h>
#include <linux/types.h>

struct onewire_bus;

/**
 * struct onewire_bus_ops - 1-Wire bus operation table.
 */
struct onewire_bus_ops {
	u8 (*reset)(struct onewire_bus *bus);
	u8 (*read_byte)(struct onewire_bus *bus);
	void (*write_byte)(struct onewire_bus *bus, u8 val);
	void (*software_reset)(struct onewire_bus *bus);
};

/**
 * struct onewire_bus - 1-Wire bus controller abstraction.
 */
struct onewire_bus {
	const struct onewire_bus_ops *ops;
	void *priv;
	struct kref ref;
	struct list_head node;
	char name[32];
};

/**
 * onewire_bus_register - register a 1-Wire bus provider.
 *
 * @bus:  bus descriptor allocated by the provider.
 * @name:  identifier string used by consumers to find this bus.
 *
 * Returns 0 on success, negative errno on failure.
 */
int onewire_bus_register(struct onewire_bus *bus, const char *name);

/**
 * onewire_bus_unregister - unregister a 1-Wire bus provider.
 * Called by onewire_gpio during remove, after setting bus->priv = NULL.
 *
 * Removes the bus from the lookup table and drops the provider's
 * reference. If consumers still hold references the struct stays alive
 * but all ops silently no-op (priv == NULL).
 */
void onewire_bus_unregister(struct onewire_bus *bus);

/**
 * onewire_bus_get - acquire a reference to a named 1-Wire bus.
 *
 * @name: bus name as registered by the provider.
 *
 * Returns a pointer to the bus on success, NULL if no bus with that
 * name is currently registered.  A NULL return means the provider has
 * not probed yet; the caller should handle negative errno on failure.
 *
 * The caller must release the reference with onewire_bus_put() when done.
 */
struct onewire_bus *onewire_bus_get(const char *name);

/**
 * onewire_bus_put - release a reference acquired with onewire_bus_get().
 * Safe to call with a NULL pointer.
 */
void onewire_bus_put(struct onewire_bus *bus);

#endif /* __ONEWIRE_GPIO_H__ */
