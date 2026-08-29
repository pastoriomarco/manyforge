import React from 'react';
import ReactDOM from 'react-dom/client';
import App from './App';
import './index.css';
import { OperationalErrorProvider } from './state/operationalErrors';

ReactDOM.createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <OperationalErrorProvider>
      <App />
    </OperationalErrorProvider>
  </React.StrictMode>,
);
