const ws = require('ws');
var net = require('net');

const wss = new ws.Server(
  {
    port: 5000,
  },
  () => console.log(`Server started on 5000`)
);

wss.on('connection', function connection(ws) {
   var broker_client = net.connect(33333, '127.0.0.1',
      function() {
         console.log('connected to broker!');
      });

   ws.on('message', function (message) {
      console.log(message);
      broker_client.write(message + "\r\n");
   });

   ws.on('close', function(){
      broker_client.end();
   });

/*
   const chunks = [];
   broker_client.on('data', function (data) {
        chunks.push(data);
        broker_client.end(); 
   });

   broker_client.on('end', function () {
        const allData = Buffer.concat(chunks).toString().trim().split('\r\n');
         ws.send(allData);
   });
*/

   
   var buff = "";
   broker_client.on('data', function(data) {
      var index = 0;
      var len = data.toString().length;
      var data = data.toString();
      while(index < len)
      {
         var start = index;
         while(index < len)
         {
            if(data[index] == '\r')
            {
               console.log("\r");
               index++;
               if(index < len && data[index] == '\n')
               {
                  index++;
                  console.log(buff.toString());
                  ws.send(buff.toString());
                  buff = "";
               }
               break;
            }
            else
            {
               buff += data.charAt(index);
            }
            index++   
         }
      }
   });

});
