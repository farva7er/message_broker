import React, { useRef, useState } from 'react';
import TextField from '@material-ui/core/TextField';

import { Button } from '@material-ui/core';

const AddPost = () => {
  const [value, setValue] = useState('');

  const socket = useRef();
  const [connected, setConnected] = useState(false);

  const [CHANNEL_NAME, setCategory] = useState('');

  function connect() {
    socket.current = new WebSocket('ws://localhost:5000');

    socket.current.onopen = () => {
      setConnected(true);
      const message = {
        QUERY_TYPE: 'create_channel',
        CHANNEL_NAME,
      };
      socket.current.send(JSON.stringify(message));
    };
    socket.current.onmessage = (event) => {
      const message = JSON.parse(event.data);
      console.log(message);
    };
    socket.current.onclose = () => {
      console.log('Socket закрыт');
    };
    socket.current.onerror = () => {
      console.log('Socket произошла ошибка');
    };
  }

  const sendMessage = async () => {
    const message = {
      QUERY_TYPE: 'publish_msg',
      MSG_BODY: value,
    };
    socket.current.send(JSON.stringify(message));
    setValue('');
  };

  if (!connected) {
    return (
      <div className="center">
        <div className="form">
          <label>
            <input
              value={CHANNEL_NAME}
              onChange={(e) => setCategory(e.target.value)}
              type="text"
              placeholder="Enter category"
            />
          </label>
          <button onClick={connect}>Connect</button>
        </div>
      </div>
    );
  }

  return (
    <div className="center">
      <div>
        <div className="form">
          <TextField
            value={value}
            onChange={(e) => setValue(e.target.value)}
            id="outlined-multiline-static"
            label="Text"
            multiline
            rows={4}
            defaultValue="Default Value"
            variant="outlined"
          />

          <Button variant="outlined" onClick={sendMessage}>
            Send post
          </Button>
        </div>
      </div>
    </div>
  );
};

export default AddPost;
