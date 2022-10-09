import React, { useRef, useState } from 'react';
import FormControl from '@material-ui/core/FormControl';
import Card from '@material-ui/core/Card';

import CardContent from '@material-ui/core/CardContent';
import Select from '@material-ui/core/Select';
import { MenuItem } from '@material-ui/core';
import Typography from '@material-ui/core/Typography';
import InputLabel from '@material-ui/core/InputLabel';
import Button from '@material-ui/core/Button';

const Posts = () => {
  const [messages, setMessages] = useState([]);
  const [categories, setCategories] = useState([]);
  const socket = useRef();
  const [connected, setConnected] = useState(false);
  const [isNotSelected, setIsNotSelected] = useState(true);
  const [selectedCategory, setSelectedCategory] = useState('');

  function connect() {
    socket.current = new WebSocket('ws://localhost:5000');

    socket.current.onopen = () => {
      const message = {
        QUERY_TYPE: 'get_channel_names',
      };
      socket.current.send(JSON.stringify(message));
    };
    socket.current.onmessage = (event) => {
      const message = JSON.parse(event.data);

      if (message.QUERY_TYPE === 'get_channel_names') {
        setCategories(message.CHANNELS);
      } else if (message.QUERY_TYPE === 'messages_for_subscriber') {
        setMessages(message.MSG_BODY_ARRAY.reverse());
        setSelectedCategory(message.CHANNEL_NAME);
      } else if (message.QUERY_TYPE === 'message_for_subscriber')
        setMessages((prev) => [message.MSG_BODY, ...prev]);
    };
    socket.current.onclose = () => {
      console.log('Socket закрыт');
    };
    socket.current.onerror = () => {
      console.log('Socket произошла ошибка');
    };
  }
  const handleChange = (e) => {
    setSelectedCategory(e.target.value);
    setIsNotSelected(false);
  };
  const subscribe = async () => {
    setConnected(true);
    const message = {
      QUERY_TYPE: 'subscribe',
      CHANNEL_NAME: selectedCategory,
    };
    socket.current.send(JSON.stringify(message));
  };

  if (!connected) {
    return (
      <FormControl fullWidth>
        <InputLabel id="demo-simple-select-label">Select category</InputLabel>
        <Select
          value={selectedCategory}
          onChange={handleChange}
          sx={{ mb: 2 }}
          onOpen={connect}
        >
          {categories?.map((categories) => {
            return (
              <MenuItem key={categories} value={categories}>
                {categories}
              </MenuItem>
            );
          })}
        </Select>
        <Button
          variant="text"
          color="primary"
          onClick={subscribe}
          disabled={isNotSelected}
        >
          Subscribe
        </Button>
      </FormControl>
    );
  }

  return (
    <div className="center">
      <div className="messages">
        {messages.map((mess, index) => (
          <div key={index}>
            <div>
              {!mess ? (
                <div className="connection_message">
                  There's no posts for {mess.CHANNEL_NAME} category now!
                </div>
              ) : (
                <div className="message">
                  <Card sx={{ minWidth: 275 }}>
                    <CardContent>
                      <Typography
                        sx={{ fontSize: 14 }}
                        color="text.secondary"
                        gutterBottom
                      >
                        {selectedCategory}
                      </Typography>
                      <Typography variant="h5" component="div">
                        {mess}
                      </Typography>
                    </CardContent>
                    {/* <CardActions>
                      <Button size="small">Learn More</Button>
                    </CardActions> */}
                  </Card>
                </div>
              )}
            </div>
          </div>
        ))}
      </div>
    </div>
  );
};

export default Posts;
