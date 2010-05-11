/*
     This file is part of GNUnet.
     (C) 2001, 2002, 2006, 2008, 2009 Christian Grothoff (and other contributing authors)

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
 * @file util/client.c
 * @brief code for access to services
 * @author Christian Grothoff
 *
 * Generic TCP code for reliable, record-oriented TCP
 * connections between clients and service providers.
 */

#include "platform.h"
#include "gnunet_common.h"
#include "gnunet_client_lib.h"
#include "gnunet_protocols.h"
#include "gnunet_server_lib.h"
#include "gnunet_scheduler_lib.h"

#define DEBUG_CLIENT GNUNET_NO

/**
 * How often do we re-try tranmsitting requests before giving up?
 * Note that if we succeeded transmitting a request but failed to read
 * a response, we do NOT re-try.
 */
#define MAX_ATTEMPTS 50


/**
 * Handle for a transmission request.
 */
struct GNUNET_CLIENT_TransmitHandle
{
  /**
   * Connection state.
   */
  struct GNUNET_CLIENT_Connection *sock;

  /**
   * Function to call to get the data for transmission.
   */
  GNUNET_CONNECTION_TransmitReadyNotify notify;

  /**
   * Closure for notify.
   */
  void *notify_cls;

  /**
   * Handle to the transmission with the underlying
   * connection.
   */
  struct GNUNET_CONNECTION_TransmitHandle *th;

  /**
   * If we are re-trying and are delaying to do so,
   * handle to the scheduled task managing the delay.
   */
  GNUNET_SCHEDULER_TaskIdentifier reconnect_task;

  /**
   * Timeout for the operation overall.
   */
  struct GNUNET_TIME_Absolute timeout;

  /**
   * Number of bytes requested.
   */
  size_t size;

  /**
   * Are we allowed to re-try to connect without telling
   * the user (of this API) about the connection troubles?
   */
  int auto_retry;

  /**
   * Number of attempts left for transmitting the request.  We may
   * fail the first time (say because the service is not yet up), in
   * which case (if auto_retry is set) we wait a bit and re-try
   * (timeout permitting).
   */
  unsigned int attempts_left;

};


/**
 * Context for processing 
 * "GNUNET_CLIENT_transmit_and_get_response" requests.
 */
struct TransmitGetResponseContext
{
  /**
   * Client handle.
   */
  struct GNUNET_CLIENT_Connection *sock;

  /**
   * Message to transmit; do not free, allocated
   * right after this struct.
   */
  const struct GNUNET_MessageHeader *hdr;

  /**
   * Timeout to use.
   */
  struct GNUNET_TIME_Absolute timeout;

  /**
   * Function to call when done.
   */
  GNUNET_CLIENT_MessageHandler rn;

  /**
   * Closure for "rn".
   */
  void *rn_cls;
};

/**
 * Context for handling the shutdown of a service.
 */
struct ShutdownContext
{
  /**
   * Scheduler to be used to call continuation
   */
  struct GNUNET_SCHEDULER_Handle *sched;
  /**
   * Connection to the service that is being shutdown.
   */
  struct GNUNET_CLIENT_Connection *sock;

  /**
   * Time allowed for shutdown to happen.
   */
  struct GNUNET_TIME_Absolute timeout;

  /**
   * Task set up to cancel the shutdown request on timeout.
   */
  GNUNET_SCHEDULER_TaskIdentifier cancel_task;

  /**
   * Task to call once shutdown complete
   */
  GNUNET_CLIENT_ShutdownTask cont;

  /**
   * Closure for shutdown continuation
   */
  void *cont_cls;

  /**
   * We received a confirmation that the service will shut down.
   */
  int confirmed;

};

/**
 * Struct to refer to a GNUnet TCP connection.
 * This is more than just a socket because if the server
 * drops the connection, the client automatically tries
 * to reconnect (and for that needs connection information).
 */
struct GNUNET_CLIENT_Connection
{

  /**
   * the socket handle, NULL if not live
   */
  struct GNUNET_CONNECTION_Handle *sock;

  /**
   * Our scheduler.
   */
  struct GNUNET_SCHEDULER_Handle *sched;

  /**
   * Our configuration.
   */
  struct GNUNET_CONFIGURATION_Handle *cfg;

  /**
   * Name of the service we interact with.
   */
  char *service_name;

  /**
   * Context of a transmit_and_get_response operation, NULL
   * if no such operation is pending.
   */
  struct TransmitGetResponseContext *tag;

  /**
   * Handler for current receiver task.
   */
  GNUNET_CLIENT_MessageHandler receiver_handler;

  /**
   * Closure for receiver_handler.
   */
  void *receiver_handler_cls;

  /**
   * Handle for a pending transmission request, NULL if there is
   * none pending.
   */
  struct GNUNET_CLIENT_TransmitHandle *th;

  /**
   * Handler for service test completion (NULL unless in service_test)
   */
  GNUNET_SCHEDULER_Task test_cb;

  /**
   * Deadline for calling 'test_cb'.
   */
  struct GNUNET_TIME_Absolute test_deadline;

  /**
   * If we are re-trying and are delaying to do so,
   * handle to the scheduled task managing the delay.
   */
  GNUNET_SCHEDULER_TaskIdentifier receive_task;

  /**
   * Closure for test_cb (NULL unless in service_test)
   */
  void *test_cb_cls;

  /**
   * Buffer for received message.
   */
  char *received_buf;

  /**
   * Timeout for receiving a response (absolute time).
   */
  struct GNUNET_TIME_Absolute receive_timeout;

  /**
   * Current value for our incremental back-off (for
   * connect re-tries).
   */
  struct GNUNET_TIME_Relative back_off;

  /**
   * Number of bytes in received_buf that are valid.
   */
  size_t received_pos;

  /**
   * Size of received_buf.
   */
  unsigned int received_size;

  /**
   * Do we have a complete response in received_buf?
   */
  int msg_complete;

  /**
   * Are we currently busy doing receive-processing?
   * GNUNET_YES if so, GNUNET_NO if not.
   */
  int in_receive;

  /**
   * Are we ignoring shutdown signals?
   */
  int ignore_shutdown;

};


static struct GNUNET_CONNECTION_Handle *
do_connect (struct GNUNET_SCHEDULER_Handle *sched,
            const char *service_name,
            const struct GNUNET_CONFIGURATION_Handle *cfg)
{
  struct GNUNET_CONNECTION_Handle *sock;
  char *hostname;
  unsigned long long port;

  if ((GNUNET_OK !=
       GNUNET_CONFIGURATION_get_value_number (cfg,
                                              service_name,
                                              "PORT",
                                              &port)) ||
      (port > 65535) ||
      (GNUNET_OK !=
       GNUNET_CONFIGURATION_get_value_string (cfg,
                                              service_name,
                                              "HOSTNAME", &hostname)))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  _
                  ("Could not determine valid hostname and port for service `%s' from configuration.\n"),
                  service_name);
      return NULL;
    }
  if (0 == strlen (hostname))
    {
      GNUNET_free (hostname);
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  _("Need a non-empty hostname for service `%s'.\n"),
                  service_name);
      return NULL;
    }
  sock = GNUNET_CONNECTION_create_from_connect (sched,
                                                cfg,
                                                hostname,
                                                port,
                                                GNUNET_SERVER_MAX_MESSAGE_SIZE);
  GNUNET_free (hostname);
  return sock;
}


/**
 * Get a connection with a service.
 *
 * @param sched scheduler to use
 * @param service_name name of the service
 * @param cfg configuration to use
 * @return NULL on error (service unknown to configuration)
 */
struct GNUNET_CLIENT_Connection *
GNUNET_CLIENT_connect (struct GNUNET_SCHEDULER_Handle *sched,
                       const char *service_name,
                       const struct GNUNET_CONFIGURATION_Handle *cfg)
{
  struct GNUNET_CLIENT_Connection *ret;
  struct GNUNET_CONNECTION_Handle *sock;

  sock = do_connect (sched, service_name, cfg);
  if (sock == NULL)
    return NULL;
  ret = GNUNET_malloc (sizeof (struct GNUNET_CLIENT_Connection));
  ret->sock = sock;
  ret->sched = sched;
  ret->service_name = GNUNET_strdup (service_name);
  ret->cfg = GNUNET_CONFIGURATION_dup (cfg);
  ret->back_off = GNUNET_TIME_UNIT_MILLISECONDS;
  return ret;
}


/**
 * Configure this connection to ignore shutdown signals.
 *
 * @param h client handle
 * @param do_ignore GNUNET_YES to ignore, GNUNET_NO to restore default
 */
void
GNUNET_CLIENT_ignore_shutdown (struct GNUNET_CLIENT_Connection *h,
			       int do_ignore)
{
  h->ignore_shutdown = do_ignore;
  if (h->sock != NULL)
    GNUNET_CONNECTION_ignore_shutdown (h->sock,
				       do_ignore);
}


/**
 * Destroy connection with the service.  This will automatically
 * cancel any pending "receive" request (however, the handler will
 * *NOT* be called, not even with a NULL message).  Any pending
 * transmission request will also be cancelled UNLESS the callback for
 * the transmission request has already been called, in which case the
 * transmission 'finish_pending_write' argument determines whether or
 * not the write is guaranteed to complete before the socket is fully
 * destroyed (unless, of course, there is an error with the server in
 * which case the message may still be lost).
 *
 * @param finish_pending_write should a transmission already passed to the
 *          handle be completed?
 * @param sock handle to the service connection
 */
void
GNUNET_CLIENT_disconnect (struct GNUNET_CLIENT_Connection *sock,
			  int finish_pending_write)
{
  GNUNET_assert (sock->sock != NULL);
  if (sock->in_receive == GNUNET_YES)
    {
      GNUNET_CONNECTION_receive_cancel (sock->sock);
      sock->in_receive = GNUNET_NO;
    }
  GNUNET_CONNECTION_destroy (sock->sock, finish_pending_write);
  sock->sock = NULL;
  if (sock->tag != NULL)
    {
      GNUNET_free (sock->tag);
      sock->tag = NULL;
    }
  sock->receiver_handler = NULL;
  if (sock->th != NULL)
    GNUNET_CLIENT_notify_transmit_ready_cancel (sock->th);
  if (sock->receive_task != GNUNET_SCHEDULER_NO_TASK)
    {
      GNUNET_SCHEDULER_cancel (sock->sched, sock->receive_task);
      sock->receive_task = GNUNET_SCHEDULER_NO_TASK;
    }
  GNUNET_array_grow (sock->received_buf, sock->received_size, 0);
  GNUNET_free (sock->service_name);
  GNUNET_CONFIGURATION_destroy (sock->cfg);
  GNUNET_free (sock);
}


/**
 * Check if message is complete
 */
static void
check_complete (struct GNUNET_CLIENT_Connection *conn)
{
  if ((conn->received_pos >= sizeof (struct GNUNET_MessageHeader)) &&
      (conn->received_pos >=
       ntohs (((const struct GNUNET_MessageHeader *) conn->received_buf)->
              size)))
    conn->msg_complete = GNUNET_YES;
}


/**
 * Callback function for data received from the network.  Note that
 * both "available" and "errCode" would be 0 if the read simply timed out.
 *
 * @param cls closure
 * @param buf pointer to received data
 * @param available number of bytes availabe in "buf",
 *        possibly 0 (on errors)
 * @param addr address of the sender
 * @param addrlen size of addr
 * @param errCode value of errno (on errors receiving)
 */
static void
receive_helper (void *cls,
                const void *buf,
                size_t available,
                const struct sockaddr *addr, socklen_t addrlen, int errCode)
{
  struct GNUNET_CLIENT_Connection *conn = cls;
  struct GNUNET_TIME_Relative remaining;
  GNUNET_CLIENT_MessageHandler receive_handler;
  void *receive_handler_cls;

  GNUNET_assert (conn->msg_complete == GNUNET_NO);
  conn->in_receive = GNUNET_NO;
  if ((available == 0) || (conn->sock == NULL) || (errCode != 0))
    {
      /* signal timeout! */
      if (NULL != (receive_handler = conn->receiver_handler))
        {
          receive_handler_cls = conn->receiver_handler_cls;
          conn->receiver_handler = NULL;
          receive_handler (receive_handler_cls, NULL);
        }
      return;
    }

  /* FIXME: optimize for common fast case where buf contains the
     entire message and we need no copying... */


  /* slow path: append to array */
  if (conn->received_size < conn->received_pos + available)
    GNUNET_array_grow (conn->received_buf,
                       conn->received_size, conn->received_pos + available);
  memcpy (&conn->received_buf[conn->received_pos], buf, available);
  conn->received_pos += available;
  check_complete (conn);
  /* check for timeout */
  remaining = GNUNET_TIME_absolute_get_remaining (conn->receive_timeout);
  if (remaining.value == 0)
    {
      /* signal timeout! */
      conn->receiver_handler (conn->receiver_handler_cls, NULL);
      return;
    }
  /* back to receive -- either for more data or to call callback! */
  GNUNET_CLIENT_receive (conn,
                         conn->receiver_handler,
                         conn->receiver_handler_cls, remaining);
}


/**
 * Continuation to call the receive callback.
 *
 * @param cls  our handle to the client connection
 * @param tc scheduler context
 */
static void
receive_task (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct GNUNET_CLIENT_Connection *sock = cls;
  GNUNET_CLIENT_MessageHandler handler = sock->receiver_handler;
  const struct GNUNET_MessageHeader *cmsg =
    (const struct GNUNET_MessageHeader *) sock->received_buf;
  void *handler_cls = sock->receiver_handler_cls;
  uint16_t msize = ntohs (cmsg->size);
  char mbuf[msize];
  struct GNUNET_MessageHeader *msg = (struct GNUNET_MessageHeader *) mbuf;

  sock->receive_task = GNUNET_SCHEDULER_NO_TASK;
  GNUNET_assert (GNUNET_YES == sock->msg_complete);
  GNUNET_assert (sock->received_pos >= msize);
  memcpy (msg, cmsg, msize);
  memmove (sock->received_buf,
           &sock->received_buf[msize], sock->received_pos - msize);
  sock->received_pos -= msize;
  sock->msg_complete = GNUNET_NO;
  sock->receiver_handler = NULL;
  check_complete (sock);
  if (handler != NULL)
    handler (handler_cls, msg);
}


/**
 * Read from the service.
 *
 * @param sock the service
 * @param handler function to call with the message
 * @param handler_cls closure for handler
 * @param timeout how long to wait until timing out
 */
void
GNUNET_CLIENT_receive (struct GNUNET_CLIENT_Connection *sock,
                       GNUNET_CLIENT_MessageHandler handler,
                       void *handler_cls, struct GNUNET_TIME_Relative timeout)
{
  if (sock->sock == NULL)
    {
      /* already disconnected, fail instantly! */
      GNUNET_break (0);         /* this should not happen in well-written code! */
      handler (handler_cls, NULL);
      return;
    }
  sock->receiver_handler = handler;
  sock->receiver_handler_cls = handler_cls;
  sock->receive_timeout = GNUNET_TIME_relative_to_absolute (timeout);
  if (GNUNET_YES == sock->msg_complete)
    {
      sock->receive_task = GNUNET_SCHEDULER_add_after (sock->sched,
                                                       GNUNET_SCHEDULER_NO_TASK,
                                                       &receive_task, sock);
    }
  else
    {
      sock->in_receive = GNUNET_YES;
      GNUNET_CONNECTION_receive (sock->sock,
                                 GNUNET_SERVER_MAX_MESSAGE_SIZE,
                                 timeout, &receive_helper, sock);
    }
}


/**
 * Handler receiving response to service shutdown requests.
 * First call with NULL: service misbehaving, or something.
 * First call with GNUNET_MESSAGE_TYPE_SHUTDOWN_ACK:
 *   - service will shutdown
 * First call with GNUNET_MESSAGE_TYPE_SHUTDOWN_REFUSE:
 *   - service will not be stopped!
 *
 * Second call with NULL:
 *   - service has now really shut down.
 *
 * @param cls closure
 * @param msg NULL, indicating socket closure.
 */
static void
service_shutdown_handler (void *cls, const struct GNUNET_MessageHeader *msg)
{
  struct ShutdownContext *shutdown_ctx = cls;

  if ((msg == NULL) && (shutdown_ctx->confirmed != GNUNET_YES))   
    {
      /* Means the other side closed the connection and never confirmed a shutdown */
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING, 
		  "Service handle shutdown before ACK!\n");
      if (shutdown_ctx->cont != NULL)
        shutdown_ctx->cont(shutdown_ctx->cont_cls, GNUNET_SYSERR);      
      GNUNET_SCHEDULER_cancel(shutdown_ctx->sched, shutdown_ctx->cancel_task);
      GNUNET_CLIENT_disconnect (shutdown_ctx->sock, GNUNET_NO);
      GNUNET_free(shutdown_ctx);
    }
  else if ((msg == NULL) && (shutdown_ctx->confirmed == GNUNET_YES))
    {
      GNUNET_log(GNUNET_ERROR_TYPE_WARNING, 
		 "Service shutdown complete.\n");
      if (shutdown_ctx->cont != NULL)
        shutdown_ctx->cont(shutdown_ctx->cont_cls, GNUNET_NO);

      GNUNET_SCHEDULER_cancel(shutdown_ctx->sched, shutdown_ctx->cancel_task);
      GNUNET_CLIENT_disconnect (shutdown_ctx->sock, GNUNET_NO);
      GNUNET_free(shutdown_ctx);
    }
  else
    {
      GNUNET_assert(ntohs(msg->size) == sizeof(struct GNUNET_MessageHeader));
      switch (ntohs(msg->type))
	{
	case GNUNET_MESSAGE_TYPE_SHUTDOWN_ACK:
	  GNUNET_log(GNUNET_ERROR_TYPE_WARNING,
		     "Received confirmation for service shutdown.\n");
	  shutdown_ctx->confirmed = GNUNET_YES;
	  GNUNET_CLIENT_receive (shutdown_ctx->sock, 
				 &service_shutdown_handler, 
				 shutdown_ctx, 
				 GNUNET_TIME_UNIT_FOREVER_REL);
	  break;
	default: /* Fall through */
	  GNUNET_log(GNUNET_ERROR_TYPE_WARNING, 
		     "Service shutdown refused!\n");
	  if (shutdown_ctx->cont != NULL)
	    shutdown_ctx->cont(shutdown_ctx->cont_cls, GNUNET_YES);
	  
	  GNUNET_SCHEDULER_cancel(shutdown_ctx->sched, shutdown_ctx->cancel_task);
	  GNUNET_CLIENT_disconnect (shutdown_ctx->sock, GNUNET_NO);
	  GNUNET_free(shutdown_ctx);
	  break;
	}
    }
}

/**
 * Shutting down took too long, cancel receive and return error.
 *
 * @param cls closure
 * @param tc context information (why was this task triggered now)
 */
void service_shutdown_cancel (void *cls,
                              const struct GNUNET_SCHEDULER_TaskContext * tc)
{
  struct ShutdownContext *shutdown_ctx = cls;
  GNUNET_log(GNUNET_ERROR_TYPE_WARNING, "service_shutdown_cancel called!\n");
  shutdown_ctx->cont(shutdown_ctx->cont_cls, GNUNET_SYSERR);
  GNUNET_CLIENT_disconnect (shutdown_ctx->sock, GNUNET_NO);
  GNUNET_free(shutdown_ctx);
}


/**
 * If possible, write a shutdown message to the target
 * buffer and destroy the client connection.
 *
 * @param cls the "struct GNUNET_CLIENT_Connection" to destroy
 * @param size number of bytes available in buf
 * @param buf NULL on error, otherwise target buffer
 * @return number of bytes written to buf
 */
static size_t
write_shutdown (void *cls, size_t size, void *buf)
{
  struct GNUNET_MessageHeader *msg;
  struct ShutdownContext *shutdown_ctx = cls;

  if (size < sizeof (struct GNUNET_MessageHeader))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  _("Failed to transmit shutdown request to client.\n"));
      shutdown_ctx->cont(shutdown_ctx->cont_cls, GNUNET_SYSERR);
      GNUNET_CLIENT_disconnect (shutdown_ctx->sock, GNUNET_NO);
      GNUNET_free(shutdown_ctx);
      return 0;                 /* client disconnected */
    }

  GNUNET_CLIENT_receive (shutdown_ctx->sock,
			 &service_shutdown_handler, shutdown_ctx, 
			 GNUNET_TIME_UNIT_FOREVER_REL);
  shutdown_ctx->cancel_task = GNUNET_SCHEDULER_add_delayed (shutdown_ctx->sched, 
							    GNUNET_TIME_absolute_get_remaining(shutdown_ctx->timeout), 
							    &service_shutdown_cancel, 
							    shutdown_ctx);
  msg = (struct GNUNET_MessageHeader *) buf;
  msg->type = htons (GNUNET_MESSAGE_TYPE_ARM_SHUTDOWN);
  msg->size = htons (sizeof (struct GNUNET_MessageHeader));
  return sizeof (struct GNUNET_MessageHeader);
}


/**
 * Request that the service should shutdown.
 * Afterwards, the connection will automatically be
 * disconnected.  Hence the "sock" should not
 * be used by the caller after this call
 * (calling this function frees "sock" after a while).
 *
 * @param sched the scheduler to use for calling shutdown continuation
 * @param sock the socket connected to the service
 * @param timeout how long to wait before giving up on transmission
 * @param cont continuation to call once the service is really down
 * @param cont_cls closure for continuation
 *
 */
void
GNUNET_CLIENT_service_shutdown (struct GNUNET_SCHEDULER_Handle *sched,
                                struct GNUNET_CLIENT_Connection *sock,
                                struct GNUNET_TIME_Relative timeout,
                                GNUNET_CLIENT_ShutdownTask cont,
                                void *cont_cls)
{
  struct ShutdownContext *shutdown_ctx;
  shutdown_ctx = GNUNET_malloc(sizeof(struct ShutdownContext));
  shutdown_ctx->sched = sched;
  shutdown_ctx->cont = cont;
  shutdown_ctx->cont_cls = cont_cls;
  shutdown_ctx->sock = sock;
  shutdown_ctx->timeout = GNUNET_TIME_relative_to_absolute(timeout);
  GNUNET_CONNECTION_notify_transmit_ready (sock->sock,
                                           sizeof (struct
                                                   GNUNET_MessageHeader),
                                           timeout,
                                           &write_shutdown, shutdown_ctx);
}


/**
 * Report service unavailable.
 */
static void
service_test_error (struct GNUNET_SCHEDULER_Handle *s,
                    GNUNET_SCHEDULER_Task task, void *task_cls)
{
  GNUNET_SCHEDULER_add_continuation (s,
                                     task,
                                     task_cls,
                                     GNUNET_SCHEDULER_REASON_TIMEOUT);
}


/**
 * Receive confirmation from test, service is up.
 *
 * @param cls closure
 * @param msg message received, NULL on timeout or fatal error
 */
static void
confirm_handler (void *cls, const struct GNUNET_MessageHeader *msg)
{
  struct GNUNET_CLIENT_Connection *conn = cls;
  /* We may want to consider looking at the reply in more
     detail in the future, for example, is this the
     correct service? FIXME! */
  if (msg != NULL)
    {
#if DEBUG_CLIENT
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "Received confirmation that service is running.\n");
#endif
      GNUNET_SCHEDULER_add_continuation (conn->sched,
                                         conn->test_cb,
                                         conn->test_cb_cls,
                                         GNUNET_SCHEDULER_REASON_PREREQ_DONE);
    }
  else
    {
      service_test_error (conn->sched, conn->test_cb, conn->test_cb_cls);
    }
  GNUNET_CLIENT_disconnect (conn, GNUNET_NO);
}


static size_t
write_test (void *cls, size_t size, void *buf)
{
  struct GNUNET_CLIENT_Connection *conn = cls;
  struct GNUNET_MessageHeader *msg;

  if (size < sizeof (struct GNUNET_MessageHeader))
    {
#if DEBUG_CLIENT
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  _("Failure to transmit TEST request.\n"));
#endif
      service_test_error (conn->sched, conn->test_cb, conn->test_cb_cls);
      GNUNET_CLIENT_disconnect (conn, GNUNET_NO);
      return 0;                 /* client disconnected */
    }
#if DEBUG_CLIENT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Transmitting `%s' request.\n", "TEST");
#endif
  msg = (struct GNUNET_MessageHeader *) buf;
  msg->type = htons (GNUNET_MESSAGE_TYPE_TEST);
  msg->size = htons (sizeof (struct GNUNET_MessageHeader));
  GNUNET_CLIENT_receive (conn, 
			 &confirm_handler, 
			 conn, 
			 GNUNET_TIME_absolute_get_remaining (conn->test_deadline));
  return sizeof (struct GNUNET_MessageHeader);
}


/**
 * Wait until the service is running.
 *
 * @param sched scheduler to use
 * @param service name of the service to wait for
 * @param cfg configuration to use
 * @param timeout how long to wait at most in ms
 * @param task task to run if service is running
 *        (reason will be "PREREQ_DONE" (service running)
 *         or "TIMEOUT" (service not known to be running))
 * @param task_cls closure for task
 */
void
GNUNET_CLIENT_service_test (struct GNUNET_SCHEDULER_Handle *sched,
                            const char *service,
                            const struct GNUNET_CONFIGURATION_Handle *cfg,
                            struct GNUNET_TIME_Relative timeout,
                            GNUNET_SCHEDULER_Task task, void *task_cls)
{
  struct GNUNET_CLIENT_Connection *conn;

#if DEBUG_CLIENT
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Testing if service `%s' is running.\n", service);
#endif
  conn = GNUNET_CLIENT_connect (sched, service, cfg);
  if (conn == NULL)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                  _
                  ("Could not connect to service `%s', must not be running.\n"),
                  service);
      service_test_error (sched, task, task_cls);
      return;
    }
  conn->test_cb = task;
  conn->test_cb_cls = task_cls;
  conn->test_deadline = GNUNET_TIME_relative_to_absolute (timeout);

  if (NULL == GNUNET_CLIENT_notify_transmit_ready (conn,
						   sizeof (struct GNUNET_MessageHeader),
						   timeout,
						   GNUNET_YES,
						   &write_test, conn))  
    {
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  _("Failure to transmit request to service `%s'\n"),
                  service);
      service_test_error (sched, task, task_cls);
      GNUNET_CLIENT_disconnect (conn, GNUNET_NO);
      return;
    }
}


/**
 * Connection notifies us about failure or success of
 * a transmission request.  Either pass it on to our
 * user or, if possible, retry.
 *
 * @param cls our "struct GNUNET_CLIENT_TransmissionHandle"
 * @param size number of bytes available for transmission
 * @param buf where to write them
 * @return number of bytes written to buf
 */
static size_t client_notify (void *cls, size_t size, void *buf);


/**
 * This task is run if we should re-try connection to the
 * service after a while.
 *
 * @param cls our "struct GNUNET_CLIENT_TransmitHandle" of the request
 * @param tc unused
 */
static void
client_delayed_retry (void *cls,
                      const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct GNUNET_CLIENT_TransmitHandle *th = cls;

  th->reconnect_task = GNUNET_SCHEDULER_NO_TASK;
  if (0 != (tc->reason & GNUNET_SCHEDULER_REASON_SHUTDOWN))
    {
#if DEBUG_CLIENT
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "Transmission failed due to shutdown.\n");
#endif
      th->sock->th = NULL;
      th->notify (th->notify_cls, 0, NULL);
      GNUNET_free (th);
      return;
    }
  th->th = GNUNET_CONNECTION_notify_transmit_ready (th->sock->sock,
                                                    th->size,
                                                    GNUNET_TIME_absolute_get_remaining
                                                    (th->timeout),
                                                    &client_notify, th);
  if (th->th == NULL)
    {
      GNUNET_break (0);
      th->notify (th->notify_cls, 0, NULL);
      GNUNET_free (th);
      return;
    }
}


/**
 * Connection notifies us about failure or success of a transmission
 * request.  Either pass it on to our user or, if possible, retry.
 *
 * @param cls our "struct GNUNET_CLIENT_TransmissionHandle"
 * @param size number of bytes available for transmission
 * @param buf where to write them
 * @return number of bytes written to buf
 */
static size_t
client_notify (void *cls, size_t size, void *buf)
{
  struct GNUNET_CLIENT_TransmitHandle *th = cls;
  size_t ret;
  struct GNUNET_TIME_Relative delay;

  th->th = NULL;
  th->sock->th = NULL;
  if (buf == NULL)
    {
      delay = GNUNET_TIME_absolute_get_remaining (th->timeout);
      delay.value /= 2;
      if ( (0 != (GNUNET_SCHEDULER_REASON_SHUTDOWN & GNUNET_SCHEDULER_get_reason (th->sock->sched))) ||
	   (GNUNET_YES != th->auto_retry) ||
	   (0 == --th->attempts_left) || 
	   (delay.value < 1) )
        {
#if DEBUG_CLIENT
          GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                      "Transmission failed %u times, giving up.\n",
                      MAX_ATTEMPTS - th->attempts_left);
#endif
          GNUNET_break (0 == th->notify (th->notify_cls, 0, NULL));
          GNUNET_free (th);
          return 0;
        }
      /* auto-retry */
      GNUNET_CONNECTION_destroy (th->sock->sock, GNUNET_NO);
      th->sock->sock = do_connect (th->sock->sched,
                                   th->sock->service_name, th->sock->cfg);
      GNUNET_assert (NULL != th->sock->sock);
      GNUNET_CONNECTION_ignore_shutdown (th->sock->sock,
					 th->sock->ignore_shutdown);
      delay = GNUNET_TIME_relative_min (delay, th->sock->back_off);
      th->sock->back_off 
	  = GNUNET_TIME_relative_min (GNUNET_TIME_relative_multiply (th->sock->back_off, 2),
				    GNUNET_TIME_UNIT_SECONDS);
#if DEBUG_CLIENT
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "Transmission failed %u times, trying again in %llums.\n",
                  MAX_ATTEMPTS - th->attempts_left,
                  (unsigned long long) delay.value);
#endif
      th->reconnect_task = GNUNET_SCHEDULER_add_delayed (th->sock->sched,
                                                         delay,
                                                         &client_delayed_retry,
                                                         th);
      th->sock->th = th;
      return 0;
    }
  GNUNET_assert (size >= th->size);
  ret = th->notify (th->notify_cls, size, buf);
  GNUNET_free (th);
  return ret;
}


/**
 * Ask the client to call us once the specified number of bytes
 * are free in the transmission buffer.  May call the notify
 * method immediately if enough space is available.
 *
 * @param sock connection to the service
 * @param size number of bytes to send
 * @param timeout after how long should we give up (and call
 *        notify with buf NULL and size 0)?
 * @param auto_retry if the connection to the service dies, should we
 *        automatically re-connect and retry (within the timeout period)
 *        or should we immediately fail in this case?  Pass GNUNET_YES
 *        if the caller does not care about temporary connection errors,
 *        for example because the protocol is stateless
 * @param notify function to call
 * @param notify_cls closure for notify
 * @return NULL if our buffer will never hold size bytes,
 *         a handle if the notify callback was queued (can be used to cancel)
 */
struct GNUNET_CLIENT_TransmitHandle *
GNUNET_CLIENT_notify_transmit_ready (struct GNUNET_CLIENT_Connection *sock,
                                     size_t size,
                                     struct GNUNET_TIME_Relative timeout,
                                     int auto_retry,
                                     GNUNET_CONNECTION_TransmitReadyNotify
                                     notify, void *notify_cls)
{
  struct GNUNET_CLIENT_TransmitHandle *th;

  if (NULL != sock->th)
    return NULL;
  th = GNUNET_malloc (sizeof (struct GNUNET_CLIENT_TransmitHandle));
  th->sock = sock;
  th->size = size;
  th->timeout = GNUNET_TIME_relative_to_absolute (timeout);
  th->auto_retry = auto_retry;
  th->notify = notify;
  th->notify_cls = notify_cls;
  th->attempts_left = MAX_ATTEMPTS;
  th->th = GNUNET_CONNECTION_notify_transmit_ready (sock->sock,
                                                    size,
                                                    timeout,
                                                    &client_notify, th);
  if (NULL == th->th)
    {
      GNUNET_break (0);
      GNUNET_free (th);
      return NULL;
    }
  sock->th = th;
  return th;
}


/**
 * Cancel a request for notification.
 * 
 * @param th handle from the original request.
 */
void
GNUNET_CLIENT_notify_transmit_ready_cancel (struct
                                            GNUNET_CLIENT_TransmitHandle *th)
{
  if (th->reconnect_task != GNUNET_SCHEDULER_NO_TASK)
    {
      GNUNET_break (NULL == th->th);
      GNUNET_SCHEDULER_cancel (th->sock->sched, th->reconnect_task);
      th->reconnect_task = GNUNET_SCHEDULER_NO_TASK;
    }
  else
    {
      GNUNET_break (NULL != th->th);
      GNUNET_CONNECTION_notify_transmit_ready_cancel (th->th);
    }
  th->sock->th = NULL;
  GNUNET_free (th);
}


/**
 * Function called to notify a client about the socket
 * begin ready to queue the message.  "buf" will be
 * NULL and "size" zero if the socket was closed for
 * writing in the meantime.
 *
 * @param cls closure of type "struct TransmitGetResponseContext*"
 * @param size number of bytes available in buf
 * @param buf where the callee should write the message
 * @return number of bytes written to buf
 */
static size_t
transmit_for_response (void *cls, size_t size, void *buf)
{
  struct TransmitGetResponseContext *tc = cls;
  uint16_t msize;

  tc->sock->tag = NULL;
  msize = ntohs (tc->hdr->size);
  if (NULL == buf)
    {
#if DEBUG_CLIENT
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
		  _("Could not submit request, not expecting to receive a response.\n"));
#endif
      tc->rn (tc->rn_cls, NULL);
      GNUNET_free (tc);
      return 0;
    }
  GNUNET_assert (size >= msize);
  memcpy (buf, tc->hdr, msize);
  GNUNET_CLIENT_receive (tc->sock,
                         tc->rn,
                         tc->rn_cls,
                         GNUNET_TIME_absolute_get_remaining (tc->timeout));
  GNUNET_free (tc);
  return msize;
}


/**
 * Convenience API that combines sending a request
 * to the service and waiting for a response.
 * If either operation times out, the callback
 * will be called with a "NULL" response (in which
 * case the connection should probably be destroyed).
 *
 * @param sock connection to use
 * @param hdr message to transmit
 * @param timeout when to give up (for both transmission
 *         and for waiting for a response)
 * @param auto_retry if the connection to the service dies, should we
 *        automatically re-connect and retry (within the timeout period)
 *        or should we immediately fail in this case?  Pass GNUNET_YES
 *        if the caller does not care about temporary connection errors,
 *        for example because the protocol is stateless
 * @param rn function to call with the response
 * @param rn_cls closure for rn 
 * @return GNUNET_OK on success, GNUNET_SYSERR if a request
 *         is already pending
 */
int
GNUNET_CLIENT_transmit_and_get_response (struct GNUNET_CLIENT_Connection
                                         *sock,
                                         const struct GNUNET_MessageHeader
                                         *hdr,
                                         struct GNUNET_TIME_Relative timeout,
                                         int auto_retry,
                                         GNUNET_CLIENT_MessageHandler rn,
                                         void *rn_cls)
{
  struct TransmitGetResponseContext *tc;
  uint16_t msize;

  if (NULL != sock->th)
    return GNUNET_SYSERR;
  GNUNET_assert (sock->tag == NULL);
  msize = ntohs (hdr->size);
  tc = GNUNET_malloc (sizeof (struct TransmitGetResponseContext) + msize);
  tc->sock = sock;
  tc->hdr = (const struct GNUNET_MessageHeader *) &tc[1];
  memcpy (&tc[1], hdr, msize);
  tc->timeout = GNUNET_TIME_relative_to_absolute (timeout);
  tc->rn = rn;
  tc->rn_cls = rn_cls;
  if (NULL == GNUNET_CLIENT_notify_transmit_ready (sock,
                                                   msize,
                                                   timeout,
                                                   auto_retry,
                                                   &transmit_for_response,
                                                   tc))
    {
      GNUNET_break (0);
      GNUNET_free (tc);
      return GNUNET_SYSERR;
    }
  sock->tag = tc;
  return GNUNET_OK;
}



/*  end of client.c */
