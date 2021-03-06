/*
     This file is part of GNUnet.
     Copyright (C) 2016 GNUnet e.V.

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
     Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
     Boston, MA 02110-1301, USA.
*/

/**
 * @author Martin Schanzenbach
 *
 * @file
 * Identity provider service; implements identity provider for GNUnet
 *
 * @defgroup identity-provider  Identity Provider service
 * @{
 */
#ifndef GNUNET_IDENTITY_PROVIDER_SERVICE_H
#define GNUNET_IDENTITY_PROVIDER_SERVICE_H

#ifdef __cplusplus
extern "C"
{
#if 0                           /* keep Emacsens' auto-indent happy */
}
#endif
#endif

#include "gnunet_util_lib.h"


/**
 * Version number of GNUnet Identity Provider API.
 */
#define GNUNET_IDENTITY_PROVIDER_VERSION 0x00000000

/**
 * Handle to access the identity service.
 */
struct GNUNET_IDENTITY_PROVIDER_Handle;

/**
 * Handle for a token.
 */
struct GNUNET_IDENTITY_PROVIDER_Token;

/**
 * Handle for a ticket
 */
struct GNUNET_IDENTITY_PROVIDER_Ticket;

/**
 * Handle for an operation with the identity provider service.
 */
struct GNUNET_IDENTITY_PROVIDER_Operation;

/**
 * Method called when a token has been exchanged for a ticket.
 * On success returns a token
 *
 * @param cls closure
 * @param token the token
 */
typedef void
(*GNUNET_IDENTITY_PROVIDER_ExchangeCallback)(void *cls,
                            const struct GNUNET_IDENTITY_PROVIDER_Token *token,
                            uint64_t ticket_nonce);

/**
 * Method called when a token has been issued.
 * On success returns a ticket that can be given to the audience to retrive the
 * token
 *
 * @param cls closure
 * @param grant the label in GNS pointing to the token
 * @param ticket the ticket
 * @param token the issued token
 * @param name name assigned by the user for this ego,
 *                   NULL if the user just deleted the ego and it
 *                   must thus no longer be used
 */
typedef void
(*GNUNET_IDENTITY_PROVIDER_IssueCallback)(void *cls,
                            const char *grant,
                            const struct GNUNET_IDENTITY_PROVIDER_Ticket *ticket,
                            const struct GNUNET_IDENTITY_PROVIDER_Token *token);


/**
 * Connect to the identity provider service.
 *
 * @param cfg Configuration to contact the identity provider service.
 * @return handle to communicate with identity provider service
 */
struct GNUNET_IDENTITY_PROVIDER_Handle *
GNUNET_IDENTITY_PROVIDER_connect (const struct GNUNET_CONFIGURATION_Handle *cfg);


/**
 * Issue a token for a specific audience.
 *
 * @param id identity provider service to use
 * @param iss issuer (identity)
 * @param aud audience (identity)
 * @param scope the identity attributes requested, comman separated
 * @param expiration the token expiration
 * @param nonce the nonce that will be included in token and ticket
 * @param cb callback to call with result
 * @param cb_cls closure
 * @return handle to abort the operation
 */
struct GNUNET_IDENTITY_PROVIDER_Operation *
GNUNET_IDENTITY_PROVIDER_issue_token (struct GNUNET_IDENTITY_PROVIDER_Handle *id,
		     const struct GNUNET_CRYPTO_EcdsaPrivateKey *iss_key,
         const struct GNUNET_CRYPTO_EcdsaPublicKey *aud_key,
         const char* scope,
         struct GNUNET_TIME_Absolute expiration,
         uint64_t nonce,
		     GNUNET_IDENTITY_PROVIDER_IssueCallback cb,
		     void *cb_cls);


/**
 * Exchange a ticket for a token. Intended to be used by audience that
 * received a ticket.
 *
 * @param id identity provider service to use
 * @param ticket the ticket to exchange
 * @param aud_privkey the audience of the ticket
 * @param cont function to call once the operation finished
 * @param cont_cls closure for @a cont
 * @return handle to abort the operation
 */
struct GNUNET_IDENTITY_PROVIDER_Operation *
GNUNET_IDENTITY_PROVIDER_exchange_ticket (struct GNUNET_IDENTITY_PROVIDER_Handle *id,
		     const struct GNUNET_IDENTITY_PROVIDER_Ticket *ticket,
         const struct GNUNET_CRYPTO_EcdsaPrivateKey *aud_privkey,
		     GNUNET_IDENTITY_PROVIDER_ExchangeCallback cont,
		     void *cont_cls);


/**
 * Disconnect from identity provider service.
 *
 * @param h identity provider service to disconnect
 */
void
GNUNET_IDENTITY_PROVIDER_disconnect (struct GNUNET_IDENTITY_PROVIDER_Handle *h);


/**
 * Cancel an identity provider operation.  Note that the operation MAY still
 * be executed; this merely cancels the continuation; if the request
 * was already transmitted, the service may still choose to complete
 * the operation.
 *
 * @param op operation to cancel
 */
void
GNUNET_IDENTITY_PROVIDER_cancel (struct GNUNET_IDENTITY_PROVIDER_Operation *op);


/**
 * Convenience API
 */

/**
 * Destroy token
 *
 * @param token the token
 */
void
GNUNET_IDENTITY_PROVIDER_token_destroy(struct GNUNET_IDENTITY_PROVIDER_Token *token);

/**
 * Returns string representation of token. A JSON-Web-Token.
 *
 * @param token the token
 * @return The JWT (must be freed)
 */
char *
GNUNET_IDENTITY_PROVIDER_token_to_string (const struct GNUNET_IDENTITY_PROVIDER_Token *token);

/**
 * Returns string representation of ticket. Base64-Encoded
 *
 * @param ticket the ticket
 * @return the Base64-Encoded ticket
 */
char *
GNUNET_IDENTITY_PROVIDER_ticket_to_string (const struct GNUNET_IDENTITY_PROVIDER_Ticket *ticket);

/**
 * Created a ticket from a string (Base64 encoded ticket)
 *
 * @param input Base64 encoded ticket
 * @param ticket pointer where the ticket is stored
 * @return GNUNET_OK
 */
int
GNUNET_IDENTITY_PROVIDER_string_to_ticket (const char* input,
                                           struct GNUNET_IDENTITY_PROVIDER_Ticket **ticket);

/**
 * Destroys a ticket
 *
 * @param ticket the ticket to destroy
 */
void
GNUNET_IDENTITY_PROVIDER_ticket_destroy(struct GNUNET_IDENTITY_PROVIDER_Ticket *ticket);

#if 0                           /* keep Emacsens' auto-indent happy */
{
#endif
#ifdef __cplusplus
}
#endif


/* ifndef GNUNET_IDENTITY_PROVIDER_SERVICE_H */
#endif

/** @} */ /* end of group identity */

/* end of gnunet_identity_provider_service.h */
