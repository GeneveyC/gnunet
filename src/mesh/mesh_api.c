/*
     This file is part of GNUnet.
     (C) 2009, 2010 Christian Grothoff (and other contributing authors)

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
 * @file mesh/mesh_api.c
 * @brief mesh service; API for the Mesh. This is used to talk to arbitrary peers
 *        as of 2011-01Jan-06 this is a mockup.
 * @author Philipp Tölke
 */
#include <platform.h>
#include <gnunet_constants.h>
#include <gnunet_mesh_service.h>
#include <gnunet_core_service.h>
#include <gnunet_container_lib.h>

struct tunnel_id
{
  uint32_t id GNUNET_PACKED;
  struct GNUNET_PeerIdentity initiator;
  struct GNUNET_PeerIdentity target;
};

static uint32_t current_id = 0;

struct tunnel_message
{
  struct GNUNET_MessageHeader hdr;
  struct tunnel_id id;
  /* followed by another GNUNET_MessageHeader */
};

struct notify_cls
{
  void* notify_cls;
  GNUNET_CONNECTION_TransmitReadyNotify notify;
  struct GNUNET_MESH_Tunnel *tunnel;
};

struct GNUNET_MESH_Tunnel
{
  /* The other peer this tunnel leads to; just unicast for the moment! */
  struct GNUNET_PeerIdentity peer;

  struct tunnel_id id;

  /* The handlers and cls for outbound tunnels. Are NULL for inbound tunnels. */
  GNUNET_MESH_TunnelDisconnectHandler disconnect_handler;
  GNUNET_MESH_TunnelConnectHandler connect_handler;
  void *handler_cls;

  struct GNUNET_MESH_Handle* handle;

  /* The context of the receive-function. */
  void *ctx;
};

struct tunnel_list_element
{
  struct GNUNET_MESH_Tunnel tunnel;
  struct tunnel_list_element *next, *prev;
};

struct tunnel_list
{
  struct tunnel_list_element *head, *tail;
};

struct peer_list_element
{
  struct GNUNET_PeerIdentity peer;
  struct GNUNET_TRANSPORT_ATS_Information atsi;
  struct peer_list_element *next, *prev;
};

struct peer_list
{
  struct peer_list_element *head, *tail;
};

struct GNUNET_MESH_Handle
{
  struct GNUNET_CORE_Handle *core;
  struct GNUNET_MESH_MessageHandler *handlers;
  struct GNUNET_PeerIdentity myself;
  unsigned int connected_to_core;
  struct peer_list connected_peers;
  struct tunnel_list established_tunnels;
  struct tunnel_list pending_tunnels;
  void *cls;
  GNUNET_MESH_TunnelEndHandler *cleaner;
};

static void
send_end_connect(void* cls,
		     const struct GNUNET_SCHEDULER_TaskContext* tc)
{
  struct GNUNET_MESH_Tunnel* tunnel = cls;

  tunnel->connect_handler(tunnel->handler_cls, NULL, NULL);
}

static void
send_self_connect(void* cls,
		        const struct GNUNET_SCHEDULER_TaskContext* tc)
{
  struct GNUNET_MESH_Tunnel* tunnel = cls;

  tunnel->connect_handler(tunnel->handler_cls, &tunnel->handle->myself, NULL);
  GNUNET_SCHEDULER_add_now(send_end_connect, tunnel);
}

static void
core_startup (void *cls,
	      struct GNUNET_CORE_Handle *core,
	      const struct GNUNET_PeerIdentity *my_identity,
	      const struct GNUNET_CRYPTO_RsaPublicKeyBinaryEncoded *publicKey)
{
  struct GNUNET_MESH_Handle *handle = cls;
  memcpy (&handle->myself, my_identity, sizeof (struct GNUNET_PeerIdentity));
  handle->connected_to_core = GNUNET_YES;
}

/**
 * Core calls this if we are connected to a new peer.
 *
 * If core tells us that we are connected to ourself, we ignore it. Otherwise, the
 * peer is added to the connected_peers-list.
 *
 */
static void
core_connect (void *cls,
	      const struct GNUNET_PeerIdentity *peer,
	      const struct GNUNET_TRANSPORT_ATS_Information *atsi)
{
  struct GNUNET_MESH_Handle *handle = cls;
  /* Check for connect-to-self-message, which we ignore */
  if (0 ==
      memcmp (peer, &handle->myself, sizeof (struct GNUNET_PeerIdentity)))
    return;


  /* put the new peer into the list of connected peers */
  struct peer_list_element *element =
    GNUNET_malloc (sizeof (struct peer_list_element));
  memcpy (&element->peer, peer, sizeof (struct GNUNET_PeerIdentity));
  memcpy (&element->atsi, atsi,
	  sizeof (struct GNUNET_TRANSPORT_ATS_Information));

  GNUNET_CONTAINER_DLL_insert_after (handle->connected_peers.head,
				     handle->connected_peers.tail,
				     handle->connected_peers.tail, element);

  struct tunnel_list_element *tunnel = handle->pending_tunnels.head;
  while (tunnel != NULL)
    {
      if (0 ==
	  memcmp (&tunnel->tunnel.peer, peer,
		  sizeof (struct GNUNET_PeerIdentity)))
	{
	  struct tunnel_list_element *next = tunnel->next;
	  GNUNET_CONTAINER_DLL_remove (handle->pending_tunnels.head,
				       handle->pending_tunnels.tail, tunnel);
	  GNUNET_CONTAINER_DLL_insert_after (handle->established_tunnels.head,
					     handle->established_tunnels.tail,
					     handle->established_tunnels.tail,
					     tunnel);
	  tunnel->tunnel.connect_handler (tunnel->tunnel.handler_cls,
					  peer, atsi);
	  GNUNET_SCHEDULER_add_now(send_end_connect, tunnel);
	  tunnel = next;
	}
      else
	tunnel = tunnel->next;
    }
}

/**
 * Core calls this if we disconnect a peer
 *
 * Remove this peer from the list of connected peers
 * Close all tunnels this peer belongs to
 */
static void
core_disconnect (void *cls, const struct GNUNET_PeerIdentity *peer)
{
  struct GNUNET_MESH_Handle *handle = cls;

  struct peer_list_element *element = handle->connected_peers.head;
  while (element != NULL)
    {
      if (0 ==
	  memcmp (&element->peer, peer, sizeof (struct GNUNET_PeerIdentity)))
	break;
      element = element->next;
    }
  if (element != NULL)
    {
      GNUNET_CONTAINER_DLL_remove (handle->connected_peers.head,
				   handle->connected_peers.tail, element);
      GNUNET_free (element);
    }

  struct tunnel_list_element *telement = handle->established_tunnels.head;
  while (telement != NULL)
    {
      if (0 ==
	  memcmp (&telement->tunnel.peer, peer,
		  sizeof (struct GNUNET_PeerIdentity)))
	{
	  /* disconnect tunnels */
	  /* outbound tunnels */
	  if (telement->tunnel.connect_handler != NULL)
	    telement->tunnel.disconnect_handler (telement->tunnel.handler_cls,
						 peer);
	  /* inbound tunnels */
	  else
	    handle->cleaner (handle->cls, &telement->tunnel,
			     &telement->tunnel.ctx);

	  struct tunnel_list_element *next = telement->next;
	  GNUNET_CONTAINER_DLL_remove (handle->established_tunnels.head,
				       handle->established_tunnels.tail,
				       telement);
	  GNUNET_free (telement);
	  telement = next;
	}
      else
	{
	  telement = telement->next;
	}
    }
}

/**
 * Receive a message from core.
 */
static int
core_receive (void *cls,
	      const struct GNUNET_PeerIdentity *other,
	      const struct GNUNET_MessageHeader *message,
	      const struct GNUNET_TRANSPORT_ATS_Information *atsi)
{
  struct GNUNET_MESH_Handle *handle = cls;
  struct tunnel_message *tmessage = (struct tunnel_message *) message;
  struct GNUNET_MessageHeader *rmessage =
    (struct GNUNET_MessageHeader *) (tmessage + 1);

  struct GNUNET_MESH_MessageHandler *handler;

  for (handler = handle->handlers; handler->callback != NULL; handler++)
    {
      if ( (ntohs (rmessage->type) == handler->type)
	   && ( (handler->expected_size == 0)
		|| (handler->expected_size == ntohs (rmessage->size))) )
	{
	  break;
	}
    }

  /* handler->callback handles this message */

  /* If no handler was found, drop the message but keep the channel open */
  if (handler->callback == NULL)
    return GNUNET_OK;

  struct tunnel_list_element *tunnel = handle->established_tunnels.head;

  while (tunnel != NULL)
    {
      if (tunnel->tunnel.id.id == tmessage->id.id &&
	  (0 ==
	   memcmp (&tmessage->id.initiator, &tunnel->tunnel.id.initiator,
		   sizeof (struct GNUNET_PeerIdentity)))
	  && (0 ==
	      memcmp (&tmessage->id.target, &tunnel->tunnel.id.target,
		      sizeof (struct GNUNET_PeerIdentity))))
	break;
      tunnel = tunnel->next;
    }

  /* if no tunnel was found: create a new inbound tunnel */
  if (tunnel == NULL)
    {
      tunnel = GNUNET_malloc (sizeof (struct tunnel_list_element));
      tunnel->tunnel.connect_handler = NULL;
      tunnel->tunnel.disconnect_handler = NULL;
      tunnel->tunnel.handler_cls = NULL;
      tunnel->tunnel.ctx = NULL;
      tunnel->tunnel.handle = handle;
      memcpy (&tunnel->tunnel.peer, other,
	      sizeof (struct GNUNET_PeerIdentity));
      memcpy (&tunnel->tunnel.id, &tmessage->id, sizeof (struct tunnel_id));

      GNUNET_CONTAINER_DLL_insert_after (handle->established_tunnels.head,
					 handle->established_tunnels.tail,
					 handle->established_tunnels.tail,
					 tunnel);
    }

  return handler->callback (handle->cls, &tunnel->tunnel,
			    &tunnel->tunnel.ctx, rmessage, atsi);
}


struct GNUNET_MESH_Tunnel *
GNUNET_MESH_peer_request_connect_all (struct GNUNET_MESH_Handle *handle,
				      struct GNUNET_TIME_Relative timeout,
				      unsigned int num_peers,
				      const struct GNUNET_PeerIdentity *peers,
				      GNUNET_MESH_TunnelConnectHandler
				      connect_handler,
				      GNUNET_MESH_TunnelDisconnectHandler
				      disconnect_handler, void *handler_cls)
{
  if (num_peers != 1)
    return NULL;

  struct tunnel_list_element *tunnel =
    GNUNET_malloc (sizeof (struct tunnel_list_element));

  tunnel->tunnel.connect_handler = connect_handler;
  tunnel->tunnel.disconnect_handler = disconnect_handler;
  tunnel->tunnel.handler_cls = handler_cls;
  tunnel->tunnel.ctx = NULL;
  tunnel->tunnel.handle = handle;
  memcpy (&tunnel->tunnel.id.initiator, &handle->myself,
	  sizeof (struct GNUNET_PeerIdentity));
  memcpy (&tunnel->tunnel.id.target, peers,
	  sizeof (struct GNUNET_PeerIdentity));
  tunnel->tunnel.id.id = current_id++;
  memcpy (&tunnel->tunnel.peer, peers, sizeof(struct GNUNET_PeerIdentity));

  struct peer_list_element *element = handle->connected_peers.head;
  while (element != NULL)
    {
      if (0 ==
	  memcmp (&element->peer, peers, sizeof (struct GNUNET_PeerIdentity)))
	break;
      element = element->next;
    }

  if (element != NULL)
    {
      /* we are connected to this peer */
      GNUNET_CONTAINER_DLL_insert_after (handle->established_tunnels.head,
					 handle->established_tunnels.tail,
					 handle->established_tunnels.tail,
					 tunnel);
      connect_handler (handler_cls, &element->peer, &element->atsi);
    }
  else if (0 ==
	   memcmp (peers, &handle->myself,
		   sizeof (struct GNUNET_PeerIdentity)))
    {
      /* we are the peer */
      GNUNET_CONTAINER_DLL_insert_after (handle->established_tunnels.head,
					 handle->established_tunnels.tail,
					 handle->established_tunnels.tail,
					 tunnel);
      GNUNET_SCHEDULER_add_now(send_self_connect, tunnel);
    }
  else
    {
      /* we are not connected to this peer */
      GNUNET_CONTAINER_DLL_insert_after (handle->pending_tunnels.head,
					 handle->pending_tunnels.tail,
					 handle->pending_tunnels.tail,
					 tunnel);
      (void) GNUNET_CORE_peer_request_connect (handle->core,
					       timeout,
					       peers,
					       NULL, NULL);					
    }

  return &tunnel->tunnel;
}

const struct GNUNET_PeerIdentity*
GNUNET_MESH_get_peer(const struct GNUNET_MESH_Tunnel* tunnel)
{
  return &tunnel->peer;
}

static size_t
core_notify(void* cls, size_t size, void* buf)
{
  struct notify_cls *ncls = cls;
  struct GNUNET_MESH_Tunnel *tunnel = ncls->tunnel;
  struct tunnel_message* message = buf;
  void* cbuf = (void*) &message[1];
  GNUNET_assert(NULL != ncls->notify);

  size_t sent = ncls->notify(ncls->notify_cls, size - sizeof(struct tunnel_message), cbuf);

  GNUNET_free(ncls);

  if (0 == sent) return 0;

  sent += sizeof(struct tunnel_message);

  message->hdr.type = htons(GNUNET_MESSAGE_TYPE_MESH);
  message->hdr.size = htons(sent);
  memcpy(&message->id, &tunnel->id, sizeof(struct tunnel_id));
  return sent;
}

struct GNUNET_MESH_TransmitHandle *
GNUNET_MESH_notify_transmit_ready (struct
				   GNUNET_MESH_Tunnel
				   *tunnel,
				   int cork,
				   uint32_t priority,
				   struct
				   GNUNET_TIME_Relative
				   maxdelay,
				   size_t
				   notify_size,
				   GNUNET_CONNECTION_TransmitReadyNotify
				   notify, void *notify_cls)
{
  struct notify_cls *cls = GNUNET_malloc(sizeof(struct notify_cls));
  cls->notify_cls = notify_cls;
  GNUNET_assert(NULL != notify);
  cls->notify = notify;
  cls->tunnel = tunnel;
  GNUNET_CORE_notify_transmit_ready(tunnel->handle->core,
				    priority,
				    maxdelay,
				    &tunnel->peer,
				    notify_size + sizeof(struct tunnel_message),
				    &core_notify,
				    (void*)cls);

  /* aborting is not implemented yet */
  return (struct GNUNET_MESH_TransmitHandle*) 1;
}


struct GNUNET_MESH_Handle *
GNUNET_MESH_connect (const struct
		     GNUNET_CONFIGURATION_Handle
		     *cfg, void *cls,
		     GNUNET_MESH_TunnelEndHandler
		     cleaner,
		     const struct GNUNET_MESH_MessageHandler *handlers)
{
  struct GNUNET_MESH_Handle *ret =
    GNUNET_malloc (sizeof (struct GNUNET_MESH_Handle));

  ret->connected_to_core = GNUNET_NO;
  ret->connected_peers.head = NULL;
  ret->connected_peers.tail = NULL;
  ret->cleaner = cleaner;
  ret->cls = cls;
    
  const struct GNUNET_MESH_MessageHandler *it;
  unsigned int len = 1;
  for (it = handlers; it->callback != NULL; it++)
    {
      len++;
    }

  ret->handlers =
    GNUNET_malloc (len * sizeof (struct GNUNET_MESH_MessageHandler));
  memset(ret->handlers, 0, len * sizeof(struct GNUNET_MESH_MessageHandler));
  memcpy (ret->handlers, handlers,
	  len * sizeof (struct GNUNET_MESH_MessageHandler));

  const static struct GNUNET_CORE_MessageHandler core_handlers[] = {
    {&core_receive, GNUNET_MESSAGE_TYPE_MESH, 0},
    {NULL, 0, 0}
  };

  ret->core = GNUNET_CORE_connect (cfg,
				   42,
				   ret,
				   &core_startup,
				   &core_connect,
				   &core_disconnect,
				   NULL,
				   NULL,
				   GNUNET_NO, NULL, GNUNET_NO, core_handlers);
  return ret;
}

void
GNUNET_MESH_disconnect (struct GNUNET_MESH_Handle *handle)
{
  GNUNET_free (handle->handlers);
  GNUNET_CORE_disconnect (handle->core);

  struct peer_list_element *element = handle->connected_peers.head;
  while (element != NULL)
    {
      struct peer_list_element *next = element->next;
      GNUNET_free (element);
      element = next;
    }

  struct tunnel_list_element *tunnel = handle->pending_tunnels.head;
  while (tunnel != NULL)
    {
      struct tunnel_list_element *next = tunnel->next;
      GNUNET_free (tunnel);
      tunnel = next;
    }
  tunnel = handle->established_tunnels.head;;
  while (tunnel != NULL)
    {
      struct tunnel_list_element *next = tunnel->next;
      GNUNET_free (tunnel);
      tunnel = next;
    }

  GNUNET_free (handle);
}

/* end of mesh_api.c */
