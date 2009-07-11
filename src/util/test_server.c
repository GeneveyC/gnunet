/*
     This file is part of GNUnet.
     (C) 2009 Christian Grothoff (and other contributing authors)

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
 * @file util/test_server.c
 * @brief tests for server.c
 */
#include "platform.h"
#include "gnunet_common.h"
#include "gnunet_scheduler_lib.h"
#include "gnunet_server_lib.h"
#include "gnunet_time_lib.h"

#define VERBOSE GNUNET_NO

#define PORT 12435

#define MY_TYPE 128
#define MY_TYPE2 129

static struct GNUNET_SERVER_Handle *server;

static struct GNUNET_SCHEDULER_Handle *sched;

static void
recv_fin_cb (void *cls,
             struct GNUNET_SERVER_Client *client,
             const struct GNUNET_MessageHeader *message)
{
  int *ok = cls;
  GNUNET_assert (2 == *ok);
  GNUNET_SERVER_receive_done (client, GNUNET_OK);
  *ok = 3;
}

struct SignalTimeoutContext
{
  GNUNET_NETWORK_Receiver cb;
  void *cb_cls;
};


static void
signal_timeout (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct SignalTimeoutContext *stctx = cls;

  stctx->cb (stctx->cb_cls, NULL, 0, NULL, 0, 0);
  GNUNET_free (stctx);
}


static GNUNET_SCHEDULER_TaskIdentifier
my_receive (void *cls,
            size_t max,
            struct GNUNET_TIME_Relative timeout,
            GNUNET_NETWORK_Receiver receiver, void *receiver_cls)
{
  int *ok = cls;
  struct GNUNET_MessageHeader msg;
  struct SignalTimeoutContext *stctx;
  GNUNET_SCHEDULER_TaskIdentifier ret;

  ret = GNUNET_SCHEDULER_NO_PREREQUISITE_TASK;
  switch (*ok)
    {
    case 1:
      *ok = 2;                  /* report success */
      msg.type = htons (MY_TYPE2);
      msg.size = htons (sizeof (struct GNUNET_MessageHeader));
      receiver (receiver_cls, &msg, sizeof (struct GNUNET_MessageHeader), NULL, 0, 0);
      break;
    case 3:
      /* called after first receive instantly
         produced a reply;
         schedule receiver call with timeout
         after timeout expires! */
      *ok = 4;
      stctx = GNUNET_malloc (sizeof (struct SignalTimeoutContext));
      stctx->cb = receiver;
      stctx->cb_cls = receiver_cls;
      ret = GNUNET_SCHEDULER_add_delayed (sched,
                                          GNUNET_NO,
                                          GNUNET_SCHEDULER_PRIORITY_KEEP,
                                          GNUNET_SCHEDULER_NO_PREREQUISITE_TASK,
                                          timeout, &signal_timeout, stctx);
      break;
    default:
      GNUNET_assert (0);
    }
  return ret;
}


static void
my_cancel (void *cls, GNUNET_SCHEDULER_TaskIdentifier ti)
{
  GNUNET_SCHEDULER_cancel (sched, ti);
}

static void *
my_transmit_ready_cb (void *cls,
                      size_t size,
                      struct GNUNET_TIME_Relative timeout,
                      GNUNET_NETWORK_TransmitReadyNotify notify,
                      void *notify_cls)
{
  static int non_null_addr;
  int *ok = cls;
  char buf[size];
  struct GNUNET_MessageHeader msg;

  GNUNET_assert (4 == *ok);
  GNUNET_assert (size == sizeof (struct GNUNET_MessageHeader));
  notify (notify_cls, size, buf);
  msg.type = htons (MY_TYPE);
  msg.size = htons (sizeof (struct GNUNET_MessageHeader));
  GNUNET_assert (0 == memcmp (&msg, buf, size));
  *ok = 5;                      /* report success */
  return &non_null_addr;
}


static void
my_transmit_ready_cancel_cb (void *cls, void *ctx)
{
  GNUNET_assert (0);
}


static int
my_check (void *cls)
{
  return GNUNET_YES;
}


static void my_destroy (void *cls);


struct CopyContext
{
  struct GNUNET_SERVER_Client *client;
  struct GNUNET_MessageHeader *cpy;
};

static size_t
copy_msg (void *cls, size_t size, void *buf)
{
  struct CopyContext *ctx = cls;
  struct GNUNET_MessageHeader *cpy = ctx->cpy;
  GNUNET_assert (sizeof (struct GNUNET_MessageHeader) == ntohs (cpy->size));
  GNUNET_assert (size >= ntohs (cpy->size));
  memcpy (buf, cpy, ntohs (cpy->size));
  GNUNET_free (cpy);
  GNUNET_free (ctx);
  return sizeof (struct GNUNET_MessageHeader);
}


static void
recv_cb (void *cls,
         struct GNUNET_SERVER_Client *argclient,
         const struct GNUNET_MessageHeader *message)
{
  struct GNUNET_SERVER_Client *client;
  struct CopyContext *cc;
  struct GNUNET_MessageHeader *cpy;

  GNUNET_assert (argclient == NULL);
  GNUNET_assert (sizeof (struct GNUNET_MessageHeader) ==
                 ntohs (message->size));
  GNUNET_assert (MY_TYPE == ntohs (message->type));
  client = GNUNET_SERVER_connect_callback (server,
                                           cls,
                                           &my_receive,
                                           &my_cancel,
                                           &my_transmit_ready_cb,
                                           &my_transmit_ready_cancel_cb,
                                           &my_check, &my_destroy);
  cc = GNUNET_malloc (sizeof (struct CopyContext));
  cc->client = client;
  cpy = GNUNET_malloc (ntohs (message->size));
  memcpy (cpy, message, ntohs (message->size));
  cc->cpy = cpy;
  GNUNET_assert (NULL !=
                 GNUNET_SERVER_notify_transmit_ready (client,
                                                      ntohs (message->size),
                                                      GNUNET_TIME_UNIT_SECONDS,
                                                      &copy_msg, cc));
  GNUNET_SERVER_client_drop (client);
}


static struct GNUNET_SERVER_MessageHandler handlers[] = {
  {&recv_cb, NULL, MY_TYPE, sizeof (struct GNUNET_MessageHeader)},
  {&recv_fin_cb, NULL, MY_TYPE2, sizeof (struct GNUNET_MessageHeader)},
  {NULL, NULL, 0, 0}
};


static void
my_destroy (void *cls)
{
  int *ok = cls;
  GNUNET_assert (5 == *ok);
  *ok = 0;                      /* report success */
  /* this will cause us to terminate */
  GNUNET_SERVER_destroy (server);
}


static void
task (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct sockaddr_in sa;
  struct GNUNET_MessageHeader msg;

  sched = tc->sched;
  memset (&sa, 0, sizeof (sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons (PORT);
  server = GNUNET_SERVER_create (tc->sched,
                                 NULL,
                                 NULL,
                                 (const struct sockaddr *) &sa,
                                 sizeof (sa),
                                 1024,
                                 GNUNET_TIME_relative_multiply
                                 (GNUNET_TIME_UNIT_MILLISECONDS, 250),
                                 GNUNET_NO);
  GNUNET_assert (server != NULL);
  handlers[0].callback_cls = cls;
  handlers[1].callback_cls = cls;
  GNUNET_SERVER_add_handlers (server, handlers);
  msg.type = htons (MY_TYPE);
  msg.size = htons (sizeof (struct GNUNET_MessageHeader));
  GNUNET_SERVER_inject (server, NULL, &msg);
  memset (&msg, 0, sizeof (struct GNUNET_MessageHeader));
}


/**
 * Main method, starts scheduler with task1,
 * checks that "ok" is correct at the end.
 */
static int
check ()
{
  int ok;

  ok = 1;
  GNUNET_SCHEDULER_run (&task, &ok);
  return ok;
}


int
main (int argc, char *argv[])
{
  int ret = 0;

  GNUNET_log_setup ("test_server", "WARNING", NULL);
  ret += check ();

  return ret;
}

/* end of test_server.c */
