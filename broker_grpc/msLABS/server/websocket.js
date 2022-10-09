const ws = require('ws');
var net = require('net');

var PROTO_PATH = __dirname + '../../../broker_grpc.proto';

var async = require('async');
var fs = require('fs');
var parseArgs = require('minimist');
var path = require('path');
var _ = require('lodash');
var grpc = require('@grpc/grpc-js');
var protoLoader = require('@grpc/proto-loader');
var packageDefinition = protoLoader.loadSync(
   PROTO_PATH,
   {keepCase: true,
      longs: String,
      enums: String,
      defaults: true,
      oneofs: true
   }
);
var broker = grpc.loadPackageDefinition(packageDefinition).broker;

const wss = new ws.Server(
  {
    port: 5000,
  },
  () => console.log(`Server started on 5000`)
);

wss.on('connection', function connection(ws) {

   var broker_client = new broker.Broker('localhost:33333',
                           grpc.credentials.createInsecure());
   var channel_name = "";
   var subscribe_call;
   ws.on('message', function (message) {
      message = JSON.parse(message);
      console.log(message);

      if(message.QUERY_TYPE === "create_channel")
      {
         function resultCallback(error, grpc_result) {
            var resp = {};
            resp.QUERY_TYPE = "create_channel";
            if(grpc_result.status !== "ok")
            {
               channel_name = "";
               resp.STATUS = "fail";
               resp.REASON = "failed_to_create";
            }
            else
            {
               resp.STATUS = "ok";
               resp.REASON = "";
            }
            ws.send(JSON.stringify(resp));
         }
         channel_name = message.CHANNEL_NAME;
         var grpc_channel = {};
         grpc_channel.name = message.CHANNEL_NAME;
         broker_client.createChannel(grpc_channel, resultCallback);
      }
      else if(message.QUERY_TYPE === "get_channel_names")
      {
         function channelNamesCallback(error, grpc_result) {
            var resp = {};
            resp.QUERY_TYPE = "get_channel_names";
            resp.CHANNELS = grpc_result.names;
            console.log(resp);
            ws.send(JSON.stringify(resp));
         }
         var grpc_empty = {};
         broker_client.getChannels(grpc_empty, channelNamesCallback);
      }
      else if(message.QUERY_TYPE === "publish_msg")
      {
         function publishCallback(error, grpc_result) {
            var resp = {};
            resp.QUERY_TYPE = "publish_msg";
            if(grpc_result.status !== "ok")
            {
               resp.STATUS = "fail";
               resp.REASON = "failed_to_publish";
            }
            else
            {
               resp.STATUS = "ok";
               resp.REASON = "";
            }

            console.log(resp);
            ws.send(JSON.stringify(resp));
         }

         if(channel_name !== "")
         {
            var grpc_msg = {};
            grpc_msg.channelName = channel_name;
            grpc_msg.body = message.MSG_BODY;
            broker_client.publishMessage(grpc_msg, publishCallback);
         }
      }
      else if(message.QUERY_TYPE === "subscribe")
      {
         var grpc_channel = {}
         grpc_channel.name = message.CHANNEL_NAME;

         subscribe_call = broker_client.subscribe();
         subscribe_call.on('data', function(grpc_msg) {
            var resp = {};
            resp.QUERY_TYPE = "message_for_subscriber";
            resp.CHANNEL_NAME = grpc_msg.channelName;
            resp.MSG_BODY = grpc_msg.body;
            ws.send(JSON.stringify(resp));
            console.log(resp);
         });
         subscribe_call.write(grpc_channel);
      }
   });

   ws.on('close', function(){
         function publishCallback(error, grpc_result) {}
         if(channel_name !== "")
         {
            var grpc_msg = {};
            grpc_msg.channelName = channel_name
            grpc_msg.body = "Channel has been deleted.";
            broker_client.publishMessage(grpc_msg, publishCallback);
         }
         else
         {
            if(subscribe_call)
            {
               var grpc_channel = {};
               grpc_channel.name = "";
               subscribe_call.write(grpc_channel);
               subscribe_call.end();
            }
         }
   }); 
});
