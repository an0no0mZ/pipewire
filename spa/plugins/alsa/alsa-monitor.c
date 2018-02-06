/* Spa ALSA Monitor
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>

#include <libudev.h>
#include <asoundlib.h>

#include <spa/support/log.h>
#include <spa/support/type-map.h>
#include <spa/support/loop.h>
#include <spa/monitor/monitor.h>

#include <lib/debug.h>

#define NAME  "alsa-monitor"

extern const struct spa_handle_factory spa_alsa_sink_factory;
extern const struct spa_handle_factory spa_alsa_source_factory;

struct type {
	uint32_t handle_factory;
	struct spa_type_monitor monitor;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->handle_factory = spa_type_map_get_id(map, SPA_TYPE__HandleFactory);
	spa_type_monitor_map(map, &type->monitor);
}

struct device {
	struct spa_list link;
	struct spa_list card_link;

	struct card *card;

	int num;
	int direction;
	bool enabled;
};

struct card {
	struct spa_list link;

	struct impl *impl;

	struct udev_device *dev;

	int num;
	char name[16];
	snd_ctl_t *ctl_hndl;

	struct spa_list devices;
};

struct impl {
	struct spa_handle handle;
	struct spa_monitor monitor;

	struct type type;
	struct spa_type_map *map;
	struct spa_log *log;
	struct spa_loop *main_loop;

	const struct spa_monitor_callbacks *callbacks;
	void *callbacks_data;

	struct udev *udev;
	int fd;
	struct spa_source source;
	struct udev_monitor *umonitor;
	uint32_t index;
	struct device *current;

	struct spa_list cards;
	struct spa_list devices;

	snd_hctl_t *hctl_hndl;
};

static const char *path_get_card_id(const char *path)
{
	const char *e;

	if (!path)
		return NULL;

	if (!(e = strrchr(path, '/')))
		return NULL;

	if (strlen(e) <= 5 || strncmp(e, "/card", 5) != 0)
		return NULL;

	return e + 5;
}

static int
fill_item(struct impl *this,
	  struct device *device,
	  snd_ctl_card_info_t *card_info,
	  struct spa_pod **item,
	  struct spa_pod_builder *builder)
{
	const char *str, *name, *klass = NULL;
	const struct spa_handle_factory *factory = NULL;
	char card_name[64];
	int err;
	struct type *t = &this->type;
	struct card *card = device->card;
	struct udev_device *dev = card->dev;
	snd_pcm_info_t *dev_info;

	snd_pcm_info_alloca(&dev_info);
	snd_pcm_info_set_device(dev_info, device->num);
	snd_pcm_info_set_subdevice(dev_info, 0);
	snd_pcm_info_set_stream(dev_info, device->direction);

	if ((err = snd_ctl_pcm_info(card->ctl_hndl, dev_info)) < 0)
		return err;

	switch (device->direction) {
	case SND_PCM_STREAM_PLAYBACK:
		factory = &spa_alsa_sink_factory;
		klass = "Audio/Sink";
		break;
	case SND_PCM_STREAM_CAPTURE:
		factory = &spa_alsa_source_factory;
		klass = "Audio/Source";
		break;
	default:
		return -EINVAL;
	}

	name = udev_device_get_property_value(dev, "ID_MODEL_FROM_DATABASE");
	if (!(name && *name)) {
		name = udev_device_get_property_value(dev, "ID_MODEL_ENC");
		if (!(name && *name)) {
			name = udev_device_get_property_value(dev, "ID_MODEL");
		}
	}
	if (!(name && *name))
		name = "Unknown";

	snprintf(card_name, 64, "%s,%d", card->name, device->num);

	spa_pod_builder_add(builder,
		"<", 0, t->monitor.MonitorItem,
		":", t->monitor.id,      "s", name,
		":", t->monitor.flags,   "i", 0,
		":", t->monitor.state,   "i", SPA_MONITOR_ITEM_STATE_AVAILABLE,
		":", t->monitor.name,    "s", name,
		":", t->monitor.klass,   "s", klass,
		":", t->monitor.factory, "p", t->handle_factory, factory, NULL);

	spa_pod_builder_add(builder,
		":", t->monitor.info,    "[", NULL);

	spa_pod_builder_add(builder,
		"s", "alsa.card",            "s", card_name,
		"s", "alsa.card.id",         "s", snd_ctl_card_info_get_id(card_info),
		"s", "alsa.card.components", "s", snd_ctl_card_info_get_components(card_info),
		"s", "alsa.card.driver",     "s", snd_ctl_card_info_get_driver(card_info),
		"s", "alsa.card.name",       "s", snd_ctl_card_info_get_name(card_info),
		"s", "alsa.card.longname",   "s", snd_ctl_card_info_get_longname(card_info),
		"s", "alsa.card.mixername",  "s", snd_ctl_card_info_get_mixername(card_info),
		"s", "udev-probed",          "s", "1",
		"s", "device.api",           "s", "alsa",
		"s", "alsa.pcm.id",          "s", snd_pcm_info_get_id(dev_info),
		"s", "alsa.pcm.name",        "s", snd_pcm_info_get_name(dev_info),
		"s", "alsa.pcm.subname",     "s", snd_pcm_info_get_subdevice_name(dev_info),
		NULL);

	if ((str = udev_device_get_property_value(dev, "SOUND_CLASS")) && *str) {
		spa_pod_builder_add(builder, "s", "device.class", "s", str, NULL);
	}

	str = udev_device_get_property_value(dev, "ID_PATH");
	if (!(str && *str))
		str = udev_device_get_syspath(dev);
	if (str && *str) {
		spa_pod_builder_add(builder, "s", "device.bus_path", "s", str, 0);
	}
	if ((str = udev_device_get_syspath(dev)) && *str) {
		spa_pod_builder_add(builder, "s", "sysfs.path", "s", str, 0);
	}
	if ((str = udev_device_get_property_value(dev, "ID_ID")) && *str) {
		spa_pod_builder_add(builder, "s", "udev.id", "s", str, 0);
	}
	if ((str = udev_device_get_property_value(dev, "ID_BUS")) && *str) {
		spa_pod_builder_add(builder, "s", "device.bus", "s", str, 0);
	}
	if ((str = udev_device_get_property_value(dev, "SUBSYSTEM")) && *str) {
		spa_pod_builder_add(builder, "s", "device.subsystem", "s", str, 0);
	}
	if ((str = udev_device_get_property_value(dev, "ID_VENDOR_ID")) && *str) {
		spa_pod_builder_add(builder, "s", "device.vendor.id", "s", str, 0);
	}
	str = udev_device_get_property_value(dev, "ID_VENDOR_FROM_DATABASE");
	if (!(str && *str)) {
		str = udev_device_get_property_value(dev, "ID_VENDOR_ENC");
		if (!(str && *str)) {
			str = udev_device_get_property_value(dev, "ID_VENDOR");
		}
	}
	if (str && *str) {
		spa_pod_builder_add(builder, "s", "device.vendor.name", "s", str, 0);
	}
	if ((str = udev_device_get_property_value(dev, "ID_MODEL_ID")) && *str) {
		spa_pod_builder_add(builder, "s", "device.product.id", "s", str, 0);
	}
	spa_pod_builder_add(builder, "s", "device.product.name", "s", name, 0);

	if ((str = udev_device_get_property_value(dev, "ID_SERIAL")) && *str) {
		spa_pod_builder_add(builder, "s", "device.serial", "s", str, 0);
	}
	if ((str = udev_device_get_property_value(dev, "SOUND_FORM_FACTOR")) && *str) {
		spa_pod_builder_add(builder, "s", "device.form_factor", "s", str, 0);
	}
	*item = spa_pod_builder_add(builder, "]>", NULL);

	return 0;
}

#if 0
static int load_hctls(struct impl *this)
{
	int err;
	snd_hctl_elem_t *elem;

	if ((err = snd_hctl_open(&this->hctl_hndl, this->card_name, 0)) < 0) {
		spa_log_warn(this->log, "can't open hcontrol for card %s: %s", this->card_name, snd_strerror(err));
		return 0;
	}

	if ((err = snd_hctl_load(this->hctl_hndl)) < 0) {
		spa_log_warn(this->log, "can't load hcontrol for card %s: %s", this->card_name, snd_strerror(err));
		return 0;
	}

	for (elem = snd_hctl_first_elem(this->hctl_hndl); elem != NULL;
	     elem = snd_hctl_elem_next(elem)) {

		snd_ctl_elem_info_t *info;
		snd_ctl_elem_value_t *val;

		snd_ctl_elem_info_alloca(&info);

		if (snd_hctl_elem_info(elem, info) < 0)
			continue;

		spa_log_info(this->log, "control %s %d %d %d",
				snd_hctl_elem_get_name(elem),
				snd_ctl_elem_info_get_type(info),
				snd_ctl_elem_info_get_interface(info),
				snd_hctl_elem_get_subdevice(elem));

		if (snd_ctl_elem_info_get_interface(info) != SND_CTL_ELEM_IFACE_CARD)
			continue;

		snd_ctl_elem_value_alloca(&val);
		if (snd_hctl_elem_read(elem, val) < 0)
			continue;

		if (snd_ctl_elem_info_get_type(info) == SND_CTL_ELEM_TYPE_BOOLEAN) {
			spa_log_info(this->log, " bool %d", snd_ctl_elem_value_get_boolean(val, 0));
		}
		else if (snd_ctl_elem_info_get_type(info) == SND_CTL_ELEM_TYPE_INTEGER)  {
			spa_log_info(this->log, " int %d", snd_ctl_elem_value_get_boolean(val, 0));
		}
	}



	return err;
}
#endif

static int create_device(struct impl *this,
			 struct udev_device *dev,
			 struct card *card,
			 snd_ctl_card_info_t *card_info,
			 snd_pcm_info_t *dev_info,
			 int dev_num,
			 int direction)
{
	struct device *device;

	device = calloc(1, sizeof(struct device));
	if (device == NULL)
		return -ENOMEM;

	device->card = card;
	device->num = dev_num;
	device->direction = direction;

	spa_list_append(&this->devices, &device->link);
	spa_list_append(&card->devices, &device->card_link);

	return 0;
}

static void free_device(struct device *device)
{
	spa_list_remove(&device->link);
	spa_list_remove(&device->card_link);

	free(device);
}

static struct card * find_card(struct impl *this, int card_num)
{
	struct card *c;

	spa_list_for_each(c, &this->cards, link)
		if (c->num == card_num)
			return c;

	return NULL;
}

static int check_card(struct impl *this, struct udev_device *dev, int *card_num)
{
	const char *str, *devpath;

	if (udev_device_get_property_value(dev, "PULSE_IGNORE"))
		return 0;

	if ((str = udev_device_get_property_value(dev, "SOUND_CLASS")) && strcmp(str, "modem") == 0)
		return 0;

	if ((devpath = udev_device_get_devpath(dev)) == NULL)
		return 0;

	if ((str = path_get_card_id(devpath)) == NULL)
		return 0;

	*card_num = atoi(str);

	return 1;
}

static struct card * create_card(struct impl *this, struct udev_device *dev)
{
	struct card *card;
	int card_num, err, dev_num;
	snd_ctl_card_info_t *card_info;
	snd_pcm_info_t *dev_info;

	if (!check_card(this, dev, &card_num))
		return NULL;

	card = find_card(this, card_num);
	if (card != NULL)
		return NULL;

	card = calloc(1, sizeof(struct card));
	if (card == 0)
		return NULL;

	spa_list_init(&card->devices);

	card->num = card_num;
	card->dev = dev;
	snprintf(card->name, sizeof(card->name), "hw:%u", card_num);

	spa_list_append(&this->cards, &card->link);

	if ((err = snd_ctl_open(&card->ctl_hndl, card->name, 0)) < 0) {
		spa_log_error(this->log, "can't open control for card %s: %s",
				card->name, snd_strerror(err));
		return NULL;
	}
	snd_ctl_card_info_alloca(&card_info);

	if ((err = snd_ctl_card_info(card->ctl_hndl, card_info)) < 0) {
		spa_log_error(this->log, "can't get card info for device: %s", snd_strerror(err));
		return NULL;
	}

	snd_pcm_info_alloca(&dev_info);

	dev_num = -1;
	while (true) {
		if ((err = snd_ctl_pcm_next_device(card->ctl_hndl, &dev_num)) < 0) {
			spa_log_error(this->log, "error iterating devices: %s", snd_strerror(err));
			return NULL;
		}
		if (dev_num < 0)
			break;

		snd_pcm_info_set_device(dev_info, dev_num);
		snd_pcm_info_set_subdevice(dev_info, 0);

		snd_pcm_info_set_stream(dev_info, SND_PCM_STREAM_PLAYBACK);

		if ((err = snd_ctl_pcm_info(card->ctl_hndl, dev_info)) == 0) {
			if ((err = create_device(this,
						 dev,
						 card,
						 card_info,
						 dev_info,
						 dev_num,
						 SND_PCM_STREAM_PLAYBACK)) < 0)
				continue;
		}

		snd_pcm_info_set_stream(dev_info, SND_PCM_STREAM_CAPTURE);

		if ((err = snd_ctl_pcm_info(card->ctl_hndl, dev_info)) == 0) {
			if ((err = create_device(this,
						 dev,
						 card,
						 card_info,
						 dev_info,
						 dev_num,
						 SND_PCM_STREAM_CAPTURE)) < 0)
				continue;
		}
	}

	return card;
}

static int remove_card(struct impl *this, struct card *card)
{
	struct device *d, *t;

	spa_list_for_each_safe(d, t, &card->devices, card_link)
		free_device(d);

	spa_list_remove(&card->link);

	if (card->ctl_hndl)
		snd_ctl_close(card->ctl_hndl);
	free(card);

	return 0;
}

static void impl_on_fd_events(struct spa_source *source)
{
	struct impl *this = source->data;
	struct udev_device *dev;
	const char *action;
	uint32_t type;
	struct spa_event *event;
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct card *card;
	struct device *d;
	snd_ctl_card_info_t *card_info;
	int card_num;

	dev = udev_monitor_receive_device(this->umonitor);

	if ((action = udev_device_get_action(dev)) == NULL)
		action = "change";

	if (strcmp(action, "add") == 0) {
		type = this->type.monitor.Added;
	} else if (strcmp(action, "change") == 0) {
		type = this->type.monitor.Changed;
	} else if (strcmp(action, "remove") == 0) {
		type = this->type.monitor.Removed;
	} else
		return;

	if (type == this->type.monitor.Added ||
	    type == this->type.monitor.Changed) {
		card = create_card(this, dev);
	}
	else {
		if (!check_card(this, dev, &card_num))
			return;

		card = find_card(this, card_num);
	}
	if (card == NULL)
		return;

	snd_ctl_card_info_alloca(&card_info);
	if (snd_ctl_card_info(card->ctl_hndl, card_info) < 0)
		return;

	spa_list_for_each(d, &card->devices, card_link) {
		struct spa_pod *item;

		event = spa_pod_builder_object(&b, 0, type);

		fill_item(this, d, card_info, &item, &b);

		this->callbacks->event(this->callbacks_data, event);
	}

	if (type == this->type.monitor.Removed)
		remove_card(this, card);
}

static int impl_udev_open(struct impl *this)
{
	struct udev_enumerate *enumerate;
        struct udev_list_entry *devices, *d;


	if (this->udev != NULL)
		return 0;

	this->udev = udev_new();

	enumerate = udev_enumerate_new(this->udev);

	udev_enumerate_add_match_subsystem(enumerate, "sound");
	udev_enumerate_scan_devices(enumerate);

	devices = udev_enumerate_get_list_entry(enumerate);

	udev_list_entry_foreach(d, devices) {
		const char *path = udev_list_entry_get_name(d);
		struct udev_device *dev = udev_device_new_from_syspath(this->udev, path);

		create_card(this, dev);
	}

        udev_enumerate_unref(enumerate);

	return 0;
}

static int
impl_monitor_set_callbacks(struct spa_monitor *monitor,
			   const struct spa_monitor_callbacks *callbacks,
			   void *data)
{
	int res;
	struct impl *this;

	spa_return_val_if_fail(monitor != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(monitor, struct impl, monitor);

	this->callbacks = callbacks;
	this->callbacks_data = data;

	if (callbacks) {
		if ((res = impl_udev_open(this)) < 0)
			return res;

		if (this->umonitor)
			udev_monitor_unref(this->umonitor);
		this->umonitor = udev_monitor_new_from_netlink(this->udev, "udev");
		if (this->umonitor == NULL)
			return -ENOMEM;

		udev_monitor_filter_add_match_subsystem_devtype(this->umonitor, "sound", NULL);
		udev_monitor_enable_receiving(this->umonitor);

		this->source.func = impl_on_fd_events;
		this->source.data = this;
		this->source.fd = udev_monitor_get_fd(this->umonitor);;
		this->source.mask = SPA_IO_IN | SPA_IO_ERR;

		spa_loop_add_source(this->main_loop, &this->source);
	} else {
		spa_loop_remove_source(this->main_loop, &this->source);
	}

	return 0;
}

static int impl_monitor_enum_items(struct spa_monitor *monitor,
				   uint32_t *index,
				   struct spa_pod **item,
				   struct spa_pod_builder *builder)
{
	int res;
	struct impl *this;
	struct device *device;
	snd_ctl_card_info_t *card_info;

	spa_return_val_if_fail(monitor != NULL, -EINVAL);
	spa_return_val_if_fail(item != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(monitor, struct impl, monitor);

	if ((res = impl_udev_open(this)) < 0)
		return res;

	if (*index == 0 || this->index > *index) {
		this->current = spa_list_first(&this->devices, struct device, link);
		this->index = 0;
	}

	while (*index > this->index) {
		this->current = spa_list_next(this->current, link);
		this->index++;
	}
	if (spa_list_is_end(this->current, &this->devices, link))
		return 0;

	device = this->current;

	snd_ctl_card_info_alloca(&card_info);
	if ((res = snd_ctl_card_info(device->card->ctl_hndl, card_info)) < 0)
		return res;

	fill_item(this, device, card_info, item, builder);

	(*index)++;

	return 1;
}

static const struct spa_monitor impl_monitor = {
	SPA_VERSION_MONITOR,
	NULL,
	impl_monitor_set_callbacks,
	impl_monitor_enum_items,
};

static int impl_get_interface(struct spa_handle *handle, uint32_t interface_id, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (interface_id == this->type.monitor.Monitor)
		*interface = &this->monitor;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
        struct impl *this = (struct impl *) handle;

        if (this->umonitor)
                udev_monitor_unref(this->umonitor);
        if (this->udev)
                udev_unref(this->udev);

	return 0;
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct impl *this;
	uint32_t i;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	for (i = 0; i < n_support; i++) {
		if (strcmp(support[i].type, SPA_TYPE__TypeMap) == 0)
			this->map = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE__Log) == 0)
			this->log = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE_LOOP__MainLoop) == 0)
			this->main_loop = support[i].data;
	}
	if (this->map == NULL) {
		spa_log_error(this->log, "an id-map is needed");
		return -EINVAL;
	}
	if (this->main_loop == NULL) {
		spa_log_error(this->log, "a main-loop is needed");
		return -EINVAL;
	}

	init_type(&this->type, this->map);

	this->monitor = impl_monitor;
	spa_list_init(&this->cards);
	spa_list_init(&this->devices);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE__Monitor,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	if (*index >= SPA_N_ELEMENTS(impl_interfaces))
		return 0;

	*info = &impl_interfaces[(*index)++];

	return 1;
}

const struct spa_handle_factory spa_alsa_monitor_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	NULL,
	sizeof(struct impl),
	impl_init,
	impl_enum_interface_info,
};
