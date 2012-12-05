/*
     This file is part of GNUnet.
     (C) 2011 Christian Grothoff (and other contributing authors)

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
 * @file ats/gnunet-service-ats_addresses.h
 * @brief ats service address management
 * @author Matthias Wachs
 * @author Christian Grothoff
 */
#ifndef GNUNET_SERVICE_ATS_ADDRESSES_H
#define GNUNET_SERVICE_ATS_ADDRESSES_H

#include "gnunet_util_lib.h"
#include "gnunet_ats_service.h"
#include "gnunet_statistics_service.h"
#include "ats.h"

#define ATS_BLOCKING_DELTA GNUNET_TIME_relative_multiply(GNUNET_TIME_UNIT_MILLISECONDS, 100)

/**
 * Address with additional information
 */
struct ATS_Address
{
  /**
   * Next element in DLL
   */
  struct ATS_Address *next;

  /**
   * Previous element in DLL
   */
  struct ATS_Address *prev;

  /**
   * Peer ID
   */
  struct GNUNET_PeerIdentity peer;

  /**
   * Session ID, 0 if no session is given
   */
  uint32_t session_id;

  /**
   * Address
   */
  const void *addr;

  /**
   * Address length
   */
  size_t addr_len;

  /**
   * Plugin name
   */
  char *plugin;

  /**
   * MLP specific information
   * TODO remove or rename
   */
  void *mlp_information;

  /**
   * ATS information
   */
  struct GNUNET_ATS_Information *ats;

  /**
   * Number of ATS information
   */
  uint32_t ats_count;

  /* CHECK USAGE */
  struct GNUNET_TIME_Relative atsp_latency;

  /* CHECK USAGE */
  struct GNUNET_BANDWIDTH_Value32NBO atsp_utilization_in;

  /* CHECK USAGE */
  struct GNUNET_BANDWIDTH_Value32NBO atsp_utilization_out;


  /* CHECK USAGE */
  uint32_t atsp_distance;

  /* CHECK USAGE */
  uint32_t atsp_cost_wan;

  /* CHECK USAGE */
  uint32_t atsp_cost_lan;

  /* CHECK USAGE */
  uint32_t atsp_cost_wlan;

  /* CHECK USAGE */
  uint32_t atsp_network_type;

  /**
   * Inbound bandwidth assigned by solver in NBO
   */
  struct GNUNET_BANDWIDTH_Value32NBO assigned_bw_in;

  /**
   * Outbound bandwidth assigned by solver in NBO
   */
  struct GNUNET_BANDWIDTH_Value32NBO assigned_bw_out;

  /**
   * Blocking interval
   */
  struct GNUNET_TIME_Relative block_interval;

  /**
   * Time when address can be suggested again
   */
  struct GNUNET_TIME_Absolute blocked_until;

  /**
   * Is this the active address for this peer?
   */
  int active;

  /**
   * Is this the address for this peer in use?
   */
  int used;
};


typedef void *
 (*GAS_solver_init) (const struct GNUNET_CONFIGURATION_Handle *cfg,
                     const struct GNUNET_STATISTICS_Handle *stats,
                     int *network,
                     unsigned long long *out_dest,
                     unsigned long long *in_dest,
                     int dest_length);

typedef void
(*GAS_solver_address_change_preference) (void *solver,
                                         const struct GNUNET_PeerIdentity *peer,
                                         enum GNUNET_ATS_PreferenceKind kind,
                                         float score);

typedef void
 (*GAS_solver_address_delete) (void *solver,
                               struct GNUNET_CONTAINER_MultiHashMap *addresses,
                               struct ATS_Address *address);

typedef void
(*GAS_solver_address_update) (void *solver,
                              struct GNUNET_CONTAINER_MultiHashMap *addresses,
                              struct ATS_Address *address);


typedef const struct ATS_Address *
(*GAS_solver_get_preferred_address) (void *solver,
                                     struct GNUNET_CONTAINER_MultiHashMap *addresses,
                                     const struct GNUNET_PeerIdentity *peer);


typedef void
 (*GAS_solver_done) (void *solver);


/**
 * Initialize address subsystem.
 *
 * @param cfg configuration to use
 * @param stats the statistics handle to use
 */
struct GAS_Addresses_Handle *
GAS_addresses_init (const struct GNUNET_CONFIGURATION_Handle *cfg,
                    const struct GNUNET_STATISTICS_Handle *stats);

/**
 * Shutdown address subsystem.
 */
void
GAS_addresses_done (struct GAS_Addresses_Handle *handle);

void
GAS_addresses_handle_backoff_reset (const struct GNUNET_PeerIdentity *peer);

/**
 * This address is now used or not used anymore
 */
int
GAS_addresses_in_use (const struct GNUNET_PeerIdentity *peer,
                      const char *plugin_name, const void *plugin_addr,
                      size_t plugin_addr_len, uint32_t session_id, int in_use);

void
GAS_addresses_update (const struct GNUNET_PeerIdentity *peer,
                      const char *plugin_name, const void *plugin_addr,
                      size_t plugin_addr_len, uint32_t session_id,
                      const struct GNUNET_ATS_Information *atsi,
                      uint32_t atsi_count);


void
GAS_addresses_destroy (const struct GNUNET_PeerIdentity *peer,
                       const char *plugin_name, const void *plugin_addr,
                       size_t plugin_addr_len, uint32_t session_id);


void
GAS_addresses_destroy_all (void);


/**
 * Cancel address suggestions for a peer
 *
 * @param peer the respective peer
 */
void
GAS_addresses_request_address_cancel (const struct GNUNET_PeerIdentity *peer);

void
GAS_addresses_request_address (const struct GNUNET_PeerIdentity *peer);

void
GAS_addresses_change_preference (const struct GNUNET_PeerIdentity *peer,
                                 enum GNUNET_ATS_PreferenceKind kind,
                                 float score);

void
GAS_addresses_add (const struct GNUNET_PeerIdentity *peer,
                      const char *plugin_name, const void *plugin_addr,
                      size_t plugin_addr_len, uint32_t session_id,
                      const struct GNUNET_ATS_Information *atsi,
                      uint32_t atsi_count);


typedef void (*GNUNET_ATS_Peer_Iterator) (void *p_it_cls,
                                          const struct GNUNET_PeerIdentity *id);

/**
 * Return all peers currently known to ATS
 *
 * @param p_it the iterator to call for every peer
 * @param p_it_cls the closure for the iterator
 */
void
GAS_addresses_iterate_peers (GNUNET_ATS_Peer_Iterator p_it, void *p_it_cls);

typedef void (*GNUNET_ATS_PeerInfo_Iterator) (void *p_it_cls,
    const struct GNUNET_PeerIdentity *id,
    const char *plugin_name,
    const void *plugin_addr, size_t plugin_addr_len,
    const int address_active,
    const struct GNUNET_ATS_Information *atsi,
    uint32_t atsi_count,
    struct GNUNET_BANDWIDTH_Value32NBO
    bandwidth_out,
    struct GNUNET_BANDWIDTH_Value32NBO bandwidth_in);

/**
 * Return information all peers currently known to ATS
 *
 * @param peer the respective peer
 * @param pi_it the iterator to call for every peer
 * @param pi_it_cls the closure for the iterator
 */
void
GAS_addresses_get_peer_info (const struct GNUNET_PeerIdentity *peer, GNUNET_ATS_PeerInfo_Iterator pi_it, void *pi_it_cls);

#endif

/* end of gnunet-service-ats_addresses.h */
