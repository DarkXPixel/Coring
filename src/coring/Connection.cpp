#include "coring/Connection.hpp"
#include "coring/IOEngine.hpp"
#include "coring/IOTask.hpp"
#include "coring_uring/UringEngine.hpp"
#include "coring/html404.hpp"


namespace Coring {
    template<IOEngineConcept Engine>
    IOTask2<> Connection<Engine>::handle_client() {
        

        static const std::string HTTP_RESPONSE = std::format( 
        "HTTP/1.1 404 Not Found\r\n"
        "Server: Coring\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Content-Length: {}\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{}"
        , Utilites::HTML_404_ERROR.length(), Utilites::HTML_404_ERROR.data());


        int bytes_needed_write = HTTP_RESPONSE.size();
        while(bytes_needed_write > 0) {
            int bytes_written = co_await socket_.async_write(HTTP_RESPONSE.data() + (HTTP_RESPONSE.size() - bytes_needed_write), bytes_needed_write);
            if(bytes_written <= 0) {
                break;
            }
            bytes_needed_write -= bytes_written;
        }
        co_return;
    }

    template class Connection<UringEngine>;

}