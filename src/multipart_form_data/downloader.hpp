#ifndef MULTIPART_FORM_DATA_DOWNLOADER_HPP
#define MULTIPART_FORM_DATA_DOWNLOADER_HPP

#include <boost/asio/read_until.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <filesystem>

namespace multipart_form_data
{
    class downloader
    {
        public:
            // Custom settings that can be applied to the downloading process to change its behavior or properties. 
            struct settings
            {
                // The size of packets that will be used to read files by chunks.
                //
                // Default packet size is 10 MB.
                size_t packet_size{10 * 1024 * 1024};
                // The waiting time of read operations' execution. After expiry of this time the operation
                // will be canceled and request will be aborted with corresponding error code.
                //
                // Default timeout is 30 seconds.
                std::chrono::steady_clock::duration operations_timeout{std::chrono::seconds(30)};    
                // The directory where the downloaded files will be placed.
                //       
                // Default output directory is the current execution one.
                std::filesystem::path output_directory{"."};
            };

            downloader(
                boost::beast::ssl_stream<boost::beast::tcp_stream>& ssl_stream, 
                const boost::beast::flat_buffer& buffer)
                : 
                _stream{ssl_stream},
                _input_buffer{buffer} 
            {}

            template<boost::asio::completion_token_for<void(
                boost::beast::error_code, 
                std::vector<std::filesystem::path>&&)> handler_t>
            void download(std::string_view content_type, const settings& custom_settings, handler_t&& handler)
            {
                // Check if the request is actually multipart/form-data
                if (content_type.find("multipart/form-data") == std::string::npos)
                {
                    return handler(boost::asio::error::invalid_argument, std::vector<std::filesystem::path>{});
                }

                _settings = custom_settings;

                prepare_files_processing(content_type, std::forward<handler_t>(handler));
            }
            
        private:
            template<boost::asio::completion_token_for<void(
                boost::beast::error_code, 
                std::vector<std::filesystem::path>&&)> handler_t>
            void prepare_files_processing(std::string_view content_type, handler_t&& handler)
            {
                // Clear the previous output file paths
                _output_file_paths.clear();

                // Assign buffer storage with input buffer data because it can store some part of the request body
                _string_buffer_storage.assign(
                    boost::asio::buffer_cast<const char*>(_input_buffer.data()), 
                    _input_buffer.size());

                // Assign buffer size to the settings' packet size
                // (buffer can only be grown so add necessary size without current buffer size)
                _buffer.grow(_settings.packet_size - _buffer.size());

                size_t boundary_position = content_type.find("boundary=");

                // Boundary was not found in the content type
                if (boundary_position == std::string::npos)
                {
                    return handler(boost::asio::error::not_found, std::vector<std::filesystem::path>{});
                }

                // Determine the boundary for multipart/form-data content type
                _boundary = content_type.substr(boundary_position + 9);

                // Set the timeout
                boost::beast::get_lowest_layer(_stream).expires_after(_settings.operations_timeout);

                // Read the boundary before the header of the first file 
                boost::asio::async_read_until(_stream, _buffer, _boundary, 
                    boost::beast::bind_front_handler(
                        [this, &handler](
                            boost::beast::error_code error_code, 
                            std::size_t bytes_transferred)
                        {
                            if (error_code)
                            {
                                return handler(error_code, std::vector<std::filesystem::path>{});
                            }

                            // Consume read bytes as it is just the boundary
                            _buffer.consume(bytes_transferred);

                            // Set the timeout
                            boost::beast::get_lowest_layer(_stream).expires_after(_settings.operations_timeout);

                            // Read the first file header obtaining bytes until the empty string 
                            // that represents the delimiter between file header and data itself
                            boost::asio::async_read_until(_stream, _buffer, "\r\n\r\n", 
                                boost::beast::bind_front_handler( 
                                    &multipart_form_data::downloader::process_file_header,
                                    this,
                                    std::forward<handler_t>(handler)));
                        }));
            }

            template<boost::asio::completion_token_for<void(
                boost::beast::error_code, 
                std::vector<std::filesystem::path>&&)> handler_t>
            void process_file_header(
                handler_t&& handler, 
                boost::beast::error_code error_code, 
                std::size_t bytes_transferred)
            {
                if (error_code)
                {
                    return handler(error_code, _output_file_paths);
                }

                // Reset the timeout
                boost::beast::get_lowest_layer(_stream).expires_never();

                // Construct the string representation of obtained file header
                std::string_view file_header_data{
                    boost::asio::buffer_cast<const char*>(_buffer.data()), 
                    bytes_transferred};

                // Position of the filename field in the file header
                size_t file_name_position = file_header_data.find("filename=\"");

                // filename field is absent
                if (file_name_position == std::string::npos)
                {
                    return handler(boost::asio::error::not_found, _output_file_paths);
                }

                // Shift to the filename itself(e.g. filename="file.txt")
                file_name_position += 10;

                std::string file_name = std::string{file_header_data.substr(
                    file_name_position, file_header_data.find('"', file_name_position) - file_name_position)};
                std::cerr << file_name << "\n";
                file_name_position = file_name.find_last_of('.');
                std::string file_extension = file_name.substr(file_name_position + 1);
                file_name = file_name.substr(0, file_name_position);
    
            }

            template<boost::asio::completion_token_for<void(
                boost::beast::error_code, 
                std::vector<std::filesystem::path>&&)> handler_t>
            void process_file_body()
            {}

            boost::beast::ssl_stream<boost::beast::tcp_stream>& _stream;
            // Buffer that is used to read requests outside this class
            // It is necessary because it can already store some part of the request body
            const boost::beast::flat_buffer& _input_buffer;
            // String buffer storage that actually contains data
            std::string _string_buffer_storage;
            // Wrapper around the string storage to control the read operations
            boost::asio::dynamic_string_buffer<char, std::char_traits<char>, std::allocator<char>> _buffer{_string_buffer_storage};
            settings _settings{};
            std::string_view _boundary{};
            std::vector<std::filesystem::path> _output_file_paths{};
    };
};

#endif