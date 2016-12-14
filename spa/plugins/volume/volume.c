/* Spa
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

#include <string.h>
#include <stddef.h>

#include <spa/log.h>
#include <spa/id-map.h>
#include <spa/node.h>
#include <spa/audio/format.h>
#include <lib/props.h>

typedef struct _SpaVolume SpaVolume;

typedef struct {
  SpaProps props;
  double volume;
  bool mute;
} SpaVolumeProps;

typedef struct {
  bool have_format;
  SpaPortInfo info;
  SpaBuffer **buffers;
  unsigned int n_buffers;
  SpaBuffer *buffer;
  void      *io;
} SpaVolumePort;

typedef struct {
  uint32_t node;
} URI;

struct _SpaVolume {
  SpaHandle  handle;
  SpaNode  node;

  URI uri;
  SpaIDMap *map;
  SpaLog *log;

  SpaVolumeProps props[2];

  SpaNodeEventCallback event_cb;
  void *user_data;

  bool have_format;
  SpaFormatAudio query_format;
  SpaFormatAudio current_format;

  SpaVolumePort in_ports[1];
  SpaVolumePort out_ports[1];
};

#define CHECK_IN_PORT(this,d,p)  ((d) == SPA_DIRECTION_INPUT && (p) == 0)
#define CHECK_OUT_PORT(this,d,p) ((d) == SPA_DIRECTION_OUTPUT && (p) == 0)
#define CHECK_PORT(this,d,p)     ((p) == 0)

static const double default_volume = 1.0;
static const double min_volume = 0.0;
static const double max_volume = 10.0;
static const bool default_mute = false;

static const SpaPropRangeInfo volume_range[] = {
  { "min", { sizeof (double), &min_volume } },
  { "max", { sizeof (double), &max_volume } },
};

enum {
  PROP_ID_VOLUME,
  PROP_ID_MUTE,
  PROP_ID_LAST,
};

static const SpaPropInfo prop_info[] =
{
  { PROP_ID_VOLUME,            offsetof (SpaVolumeProps, volume),
                               "volume",
                               SPA_PROP_FLAG_READWRITE,
                               SPA_PROP_TYPE_DOUBLE, sizeof (double),
                               SPA_PROP_RANGE_TYPE_MIN_MAX, 2, volume_range,
                               NULL },
  { PROP_ID_MUTE,              offsetof (SpaVolumeProps, mute),
                               "mute",
                               SPA_PROP_FLAG_READWRITE,
                               SPA_PROP_TYPE_BOOL, sizeof (bool),
                               SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                               NULL },
};

static void
reset_volume_props (SpaVolumeProps *props)
{
  props->volume = default_volume;
  props->mute = default_mute;
}

static void
update_state (SpaVolume *this, SpaNodeState state)
{
    this->node.state = state;
}

static SpaResult
spa_volume_node_get_props (SpaNode        *node,
                           SpaProps     **props)
{
  SpaVolume *this;

  if (node == NULL || props == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaVolume, node);

  memcpy (&this->props[0], &this->props[1], sizeof (this->props[1]));
  *props = &this->props[0].props;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_set_props (SpaNode          *node,
                           const SpaProps  *props)
{
  SpaVolume *this;
  SpaVolumeProps *p;
  SpaResult res;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaVolume, node);
  p = &this->props[1];

  if (props == NULL) {
    reset_volume_props (p);
    return SPA_RESULT_OK;
  }
  res = spa_props_copy_values (props, &p->props);

  return res;
}

static SpaResult
spa_volume_node_send_command (SpaNode        *node,
                              SpaNodeCommand *command)
{
  SpaVolume *this;

  if (node == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaVolume, node);

  switch (command->type) {
    case SPA_NODE_COMMAND_INVALID:
      return SPA_RESULT_INVALID_COMMAND;

    case SPA_NODE_COMMAND_START:
      update_state (this, SPA_NODE_STATE_STREAMING);
      break;

    case SPA_NODE_COMMAND_PAUSE:
      update_state (this, SPA_NODE_STATE_PAUSED);
      break;

    case SPA_NODE_COMMAND_FLUSH:
    case SPA_NODE_COMMAND_DRAIN:
    case SPA_NODE_COMMAND_MARKER:
    case SPA_NODE_COMMAND_CLOCK_UPDATE:
      return SPA_RESULT_NOT_IMPLEMENTED;
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_set_event_callback (SpaNode              *node,
                                    SpaNodeEventCallback  event,
                                    void                 *user_data)
{
  SpaVolume *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaVolume, node);

  this->event_cb = event;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_get_n_ports (SpaNode       *node,
                             unsigned int  *n_input_ports,
                             unsigned int  *max_input_ports,
                             unsigned int  *n_output_ports,
                             unsigned int  *max_output_ports)
{
  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (n_input_ports)
    *n_input_ports = 1;
  if (max_input_ports)
    *max_input_ports = 1;
  if (n_output_ports)
    *n_output_ports = 1;
  if (max_output_ports)
    *max_output_ports = 1;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_get_port_ids (SpaNode       *node,
                              unsigned int   n_input_ports,
                              uint32_t      *input_ids,
                              unsigned int   n_output_ports,
                              uint32_t      *output_ids)
{
  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (n_input_ports > 0 && input_ids)
    input_ids[0] = 0;
  if (n_output_ports > 0 && output_ids)
    output_ids[0] = 0;

  return SPA_RESULT_OK;
}


static SpaResult
spa_volume_node_add_port (SpaNode        *node,
                          SpaDirection    direction,
                          uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_volume_node_remove_port (SpaNode        *node,
                             SpaDirection    direction,
                             uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_volume_node_port_enum_formats (SpaNode          *node,
                                   SpaDirection      direction,
                                   uint32_t          port_id,
                                   SpaFormat       **format,
                                   const SpaFormat  *filter,
                                   void            **state)
{
  SpaVolume *this;
  int index;

  if (node == NULL || format == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaVolume, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  index = (*state == NULL ? 0 : *(int*)state);

  switch (index) {
    case 0:
      spa_format_audio_init (SPA_MEDIA_TYPE_AUDIO,
                             SPA_MEDIA_SUBTYPE_RAW,
                             &this->query_format);
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *format = &this->query_format.format;
  *(int*)state = ++index;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_port_set_format (SpaNode            *node,
                                 SpaDirection        direction,
                                 uint32_t            port_id,
                                 SpaPortFormatFlags  flags,
                                 const SpaFormat    *format)
{
  SpaVolume *this;
  SpaVolumePort *port;
  SpaResult res;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaVolume, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  if (format == NULL) {
    port->have_format = false;
    return SPA_RESULT_OK;
  }

  if ((res = spa_format_audio_parse (format, &this->current_format)) < 0)
    return res;

  port->have_format = true;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_port_get_format (SpaNode          *node,
                                 SpaDirection      direction,
                                 uint32_t          port_id,
                                 const SpaFormat **format)
{
  SpaVolume *this;
  SpaVolumePort *port;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaVolume, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  if (!port->have_format)
    return SPA_RESULT_NO_FORMAT;

  *format = &this->current_format.format;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_port_get_info (SpaNode            *node,
                               SpaDirection        direction,
                               uint32_t            port_id,
                               const SpaPortInfo **info)
{
  SpaVolume *this;
  SpaVolumePort *port;

  if (node == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaVolume, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];
  *info = &port->info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_port_get_props (SpaNode       *node,
                                SpaDirection   direction,
                                uint32_t       port_id,
                                SpaProps     **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_volume_node_port_set_props (SpaNode        *node,
                                SpaDirection    direction,
                                uint32_t        port_id,
                                const SpaProps *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_volume_node_port_use_buffers (SpaNode         *node,
                                  SpaDirection     direction,
                                  uint32_t         port_id,
                                  SpaBuffer      **buffers,
                                  uint32_t         n_buffers)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_volume_node_port_alloc_buffers (SpaNode         *node,
                                    SpaDirection     direction,
                                    uint32_t         port_id,
                                    SpaAllocParam  **params,
                                    uint32_t         n_params,
                                    SpaBuffer      **buffers,
                                    uint32_t        *n_buffers)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_volume_node_port_set_input (SpaNode      *node,
                                uint32_t      port_id,
                                SpaPortInput *input)
{
  SpaVolume *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaVolume, node);

  if (!CHECK_PORT (this, SPA_DIRECTION_INPUT, port_id))
    return SPA_RESULT_INVALID_PORT;

  this->in_ports[port_id].io = input;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_port_set_output (SpaNode       *node,
                                 uint32_t       port_id,
                                 SpaPortOutput *output)
{
  SpaVolume *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaVolume, node);

  if (!CHECK_PORT (this, SPA_DIRECTION_OUTPUT, port_id))
    return SPA_RESULT_INVALID_PORT;

  this->in_ports[port_id].io = output;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_port_reuse_buffer (SpaNode         *node,
                                   uint32_t         port_id,
                                   uint32_t         buffer_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_volume_node_port_send_command (SpaNode        *node,
                                   SpaDirection    direction,
                                   uint32_t        port_id,
                                   SpaNodeCommand *command)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_volume_node_process_input (SpaNode *node)
{
  SpaVolume *this;
  SpaPortInput *input;
  SpaPortOutput *output;
  SpaVolumePort *port;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaVolume, node);

  port = &this->in_ports[0];

  if ((input = port->io) == NULL)
    return SPA_RESULT_ERROR;
  if ((output = this->out_ports[0].io) == NULL)
    return SPA_RESULT_ERROR;

  if (input->buffer_id >= port->n_buffers) {
    input->status = SPA_RESULT_INVALID_BUFFER_ID;
    return SPA_RESULT_ERROR;
  }
  if (!port->have_format) {
    input->status = SPA_RESULT_NO_FORMAT;
    return SPA_RESULT_ERROR;
  }
  port->buffer = port->buffers[input->buffer_id];

  input->status = SPA_RESULT_HAVE_ENOUGH_INPUT;
  input->flags &= ~SPA_PORT_STATUS_FLAG_NEED_INPUT;
  output->flags |= SPA_PORT_STATUS_FLAG_HAVE_OUTPUT;

  return SPA_RESULT_HAVE_ENOUGH_INPUT;
}

static SpaBuffer *
find_free_buffer (SpaVolume *this, SpaVolumePort *port)
{
  return NULL;
}

static void
release_buffer (SpaVolume *this, SpaBuffer *buffer)
{
  SpaNodeEventReuseBuffer rb;

  rb.event.type = SPA_NODE_EVENT_TYPE_REUSE_BUFFER;
  rb.event.size = sizeof (rb);
  rb.port_id = 0;
  rb.buffer_id = buffer->id;
  this->event_cb (&this->node, &rb.event, this->user_data);
}

static SpaResult
spa_volume_node_process_output (SpaNode *node)
{
  SpaVolume *this;
  SpaVolumePort *port;
  unsigned int si, di, i, n_samples, n_bytes, soff, doff ;
  SpaBuffer *sbuf, *dbuf;
  SpaData *sd, *dd;
  uint16_t *src, *dst;
  double volume;
  SpaPortInput *input;
  SpaPortInput *output;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaVolume, node);

  port = &this->out_ports[0];

  if ((output = port->io) == NULL)
    return SPA_RESULT_ERROR;
  if ((input = this->in_ports[0].io) == NULL)
    return SPA_RESULT_ERROR;

  if (!port->have_format)
    return SPA_RESULT_NO_FORMAT;

  if (this->in_ports[0].buffer == NULL)
    return SPA_RESULT_NEED_MORE_INPUT;

  volume = this->props[1].volume;

  sbuf = this->in_ports[0].buffer;
  dbuf = find_free_buffer (this, port);

  si = di = 0;
  soff = doff = 0;

  while (true) {
    if (si == sbuf->n_datas || di == dbuf->n_datas)
      break;

    sd = &sbuf->datas[si];
    dd = &dbuf->datas[di];

    src = (uint16_t*) ((uint8_t*)SPA_DATA_CHUNK_PTR (sd) + soff);
    dst = (uint16_t*) ((uint8_t*)SPA_DATA_CHUNK_PTR (dd) + doff);

    n_bytes = SPA_MIN (SPA_DATA_CHUNK_SIZE (sd) - soff, SPA_DATA_CHUNK_SIZE (dd) - doff);
    n_samples = n_bytes / sizeof (uint16_t);

    for (i = 0; i < n_samples; i++)
      *src++ = *dst++ * volume;

    soff += n_bytes;
    doff += n_bytes;

    if (soff >= SPA_DATA_CHUNK_SIZE (sd)) {
      si++;
      soff = 0;
    }
    if (doff >= SPA_DATA_CHUNK_SIZE (dd)) {
      di++;
      doff = 0;
    }
  }
  if (sbuf != dbuf)
    release_buffer (this, sbuf);

  this->in_ports[0].buffer = NULL;
  output->buffer_id = dbuf->id;
  output->status = SPA_RESULT_OK;

  input->flags |= SPA_PORT_STATUS_FLAG_NEED_INPUT;
  output->flags &= ~SPA_PORT_STATUS_FLAG_HAVE_OUTPUT;

  return SPA_RESULT_OK;
}

static const SpaNode volume_node = {
  sizeof (SpaNode),
  NULL,
  SPA_NODE_STATE_INIT,
  spa_volume_node_get_props,
  spa_volume_node_set_props,
  spa_volume_node_send_command,
  spa_volume_node_set_event_callback,
  spa_volume_node_get_n_ports,
  spa_volume_node_get_port_ids,
  spa_volume_node_add_port,
  spa_volume_node_remove_port,
  spa_volume_node_port_enum_formats,
  spa_volume_node_port_set_format,
  spa_volume_node_port_get_format,
  spa_volume_node_port_get_info,
  spa_volume_node_port_get_props,
  spa_volume_node_port_set_props,
  spa_volume_node_port_use_buffers,
  spa_volume_node_port_alloc_buffers,
  spa_volume_node_port_set_input,
  spa_volume_node_port_set_output,
  spa_volume_node_port_reuse_buffer,
  spa_volume_node_port_send_command,
  spa_volume_node_process_input,
  spa_volume_node_process_output,
};

static SpaResult
spa_volume_get_interface (SpaHandle               *handle,
                          uint32_t                 interface_id,
                          void                   **interface)
{
  SpaVolume *this;

  if (handle == NULL || interface == 0)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVolume *) handle;

  if (interface_id == this->uri.node)
    *interface = &this->node;
  else
    return SPA_RESULT_UNKNOWN_INTERFACE;

  return SPA_RESULT_OK;
}

static SpaResult
volume_clear (SpaHandle *handle)
{
  return SPA_RESULT_OK;
}

static SpaResult
volume_init (const SpaHandleFactory  *factory,
             SpaHandle               *handle,
             const SpaDict           *info,
             const SpaSupport        *support,
             unsigned int             n_support)
{
  SpaVolume *this;
  unsigned int i;

  if (factory == NULL || handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  handle->get_interface = spa_volume_get_interface;
  handle->clear = volume_clear;

  this = (SpaVolume *) handle;

  for (i = 0; i < n_support; i++) {
    if (strcmp (support[i].uri, SPA_ID_MAP_URI) == 0)
      this->map = support[i].data;
    else if (strcmp (support[i].uri, SPA_LOG_URI) == 0)
      this->log = support[i].data;
  }
  if (this->map == NULL) {
    spa_log_error (this->log, "an id-map is needed");
    return SPA_RESULT_ERROR;
  }
  this->uri.node = spa_id_map_get_id (this->map, SPA_NODE_URI);

  this->node = volume_node;
  this->props[1].props.n_prop_info = PROP_ID_LAST;
  this->props[1].props.prop_info = prop_info;
  reset_volume_props (&this->props[1]);

  this->in_ports[0].info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
                                 SPA_PORT_INFO_FLAG_IN_PLACE;
  this->out_ports[0].info.flags = SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS |
                                  SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
                                  SPA_PORT_INFO_FLAG_NO_REF;

  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo volume_interfaces[] =
{
  { SPA_NODE_URI, },
};

static SpaResult
volume_enum_interface_info (const SpaHandleFactory  *factory,
                            const SpaInterfaceInfo **info,
                            void                   **state)
{
  int index;

  if (factory == NULL || info == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  index = (*state == NULL ? 0 : *(int*)state);

  switch (index) {
    case 0:
      *info = &volume_interfaces[index];
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *(int*)state = ++index;

  return SPA_RESULT_OK;
}

const SpaHandleFactory spa_volume_factory =
{ "volume",
  NULL,
  sizeof (SpaVolume),
  volume_init,
  volume_enum_interface_info,
};
