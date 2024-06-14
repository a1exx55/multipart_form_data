#include <boost/asio/strand.hpp>
#include <boost/asio/signal_set.hpp>
// #include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <iostream>
#include <optional>
#include <thread>

#include <multipart_form_data/multipart_form_data.hpp>

namespace beast = boost::beast;        
namespace asio = boost::asio;  
namespace http = boost::beast::http;      
namespace ssl = boost::asio::ssl;       
using tcp = boost::asio::ip::tcp;

class http_session : public std::enable_shared_from_this<http_session>
{
    public:
        explicit http_session(tcp::socket&& socket, ssl::context& ssl_context)
            : _stream(std::move(socket), ssl_context), _form_data{std::make_shared<multipart_form_data::downloader>(_stream, _buffer)}
        {
            _response.keep_alive(true);
            _response.version(11);
        }

        // Start the asynchronous http session
        void run()
        {
            // We need to be executing within a strand to perform async operations
            // on the I/O objects as we used make_strand in listener to accept connection
            asio::dispatch(
                _stream.get_executor(),
                beast::bind_front_handler(
                    &http_session::on_run,
                    shared_from_this()));
        }

    private:
        void on_run()
        {
            // Set the timeout.
            beast::get_lowest_layer(_stream).expires_after(std::chrono::seconds(30));
            
            // Perform the SSL handshake
            _stream.async_handshake(
                ssl::stream_base::server,
                beast::bind_front_handler(
                    &http_session::on_handshake,
                    shared_from_this()));
        }
        
        void on_handshake(beast::error_code error_code)
        {
            // The error means that the timer on the logical operation(write/read) is expired
            if (error_code == beast::error::timeout)
            {
                return;
            }

            // The error means that client can't make correct ssl handshake or just send random data packets
            if (error_code)
            {
                return do_close();
            }

            set_request_props();
            do_read_header();
        }

        void set_request_props()
        {
            // Make the request parser empty before reading new request,
            // otherwise the operation behavior is undefined
            _request_parser.emplace();

            // Erase previous Set-Cookie field values
            _response.erase(http::field::set_cookie);
            // Clear previous body data
            _response.body().clear();

            // Set unlimited body to prevent "body limit exceeded" error
            _request_parser->body_limit(boost::none);

            // Clear the buffer for each request
            _buffer.clear();
        }

        void do_read_header()
        {
            // Set the timeout for next operation
            beast::get_lowest_layer(_stream).expires_after(std::chrono::seconds(30));
            
            // Read a request header
            http::async_read_header(_stream, _buffer, *_request_parser,
                beast::bind_front_handler(
                    &http_session::on_read_header,
                    shared_from_this()));
        }

        void on_read_header(beast::error_code error_code, std::size_t bytes_transferred)
        {
            // Suppress compiler warnings about unused variable bytes_transferred  
            boost::ignore_unused(bytes_transferred);

            // The errors mean that client closed the connection 
            if (error_code == http::error::end_of_stream || error_code == asio::ssl::error::stream_truncated)
            {
                return do_close();
            }

            // The error means that the timer on the logical operation(write/read) is expired
            if (error_code == beast::error::timeout)
            {
                return;
            }

            // Request has invalid header structure so it can't be processed
            if (error_code)
            {
                return do_write_response(false);
            }   

            // Reset the timeout
            beast::get_lowest_layer(_stream).expires_never();

            _form_data->download(_request_parser->get()[http::field::content_type], 
                {
                    .on_read_file_header_handler = [](std::string_view file_name){return std::filesystem::path{".."} / file_name;}
                }, 
                beast::bind_front_handler(
                    &http_session::on_download_files, shared_from_this()), 
                shared_from_this());
        }

        void on_download_files(beast::error_code error_code, std::vector<std::filesystem::path>&& file_paths)
        {
            if (error_code)
            {
                _response.body() = error_code.message();
            }
            else
            {
                _response.body() = "Success";
            }

            return do_write_response(false);
        }

        void do_write_response(bool keep_alive)
        {
            _response.result(200);
            _response.prepare_payload();
            // Set the timeout for next operation
            beast::get_lowest_layer(_stream).expires_after(std::chrono::seconds(30));

            // Write the response
            http::async_write(
                _stream,
                _response,
                beast::bind_front_handler(
                    &http_session::on_write_response,
                    shared_from_this(), 
                    keep_alive));
        }

        void on_write_response(bool keep_alive, beast::error_code error_code, std::size_t bytes_transferred)
        {
            // Suppress compiler warnings about unused variable bytes_transferred  
            boost::ignore_unused(bytes_transferred);

            // The error means that the timer on the logical operation(write/read) is expired
            if (error_code == beast::error::timeout)
            {
                return;
            }

            if (error_code)
            {
                return do_close();
            }

            // Determine either we have to read the next request or close the connection
            // depending on either the response was regular or error
            if (keep_alive)
            {
                set_request_props();    
                do_read_header();
            }
            else
            {
                do_close();
            }
        }

        void do_close()
        {
            // Set the timeout.
            beast::get_lowest_layer(_stream).expires_after(std::chrono::seconds(30));

            // Perform the SSL shutdown
            _stream.async_shutdown([self = shared_from_this()](beast::error_code){});
        }

        beast::ssl_stream<beast::tcp_stream> _stream;
        beast::flat_buffer _buffer;
        std::optional<http::request_parser<http::string_body>> _request_parser;
        http::response<http::string_body> _response;
        std::shared_ptr<multipart_form_data::downloader> _form_data;
};

class listener : public std::enable_shared_from_this<listener>
{
    public:
        listener(asio::io_context& io_context, ssl::context& ssl_context, tcp::endpoint endpoint)
            :_io_context(io_context), _ssl_context(ssl_context), _acceptor(io_context)
        {
            beast::error_code error_code;
            
            _acceptor.open(endpoint.protocol(), error_code);
            if (error_code)
            {
                std::cerr << error_code.message();
                return;
            }

            _acceptor.set_option(asio::socket_base::reuse_address(true), error_code);
            if (error_code)
            {
                std::cerr << error_code.message();
                return;
            }

            _acceptor.bind(endpoint, error_code);
            if (error_code)
            {
                std::cerr << error_code.message();
                return;
            }

            _acceptor.listen(asio::socket_base::max_listen_connections, error_code);
            if (error_code)
            {
                std::cerr << error_code.message();
                return;
            }
        }
            
        void run()
        {
            do_accept();
        }

    private:
        void do_accept()
        {
            // Use make_strand to simplify work with access to shared resourses while using multiple threads
            _acceptor.async_accept(
                asio::make_strand(_io_context),
                beast::bind_front_handler(
                    &listener::on_accept,
                    shared_from_this()));
        }

        void on_accept(beast::error_code error_code, tcp::socket socket)
        {
            if (error_code)
            {
                std::cerr << error_code.message();
                return;
            }
            else
            {
                // Create the session and run it
                std::make_shared<http_session>(
                    std::move(socket),
                    _ssl_context)->run();
            }

            // Accept another connection
            do_accept();
        }
        
        asio::io_context& _io_context;
        ssl::context& _ssl_context;
        tcp::acceptor _acceptor;
};

int main()
{
    // The io_context is required for all I/O
    asio::io_context io_context{1};

    // The SSL context is required, and holds certificates
    ssl::context ssl_context{ssl::context::tlsv12};

    // Load and set server certificate to enable ssl
    ssl_context.set_options(
        boost::asio::ssl::context::default_workarounds |
        boost::asio::ssl::context::no_sslv2 |
        boost::asio::ssl::context::no_sslv3 |
        boost::asio::ssl::context::no_tlsv1_1);

    // Set ssl certificate
    ssl_context.use_certificate_chain_file("");

    // Set ssl key
    ssl_context.use_private_key_file("", boost::asio::ssl::context::file_format::pem);

    // Create and launch a listening port
    std::make_shared<listener>(
        io_context,
        ssl_context,
        tcp::endpoint{asio::ip::make_address("127.0.0.1"), 12345})->run();

    // Capture SIGINT and SIGTERM to perform a clean shutdown
    asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait(
        [&io_context](const beast::error_code&, int)
        {
            // Stop the io_context. This will cause run()
            // to return immediately, eventually destroying the
            // io_context and all of the sockets in it
            io_context.stop();
        });

    io_context.run();
}