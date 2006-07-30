/*
      This file is part of GNUnet
      (C) 2002, 2003, 2004, 2005, 2006 Christian Grothoff (and other contributing authors)

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
 * @file include/gnunet_stats_lib.h
 * @brief convenience API to the STATS service
 * @author Christian Grothoff
 */

#ifndef GNUNET_STATS_LIB_H
#define GNUNET_STATS_LIB_H

#include "gnunet_util.h"
#include "gnunet_util_network_client.h"

#ifdef __cplusplus
extern "C" {
#if 0 /* keep Emacsens' auto-indent happy */
}
#endif
#endif

#define STATS_VERSION "3.0.0"

/**
 * Return a descriptive name for a p2p message type
 */
const char * p2pMessageName(unsigned short type);

/**
 * Return a descriptive name for a client server message type
 */
const char * csMessageName(unsigned short type);

/**
 * @param name the name of the datum
 * @param value the value
 * @return OK to continue, SYSERR to abort iteration
 */
typedef int (*StatisticsProcessor)(const char * name,
				   unsigned long long value,
				   void * cls);

/**
 * Request statistics from TCP socket.
 * @param sock the socket to use
 * @param processor function to call on each value
 * @return OK on success, SYSERR on error
 */
int requestStatistics(struct GE_Context * ectx,
		      struct ClientServerConnection * sock,
		      StatisticsProcessor processor,
		      void * cls);

/**
 * @param type the type ID of the message
 * @param isP2P YES for P2P, NO for CS types
 * @return OK to continue, SYSERR to abort iteration
 */
typedef int (*ProtocolProcessor)(unsigned short type,
				 int isP2P,
				 void * cls);

/**
 * Request available protocols from TCP socket.
 * @param sock the socket to use
 * @param processor function to call on each value
 * @return OK on success, SYSERR on error
 */
int requestAvailableProtocols(struct GE_Context * ectx,
			      struct ClientServerConnection * sock,
			      ProtocolProcessor processor,
			      void * cls);

#if 0 /* keep Emacsens' auto-indent happy */
{
#endif
#ifdef __cplusplus
}
#endif

#endif
