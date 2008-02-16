/*
     This file is part of GNUnet
     (C) 2002, 2003, 2004, 2005, 2006, 2007, 2008 Christian Grothoff (and other contributing authors)

     GNUnet is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License as published
     by the Free Software Foundation; either version 2, or (at your
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
 * @file transports/common.c
 * @brief Common features between TCP and UDP transports
 * @author Christian Grothoff
 */
#include "common.h"

static GNUNET_UPnP_ServiceAPI *upnp;

static struct GNUNET_IPv4NetworkSet *filteredNetworksIPv4;

static struct GNUNET_IPv4NetworkSet *allowedNetworksIPv4;

static struct GNUNET_IPv6NetworkSet *filteredNetworksIPv6;

static struct GNUNET_IPv6NetworkSet *allowedNetworksIPv6;

static struct GNUNET_GC_Configuration *cfg;

static struct GNUNET_Mutex *lock;

static GNUNET_TransportAPI myAPI;

/**
 * apis (our advertised API and the core api )
 */
static GNUNET_CoreAPIForTransport *coreAPI;

static GNUNET_Stats_ServiceAPI *stats;

static int available_protocols;

/**
 * Check if we are allowed to connect to the given IP.
 */
static int
is_blacklisted_ipv6 (const void *addr, unsigned int addr_len)
{
  GNUNET_IPv6Address ip;
  int ret;

  if (addr_len == sizeof (GNUNET_IPv6Address))
    {
      memcpy (&ip, addr, sizeof (GNUNET_IPv6Address));
    }
  else if (addr_len == sizeof (struct sockaddr_in6))
    {
      memcpy (&ip,
              &((struct sockaddr_in6 *) addr)->sin6_addr,
              sizeof (GNUNET_IPv6Address));
    }
  else
    {
      return GNUNET_SYSERR;
    }
  GNUNET_mutex_lock (lock);
  ret = GNUNET_check_ipv6_listed (filteredNetworksIPv6, ip);
  GNUNET_mutex_unlock (lock);
  return ret;
}

/**
 * Check if we are allowed to connect to the given IP.
 */
static int
is_whitelisted_ipv6 (const void *addr, unsigned int addr_len)
{
  GNUNET_IPv6Address ip;
  int ret;

  if (addr_len == sizeof (GNUNET_IPv6Address))
    {
      memcpy (&ip, addr, sizeof (GNUNET_IPv6Address));
    }
  else if (addr_len == sizeof (struct sockaddr_in6))
    {
      memcpy (&ip,
              &((struct sockaddr_in6 *) addr)->sin6_addr,
              sizeof (GNUNET_IPv6Address));
    }
  else
    {
      return GNUNET_SYSERR;
    }
  ret = GNUNET_OK;
  GNUNET_mutex_lock (lock);
  if (allowedNetworksIPv6 != NULL)
    ret = GNUNET_check_ipv6_listed (filteredNetworksIPv6, ip);
  GNUNET_mutex_unlock (lock);
  return ret;
}

static int
is_rejected_ipv6 (const void *addr, unsigned int addr_len)
{
  if ((GNUNET_YES == is_blacklisted_ipv6 (addr,
                                          addr_len)) ||
      (GNUNET_YES != is_whitelisted_ipv6 (addr, addr_len)))
    return GNUNET_YES;
  return GNUNET_NO;
}

/**
 * Check if we are allowed to connect to the given IP.
 */
static int
is_blacklisted_ipv4 (const void *addr, unsigned int addr_len)
{
  GNUNET_IPv4Address ip;
  int ret;

  if (addr_len == sizeof (struct sockaddr_in))
    {
      memcpy (&ip, &((struct sockaddr_in *) addr)->sin_addr,
              sizeof (GNUNET_IPv4Address));
    }
  else if (addr_len == sizeof (GNUNET_IPv4Address))
    {
      memcpy (&ip, addr, addr_len);
    }
  else
    {
      return GNUNET_SYSERR;
    }
  if (ip.addr == 0)
    return GNUNET_SYSERR;
  GNUNET_mutex_lock (lock);
  ret = GNUNET_check_ipv4_listed (filteredNetworksIPv4, ip);
  GNUNET_mutex_unlock (lock);
  return ret;
}

/**
 * Check if we are allowed to connect to the given IP.
 */
static int
is_whitelisted_ipv4 (const void *addr, unsigned int addr_len)
{
  GNUNET_IPv4Address ip;
  int ret;

  if (addr_len == sizeof (struct sockaddr_in))
    {
      memcpy (&ip, &((struct sockaddr_in *) addr)->sin_addr,
              sizeof (GNUNET_IPv4Address));
    }
  else if (addr_len == sizeof (GNUNET_IPv4Address))
    {
      memcpy (&ip, addr, addr_len);
    }
  else
    {
      return GNUNET_SYSERR;
    }
  ret = GNUNET_YES;
  GNUNET_mutex_lock (lock);
  if (allowedNetworksIPv4 != NULL)
    ret = GNUNET_check_ipv4_listed (allowedNetworksIPv4, ip);
  GNUNET_mutex_unlock (lock);
  return ret;
}

static int
is_rejected_ipv4 (const void *addr, unsigned int addr_len)
{
  if ((GNUNET_NO != is_blacklisted_ipv4 (addr,
                                         addr_len)) ||
      (GNUNET_YES != is_whitelisted_ipv4 (addr, addr_len)))
    return GNUNET_YES;
  return GNUNET_NO;
}

static int
is_rejected_tester (const void *addr, unsigned int addr_len)
{
  if (addr_len == sizeof (struct sockaddr_in))
    return is_rejected_ipv4 (addr, addr_len);
  return is_rejected_ipv6 (addr, addr_len);
}

/**
 * Verify that a Hello-Message is correct (a node
 * is reachable at that address). Since the reply
 * will be asynchronous, a method must be called on
 * success.
 * @param hello the Hello message to verify
 *        (the signature/crc have been verified before)
 * @return GNUNET_OK on success, GNUNET_SYSERR on error
 */
static int
verify_hello (const GNUNET_MessageHello * hello)
{
  HostAddress *haddr;

  haddr = (HostAddress *) & hello[1];
  if ((ntohs (hello->senderAddressSize) != sizeof (HostAddress)) ||
      (ntohs (hello->header.size) != GNUNET_sizeof_hello (hello)) ||
      (ntohs (hello->header.type) != GNUNET_P2P_PROTO_HELLO) ||
      (ntohs (hello->protocol) != myAPI.protocolNumber) ||
      (0 ==
       (ntohs (haddr->availability) &
        (VERSION_AVAILABLE_IPV6 | VERSION_AVAILABLE_IPV4)))
      || ((0 != (ntohs (haddr->availability) & VERSION_AVAILABLE_IPV4))
          &&
          ((GNUNET_YES ==
            is_blacklisted_ipv4 (&haddr->ipv4, sizeof (GNUNET_IPv4Address)))
           || (GNUNET_YES !=
               is_whitelisted_ipv4 (&haddr->ipv4,
                                    sizeof (GNUNET_IPv4Address)))))
      || ((0 != (ntohs (haddr->availability) & VERSION_AVAILABLE_IPV6))
          &&
          ((GNUNET_YES ==
            is_blacklisted_ipv6 (&haddr->ipv6, sizeof (GNUNET_IPv6Address)))
           || (GNUNET_YES !=
               is_whitelisted_ipv6 (&haddr->ipv6,
                                    sizeof (GNUNET_IPv6Address))))))
    return GNUNET_SYSERR;       /* invalid */
  return GNUNET_OK;
}


/**
 * Reload the configuration. Should never fail (keep old
 * configuration on error, syslog errors!)
 */
static int
reload_configuration (void *ctx,
                      struct GNUNET_GC_Configuration *cfg,
                      struct GNUNET_GE_Context *ectx,
                      const char *section, const char *option)
{
  char *ch;

  if (0 != strcmp (section, MY_TRANSPORT_NAME))
    return 0;                   /* fast path */

  GNUNET_mutex_lock (lock);
  GNUNET_free_non_null (filteredNetworksIPv4);
  GNUNET_free_non_null (allowedNetworksIPv4);
  ch = NULL;
  GNUNET_GC_get_configuration_value_string (cfg, MY_TRANSPORT_NAME,
                                            "BLACKLISTV4", "", &ch);
  filteredNetworksIPv4 = GNUNET_parse_ipv4_network_specification (ectx, ch);
  GNUNET_free (ch);
  ch = NULL;
  GNUNET_GC_get_configuration_value_string (cfg, MY_TRANSPORT_NAME,
                                            "WHITELISTV4", "", &ch);
  if (strlen (ch) > 0)
    allowedNetworksIPv4 = GNUNET_parse_ipv4_network_specification (ectx, ch);
  else
    allowedNetworksIPv4 = NULL;
  GNUNET_free (ch);
  GNUNET_free_non_null (filteredNetworksIPv6);
  GNUNET_free_non_null (allowedNetworksIPv6);
  GNUNET_GC_get_configuration_value_string (cfg, MY_TRANSPORT_NAME,
                                            "BLACKLISTV6", "", &ch);
  filteredNetworksIPv6 = GNUNET_parse_ipv6_network_specification (ectx, ch);
  GNUNET_free (ch);
  GNUNET_GC_get_configuration_value_string (cfg, MY_TRANSPORT_NAME,
                                            "WHITELISTV6", "", &ch);
  if (strlen (ch) > 0)
    allowedNetworksIPv6 = GNUNET_parse_ipv6_network_specification (ectx, ch);
  else
    allowedNetworksIPv6 = NULL;
  GNUNET_free (ch);
  GNUNET_mutex_unlock (lock);
  /* TODO: error handling! */
  return 0;
}

/**
 * Get the GNUnet port from the configuration,
 * or from /etc/services if it is not specified in
 * the config file.
 */
static unsigned short
get_port ()
{
  struct servent *pse;          /* pointer to service information entry        */
  unsigned long long port;

  if (-1 == GNUNET_GC_get_configuration_value_number (cfg,
                                                      MY_TRANSPORT_NAME,
                                                      "PORT", 0, 65535, 2086,
                                                      &port))
    {
      if ((pse = getservbyname ("gnunet", MY_TRANSPORT_NAME)))
        port = htons (pse->s_port);
      else
        port = 0;
    }
  return (unsigned short) port;
}

/**
 * Get the GNUnet advertised port from the configuration.
 */
static unsigned short
get_advertised_port ()
{
  unsigned long long port;

  if (!GNUNET_GC_have_configuration_value
      (coreAPI->cfg, MY_TRANSPORT_NAME, "ADVERTISED-PORT"))
    {
      port = get_port ();
    }
  else if (-1 == GNUNET_GC_get_configuration_value_number (coreAPI->cfg,
                                                           MY_TRANSPORT_NAME,
                                                           "ADVERTISED-PORT",
                                                           0, 65535, 80,
                                                           &port))
    port = get_port ();
  return (unsigned short) port;
}

/**
 * Create a hello-Message for the current node. The hello is
 * created without signature and without a timestamp. The
 * GNUnet core will GNUNET_RSA_sign the message and add an expiration time.
 *
 * @return hello on success, NULL on error
 */
static GNUNET_MessageHello *
create_hello ()
{
  static GNUNET_IPv4Address last_addrv4;
  GNUNET_MessageHello *msg;
  HostAddress *haddr;
  unsigned short port;
  unsigned short available;

  port = get_advertised_port ();
  if (0 == port)
    {
      static int once = 0;
      if (once == 0)
        {
          once = 1;
          GNUNET_GE_LOG (coreAPI->ectx,
                         GNUNET_GE_INFO | GNUNET_GE_USER | GNUNET_GE_BULK,
                         _("Port is 0, will only send using %s.\n"),
                         MY_TRANSPORT_NAME);
        }
      return NULL;              /* TCP transport is configured SEND-only! */
    }
  msg = GNUNET_malloc (sizeof (GNUNET_MessageHello) + sizeof (HostAddress));
  haddr = (HostAddress *) & msg[1];

  available = VERSION_AVAILABLE_NONE;
  if ((((upnp != NULL) &&
        (GNUNET_OK == upnp->get_ip (port,
                                    MY_TRANSPORT_NAME,
                                    &haddr->ipv4))) ||
       (GNUNET_SYSERR !=
        GNUNET_IP_get_public_ipv4_address (cfg, coreAPI->ectx,
                                           &haddr->ipv4))))
    {
      if (0 != memcmp (&haddr->ipv4,
                       &last_addrv4, sizeof (GNUNET_IPv4Address)))
        {
          GNUNET_GE_LOG (coreAPI->ectx,
                         GNUNET_GE_DEBUG | GNUNET_GE_USER | GNUNET_GE_BULK,
                         "%s uses IPv4 address %u.%u.%u.%u.\n",
                         MY_TRANSPORT_NAME,
                         GNUNET_PRIP (ntohl (*(int *) &haddr->ipv4)));
          last_addrv4 = haddr->ipv4;
        }
      available |= VERSION_AVAILABLE_IPV4;
    }

  if (GNUNET_SYSERR !=
      GNUNET_IP_get_public_ipv6_address (cfg, coreAPI->ectx, &haddr->ipv6))
    {
      available |= VERSION_AVAILABLE_IPV6;
    }
  if (available == VERSION_AVAILABLE_NONE)
    {
      GNUNET_free (msg);
      return NULL;
    }
  haddr->port = htons (port);
  haddr->availability = htons (available);
  msg->senderAddressSize = htons (sizeof (HostAddress));
  msg->protocol = htons (myAPI.protocolNumber);
  msg->MTU = htonl (myAPI.mtu);
  return msg;
}

/**
 * Convert TCP hello to IP address
 */
static int
hello_to_address (const GNUNET_MessageHello * hello,
                  void **sa, unsigned int *sa_len)
{
  const HostAddress *haddr = (const HostAddress *) &hello[1];
  struct sockaddr_in *serverAddr4;
  struct sockaddr_in6 *serverAddr6;
  unsigned short available;

  available = ntohs (haddr->availability);
  if (0 != (available & VERSION_AVAILABLE_IPV4))
    {
      *sa_len = sizeof (struct sockaddr_in);
      serverAddr4 = GNUNET_malloc (sizeof (struct sockaddr_in));
      *sa = serverAddr4;
      memset (serverAddr4, 0, sizeof (struct sockaddr_in));
      serverAddr4->sin_family = AF_INET;
      memcpy (&serverAddr4->sin_addr, &haddr->ipv4,
              sizeof (GNUNET_IPv4Address));
      serverAddr4->sin_port = haddr->port;
    }
  else if (0 != (available & VERSION_AVAILABLE_IPV6))
    {
      *sa_len = sizeof (struct sockaddr_in6);
      serverAddr6 = GNUNET_malloc (sizeof (struct sockaddr_in6));
      *sa = serverAddr6;
      memset (serverAddr6, 0, sizeof (struct sockaddr_in6));
      serverAddr6->sin6_family = AF_INET6;
      memcpy (&serverAddr6->sin6_addr, &haddr->ipv6,
              sizeof (GNUNET_IPv6Address));
      serverAddr6->sin6_port = haddr->port;
    }
  else
    return GNUNET_SYSERR;
  return GNUNET_OK;
}


static void
do_shutdown ()
{
  GNUNET_GC_detach_change_listener (cfg, &reload_configuration, NULL);
  if (stats != NULL)
    {
      coreAPI->release_service (stats);
      stats = NULL;
    }
  if (upnp != NULL)
    {
      coreAPI->release_service (upnp);
      upnp = NULL;
    }
  GNUNET_free_non_null (filteredNetworksIPv4);
  filteredNetworksIPv4 = NULL;
  GNUNET_free_non_null (allowedNetworksIPv4);
  allowedNetworksIPv4 = NULL;
  GNUNET_free_non_null (filteredNetworksIPv6);
  filteredNetworksIPv6 = NULL;
  GNUNET_free_non_null (allowedNetworksIPv6);
  allowedNetworksIPv6 = NULL;
  GNUNET_mutex_destroy (lock);
  lock = NULL;
}
