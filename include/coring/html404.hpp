#pragma once
#include <string_view>

namespace Coring::Utilites {
    inline constexpr std::string_view HTML_404_ERROR = 
    R"(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>404 Not Found</title>
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
            text-align: center;
            padding: 40px;
            max-width: 600px;
            width: 100%;
        }
        .error-code {
            font-size: 150px;
            font-weight: 900;
            line-height: 1;
            margin-bottom: 20px;
            background: linear-gradient(to right, #38bdf8, #818cf8);
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
            text-shadow: 0px 10px 30px rgba(56, 189, 248, 0.2);
            animation: float 6s ease-in-out infinite;
        }
        .title {
            font-size: 2rem;
            font-weight: 700;
            margin-bottom: 16px;
            color: #f8fafc;
        }
        .description {
            font-size: 1.125rem;
            color: #94a3b8;
            line-height: 1.6;
        }
        .server-signature {
            margin-top: 40px;
            padding-top: 20px;
            border-top: 1px solid #1e293b;
            font-size: 0.875rem;
            color: #475569;
            font-weight: 500;
            letter-spacing: 0.05em;
        }
        
        @keyframes float {
            0% { transform: translateY(0px); }
            50% { transform: translateY(-15px); }
            100% { transform: translateY(0px); }
        }

        @media (max-width: 480px) {
            .error-code { font-size: 100px; }
            .title { font-size: 1.5rem; }
            .description { font-size: 1rem; padding: 0 20px; }
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="error-code">404</div>
        <h1 class="title">Not Found</h1>
        <p class="description">The requested resource could not be found on this server.</p>
        
        <div class="server-signature">coring</div>
    </div>
</body>
</html>)";
}