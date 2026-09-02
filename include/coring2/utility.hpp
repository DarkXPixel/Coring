#pragma once

#include <cstdint>
#include <string_view>
namespace Coring2 {
enum class HttpStatus : uint16_t {
  OK = 200,
  Created = 201,
  Accepted = 202,
  NoContent = 204,
  BadRequest = 400,
  Unauthorized = 401,
  Forbidden = 403,
  NotFound = 404,
  HeaderTooLarge = 431,
  InternalServerError = 500,
  BadGateway = 502,
  ServiceUnavaiable = 503
};

constexpr std::string_view get_status_message(uint16_t code) {
  switch (code) {
  case 200:
    return "OK";
  case 201:
    return "Created";
  case 202:
    return "Accepted";
  case 204:
    return "No Content";
  case 400:
    return "Bad Request";
  case 401:
    return "Unauthorized";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 431:
    return "Request Header Fields Too Large";
  case 500:
    return "Internal Server Error";
  case 502:
    return "Bad Gateway";
  case 503:
    return "Service Unavailable";
  default:
    return "Unknown";
  }
}
} // namespace Coring2