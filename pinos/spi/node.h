/* Simple Plugin Interface
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

#ifndef __SPI_NODE_H__
#define __SPI_NODE_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpiNode SpiNode;

#include <spi/defs.h>
#include <spi/plugin.h>
#include <spi/params.h>
#include <spi/port.h>
#include <spi/event.h>
#include <spi/buffer.h>
#include <spi/command.h>

/**
 * SpiInputFlags:
 * @SPI_INPUT_FLAG_NONE: no flag
 */
typedef enum {
  SPI_INPUT_FLAG_NONE                  =  0,
} SpiInputFlags;

/**
 * SpiInputInfo:
 * @port_id: the port id
 * @flags: extra flags
 * @buffer: a buffer
 *
 * Input information for a node.
 */
typedef struct {
  uint32_t        port_id;
  SpiInputFlags   flags;
  SpiBuffer      *buffer;
  SpiEvent       *event;
  SpiResult       status;
} SpiInputInfo;

/**
 * SpiOutputFlags:
 * @SPI_OUTPUT_FLAG_NONE: no flag
 * @SPI_OUTPUT_FLAG_PULL: force a #SPI_EVENT_NEED_INPUT event on the
 *                        peer input ports when no data is available.
 * @SPI_OUTPUT_FLAG_DISCARD: discard the buffer data
 */
typedef enum {
  SPI_OUTPUT_FLAG_NONE                  =  0,
  SPI_OUTPUT_FLAG_PULL                  = (1 << 0),
  SPI_OUTPUT_FLAG_DISCARD               = (1 << 1),
} SpiOutputFlags;

/**
 * SpiOutputInfo:
 * @port_id: the port id
 * @flags: extra flags
 * @buffer: a buffer
 * @event: an event
 *
 * Output information for a node.
 */
typedef struct {
  uint32_t        port_id;
  SpiOutputFlags  flags;
  SpiBuffer      *buffer;
  SpiEvent       *event;
  SpiResult       status;
} SpiOutputInfo;

/**
 * SpiEventCallback:
 * @node: a #SpiHandle emiting the event
 * @event: the event that was emited
 * @user_data: user data provided when registering the callback
 *
 * This will be called when an out-of-bound event is notified
 * on @node.
 */
typedef void   (*SpiEventCallback)   (SpiHandle   *handle,
                                      SpiEvent    *event,
                                      void        *user_data);

#define SPI_INTERFACE_ID_NODE                   0
#define SPI_INTERFACE_ID_NODE_NAME              "Node interface"
#define SPI_INTERFACE_ID_NODE_DESCRIPTION       "Main processing node interface"

/**
 * SpiNode:
 *
 * The main processing nodes.
 */
struct _SpiNode {
  /* the total size of this node. This can be used to expand this
   * structure in the future */
  size_t size;
  /**
   * SpiNode::get_params:
   * @handle: a #SpiHandle
   * @props: a location for a #SpiParams pointer
   *
   * Get the configurable parameters of @node.
   *
   * The returned @props is a snapshot of the current configuration and
   * can be modified. The modifications will take effect after a call
   * to SpiNode::set_params.
   *
   * Returns: #SPI_RESULT_OK on success
   *          #SPI_RESULT_INVALID_ARGUMENTS when node or props are %NULL
   *          #SPI_RESULT_NOT_IMPLEMENTED when there are no properties
   *                 implemented on @node
   */
  SpiResult   (*get_params)           (SpiHandle        *handle,
                                       SpiParams       **props);
  /**
   * SpiNode::set_params:
   * @handle: a #SpiHandle
   * @props: a #SpiParams
   *
   * Set the configurable parameters in @node.
   *
   * Usually, @props will be obtained from SpiNode::get_params and then
   * modified but it is also possible to set another #SpiParams object
   * as long as its keys and types match those of SpiParams::get_params.
   *
   * Properties with keys that are not known are ignored.
   *
   * If @props is NULL, all the parameters are reset to their defaults.
   *
   * Returns: #SPI_RESULT_OK on success
   *          #SPI_RESULT_INVALID_ARGUMENTS when node is %NULL
   *          #SPI_RESULT_NOT_IMPLEMENTED when no properties can be
   *                 modified on @node.
   *          #SPI_RESULT_WRONG_PARAM_TYPE when a property has the wrong
   *                 type.
   */
  SpiResult   (*set_params)           (SpiHandle        *handle,
                                       const SpiParams  *props);
  /**
   * SpiNode::send_command:
   * @handle: a #SpiHandle
   * @command: a #SpiCommand
   *
   * Send a command to @node.
   *
   * Returns: #SPI_RESULT_OK on success
   *          #SPI_RESULT_INVALID_ARGUMENTS when node or command is %NULL
   *          #SPI_RESULT_NOT_IMPLEMENTED when this node can't process commands
   *          #SPI_RESULT_INVALID_COMMAND @command is an invalid command
   */
  SpiResult   (*send_command)         (SpiHandle        *handle,
                                       SpiCommand       *command);
  /**
   * SpiNode::set_event_callback:
   * @handle: a #SpiHandle
   * @callback: a callback
   * @user_data: user data passed in the callback
   *
   * Set a callback to receive events from @node. if @callback is %NULL, the
   * current callback is removed.
   *
   * The callback can be emited from any thread. The caller should take
   * appropriate actions to handle the event in other threads when needed.
   *
   * Returns: #SPI_RESULT_OK on success
   *          #SPI_RESULT_INVALID_ARGUMENTS when node is %NULL
   */
  SpiResult   (*set_event_callback)   (SpiHandle        *handle,
                                       SpiEventCallback  callback,
                                       void             *user_data);
  /**
   * SpiNode::get_n_ports:
   * @handle: a #SpiHandle
   * @n_input_ports: location to hold the number of input ports or %NULL
   * @max_input_ports: location to hold the maximum number of input ports or %NULL
   * @n_output_ports: location to hold the number of output ports or %NULL
   * @max_output_ports: location to hold the maximum number of output ports or %NULL
   *
   * Get the current number of input and output ports and also the maximum
   * number of ports.
   *
   * Returns: #SPI_RESULT_OK on success
   *          #SPI_RESULT_INVALID_ARGUMENTS when node is %NULL
   */
  SpiResult   (*get_n_ports)          (SpiHandle        *handle,
                                       unsigned int     *n_input_ports,
                                       unsigned int     *max_input_ports,
                                       unsigned int     *n_output_ports,
                                       unsigned int     *max_output_ports);
  /**
   * SpiNode::get_port_ids:
   * @handle: a #SpiHandle
   * @n_input_ports: size of the @input_ids array
   * @input_ids: array to store the input stream ids
   * @n_output_ports: size of the @output_ids array
   * @output_ids: array to store the output stream ids
   *
   * Get the current number of input and output ports and also the maximum
   * number of ports.
   *
   * Returns: #SPI_RESULT_OK on success
   *          #SPI_RESULT_INVALID_ARGUMENTS when node is %NULL
   */
  SpiResult   (*get_port_ids)         (SpiHandle        *handle,
                                       unsigned int      n_input_ports,
                                       uint32_t         *input_ids,
                                       unsigned int      n_output_ports,
                                       uint32_t         *output_ids);

  SpiResult   (*add_port)             (SpiHandle        *handle,
                                       SpiDirection      direction,
                                       uint32_t         *port_id);
  SpiResult   (*remove_port)          (SpiHandle        *handle,
                                       uint32_t          port_id);

  /**
   * SpiNode::enum_port_formats:
   * @handle: a #SpiHandle
   * @port_id: the port to query
   * @index: the format index to retrieve
   * @format: pointer to a format
   *
   * Enumerate all possible formats on @port_id of @node.
   *
   * Use the index to retrieve the formats one by one until the function
   * returns #SPI_RESULT_ENUM_END.
   *
   * The result format can be queried and modified and ultimately be used
   * to call SpiNode::set_port_format.
   *
   * Returns: #SPI_RESULT_OK on success
   *          #SPI_RESULT_INVALID_ARGUMENTS when node or format is %NULL
   *          #SPI_RESULT_INVALID_PORT when port_id is not valid
   *          #SPI_RESULT_ENUM_END when no format exists for @index
   *
   */
  SpiResult   (*enum_port_formats)    (SpiHandle        *handle,
                                       uint32_t          port_id,
                                       unsigned int      index,
                                       SpiParams       **format);
  /**
   * SpiNode::set_port_format:
   * @handle: a #SpiHandle
   * @port_id: the port to configure
   * @format: a #SpiParam with the format
   *
   * Set a format on @port_id of @node.
   *
   * When @format is %NULL, the current format will be removed.
   *
   * Returns: #SPI_RESULT_OK on success
   *          #SPI_RESULT_INVALID_ARGUMENTS when node is %NULL
   *          #SPI_RESULT_INVALID_PORT when port_id is not valid
   *          #SPI_RESULT_INVALID_MEDIA_TYPE when the media type is not valid
   *          #SPI_RESULT_INVALID_FORMAT_PARAMS when one of the mandatory format
   *                 parameters is not specified.
   *          #SPI_RESULT_WRONG_PARAM_TYPE when the type of size of a parameter
   *                 is not correct.
   */
  SpiResult   (*set_port_format)      (SpiHandle        *handle,
                                       uint32_t          port_id,
                                       int               test_only,
                                       const SpiParams  *format);
  /**
   * SpiNode::get_port_format:
   * @handle: a #SpiHandle
   * @port_id: the port to query
   * @format: a pointer to a location to hold the #SpiParam
   *
   * Get the format on @port_id of @node. The result #SpiParam can
   * not be modified.
   *
   * Returns: #SPI_RESULT_OK on success
   *          #SPI_RESULT_INVALID_ARGUMENTS when @node or @format is %NULL
   *          #SPI_RESULT_INVALID_PORT when @port_id is not valid
   *          #SPI_RESULT_INVALID_NO_FORMAT when no format was set
   */
  SpiResult   (*get_port_format)      (SpiHandle        *handle,
                                       uint32_t          port_id,
                                       const SpiParams **format);

  SpiResult   (*get_port_info)        (SpiHandle        *handle,
                                       uint32_t          port_id,
                                       SpiPortInfo      *info);

  SpiResult   (*get_port_params)      (SpiHandle        *handle,
                                       uint32_t          port_id,
                                       SpiParams       **params);
  SpiResult   (*set_port_params)      (SpiHandle        *handle,
                                       uint32_t          port_id,
                                       const SpiParams  *params);

  SpiResult   (*get_port_status)      (SpiHandle        *handle,
                                       uint32_t          port_id,
                                       SpiPortStatus    *status);

  /**
   * SpiNode::push_port_input:
   * @handle: a #SpiHandle
   * @n_info: number of #SpiInputInfo in @info
   * @info: array of #SpiInputInfo
   *
   * Push a buffer and/or an event into one or more input ports of
   * @node.
   *
   * Returns: #SPI_RESULT_OK on success
   *          #SPI_RESULT_INVALID_ARGUMENTS when node or info is %NULL
   *          #SPI_RESULT_ERROR when one or more of the @info has an
   *                         error result. Check the status of all the
   *                         @info.
   *          #SPI_RESULT_HAVE_ENOUGH_INPUT when output can be produced.
   */
  SpiResult   (*push_port_input)      (SpiHandle        *handle,
                                       unsigned int      n_info,
                                       SpiInputInfo     *info);
  /**
   * SpiNode::pull_port_output:
   * @handle: a #SpiHandle
   * @n_info: number of #SpiOutputInfo in @info
   * @info: array of #SpiOutputInfo
   *
   * Pull a buffer and/or an event from one or more output ports of
   * @node.
   *
   * Returns: #SPI_RESULT_OK on success
   *          #SPI_RESULT_INVALID_ARGUMENTS when node or info is %NULL
   *          #SPI_RESULT_PORTS_CHANGED the number of ports has changed. None
   *                   of the @info fields are modified
   *          #SPI_RESULT_FORMAT_CHANGED a format changed on some port.
   *                   the ports that changed are marked in the status.
   *          #SPI_RESULT_PROPERTIES_CHANGED port properties changed. The
   *                   changed ports are marked in the status.
   *          #SPI_RESULT_ERROR when one or more of the @info has an
   *                   error result. Check the status of all the @info.
   *          #SPI_RESULT_NEED_MORE_INPUT when no output can be produced
   *                   because more input is needed.
   */
  SpiResult   (*pull_port_output)     (SpiHandle        *handle,
                                       unsigned int      n_info,
                                       SpiOutputInfo    *info);
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPI_NODE_H__ */
