#pragma once
#include <string_view>

namespace Coring::Utilites {
    inline constexpr std::string_view HTML_200_STATUS = 
    R"(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Coring - System Status</title>
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }
        body {
            font-family: 'Inter', -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
            background-color: #0f172a;
            color: #e2e8f0;
            display: flex;
            justify-content: center;
            align-items: center;
            height: 100vh;
            overflow: hidden;
        }
        .container {
            background: #1e293b;
            padding: 40px;
            border-radius: 24px;
            box-shadow: 0 20px 40px rgba(0, 0, 0, 0.4);
            max-width: 480px;
            width: 90%;
            border: 1px solid #334155;
            text-align: center;
        }
        .status-header {
            display: flex;
            align-items: center;
            justify-content: center;
            gap: 12px;
            margin-bottom: 20px;
        }
        .status-dot {
            width: 16px;
            height: 16px;
            background-color: #22c55e;
            border-radius: 50%;
            box-shadow: 0 0 0 0 rgba(34, 197, 94, 0.7);
            animation: pulse 2s infinite;
        }
        .title {
            font-size: 1.75rem;
            font-weight: 700;
            color: #f8fafc;
        }
        .subtitle {
            font-size: 1.125rem;
            color: #22c55e;
            font-weight: 500;
            margin-bottom: 30px;
        }
        .metrics {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 16px;
            margin-bottom: 30px;
        }
        .metric-card {
            background: #0f172a;
            padding: 16px;
            border-radius: 16px;
            border: 1px solid #334155;
        }
        .metric-label {
            font-size: 0.875rem;
            color: #94a3b8;
            margin-bottom: 8px;
            text-transform: uppercase;
            letter-spacing: 0.05em;
        }
        .metric-value {
            font-size: 1.25rem;
            font-weight: 700;
            color: #f8fafc;
        }
        .server-signature {
            padding-top: 20px;
            border-top: 1px solid #334155;
            font-size: 0.875rem;
            color: #475569;
            font-weight: 500;
            letter-spacing: 0.05em;
        }

        @keyframes pulse {
            0% {
                transform: scale(0.95);
                box-shadow: 0 0 0 0 rgba(34, 197, 94, 0.7);
            }
            70% {
                transform: scale(1);
                box-shadow: 0 0 0 12px rgba(34, 197, 94, 0);
            }
            100% {
                transform: scale(0.95);
                box-shadow: 0 0 0 0 rgba(34, 197, 94, 0);
            }
        }

        @media (max-width: 480px) {
            .metrics { grid-template-columns: 1fr; }
            .container { padding: 30px 20px; }
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="status-header">
            <div class="status-dot"></div>
            <h1 class="title">System Status</h1>
        </div>
        <p class="subtitle">All systems operational</p>
        
        <div class="metrics">
            <div class="metric-card">
                <div class="metric-label">Server Engine</div>
                <div class="metric-value">Coring</div>
            </div>
            <div class="metric-card">
                <div class="metric-label">Response Time</div>
                <div class="metric-value">&lt; 1ms</div>
            </div>
        </div>

        <div class="server-signature">coring / http-server</div>
    </div>
</body>
</html>
    )";
}