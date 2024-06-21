#include <boost/asio/strand.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/core/flat_buffer.hpp>

#include <iostream>
#include <optional>
#include <thread>

#include <multipart_form_data/multipart_form_data.hpp>

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
using tcp = boost::asio::ip::tcp;

// Handles an HTTP server connection
void do_session(tcp::socket& socket)
{
    beast::error_code error_code;

    beast::flat_buffer buffer;
    std::optional<http::request_parser<http::string_body>> request_parser;
    http::response<http::string_body> response;
    multipart_form_data::downloader form_data{socket, buffer};

    for(;;)
    {
        // Make the request parser empty before reading new request,
        // otherwise the operation behavior is undefined
        request_parser.emplace();

        // Clear previous body data
        response.body().clear();

        // Set unlimited body to prevent "body limit exceeded" error
        request_parser->body_limit(boost::none);

        // Clear the buffer for each request
        buffer.clear();

        // Read a request
        http::read_header(socket, buffer, *request_parser, error_code);
        
        if (error_code)
        {
            break;
        }   

        std::vector<std::filesystem::path> file_paths = form_data.sync_download(
            request_parser->get()[http::field::content_type], 
            {
                .on_read_file_header_handler = [](std::string_view file_name){return std::filesystem::path{".."} / file_name;},
                .on_read_file_body_handler = [](const std::filesystem::path& file_path){std::cerr << file_path << " is downloaded!\n";}
            },
            error_code);

        if (error_code)
        {
            response.body() = error_code.message();
        }
        else
        {
            response.body() = "Successfully downloaded files:\n";
            for (const auto& file_path : file_paths)
            {
                response.body() += file_path.string() + "\n";
            }
        }

        response.result(200);
        response.prepare_payload();

        // Send the response
        http::write(socket, response, error_code);

        if (error_code)
        {
            break;
        }
    }

    // Send a TCP shutdown
    socket.shutdown(tcp::socket::shutdown_send, error_code);

    // At this point the connection is closed gracefully
}

//------------------------------------------------------------------------------

void sync_downloading_example()
{
    try
    {
        // The io_context is required for all I/O
        asio::io_context ioc{1};

        // The acceptor receives incoming connections
        tcp::acceptor acceptor{ioc, tcp::endpoint{asio::ip::make_address("127.0.0.1"), 12345}};
        for(;;)
        {
            // This will receive the new connection
            tcp::socket socket{ioc};

            // Block until we get a connection
            acceptor.accept(socket);

            // Launch the session, transferring ownership of the socket
            std::thread{
                std::bind(&do_session, std::move(socket))}.detach();
        }
    }
    catch (const std::exception& ex)
    {
        std::cerr << ex.what() << "\n";
    }
}