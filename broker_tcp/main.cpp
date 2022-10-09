#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <cerrno>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include "dependencies/jsmn/jsmn.h"

#define SERVER_PORT 33333
#define LISTEN_QLEN 16

static int jsoneq(const char *json, jsmntok_t *tok, const char *s)
{
   if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start &&
         strncmp(json + tok->start, s, tok->end - tok->start) == 0)
   {
      return 0;
   }
   return -1;
}

static char *get_new_str(const char *str, int start_index, int one_past_end_index)
{
   char *res;
   if(one_past_end_index - start_index <= 0)
   {
      res = new char[1];
      *res = 0;
   }
   else
   {
      int length = one_past_end_index - start_index;
      res = new char[length +  1];
      memcpy(res, str + start_index, length);
      res[length] = 0;
   }
   return res;
}

static char *get_new_str(const char *str)
{
   return get_new_str(str, 0, strlen(str));
}

/*
 General Architecture:
 Query - Response
 Query Queue - Response Queue

read -> Query -> Query Queue -> Processing By Broker -> Response -> Response Queue -> write

*/

// Response from server to query from client
struct response
{
   char *data;
   int count;
   response *next;
};

// Queue that stores responses that need to be sent to the client
struct response_queue
{
   response *head;
   response *tail;
};

enum query_status
{
   query_status_uncompleted,
   query_status_completed,
   query_status_finished
};

// Query from client to server
struct query
{
   query_status status;
   char *data;
   int data_count;
   query *next;
};

// Queue that stores queries from client to server
struct query_queue
{
   query *head;
   query *tail;
};

struct channel;

struct client_sockets_storage
{
   channel *subscriptions[FD_SETSIZE];
   int fd[FD_SETSIZE]; // 1024
   response_queue response_queues[FD_SETSIZE];
   query_queue query_queues[FD_SETSIZE];
   int count;
};

struct broker_data
{
   channel *head;
   client_sockets_storage client_sockets;
};

// Broker details

struct message
{
   char *data;
   int count;
   message *next;
};

struct subscriber
{
   int fd;
   subscriber *next;
};

struct channel
{
   int fd;
   char *name;
   message *msg_head;
   message *msg_tail;
   subscriber *subs_head;
   channel *next;
};


void free_query_queue(query_queue *q)
{
   query *curr = q->head;
   while(curr)
   {
      query *tmp = curr;
      curr = curr->next;
      delete tmp;
   }
   q->head = 0;
   q->tail = 0;
}

void free_response_queue(response_queue *q)
{
   response *curr = q->head;
   while(curr)
   {
      response *tmp = curr;
      curr = curr->next;
      delete tmp;
   }
   q->head = 0;
   q->tail = 0;
}

void free_response_from_head(response_queue *q)
{
   response *tmp = q->head;
   q->head = q->head->next;
   delete tmp;
   if(!q->head)
   {
      q->tail = 0;
   }
}

void init_new_query(query_queue *queue)
{
   query **head = &queue->head;
   query **tail = &queue->tail;

   query *new_query = new query;
   new_query->status = query_status_uncompleted;
   new_query->data = 0;
   new_query->next = 0;
   new_query->data_count = 0;

   if(*head == 0 && *tail == 0)
   {
      *head = *tail = new_query;
   }
   else
   {
      (*tail)->next = new_query;
      *tail = new_query;
   }
}


void add_response(char *data, int count, response_queue *queue)
{
   response **head = &queue->head;
   response **tail = &queue->tail;

   response *resp = new response;
   resp->data = data;
   resp->count = count;
   resp->next = 0;

   if(*head == 0 && *tail == 0)
   {
      *head = *tail = resp;
   }
   else
   {
      (*tail)->next = resp;
      *tail = resp;
   }
}

void remove_subscriber(subscriber **head, int fd)
{
   for(subscriber **curr_sub = head;
      *curr_sub;
      curr_sub = &((*curr_sub)->next))
   {
      if((*curr_sub)->fd == fd)
      {
         subscriber *tmp = *curr_sub;
         *curr_sub = (*curr_sub)->next;
         delete tmp;
         break;
      }
   }
}

bool remove_client_socket(int i, client_sockets_storage *client_sockets)
{
   bool is_last = true;
   int fd = client_sockets->fd[i];
   free_response_queue(&client_sockets->response_queues[fd]);
   free_query_queue(&client_sockets->query_queues[fd]);

   channel* subscription = client_sockets->subscriptions[fd];
   if(subscription)
   {
      remove_subscriber(&subscription->subs_head, fd);
      client_sockets->subscriptions[fd] = 0;
   }
   close(client_sockets->fd[i]);

   if(i != client_sockets->count - 1)
   {
      client_sockets->fd[i] = client_sockets->fd[client_sockets->count - 1];
      is_last = false;
   }

   client_sockets->count--;
   return is_last;
}

void remove_finished_queries(query_queue *q)
{
   query **head = &q->head;
 
   for(query **curr = head;
      *curr;)
   {
      if((*curr)->status == query_status_finished)
      {
         query *tmp = *curr;
         *curr = (*curr)->next;
         delete tmp;
      }
      else
      {
         q->tail = *curr;
         curr = &((*curr)->next);
      }
   }

   if(!q->head)
   {
      q->tail = 0;
   }
}

bool add_new_channel(const char *name, int fd, broker_data *broker)
{
   channel *c = broker->head;
   while(c)
   {
      if(0 == strcmp(c->name, name))
      {
         return false;
      } 
      c = c->next;
   }

   channel *new_channel = new channel;
   new_channel->fd = fd;
   new_channel->name = get_new_str(name);
   new_channel->msg_head = 0;
   new_channel->msg_tail = 0;
   new_channel->subs_head = 0;
   new_channel->next = broker->head;
   broker->head = new_channel;
   return true;
}

void remove_messages_from_channel(channel *ch)
{
   message *msg = ch->msg_head;
   while(msg)
   {
      message *tmp = msg;
      msg = msg->next;
      delete [] tmp->data;
      delete tmp;
   }
   ch->msg_head = ch->msg_tail = 0;
}

void remove_subs_from_channel(channel *ch, broker_data *b)
{
   subscriber *s = ch->subs_head;
   while(s)
   {
      b->client_sockets.subscriptions[s->fd] = 0;
      subscriber *tmp = s;
      s = s->next;
      delete tmp;
   }
}

void remove_channel(int fd, broker_data *broker)
{
   for(channel **curr = &broker->head;
      *curr;
      curr = &((*curr)->next))
   {
      if((*curr)->fd == fd)
      {
         if((*curr)->name)
         {
            delete [] (*curr)->name;
         } 
         remove_messages_from_channel(*curr);
         remove_subs_from_channel(*curr, broker);
         channel *tmp = *curr;
         *curr = (*curr)->next;
         delete tmp;
         break;
      }
   }
}

class response_maker
{
public:
   response_maker() 
      : data(0), count(0), in_obj(false), obj_begin(false),
      in_array(false), array_begin(false) {}

   ~response_maker() { if(data) delete [] data; }

   char *get_data() { return get_new_str(data, 0, count); }
   int get_count() { return count; }
   char *get_null_terminated()
   {
      char *res = new char[count + 1];
      memcpy(res, data, count);
      res[count] = 0;
      return res;
   }

   void append(const char *str, int length)
   {
      char *tmp = new char[count + length];
      memcpy(tmp, data, count);
      memcpy(tmp + count, str, length);
      delete [] data;
      data = tmp;
      count += length;
   }

   void append(const char *str) { append(str, strlen(str)); }

   void begin_obj() 
   { 
      in_obj = obj_begin = true; 
      append("{");
   }

   void end_obj()
   {
      in_obj = obj_begin = false;
      append("}");
   }

   void add_string(const char *str, int length)
   {
      if(in_array)
      {
         if(array_begin)
         {
            array_begin = false;
         }
         else
         {
            append(",");
         }
      }
      append("\"");
      append(str, length);
      append("\"");
   }

   void add_string(const char *str)
   {
      int len = str ? strlen(str) : 0;
      add_string(str, len);
   }

   void add_name(const char *name)
   {
      if(in_obj)
      {
         if(obj_begin)
         {
            obj_begin = false;
         }
         else
         {
            append(",");
         }
      }
      add_string(name);
      append(":");
   }

   void add_pair(const char *name, const char *str)
   {
      add_name(name);
      add_string(str);
   }
   
   void begin_array()
   {
      in_array = array_begin = true;
      append("[");
   }

   void end_array()
   {
      in_array = array_begin = false;
      append("]");
   }

private:
   char *data;
   int count;
   bool in_obj;
   bool obj_begin;
   bool in_array;
   bool array_begin;
};


void notify_subscribers(message *m, channel *t, broker_data *broker)
{
   if(m->count <= 0)
      return;
   subscriber *curr = t->subs_head;
   response_maker resp;
   resp.begin_obj();
   resp.add_pair("QUERY_TYPE", "message_for_subscriber"); 
   resp.add_pair("CHANNEL_NAME", t->name);
   resp.add_name("MSG_BODY");
   resp.add_string(m->data, m->count);
   resp.end_obj();
   resp.append("\r\n");
   while(curr)
   {
      add_response(resp.get_data(), resp.get_count(),
            &broker->client_sockets.response_queues[curr->fd]);
      curr = curr->next;
   }
}

void send_all_channel_messages_to_client(channel *t, response_queue *q)
{
   message *curr = t->msg_head;

   response_maker resp;
   resp.begin_obj();
   resp.add_pair("QUERY_TYPE", "messages_for_subscriber");
   resp.add_pair("CHANNEL_NAME", t->name);
   resp.add_name("MSG_BODY_ARRAY");
   resp.begin_array();

   while(curr)
   {
      if(curr->count > 0)
      {
         resp.add_string(curr->data, curr->count);
      }
      curr = curr->next;
   }

   resp.end_array();
   resp.end_obj();
   resp.append("\r\n");
   add_response(resp.get_data(), resp.get_count(), q);
}

channel* get_channel(const char *name, broker_data *broker)
{
   channel *curr = broker->head;
   while(curr)
   {
      if(0 == strcmp(name, curr->name))
      {
         return curr;
      }
      curr = curr->next;
   }

   return 0;
}

channel* get_channel(int fd, broker_data *broker)
{
   channel *curr = broker->head;
   while(curr)
   {
      if(fd == curr->fd)
      {
         return curr;
      }
      curr = curr->next;
   }

   return 0;
}

void add_new_message_to_channel(char *msg, int msg_count, channel *c, broker_data *broker)
{        
   message *m = new message;
   m->data = msg;
   m->count = msg_count;
   m->next = 0;

   if(c->msg_head == 0 && c->msg_tail == 0)
   {
      c->msg_head = c->msg_tail = m;
   }
   else
   {
      c->msg_tail->next = m;
      c->msg_tail = m;
   }

   notify_subscribers(m, c, broker);
}

int change_client_subscription(channel **current_subsc, const char *channel_name, int fd,
                              broker_data *broker)
{
   if(*current_subsc)
   {
      if(0 == strcmp((*current_subsc)->name, channel_name))
      {
         return -1;
      }
      remove_subscriber(&((*current_subsc)->subs_head), fd);
   }

   channel *ch = get_channel(channel_name, broker);
   if(ch)
   {
      subscriber *new_s = new subscriber;
      new_s->fd = fd;
      new_s->next = ch->subs_head;
      ch->subs_head = new_s;

      *current_subsc = ch;
      send_all_channel_messages_to_client(ch, &broker->client_sockets.response_queues[fd]);
      return 0;
   }
   else
   {
      *current_subsc = 0;
      return -2;
   }
}

char *get_all_channels_json(broker_data *broker)
{ 
   response_maker resp;
   resp.begin_obj();
   resp.add_pair("QUERY_TYPE", "get_channel_names");
   resp.add_name("CHANNELS");
   resp.begin_array();

   channel *curr = broker->head;
   while(curr)
   {
      resp.add_string(curr->name);
      curr = curr->next;
   }

   resp.end_array();
   resp.end_obj();
   resp.append("\r\n");
   return resp.get_null_terminated();
}

// End of Broker details

void fill_query(query_queue *qq, char *buff, int rc)
{
   query *q = qq->head;

   int buffIndex = 0;
   while(buffIndex < rc)
   {
      if(!q || q->status == query_status_completed)
      {
         init_new_query(qq); 
         q = qq->tail;
      }

      int startingPos = buffIndex;

      for(; buffIndex < rc; buffIndex++)
      {
         if(buff[buffIndex] == '\r')
         {
            buffIndex++;
            if(buffIndex < rc && buff[buffIndex] == '\n')
            {
               buffIndex++;
            }
            q->status = query_status_completed;
            break;
         } 
      }

      int queryDataCount = buffIndex - startingPos;

      if(queryDataCount > 0)
      {
         int new_data_count = q->data_count + queryDataCount;
         if(q->status == query_status_completed)
         {
            new_data_count += 1;
         }

         char* new_data = new char[new_data_count];
         strncpy(new_data, q->data, q->data_count);
         strncpy(new_data + q->data_count,
               buff + startingPos, queryDataCount);
         if(q->status == query_status_completed)
         {
            new_data[new_data_count - 1] = 0;
         }
         if(q->data)
         {
            delete [] q->data;
         }
         q->data = new_data;
         q->data_count += queryDataCount;
      }
   }
}

void process_query(int fd, query *q, broker_data *b)
{
   if(q->status == query_status_completed)
   {
      // Process query here
      int r;
      bool success = true;
      jsmn_parser p;
      jsmntok_t t[128]; /* We expect no more than 128 tokens */

      char* jsonstr = q->data;
      jsmn_init(&p);
      r = jsmn_parse(&p, jsonstr, strlen(jsonstr), t,
            sizeof(t) / sizeof(t[0]));
      if (r < 0)
      {
         success = false;
         response_maker resp;
         resp.begin_obj();
         resp.add_pair("QUERY_TYPE", "unknown");
         resp.add_pair("STATUS", "fail");
         resp.add_pair("REASON", "incorrect_format");
         resp.end_obj();
         resp.append("\r\n");
         add_response(resp.get_data(), resp.get_count(),
               &b->client_sockets.response_queues[fd]);
      }

      /* Assume the top-level element is an object */
      if (success && (r < 1 || t[0].type != JSMN_OBJECT))
      {
         success = false;

         response_maker resp;
         resp.begin_obj();
         resp.add_pair("QUERY_TYPE", "unknown");
         resp.add_pair("STATUS", "fail");
         resp.add_pair("REASON", "top_level_object_not_found");
         resp.end_obj();
         resp.append("\r\n");
         add_response(resp.get_data(), resp.get_count(),
               &b->client_sockets.response_queues[fd]);
      }

      if(success)
      {
         if (r >= 1 && jsoneq(jsonstr, &t[1], "QUERY_TYPE") == 0)
         {
            if(r >= 2 && jsoneq(jsonstr, &t[2], "get_channel_names") == 0)
            {
               char *ans = get_all_channels_json(b);
               add_response(ans, strlen(ans),
                     &b->client_sockets.response_queues[fd]);
            }
            else if(r >= 2 && jsoneq(jsonstr, &t[2], "create_channel") == 0)
            {
               if(r >= 3 && jsoneq(jsonstr, &t[3], "CHANNEL_NAME") == 0)
               {
                  if(r >= 4 && t[4].type == JSMN_STRING)
                  {
                     char *channel_name = 0;
                     channel_name = get_new_str(jsonstr, t[4].start, t[4].end);
                     channel *prev_ch = get_channel(fd, b);

                     if(prev_ch && 0 == strcmp(prev_ch->name, channel_name))
                     {
                        response_maker resp;
                        resp.begin_obj();
                        resp.add_pair("QUERY_TYPE", "create_channel");
                        resp.add_pair("STATUS", "fail");
                        resp.add_pair("REASON", "channel_is_already_created");
                        resp.end_obj();
                        resp.append("\r\n");
                        add_response(resp.get_data(), resp.get_count(),
                              &b->client_sockets.response_queues[fd]);
                     }
                     else
                     {
                        if(prev_ch)
                        {
                           message *msg = new message;
                           msg->data = get_new_str("Channel has been deleted.");
                           msg->count = strlen(msg->data);
                           msg->next = 0;

                           notify_subscribers(msg, prev_ch, b);
                           delete [] msg->data;
                           delete msg;

                           remove_channel(fd, b);
                        }

                        bool res = add_new_channel(channel_name, fd, b);
                        if(res)
                        {
                           response_maker resp;
                           resp.begin_obj();
                           resp.add_pair("QUERY_TYPE", "create_channel");
                           resp.add_pair("STATUS", "ok");
                           resp.end_obj();
                           resp.append("\r\n");
                           add_response(resp.get_data(), resp.get_count(),
                                       &b->client_sockets.response_queues[fd]);
                        }
                        else
                        { 
                           response_maker resp;
                           resp.begin_obj();
                           resp.add_pair("QUERY_TYPE", "create_channel");
                           resp.add_pair("STATUS", "fail");
                           resp.add_pair("REASON", "channel_already_exists");
                           resp.end_obj();
                           resp.append("\r\n");
                           add_response(resp.get_data(), resp.get_count(),
                                       &b->client_sockets.response_queues[fd]);
                        }
                     }

                     delete [] channel_name;
                  }
                  else
                  {
                     response_maker resp;
                     resp.begin_obj();
                     resp.add_pair("QUERY_TYPE", "create_channel");
                     resp.add_pair("STATUS", "fail");
                     resp.add_pair("REASON", "channel_name_is_not_specified");
                     resp.end_obj();
                     resp.append("\r\n");
                     add_response(resp.get_data(), resp.get_count(),
                           &b->client_sockets.response_queues[fd]);
                  } 
               }
               else
               {
                  response_maker resp;
                  resp.begin_obj();
                  resp.add_pair("QUERY_TYPE", "create_channel");
                  resp.add_pair("STATUS", "fail");
                  resp.add_pair("REASON", "channel_name_property_required");
                  resp.end_obj();
                  resp.append("\r\n");
                  add_response(resp.get_data(), resp.get_count(),
                        &b->client_sockets.response_queues[fd]);
               }
            }
            else if(r >= 2 && jsoneq(jsonstr, &t[2], "publish_msg") == 0)
            {
               char *msg_body = 0;
               int msg_count = 0;

               if(r >= 3 && jsoneq(jsonstr, &t[3], "MSG_BODY") == 0)
               {
                  if(r >= 4 && t[4].type == JSMN_STRING)
                  {
                     msg_body = get_new_str(jsonstr, t[4].start, t[4].end);
                     msg_count = t[4].end - t[4].start;   
                     channel *ch = get_channel(fd, b);

                     if(ch)
                     {
                        add_new_message_to_channel(msg_body, msg_count, ch, b);

                        response_maker resp;
                        resp.begin_obj();
                        resp.add_pair("QUERY_TYPE", "publish_msg");
                        resp.add_pair("STATUS", "ok");
                        resp.end_obj();
                        resp.append("\r\n");
                        add_response(resp.get_data(), resp.get_count(),
                              &b->client_sockets.response_queues[fd]);
                     }
                     else
                     {
                        response_maker resp;
                        resp.begin_obj();
                        resp.add_pair("QUERY_TYPE", "publish_msg");
                        resp.add_pair("STATUS", "fail");
                        resp.add_pair("REASON", "no_channel_created");
                        resp.end_obj();
                        resp.append("\r\n");
                        add_response(resp.get_data(), resp.get_count(),
                              &b->client_sockets.response_queues[fd]); 
                     }

                  }
                  else
                  {
                     response_maker resp;
                     resp.begin_obj();
                     resp.add_pair("QUERY_TYPE", "publish_msg");
                     resp.add_pair("STATUS", "fail");
                     resp.add_pair("REASON", "no_msg_body");
                     resp.end_obj();
                     resp.append("\r\n");
                     add_response(resp.get_data(), resp.get_count(),
                           &b->client_sockets.response_queues[fd]); 
                  }
               }
               else
               {
                  response_maker resp;
                  resp.begin_obj();
                  resp.add_pair("QUERY_TYPE", "publish_msg");
                  resp.add_pair("STATUS", "fail");
                  resp.add_pair("REASON", "msg_body_property_required");
                  resp.end_obj();
                  resp.append("\r\n");
                  add_response(resp.get_data(), resp.get_count(),
                        &b->client_sockets.response_queues[fd]); 

               }
            }
            else if(r >= 2 && jsoneq(jsonstr, &t[2], "subscribe") == 0)
            {
               if(r >= 3 && jsoneq(jsonstr, &t[3], "CHANNEL_NAME") == 0)
               {
                  if(r >= 4 && t[4].type == JSMN_STRING)
                  { 
                     char *channel_name = get_new_str(jsonstr, t[4].start, t[4].end); 
                     int res = change_client_subscription(&b->client_sockets.subscriptions[fd],
                           channel_name, fd, b);
                     if(res == 0)
                     {
                        response_maker resp;
                        resp.begin_obj();
                        resp.add_pair("QUERY_TYPE", "subscribe");
                        resp.add_pair("STATUS", "ok");
                        resp.end_obj();
                        resp.append("\r\n");

                        add_response(resp.get_data(), resp.get_count(),
                              &b->client_sockets.response_queues[fd]); 
                     }
                     else if(res == -1)
                     {
                        response_maker resp;
                        resp.begin_obj();
                        resp.add_pair("QUERY_TYPE", "subscribe");
                        resp.add_pair("STATUS", "fail");
                        resp.add_pair("REASON", "already_subscribed");
                        resp.end_obj();
                        resp.append("\r\n");

                        add_response(resp.get_data(), resp.get_count(),
                              &b->client_sockets.response_queues[fd]); 
                     }
                     else
                     {
                        response_maker resp;
                        resp.begin_obj();
                        resp.add_pair("QUERY_TYPE", "subscribe");
                        resp.add_pair("STATUS", "fail");
                        resp.add_pair("REASON", "channel_not_found");
                        resp.end_obj();
                        resp.append("\r\n");

                        add_response(resp.get_data(), resp.get_count(),
                              &b->client_sockets.response_queues[fd]); 
                     }
                     delete [] channel_name;
                  }
                  else
                  {
                     response_maker resp;
                     resp.begin_obj();
                     resp.add_pair("QUERY_TYPE", "subscribe");
                     resp.add_pair("STATUS", "fail");
                     resp.add_pair("REASON", "channel_name_not_specified");
                     resp.end_obj();
                     resp.append("\r\n");

                     add_response(resp.get_data(), resp.get_count(),
                           &b->client_sockets.response_queues[fd]); 
                  }
               }
               else
               {
                  response_maker resp;
                  resp.begin_obj();
                  resp.add_pair("QUERY_TYPE", "subscribe");
                  resp.add_pair("STATUS", "fail");
                  resp.add_pair("REASON", "channel_name_property_required");
                  resp.end_obj();
                  resp.append("\r\n");

                  add_response(resp.get_data(), resp.get_count(),
                        &b->client_sockets.response_queues[fd]); 
               }
            }
            else
            {
               response_maker resp;
               resp.begin_obj();
               resp.add_pair("QUERY_TYPE", "unknown");
               resp.add_pair("STATUS", "fail");
               resp.add_pair("REASON", "query_type_incorrect");
               resp.end_obj();
               resp.append("\r\n");

               add_response(resp.get_data(), resp.get_count(),
                     &b->client_sockets.response_queues[fd]); 
            }
         }
         else
         {
            response_maker resp;
            resp.begin_obj();
            resp.add_pair("QUERY_TYPE", "unknown");
            resp.add_pair("STATUS", "fail");
            resp.add_pair("REASON", "query_type_property_required");
            resp.end_obj();
            resp.append("\r\n");

            add_response(resp.get_data(), resp.get_count(),
                  &b->client_sockets.response_queues[fd]); 
         }
      } 
      q->status = query_status_finished;
   }
}

int main()
{
   int ls, res;
   broker_data broker = {};

   ls = socket(AF_INET, SOCK_STREAM, 0);

   if(ls == -1)
   {
      fprintf(stderr, "Coudn't create socket\n");
   }

   sockaddr_in addr;
   addr.sin_family = AF_INET;
   addr.sin_port = htons(SERVER_PORT);
   addr.sin_addr.s_addr = htonl(INADDR_ANY);

   int opt = 1;
   setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

   res = bind(ls, (struct sockaddr*)&addr, sizeof(addr));
   if(res == -1)
   {
      fprintf(stderr, "Couldn't bind address to socket\n");
   }

   res = listen(ls, LISTEN_QLEN);
   if(res == -1)
   {
      fprintf(stderr, "Couldn't listen on socket\n");
   }

   for(;;)
   {
      int res;
      fd_set readfds, writefds;
      int max_d = ls;

      FD_ZERO(&readfds);
      FD_ZERO(&writefds);
      FD_SET(ls, &readfds);

      for(int i = 0; i < broker.client_sockets.count; i++)
      {
         int fd = broker.client_sockets.fd[i];

         FD_SET(fd, &readfds);
         if(broker.client_sockets.response_queues[fd].head != 0)
         {
            FD_SET(fd, &writefds);
         }

         if(fd > max_d)
            max_d = fd;
      }

      timeval timeout;
      timeout.tv_sec = 1; // 1 sec
      timeout.tv_usec = 0;

      res = select(max_d + 1, &readfds, &writefds, 0, &timeout);
      
      if(res == -1)
      {
         if(errno == EINTR)
         {
            // process signal
         }
         else
         {
            // process error
            fprintf(stderr, "Error occurred during event loop\n");
         }
         continue; // no need to process fds
      }

      if(res == 0)
      {
         // process timeout
      }

      if(FD_ISSET(ls, &readfds))
      {
         // new connection
         // need to use "accept" syscall
         // use fcntl to make this connection non-blocking
         int fd = accept(ls, 0, 0);
         int flags = fcntl(fd, F_GETFL);
         fcntl(fd, F_SETFL, flags | O_NONBLOCK);
         broker.client_sockets.fd[broker.client_sockets.count++] = fd;
      }

      for(int curr_client = 0; curr_client < broker.client_sockets.count; curr_client++)
      {
         int fd = broker.client_sockets.fd[curr_client];

         if(FD_ISSET(fd, &readfds))
         {
            char buff[1024];
            int rc = read(fd, buff, sizeof(buff));

            if(rc == 0)
            {
               // EOF
               channel *ch = get_channel(fd, &broker);
               if(ch)
               {
                  message *msg = new message;
                  msg->data = get_new_str("Channel has been deleted.");
                  msg->count = strlen(msg->data);
                  msg->next = 0;

                  notify_subscribers(msg, ch, &broker);
                  delete [] msg->data;
                  delete msg;

                  remove_channel(fd, &broker);
               }
               
               bool removedLast = remove_client_socket(curr_client, &broker.client_sockets);
               if(!removedLast)
               {
                  // new client now has index curr_client.
                  // To process him we need to decrease it by one,
                  // so on the next iteration it will become this value again
                  curr_client--;
               }
               continue;
            }
            else
            {
               query_queue* qq = &broker.client_sockets.query_queues[fd];
               fill_query(qq, buff, rc);

               query* curr_q = qq->head;
               while(curr_q)
               {
                  process_query(fd, curr_q, &broker);
                  curr_q = curr_q->next;
               }

               remove_finished_queries(qq);
            }
         }

         if(FD_ISSET(fd, &writefds))
         { 
            response* resp = broker.client_sockets.response_queues[fd].head;
            int wc = write(fd, resp->data, resp->count);
            if(wc < resp->count)
            {
               memmove(resp->data, resp->data + wc, resp->count - wc);
               resp->count -= wc;
            }
            else
            {
               free_response_from_head(&broker.client_sockets.response_queues[fd]);
            }
         }
      }
   }

   return 0;
}
