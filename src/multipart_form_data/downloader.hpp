#ifndef MULTIPART_FORM_DATA_DOWNLOADER_HPP
#define MULTIPART_FORM_DATA_DOWNLOADER_HPP

#include <boost/beast/ssl.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <filesystem>

namespace multipart_form_data
{
    class downloader
    {
        public:
            struct settings
            {
                // Default packet size is 10 MB
                size_t packet_size{10 * 1024 * 1024};
                
                static settings default_value() 
                { 
                    return {}; 
                }
            };

            downloader(
                boost::beast::ssl_stream<boost::beast::tcp_stream>& ssl_stream, 
                const boost::asio::mutable_buffer& buffer,
                std::string_view content_type,
                const std::filesystem::path& output_directory,
                const settings& custom_settings = settings::default_value())
                : 
                _stream{ssl_stream}, 
                _string_buffer_storage{boost::asio::buffer_cast<const char*>(buffer), buffer.size()},
                _content_type{content_type}, 
                _settings{custom_settings},
                _buffer{_string_buffer_storage, _settings.packet_size},
                _output_directory{output_directory}
            {}

            template<::boost::asio::completion_token_for<void(
                ::boost::beast::error_code, 
                std::vector<std::filesystem::path>&&)> handler_t>
            void download(handler_t&& handler)
            {
                prepare_files_processing(std::forward<handler_t>(handler));
            }
            
        private:
            template<::boost::asio::completion_token_for<void(
                ::boost::beast::error_code, 
                std::vector<std::filesystem::path>&&)> handler_t>
            void prepare_files_processing(handler_t&& handler)
            {
            }

            template<::boost::asio::completion_token_for<void(
                ::boost::beast::error_code, 
                std::vector<std::filesystem::path>&&)> handler_t>
            void process_file_header()
            {}

            template<::boost::asio::completion_token_for<void(
                ::boost::beast::error_code, 
                std::vector<std::filesystem::path>&&)> handler_t>
            void process_file_body()
            {}

            boost::beast::ssl_stream<boost::beast::tcp_stream>& _stream;
            std::string _string_buffer_storage;
            std::string_view _content_type;
            settings _settings{};
            boost::asio::dynamic_string_buffer<char, std::char_traits<char>, std::allocator<char>> _buffer;
            std::filesystem::path _output_directory;
    };
};

#endif