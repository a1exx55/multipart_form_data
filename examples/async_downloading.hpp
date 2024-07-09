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
namespace asio = boost::asio;  
namespace http = boost::beast::http;      
using tcp = boost::asio::ip::tcp;

class http_session : public std::enable_shared_from_this<http_session>
{
    public:
        explicit http_session(tcp::socket&& socket)
            : _stream(std::move(socket)), _form_data{_stream, _buffer}
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
            set_request_props();
            do_read_header();
        }

        void set_request_props()
        {
            // Make the request parser empty before reading new request,
            // otherwise the operation behavior is undefined
            _request_parser.emplace();

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

            if (error_code)
            {
                return do_close();
            }

            // Reset the timeout
            beast::get_lowest_layer(_stream).expires_never();

            _form_data.async_download(
                _request_parser->get()[http::field::content_type], 
                {
                    .on_read_file_header_handler = 
                        [](std::string_view file_name, int& some_data, std::string& some_string)
                        {
                            some_data = 3; 
                            std::cout << "header: " << some_data << "\t" << some_string << "\n";
                            return std::filesystem::path{".."} / file_name;
                        },

                    .on_read_file_body_handler = 
                        [](const std::filesystem::path& file_path, int& some_data, std::string& some_string)
                        {
                            some_string = "world";
                            std::cout << "body: " << some_data << "\t" << some_string << "\n";
                            std::cout << file_path << " is downloaded!\n";
                        }
                },
                beast::bind_front_handler(
                    &http_session::on_download_files, 
                    shared_from_this()), 
                shared_from_this(),
                std::ref(_some_data),
                std::string{"hello"});
        }

        void on_download_files(
            beast::error_code error_code, 
            std::vector<std::filesystem::path>&& file_paths, 
            int some_data, 
            std::string&& some_string)
        {
            std::cout << "result: " << some_data << "\t" << some_string << "\n";

            if (error_code)
            {
                _response.body() = error_code.message();
            }
            else
            {
                _response.body() = "Successfully downloaded files:\n";
                for (const auto& file_path : file_paths)
                {
                    _response.body() += file_path.string() + "\n";
                }
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

            beast::error_code error_code;

            // Perform the shutdown
            _stream.socket().shutdown(tcp::socket::shutdown_send, error_code);
        }

        int _some_data = 5;
        beast::tcp_stream _stream;
        beast::flat_buffer _buffer{};
        std::optional<http::request_parser<http::string_body>> _request_parser;
        http::response<http::string_body> _response;
        multipart_form_data::downloader<beast::tcp_stream, beast::flat_buffer> _form_data;
};

class listener : public std::enable_shared_from_this<listener>
{
    public:
        listener(asio::io_context& io_context, tcp::endpoint endpoint)
            :_io_context(io_context), _acceptor(asio::make_strand(io_context))
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
                    std::move(socket))->run();
            }

            // Accept another connection
            do_accept();
        }
        
        asio::io_context& _io_context;
        tcp::acceptor _acceptor;
};

void async_downloading_example()
{
    // The io_context is required for all I/O
    asio::io_context io_context{1};

    // Create and launch a listening port
    std::make_shared<listener>(
        io_context,
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