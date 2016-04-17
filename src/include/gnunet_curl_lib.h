/*
  This file is part of GNUnet
  Copyright (C) 2014, 2015, 2016 GNUnet e.V.

  GNUnet is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  GNUnet is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  GNUnet; see the file COPYING.  If not, If not, see
  <http://www.gnu.org/licenses/>
*/
/**
 * @file src/include/gnunet_curl_lib.h
 * @brief library to make it easy to download JSON replies over HTTP
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 * @author Christian Grothoff
 *
 * @defgroup curl CURL integration library
 * Download JSON using libcurl.
 * @{
 */
#ifndef GNUNET_CURL_LIB_H
#define GNUNET_CURL_LIB_H
#include <curl/curl.h>
#include <jansson.h>
#include <gnunet/gnunet_util_lib.h>


/**
 * Initialise this library.  This function should be called before using any of
 * the following functions.
 *
 * @return library context
 */
struct GNUNET_CURL_Context *
GNUNET_CURL_init (void);


/**
 * Obtain the information for a select() call to wait until
 * #GNUNET_CURL_perform() is ready again.  Note that calling
 * any other TALER_EXCHANGE-API may also imply that the library
 * is again ready for #GNUNET_CURL_perform().
 *
 * Basically, a client should use this API to prepare for select(),
 * then block on select(), then call #GNUNET_CURL_perform() and then
 * start again until the work with the context is done.
 *
 * This function will NOT zero out the sets and assumes that @a max_fd
 * and @a timeout are already set to minimal applicable values.  It is
 * safe to give this API FD-sets and @a max_fd and @a timeout that are
 * already initialized to some other descriptors that need to go into
 * the select() call.
 *
 * @param ctx context to get the event loop information for
 * @param read_fd_set will be set for any pending read operations
 * @param write_fd_set will be set for any pending write operations
 * @param except_fd_set is here because curl_multi_fdset() has this argument
 * @param max_fd set to the highest FD included in any set;
 *        if the existing sets have no FDs in it, the initial
 *        value should be "-1". (Note that `max_fd + 1` will need
 *        to be passed to select().)
 * @param timeout set to the timeout in milliseconds (!); -1 means
 *        no timeout (NULL, blocking forever is OK), 0 means to
 *        proceed immediately with #GNUNET_CURL_perform().
 */
void
GNUNET_CURL_get_select_info (struct GNUNET_CURL_Context *ctx,
                             fd_set *read_fd_set,
                             fd_set *write_fd_set,
                             fd_set *except_fd_set,
                             int *max_fd,
                             long *timeout);


/**
 * Run the main event loop for the Taler interaction.
 *
 * @param ctx the library context
 */
void
GNUNET_CURL_perform (struct GNUNET_CURL_Context *ctx);


/**
 * Cleanup library initialisation resources.  This function should be called
 * after using this library to cleanup the resources occupied during library's
 * initialisation.
 *
 * @param ctx the library context
 */
void
GNUNET_CURL_fini (struct GNUNET_CURL_Context *ctx);


/**
 * Entry in the context's job queue.
 */
struct GNUNET_CURL_Job;

/**
 * Function to call upon completion of a job.
 *
 * @param cls closure
 * @param response_code HTTP response code from server, 0 on hard error
 * @param json response, NULL if response was not in JSON format
 */
typedef void
(*GNUNET_CURL_JobCompletionCallback)(void *cls,
                                     long response_code,
                                     const json_t *json);


/**
 * Schedule a CURL request to be executed and call the given @a jcc
 * upon its completion. Note that the context will make use of the
 * CURLOPT_PRIVATE facility of the CURL @a eh.
 *
 * This function modifies the CURL handle to add the
 * "Content-Type: application/json" header if @a add_json is set.
 *
 * @param ctx context to execute the job in
 * @param eh curl easy handle for the request, will
 *           be executed AND cleaned up
 * @param add_json add "application/json" content type header
 * @param jcc callback to invoke upon completion
 * @param jcc_cls closure for @a jcc
 * @return NULL on error (in this case, @eh is still released!)
 */
struct GNUNET_CURL_Job *
GNUNET_CURL_job_add (struct GNUNET_CURL_Context *ctx,
                     CURL *eh,
                     int add_json,
                     GNUNET_CURL_JobCompletionCallback jcc,
                     void *jcc_cls);


/**
 * Cancel a job.  Must only be called before the job completion
 * callback is called for the respective job.
 *
 * @param job job to cancel
 */
void
GNUNET_CURL_job_cancel (struct GNUNET_CURL_Job *job);

#endif
/** @} */  /* end of group */

/* end of gnunet_curl_lib.h */
