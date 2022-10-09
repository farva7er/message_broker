#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>

#include <grpc/grpc.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

#include "broker_grpc.pb.h"
#include "broker_grpc.grpc.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;

using broker::Broker;
using broker::Empty;
using broker::Channel;
using broker::ChannelNames;
using broker::Message;
using broker::BrokerResult;


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





///////////////////////////////////////////////

struct channel;

#define CLIENT_MAX 1024

struct client_data
{
   channel *subscriptions[CLIENT_MAX]; 
   ServerReaderWriter<Message, Channel>* streams[CLIENT_MAX];
   int count;
};


std::mutex broker_data_mutex;

struct broker_data
{
   channel *head;
   client_data clients;
};

struct message
{
   char *data;
   int count;
   message *next;
};

struct subscriber
{
   ServerReaderWriter<Message, Channel>* stream;
   subscriber *next;
};

struct channel
{ 
   char *name;
   message *msg_head;
   message *msg_tail;
   subscriber *subs_head;
   channel *next;
};


bool add_new_channel_safe(const char *name, broker_data *broker)
{
   std::lock_guard<std::mutex> lock(broker_data_mutex);

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
   new_channel->name = get_new_str(name);
   new_channel->msg_head = 0;
   new_channel->msg_tail = 0;
   new_channel->subs_head = 0;
   new_channel->next = broker->head;
   broker->head = new_channel;
   return true;
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


void notify_subscribers(message *m, channel *t, broker_data *broker)
{
   if(m->count <= 0)
      return;
   subscriber *curr = t->subs_head;

   Message msg;
   msg.set_channelname(t->name);
   msg.set_body(m->data, m->count);

   while(curr)
   { 
      curr->stream->Write(msg);
      curr = curr->next;
   }
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

void remove_subscriber(subscriber **head, ServerReaderWriter<Message, Channel>* stream)
{
   for(subscriber **curr_sub = head;
         *curr_sub;
         curr_sub = &((*curr_sub)->next))
   {
      if((*curr_sub)->stream == stream)
      {
         subscriber *tmp = *curr_sub;
         *curr_sub = (*curr_sub)->next;
         delete tmp;
         break;
      }
   }
}


void send_all_channel_messages_to_client(channel *t, ServerReaderWriter<Message, Channel>* stream)
{
   message *curr = t->msg_head;
   while(curr)
   {
      if(curr->count > 0)
      {
         Message msg;
         msg.set_channelname(t->name);
         msg.set_body(curr->data, curr->count);
         stream->Write(msg);
      }
      curr = curr->next;

   }
}


int change_client_subscription(channel **current_subsc, const char *channel_name,
                           ServerReaderWriter<Message, Channel>* stream, broker_data *broker)
{
   if(*current_subsc)
   {
      if(0 == strcmp((*current_subsc)->name, channel_name))
      {
         return -1;
      }
      remove_subscriber(&((*current_subsc)->subs_head), stream);
   }

   channel *ch = get_channel(channel_name, broker);
   if(ch)
   {
      subscriber *new_s = new subscriber;
      new_s->stream = stream;
      new_s->next = ch->subs_head;
      ch->subs_head = new_s;

      *current_subsc = ch;
      send_all_channel_messages_to_client(ch, stream);
      return 0;
   }
   else
   {
      *current_subsc = 0;
      return -2;
   }

}

channel **get_curr_subsc(ServerReaderWriter<Message, Channel> *stream, broker_data *broker)
{
   for(int i = 0; i < broker->clients.count; i++)
   {
      if(broker->clients.streams[i] == stream)
      {
         return &broker->clients.subscriptions[i];
      }
   }
   return 0;
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
      *(get_curr_subsc(s->stream, b)) = 0;
      subscriber *tmp = s;
      s = s->next;
      delete tmp;
   }
}


void remove_channel(const char *channel_name, broker_data *broker)
{
   for(channel **curr = &broker->head;
         *curr;
         curr = &((*curr)->next))
   {
      if(0 == strcmp((*curr)->name, channel_name))
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


void remove_client_safe(ServerReaderWriter<Message, Channel> *stream, broker_data *broker)
{ 
   std::lock_guard<std::mutex> lock(broker_data_mutex);

   channel** subscription = get_curr_subsc(stream, broker);
   if(*subscription)
   {
      remove_subscriber(&((*subscription)->subs_head), stream);
      *subscription = 0;
   }

   for(int i = 0; i < broker->clients.count; i++)
   {
      if(broker->clients.streams[i] == stream)
      {
         if(i != broker->clients.count - 1)
         {
            broker->clients.streams[i] = broker->clients.streams[broker->clients.count - 1];
         }
      }
   }

   broker->clients.count--; 
}


//////////////////////////////////////////////////



class BrokerImpl final : public Broker::Service
{

   public:
      Status CreateChannel(ServerContext* context, const Channel* c,
                           BrokerResult* result) override
      {
         bool res = add_new_channel_safe(c->name().c_str(), &data);
         if(res)
         {
            result->set_status("ok");
            result->set_reason("");
         }
         else
         {
            result->set_status("fail");
            result->set_reason("channel_name_is_already_taken");
         }
         return Status::OK;
      }

      Status GetChannels(ServerContext* context, const Empty* e, ChannelNames* channels) override
      {
         std::lock_guard<std::mutex> lock(broker_data_mutex);

         channel *curr = data.head;
         while(curr)
         {
            channels->add_names(curr->name);
            curr = curr->next;
         }

         return Status::OK;
      }

      Status PublishMessage(ServerContext* context, const Message* m, BrokerResult* result) override
      {
         std::lock_guard<std::mutex> lock(broker_data_mutex);
         channel *ch = get_channel(m->channelname().c_str(), &data);
         if(ch)
         {
            add_new_message_to_channel(get_new_str(m->body().c_str()),
                                       m->body().size(), ch, &data);
            if(m->body() == "Channel has been deleted.")
            {
               remove_channel(m->channelname().c_str(), &data);
            }
            result->set_status("ok");
            result->set_reason("");
         }
         else
         {
            result->set_status("fail");
            result->set_reason("channel_not_found");
         }

         return Status::OK;
      }

      Status Subscribe(ServerContext* context,
                        ServerReaderWriter<Message, Channel>* stream) override
      {
         {
            std::lock_guard<std::mutex> lock(broker_data_mutex);
            data.clients.streams[data.clients.count] = stream;
            data.clients.subscriptions[data.clients.count++] = 0;
         }

         Channel ch;
         while(stream->Read(&ch))
         {
            if(ch.name() == "") break;

            std::lock_guard<std::mutex> lock(broker_data_mutex);
            int res = change_client_subscription(get_curr_subsc(stream, &data),
                                             ch.name().c_str(), stream, &data);
            if(res != 0)
               break;
         }
         remove_client_safe(stream, &data);
         return Status::OK;
      }

   private:
      broker_data data;
};

void RunServer()
{
   std::string server_address("0.0.0.0:33333");
   BrokerImpl service;

   ServerBuilder builder;
   builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
   builder.RegisterService(&service);
   std::unique_ptr<Server> server(builder.BuildAndStart());
   std::cout << "Server listening on " << server_address << std::endl;
   server->Wait();
}

int main(int argc, char** argv)
{
   RunServer();
   return 0;
}
