/*
     This file is part of GNUnet.
     (C) 2001 - 2011 Christian Grothoff (and other contributing authors)

     GNUnet is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License as published
     by the Free Software Foundation; either version 3, or (at your
     option) any later version.

     GNUnet is distributed in the hope that it will be useful, but
     WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
     General Public License for more details.

     You should have received a copy of the GNU General Public License
     along with GNUnet; see the file COPYING.  If not, write to the
     Free Software Foundation, Inc., 59 Temple Place - Suite 330,
     Boston, MA 02111-1307, USA.
*/

/**
 * @author Bartlomiej Polot
 * @file mesh/mesh.h
 */

#ifndef MESH_H_
#define MESH_H_

#ifdef __cplusplus
extern "C"
{
#if 0                           /* keep Emacsens' auto-indent happy */
}
#endif
#endif

#include <stdint.h>

#define MESH_DEBUG              GNUNET_YES

#include "platform.h"
#include "gnunet_common.h"
#include "gnunet_util_lib.h"
#include "gnunet_peer_lib.h"
#include "gnunet_core_service.h"
#include "gnunet_protocols.h"
#include <gnunet_mesh_service.h>

/******************************************************************************/
/********************        MESH LOCAL MESSAGES      *************************/
/******************************************************************************/
/*  Any API call should be documented in the folowing table under API CALL.
 *  Also, any message type should be documented in the following table, with the
 * associated event.
 *
 * API CALL (GNUNET_MESH_*)             MESSAGE USED
 * ------------------------             ------------
 * connect                              GNUNET_MESH_ClientConnect
 * disconnect                           None (network level disconnect)
 *
 * tunnel_create                        GNUNET_MESH_TunnelMessage
 * tunnel_destroy                       GNUNET_MESH_TunnelMessage
 * tunnel_buffer                        GNUNET_MESH_TunnelMessage
 *
 * notify_transmit_ready                None (queue / GNUNET_CLIENT_ntf_tmt_rdy)
 * notify_transmit_ready_cancel         None (clear of internal data structures)
 *
 * 
 * EVENT                                MESSAGE USED
 * -----                                ------------
 * data                                 GNUNET_MESH_Unicast OR
 *                                      GNUNET_MESH_Multicast OR
 *                                      GNUNET_MESH_ToOrigin
 * data ack                             GNUNET_MESH_LocalAck
 * 
 * new incoming tunnel                  GNUNET_MESH_PeerControl
 * peer connects to a tunnel            FIXME
 * peer disconnects from a tunnel       FIXME
 */

/******************************************************************************/
/**************************       CONSTANTS      ******************************/
/******************************************************************************/

#define GNUNET_MESH_LOCAL_TUNNEL_ID_CLI         0x80000000
#define GNUNET_MESH_LOCAL_TUNNEL_ID_SERV        0xB0000000

#define HIGH_PID                                0xFFFF0000
#define LOW_PID                                 0x0000FFFF

/**
 * Value in tunnel info: *int (GNUNET_YES/GNUNET_NO)
 */
typedef enum
{
  /**
   * Disable buffering on intermediate nodes (for minimum latency).
   */
  GNUNET_MESH_OPTION_NOBUFFER   = 0x1,

  /**
   * Enable tunnel reliability, lost messages will be retransmitted.
   */
  GNUNET_MESH_OPTION_RELIABLE   = 0x2,

  /**
   * Enable out of order delivery of messages.
   */
  GNUNET_MESH_OPTION_OOORDER    = 0x4

} MeshTunnelOption;

#define PID_OVERFLOW(pid, max) (pid > HIGH_PID && max < LOW_PID)

/******************************************************************************/
/**************************        MESSAGES      ******************************/
/******************************************************************************/

GNUNET_NETWORK_STRUCT_BEGIN

/**
 * Message for a client to register to the service
 */
struct GNUNET_MESH_ClientConnect
{
    /**
     * Type: GNUNET_MESSAGE_TYPE_MESH_LOCAL_CONNECT
     *
     * Size: sizeof(struct GNUNET_MESH_ClientConnect) +
     *       sizeof(MESH_ApplicationType) * applications +
     *       sizeof(uint16_t) * types
     */
  struct GNUNET_MessageHeader header;
  /* uint32_t                 list_ports[]           */
};


/**
 * Type for tunnel numbering.
 * - Local tunnel numbers given by the service (incoming) are >= 0xB0000000
 * - Local tunnel numbers given by the client (created) are >= 0x80000000
 * - Global tunnel numbers are < 0x80000000
 */
typedef uint32_t MESH_TunnelNumber;


/**
 * Message for a client to create and destroy tunnels.
 */
struct GNUNET_MESH_TunnelMessage
{
    /**
     * Type: GNUNET_MESSAGE_TYPE_MESH_LOCAL_TUNNEL_[CREATE|DESTROY]
     *
     * Size: sizeof(struct GNUNET_MESH_TunnelMessage)
     */
  struct GNUNET_MessageHeader header;

    /**
     * ID of a tunnel controlled by this client.
     */
  MESH_TunnelNumber tunnel_id GNUNET_PACKED;

    /**
     * Tunnel's peer
     */
  struct GNUNET_PeerIdentity peer;

    /**
     * Port of the tunnel.
     */
  uint32_t port GNUNET_PACKED;

    /**
     * Options.
     */
  uint32_t options GNUNET_PACKED;
};


/**
 * Message for mesh data traffic.
 */
struct GNUNET_MESH_LocalData
{
    /**
     * Type: GNUNET_MESSAGE_TYPE_MESH_LOCAL_DATA
     */
  struct GNUNET_MessageHeader header;

    /**
     * TID of the tunnel
     */
  uint32_t tid GNUNET_PACKED;

    /**
     * Payload follows
     */
};


/**
 * Message to allow the client send more data to the service
 * (always service -> client).
 */
struct GNUNET_MESH_LocalAck
{
    /**
     * Type: GNUNET_MESSAGE_TYPE_MESH_LOCAL_ACK
     */
  struct GNUNET_MessageHeader header;

    /**
     * ID of the tunnel allowed to send more data.
     */
  MESH_TunnelNumber tunnel_id GNUNET_PACKED;

};


/**
 * Message to inform the client about tunnels in the service.
 */
struct GNUNET_MESH_LocalMonitor
{
  /**
     * Type: GNUNET_MESSAGE_TYPE_MESH_LOCAL_MONITOR[_TUNNEL]
   */
  struct GNUNET_MessageHeader header;

  /**
   * ID of the tunnel allowed to send more data.
   */
  MESH_TunnelNumber tunnel_id GNUNET_PACKED;

  /**
   * Alignment.
   */
  uint32_t reserved GNUNET_PACKED;

  /**
   * ID of the owner of the tunnel (can be local peer).
   */
  struct GNUNET_PeerIdentity owner;

  /**
   * ID of the destination of the tunnel (can be local peer).
   */
  struct GNUNET_PeerIdentity destination;
};


GNUNET_NETWORK_STRUCT_END

/******************************************************************************/
/************************        ENUMERATIONS      ****************************/
/******************************************************************************/

/**
 * All the states a tunnel can be in.
 */
enum MeshTunnelState
{
    /**
     * Uninitialized status, should never appear in operation.
     */
  MESH_TUNNEL_NEW,

    /**
     * Path to the peer not known yet
     */
  MESH_TUNNEL_SEARCHING,

    /**
     * Request sent, not yet answered.
     */
  MESH_TUNNEL_WAITING,

    /**
     * Peer connected and ready to accept data
     */
  MESH_TUNNEL_READY,

    /**
     * Peer connected previosly but not responding
     */
  MESH_TUNNEL_RECONNECTING
};


/**
 * Check if one pid is bigger than other, accounting for overflow.
 *
 * @param bigger Argument that should be bigger.
 * @param smaller Argument that should be smaller.
 *
 * @return True if bigger (arg1) has a higher value than smaller (arg 2).
 */
int
GMC_is_pid_bigger (uint32_t bigger, uint32_t smaller);


/**
 * Get the higher ACK value out of two values, taking in account overflow.
 *
 * @param a First ACK value.
 * @param b Second ACK value.
 *
 * @return Highest ACK value from the two.
 */
uint32_t
GMC_max_pid (uint32_t a, uint32_t b);


/**
 * Get the lower ACK value out of two values, taking in account overflow.
 *
 * @param a First ACK value.
 * @param b Second ACK value.
 *
 * @return Lowest ACK value from the two.
 */
uint32_t
GMC_min_pid (uint32_t a, uint32_t b);


/**
 * Convert a message type into a string to help debug
 * Generated with:
 * FIND:        "#define ([^ ]+)[ ]*([0-9]+)"
 * REPLACE:     "    case \2: return "\1"; break;"
 * 
 * @param m Message type.
 * 
 * @return Human readable string description.
 */
const char *
GNUNET_MESH_DEBUG_M2S (uint16_t m);

#if 0                           /* keep Emacsens' auto-indent happy */
{
#endif
#ifdef __cplusplus
}
#endif

#endif
