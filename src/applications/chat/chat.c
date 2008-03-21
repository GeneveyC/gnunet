/*
     This file is part of GNUnet.
     (C) 2008 Christian Grothoff (and other contributing authors)

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
 * CHAT CORE. This is the code that is plugged
 * into the GNUnet core to enable chatting.
 *
 * @author Christian Grothoff
 * @author Nathan Evans
 * @file applications/chat/chat.c
 */

#include "platform.h"
#include "gnunet_protocols.h"
#include "gnunet_util.h"
#include "gnunet_core.h"
#include "chat.h"

static int shutdown_flag;

static GNUNET_CoreAPIForPlugins *coreAPI;

static struct GNUNET_Mutex *chatMutex;

static struct GNUNET_GE_Context *ectx;

struct GNUNET_CS_chat_client
{
  struct GNUNET_ClientHandle *client;
  struct GNUNET_CS_chat_client *next;
  struct GNUNET_CS_chat_client *prev;
  GNUNET_HashCode room_name_hash;
  char *nick;

};

static struct GNUNET_CS_chat_client *client_list_head;

/* Thread that tells clients about chat room members
 */
static void *
update_client_thread (void *cls)
{
  struct GNUNET_CS_chat_client *pos;
  struct GNUNET_CS_chat_client *compare_pos;
  CS_chat_ROOM_MEMBER_MESSAGE *message;
  int message_size;

  while (shutdown_flag != GNUNET_YES)
    {
      fprintf (stderr, "Checking room members\n");
      pos = client_list_head;
      GNUNET_mutex_lock (chatMutex);
      while (pos != NULL)
        {
          compare_pos = client_list_head;
          while (compare_pos != NULL)
            {
              if (memcmp
                  (&pos->room_name_hash, &compare_pos->room_name_hash,
                   sizeof (GNUNET_HashCode)) == 0)
                {
                  /*Send nick to this client, so it knows who is in the same room! (Including itself...) */
                  fprintf (stderr, "Found matching member %s length is %d\n",
                           compare_pos->nick, strlen (compare_pos->nick));

                  message_size =
                    sizeof (CS_chat_ROOM_MEMBER_MESSAGE) +
                    strlen (compare_pos->nick);
                  message = GNUNET_malloc (message_size);
                  message->header.size = htons (message_size);
                  message->header.type =
                    htons (GNUNET_CS_PROTO_CHAT_ROOM_MEMBER_MESSAGE);
                  message->nick_len = htons (strlen (compare_pos->nick));
                  memcpy (&message->nick[0], compare_pos->nick,
                          strlen (compare_pos->nick));

                  coreAPI->cs_send_to_client (pos->client, &message->header,
                                              GNUNET_YES);
                }
              compare_pos = compare_pos->next;
            }
          pos = pos->next;
        }
      GNUNET_mutex_unlock (chatMutex);
      if (shutdown_flag == GNUNET_NO)
        GNUNET_thread_sleep (30 * GNUNET_CRON_SECONDS);
    }
  return NULL;
}

static int
csHandleChatMSG (struct GNUNET_ClientHandle *client,
                 const GNUNET_MessageHeader * message)
{
  CS_chat_MESSAGE *cmsg;

  struct GNUNET_CS_chat_client *tempClient;

  GNUNET_HashCode hc;
  GNUNET_HashCode room_name_hash;

  char *nick;
  char *message_content;
  char *room_name;

  int header_size;
  unsigned long nick_len;
  unsigned long msg_len;
  unsigned long room_name_len;

  cmsg = (CS_chat_MESSAGE *) message;
  if (ntohs (cmsg->header.size) < sizeof (CS_chat_MESSAGE))
    {
      GNUNET_GE_BREAK (NULL, 0);
      return GNUNET_SYSERR;     /* invalid message */
    }


  header_size = ntohs (cmsg->header.size);
  nick_len = ntohs (cmsg->nick_len);
  msg_len = ntohs (cmsg->msg_len);
  room_name_len = header_size - sizeof (CS_chat_MESSAGE) - nick_len - msg_len;

  if (header_size < (nick_len + msg_len + room_name_len))
    {
      GNUNET_GE_BREAK (NULL, 0);
      return GNUNET_SYSERR;     /* invalid message */
    }

  nick = GNUNET_malloc (nick_len + 1);
  message_content = GNUNET_malloc (msg_len + 1);
  room_name = GNUNET_malloc (room_name_len + 1);

  memcpy (nick, &cmsg->nick[0], nick_len);
  memcpy (message_content, &cmsg->nick[nick_len], msg_len);
  memcpy (room_name, &cmsg->nick[nick_len + msg_len], room_name_len);

  nick[nick_len] = '\0';
  message_content[msg_len] = '\0';
  room_name[room_name_len] = '\0';

  GNUNET_hash (room_name, strlen (room_name), &room_name_hash);

  GNUNET_GE_LOG (ectx,
                 GNUNET_GE_WARNING | GNUNET_GE_BULK | GNUNET_GE_USER,
                 "Received chat message from client.\n Message is `%s'\n from `%s'\n intended for room `%s'\n",
                 message_content, nick, room_name);

  GNUNET_hash (cmsg, header_size, &hc);
  /* check if we have seen this message already */

  GNUNET_mutex_lock (chatMutex);

  /*TODO: we have received a message intended for some room, check current client contexts for matching room and send to those clients */
  /*TODO: p2p messages will need to be sent as well at some point */

  tempClient = client_list_head;
  while ((tempClient != NULL) && (tempClient->client != NULL))
    {
      if (memcmp
          (&room_name_hash, &tempClient->room_name_hash,
           sizeof (GNUNET_HashCode)) == 0)
        {
          fprintf (stderr,
                   "room names match, must send message to others!!\n");
          coreAPI->cs_send_to_client (tempClient->client, message,
                                      GNUNET_YES);
        }

      tempClient = tempClient->next;
    }
  GNUNET_mutex_unlock (chatMutex);

  GNUNET_free (room_name);
  GNUNET_free (nick);
  GNUNET_free (message_content);

  return GNUNET_OK;
}

static int
csHandleChatJoinRequest (struct GNUNET_ClientHandle *client,
                         const GNUNET_MessageHeader * message)
{
  const CS_chat_JOIN_MESSAGE *cmsg;
  GNUNET_HashCode hc;
  GNUNET_HashCode room_name_hash;

  char *nick;
  GNUNET_RSA_PublicKey *client_key;
  char *room_name;

  int header_size;
  int tempCount;
  int nick_len;
  int pubkey_len;
  int room_name_len;
  struct GNUNET_CS_chat_client *tempClient;

  cmsg = (CS_chat_JOIN_MESSAGE *) message;

  if (ntohs (cmsg->header.size) < sizeof (CS_chat_JOIN_MESSAGE))
    {
      GNUNET_GE_BREAK (NULL, 0);
      return GNUNET_SYSERR;     /* invalid message */
    }


  header_size = ntohs (cmsg->header.size);
  nick_len = ntohs (cmsg->nick_len);
  pubkey_len = ntohs (cmsg->pubkey_len);
  room_name_len =
    header_size - sizeof (CS_chat_JOIN_MESSAGE) - nick_len - pubkey_len;

  fprintf (stderr,
           "JOIN_MESSAGE size : %d\nheader_size : %d\nnick_len : %d\npubkey_len : %d\nroom_name_len : %d\n",
           sizeof (CS_chat_JOIN_MESSAGE), header_size, nick_len, pubkey_len,
           room_name_len);
  fprintf (stderr, "According to my addition, header_size should be %d\n",
           nick_len + pubkey_len + room_name_len +
           sizeof (CS_chat_JOIN_MESSAGE));
  if (header_size < (nick_len + pubkey_len + room_name_len))
    {
      GNUNET_GE_BREAK (NULL, 0);
      return GNUNET_SYSERR;     /* invalid message */
    }

  nick = GNUNET_malloc (nick_len + 1);
  client_key = GNUNET_malloc (sizeof (GNUNET_RSA_PublicKey));
  room_name = GNUNET_malloc (room_name_len + 1);

  memcpy (nick, &cmsg->nick[0], nick_len);
  memcpy (client_key, &cmsg->nick[nick_len], pubkey_len);
  memcpy (room_name, &cmsg->nick[nick_len + pubkey_len], room_name_len);

  GNUNET_GE_LOG (ectx,
                 GNUNET_GE_DEBUG | GNUNET_GE_REQUEST | GNUNET_GE_DEVELOPER,
                 "Received join chat room message from client.\n From `%s'\n for room `%s'\n",
                 nick, room_name);

  nick[nick_len] = '\0';
  room_name[room_name_len] = '\0';
  GNUNET_hash (cmsg, header_size, &hc);
  GNUNET_hash (room_name, strlen (room_name), &room_name_hash);
  GNUNET_mutex_lock (chatMutex);

  /*TODO: create client context on the server, very simple as of now */
#if EXTRA_CHECKS
  tempClient = client_list;
  while ((tempClient != NULL) && (tempClient->client != client))
    tempClient = tempClient->next;
  if (tempClient != NULL)
    {
      GNUNET_GE_BREAK (NULL, 0);
      GNUNET_free (nick);
      GNUNET_free (client_key);
      GNUNET_free (room_name);
      GNUNET_mutex_unlock (chatMutex);
      return GNUNET_SYSERR;
    }
#endif
  tempClient = GNUNET_malloc (sizeof (struct GNUNET_CS_chat_client));
  memset (tempClient, 0, sizeof (struct GNUNET_CS_chat_client));
  tempClient->next = client_list_head;
  if (client_list_head != NULL)
    client_list_head->prev = tempClient;
  client_list_head = tempClient;
  tempClient->client = client;
  tempClient->nick = GNUNET_malloc (nick_len + 1);
  memcpy (&tempClient->room_name_hash, &room_name_hash,
          sizeof (GNUNET_HashCode));
  memcpy (tempClient->nick, nick, nick_len + 1);

  tempCount = 0;

  while (tempClient != NULL)
    {
      tempCount++;
      tempClient = tempClient->next;
    }

  fprintf (stderr, "Number of clients currently is... %d\n", tempCount);


  GNUNET_free (nick);
  GNUNET_free (client_key);
  GNUNET_free (room_name);

  GNUNET_mutex_unlock (chatMutex);
  fprintf (stderr, "End of handleChatRequest\n");
  return GNUNET_OK;
}


static void
chatClientExitHandler (struct GNUNET_ClientHandle *client)
{
  int tempCount;
  int message_size;
  int found;
  struct GNUNET_CS_chat_client *tempClient;
  struct GNUNET_CS_chat_client *pos;
  struct GNUNET_CS_chat_client *prev;
  char *nick_to_remove;
  CS_chat_ROOM_MEMBER_MESSAGE *message;

  GNUNET_GE_LOG (ectx,
                 GNUNET_GE_DEBUG | GNUNET_GE_REQUEST | GNUNET_GE_DEVELOPER,
                 "Received leave chat room message from client.\n");

  GNUNET_mutex_lock (chatMutex);


  pos = client_list_head;
  prev = NULL;
  found = GNUNET_NO;
  nick_to_remove = NULL;
  while ((pos != NULL) && (pos->client != client))
    {
      prev = pos;
      pos = pos->next;
    }
  if (pos != NULL)
    {
      found = GNUNET_YES;
      nick_to_remove = GNUNET_malloc (strlen (pos->nick));
      strcpy (nick_to_remove, pos->nick);
      if (prev == NULL)
        client_list_head = pos->next;
      else
        prev->next = pos->next;
      if (pos->next != NULL)
        pos->next->prev = pos->prev;
      GNUNET_free (pos);
    }
  /*Count the number of current clients, will be removed */

  tempClient = client_list_head;
  tempCount = 0;
  while (tempClient != NULL)
    {
      tempCount++;
      tempClient = tempClient->next;
    }
  fprintf (stderr, "Number of clients currently is... %d\n", tempCount);
  /*End of client count code */

  /*Send remove member message here */
  if (found == GNUNET_YES)
    {
      message_size =
        sizeof (CS_chat_ROOM_MEMBER_MESSAGE) + strlen (nick_to_remove);
      message = GNUNET_malloc (message_size);
      message->header.size = htons (message_size);
      message->header.type =
        htons (GNUNET_CS_PROTO_CHAT_ROOM_MEMBER_LEAVE_MESSAGE);
      message->nick_len = htons (strlen (nick_to_remove));
      memcpy (&message->nick[0], nick_to_remove, strlen (nick_to_remove));

      pos = client_list_head;
      while (pos != NULL)
        {
          coreAPI->cs_send_to_client (pos->client, &message->header,
                                      GNUNET_YES);
          pos = pos->next;
        }

      GNUNET_free (message);
      GNUNET_free (nick_to_remove);

    }
  GNUNET_mutex_unlock (chatMutex);
  return;
}

int
initialize_module_chat (GNUNET_CoreAPIForPlugins * capi)
{
  int ok = GNUNET_OK;
  shutdown_flag = GNUNET_NO;

  chatMutex = GNUNET_mutex_create (GNUNET_NO);

  coreAPI = capi;
  GNUNET_thread_create (&update_client_thread, NULL, 1024 * 128);       /* What's a good stack size? */
  GNUNET_GE_LOG (ectx, GNUNET_GE_DEBUG | GNUNET_GE_REQUEST | GNUNET_GE_USER,
                 _("`%s' registering handlers %d and %d\n"),
                 "chat", GNUNET_P2P_PROTO_CHAT_MSG, GNUNET_CS_PROTO_CHAT_MSG);

  /*if (GNUNET_SYSERR ==
     capi->registerHandler (GNUNET_P2P_PROTO_CHAT_MSG, &handleChatMSG))
     ok = GNUNET_SYSERR; */
  if (GNUNET_SYSERR ==
      capi->cs_exit_handler_register (&chatClientExitHandler))
    ok = GNUNET_SYSERR;

  if (GNUNET_SYSERR ==
      capi->registerClientHandler (GNUNET_CS_PROTO_CHAT_JOIN_MSG,
                                   &csHandleChatJoinRequest))
    ok = GNUNET_SYSERR;

  if (GNUNET_SYSERR == capi->registerClientHandler (GNUNET_CS_PROTO_CHAT_MSG,
                                                    &csHandleChatMSG))
    ok = GNUNET_SYSERR;

  GNUNET_GE_ASSERT (capi->ectx,
                    0 == GNUNET_GC_set_configuration_value_string (capi->cfg,
                                                                   capi->ectx,
                                                                   "ABOUT",
                                                                   "chat",
                                                                   _
                                                                   ("enables P2P-chat (incomplete)")));
  return ok;
}

void
done_module_chat ()
{
  shutdown_flag = GNUNET_YES;
  /*coreAPI->unregisterHandler (GNUNET_P2P_PROTO_CHAT_MSG, &handleChatMSG); */
  coreAPI->cs_exit_handler_unregister (&chatClientExitHandler);
  coreAPI->unregisterClientHandler (GNUNET_CS_PROTO_CHAT_MSG,
                                    &csHandleChatMSG);
  coreAPI->unregisterClientHandler (GNUNET_CS_PROTO_CHAT_JOIN_MSG,
                                    &csHandleChatJoinRequest);

  GNUNET_mutex_destroy (chatMutex);
  coreAPI = NULL;
}


/* end of chat.c */
