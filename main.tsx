console.log("1. main.tsx is loading...");

import React from 'react';
console.log("2. React imported");

import ReactDOM from 'react-dom/client';
console.log("3. ReactDOM imported");

import App from './App';
console.log("4. App imported");

const rootElement = document.getElementById('root');
console.log("5. rootElement:", rootElement);

if (!rootElement) {
  throw new Error("Could not find root element to mount to");
}

const root = ReactDOM.createRoot(rootElement);
console.log("6. root created");

root.render(
  <React.StrictMode>
    <App />
  </React.StrictMode>
);
console.log("7. render() called");