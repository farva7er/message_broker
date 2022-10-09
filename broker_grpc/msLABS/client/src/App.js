import React from 'react';
import './app.css';
import { Routes, Route } from 'react-router-dom';
import { MuiThemeProvider, createTheme } from '@material-ui/core/styles';
import { Container } from '@material-ui/core';
import AddPost from './WebSock';
import Posts from './Posts';

const styles = {
  paperContainer: {
    height: 1450,
    backgroundImage: `url(https://encrypted-tbn0.gstatic.com/images?q=tbn:ANd9GcQixOjlEt-Bzow0QjUbnmYKtUDyhuZ_yFLuTw&usqp=CAU)`,
  },
};

function App() {
  return (
    <div style={styles.paperContainer}>
      <Container>
        <Routes>
          <Route path="/" element={<AddPost />} />
          <Route path="/posts" element={<Posts />} />
        </Routes>
      </Container>
    </div>
  );
}

export default App;
