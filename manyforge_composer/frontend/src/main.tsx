import React from 'react';
import ReactDOM from 'react-dom/client';
import App from './App';
import './styles/index.css';
import { OperationalErrorProvider } from './state/operationalErrors';
import { WorkbenchNoticeProvider } from './state/workbenchNotices';

ReactDOM.createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <OperationalErrorProvider>
      <WorkbenchNoticeProvider>
        <App />
      </WorkbenchNoticeProvider>
    </OperationalErrorProvider>
  </React.StrictMode>,
);
