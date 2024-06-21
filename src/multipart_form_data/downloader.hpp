#ifndef MULTIPART_FORM_DATA_DOWNLOADER_HPP
#define MULTIPART_FORM_DATA_DOWNLOADER_HPP

#include <boost/asio/read_until.hpp>
#include <filesystem>
#include <fstream>

namespace multipart_form_data
{
    template<typename read_stream, typename dynamic_buffer>
    class downloader
    {
        public:
            // Custom settings that can be applied to the downloading process to change its behavior or properties. 
            struct settings
            {
                // The size of packets that will be used to read files by chunks.
                //
                // Default packet size is 10 MB.
                size_t packets_size{10 * 1024 * 1024};
                // The waiting time of asynchronous read operations' execution. After expiry of this time 
                // the operation will be canceled and request will be aborted with corresponding error code.
                // Does nothing if used in sync_download
                //
                // Default timeout is 30 seconds.
                std::chrono::steady_clock::duration operations_timeout{std::chrono::seconds(30)};    
                // The directory where the downloaded files will be placed 
                // if on_read_file_header_handler is not defined or it returns empty path.
                //       
                // Default output directory is the current execution one.
                std::filesystem::path output_directory{"."};
                // The function that will be invoked when each file header, containing file metadata, is read.
                // File name is provided as the function argument.
                // Function return value can be used to provide file path to write into.
                // If this handler is not defined or it returns empty path then file will be written into output_directory with unique name.
                std::function<std::filesystem::path(std::string_view)> on_read_file_header_handler{};
                // The function that will be invoked when each file body is entirely read and written to filesystem.
                // Path of the output file is provided as the function argument.
                std::function<void(const std::filesystem::path&)> on_read_file_body_handler{};
            };

            downloader(
                read_stream& stream, 
                const dynamic_buffer& buffer)
                : 
                _stream{stream},
                _input_buffer{buffer}
            {}

            template<
                boost::asio::completion_token_for<void(
                    boost::beast::error_code, 
                    std::vector<std::filesystem::path>&&)> handler_t, 
                typename session_t>
            void async_download(
                std::string_view content_type, 
                const settings& custom_settings, 
                handler_t&& handler, 
                std::shared_ptr<session_t> self_ptr)
            {
                // Check if the request is actually multipart/form-data
                if (content_type.find("multipart/form-data") == std::string::npos)
                {
                    return handler(boost::asio::error::invalid_argument, std::vector<std::filesystem::path>{});
                }

                _settings = custom_settings;

                async_prepare_files_processing(content_type, std::forward<handler_t>(handler), std::move(self_ptr));
            }

            std::vector<std::filesystem::path> sync_download(
                std::string_view content_type, 
                const settings& custom_settings,
                boost::beast::error_code& error_code)
            {
                // Check if the request is actually multipart/form-data
                if (content_type.find("multipart/form-data") == std::string::npos)
                {
                    error_code = boost::asio::error::invalid_argument;
                    
                    return std::vector<std::filesystem::path>{};
                }

                _settings = custom_settings;

                sync_prepare_files_processing(content_type, error_code);

                return _output_file_paths;
            }
            
        private:
            template<
                boost::asio::completion_token_for<void(
                    boost::beast::error_code, 
                    std::vector<std::filesystem::path>&&)> handler_t, 
                typename session_t>
            void async_prepare_files_processing(
                std::string_view content_type, 
                handler_t&& handler, 
                std::shared_ptr<session_t>&& self_ptr)
            {
                // Clear the previous output file paths
                _output_file_paths.clear();

                // Assign buffer storage with input buffer data because it can store some part of the request body
                _buffer_storage.assign(
                    boost::asio::buffers_begin(_input_buffer.data()),
                    boost::asio::buffers_end(_input_buffer.data()));

                // Reinitialize buffer with specified packets size limit
                _buffer.emplace(_buffer_storage, _settings.packets_size);

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
                boost::asio::async_read_until(_stream, *_buffer, _boundary, 
                    boost::beast::bind_front_handler(
                        [this, self_ptr](
                            handler_t&& handler,
                            boost::beast::error_code error_code, 
                            std::size_t bytes_transferred) mutable
                        {
                            if (error_code)
                            {
                                return handler(error_code, std::vector<std::filesystem::path>{});
                            }

                            // Consume read bytes as it is just the boundary
                            _buffer->consume(bytes_transferred);

                            // Set the timeout
                            boost::beast::get_lowest_layer(_stream).expires_after(_settings.operations_timeout);

                            // Read the first file header obtaining bytes until the empty string 
                            // that represents the delimiter between file header and data itself
                            boost::asio::async_read_until(_stream, *_buffer, "\r\n\r\n", 
                                boost::beast::bind_front_handler(
                                    [this, self_ptr](
                                        handler_t&& handler,
                                        boost::beast::error_code error_code, 
                                        std::size_t bytes_transferred) mutable
                                    {
                                        async_process_file_header(
                                            std::forward<handler_t>(handler), 
                                            std::move(self_ptr), 
                                            error_code, 
                                            bytes_transferred);
                                    },
                                    std::forward<handler_t>(handler)));
                        },
                        std::forward<handler_t>(handler)));
            }

            template<
                boost::asio::completion_token_for<void(
                    boost::beast::error_code, 
                    std::vector<std::filesystem::path>&&)> handler_t, 
                typename session_t>
            void async_process_file_header(
                handler_t&& handler, 
                std::shared_ptr<session_t>&& self_ptr,
                boost::beast::error_code error_code, 
                std::size_t bytes_transferred)
            {
                if (error_code)
                {
                    // Reset the timeout
                    boost::beast::get_lowest_layer(_stream).expires_never();
                    
                    return handler(error_code, std::move(_output_file_paths));
                }

                // Construct the string representation of obtained file header
                std::string_view file_header_data{
                    _buffer_storage.data(),
                    bytes_transferred};

                // Position of the filename field in the file header
                size_t file_name_position = file_header_data.find("filename=\"");

                // filename field is absent
                if (file_name_position == std::string::npos)
                {
                    // Reset the timeout
                    boost::beast::get_lowest_layer(_stream).expires_never();
                    
                    return handler(boost::asio::error::not_found, std::move(_output_file_paths));
                }

                // Remove the data before the actual file name
                file_header_data.remove_prefix(file_name_position + 10);

                // Look for the end of the file name
                // Find from the end because actual file name can contain double quotes
                file_name_position = file_header_data.rfind('"');

                // No closing double quote in filename
                if (file_name_position == std::string::npos)
                {
                    // Reset the timeout
                    boost::beast::get_lowest_layer(_stream).expires_never();
                    
                    return handler(boost::asio::error::not_found, std::move(_output_file_paths));
                }

                // Get the actual file name
                file_header_data.remove_suffix(file_header_data.size() - file_name_position);

                if (_settings.on_read_file_header_handler)
                {
                    _file_path = _settings.on_read_file_header_handler(file_header_data);

                    if (_file_path.empty())
                    {
                        if (!generate_file_path(file_header_data))
                        {
                            return handler(boost::asio::error::bad_descriptor, std::move(_output_file_paths));
                        }
                    }
                }
                else
                {
                    if (!generate_file_path(file_header_data))
                    {
                        return handler(boost::asio::error::bad_descriptor, std::move(_output_file_paths));
                    }
                }

                // Open the file to write the obtaining data
                _file.open(_file_path, std::ios::binary);

                // Invalid file path was provided
                if (!_file.is_open())
                {
                    // Reset the timeout
                    boost::beast::get_lowest_layer(_stream).expires_never();
                     
                    return handler(boost::asio::error::invalid_argument, std::move(_output_file_paths));
                }
                
                // Store provided file path
                _output_file_paths.emplace_back(_file_path);

                // Consume the file header bytes 
                _buffer->consume(bytes_transferred);

                // Set the timeout
                boost::beast::get_lowest_layer(_stream).expires_after(_settings.operations_timeout);

                // Read the file body obtaining bytes until the boundary that represents the end of file
                boost::asio::async_read_until(_stream, *_buffer, _boundary,
                    boost::beast::bind_front_handler(
                        [this, self_ptr](
                            handler_t&& handler,
                            boost::beast::error_code error_code, 
                            std::size_t bytes_transferred) mutable
                        {
                            async_process_file_body(
                                std::forward<handler_t>(handler), 
                                std::move(self_ptr), 
                                error_code, 
                                bytes_transferred);
                        },
                        std::forward<handler_t>(handler)));
            }

            template<
                boost::asio::completion_token_for<void(
                    boost::beast::error_code, 
                    std::vector<std::filesystem::path>&&)> handler_t, 
                typename session_t>
            void async_process_file_body(
                handler_t&& handler, 
                std::shared_ptr<session_t>&& self_ptr,
                boost::beast::error_code error_code, 
                std::size_t bytes_transferred)
            {
                // File can't be read at once as it is too big(more than _settings.packets_size bytes)
                // Process obtained packet and go on reading
                if (error_code == boost::asio::error::not_found)
                {
                    // Write obtained packet to the file
                    // Don't touch last symbols with boundary length as we could stop in the middle of boundary
                    // so we would write the part of boundary to the file
                    _file.write(
                        _buffer_storage.data(),
                        _buffer_storage.size() - _boundary.size());

                    // Consume written bytes
                    _buffer->consume(_buffer_storage.size() - _boundary.size());

                    // Set the timeout
                    boost::beast::get_lowest_layer(_stream).expires_after(_settings.operations_timeout);

                    // Read the next data until either we find a boundary or read the packet of maximum size again 
                    return boost::asio::async_read_until(_stream, *_buffer, _boundary, 
                        boost::beast::bind_front_handler(
                            [this, self_ptr](
                                handler_t&& handler,
                                boost::beast::error_code error_code, 
                                std::size_t bytes_transferred) mutable
                            {
                                async_process_file_body(
                                    std::forward<handler_t>(handler), 
                                    std::move(self_ptr), 
                                    error_code, 
                                    bytes_transferred);
                            },
                            std::forward<handler_t>(handler)));
                }

                // Unexpected error occured so clean up everything about not uploaded file
                if (error_code)
                {
                    // Reset the timeout
                    boost::beast::get_lowest_layer(_stream).expires_never();
                    
                    _file.close();

                    // Remove the file from the file system
                    try
                    {
                        std::filesystem::remove(_output_file_paths.back());
                    }
                    catch (const std::exception& ex)
                    {}

                    // Remove the file from the list of uploaded files 
                    _output_file_paths.pop_back();

                    return handler(error_code, std::move(_output_file_paths));
                }

                // Write obtained bytes to the file excluding CRLF after the file data and -- followed by boundary
                // -- is the part of the boundary, used only in body, so we have to consider this -- length because
                // _boudary variable doesn't contain it
                _file.write(_buffer_storage.data(), bytes_transferred - _boundary.size() - 4);

                // Close the file as its uploading is over
                _file.close();

                // Invoke handler after reading the whole file body if it is defined
                if (_settings.on_read_file_body_handler)
                {
                    _settings.on_read_file_body_handler(_output_file_paths.back());
                }

                // Consume obtained bytes
                _buffer->consume(bytes_transferred); 

                // If there is "--" after the boundary then there are no more files and request body is over
                if (std::string_view{_buffer_storage.data(), _buffer_storage.size()} == "--\r\n")
                {
                    // Reset the timeout
                    boost::beast::get_lowest_layer(_stream).expires_never();
                   
                    return handler(error_code, std::move(_output_file_paths));
                }
                
                // Set the timeout
                boost::beast::get_lowest_layer(_stream).expires_after(_settings.operations_timeout);
                
                // Read the next file header
                boost::asio::async_read_until(_stream, *_buffer, "\r\n\r\n", 
                    boost::beast::bind_front_handler(
                        [this, self_ptr](
                            handler_t&& handler,
                            boost::beast::error_code error_code, 
                            std::size_t bytes_transferred) mutable
                        {
                            async_process_file_header(
                                std::forward<handler_t>(handler), 
                                std::move(self_ptr), 
                                error_code, 
                                bytes_transferred);
                        },
                        std::forward<handler_t>(handler)));
            }

            void sync_prepare_files_processing(
                std::string_view content_type, 
                boost::beast::error_code& error_code)
            {
                // Clear the previous output file paths
                _output_file_paths.clear();

                // Assign buffer storage with input buffer data because it can store some part of the request body
                _buffer_storage.assign(
                    boost::asio::buffers_begin(_input_buffer.data()),
                    boost::asio::buffers_end(_input_buffer.data()));

                // Reinitialize buffer with specified packets size limit
                _buffer.emplace(_buffer_storage, _settings.packets_size);

                size_t boundary_position = content_type.find("boundary=");

                // Boundary was not found in the content type
                if (boundary_position == std::string::npos)
                {
                    error_code = boost::asio::error::not_found;
                    _output_file_paths = {};

                    return;
                }

                // Determine the boundary for multipart/form-data content type
                _boundary = content_type.substr(boundary_position + 9);

                // Read the boundary before the header of the first file 
                std::size_t bytes_transferred =  boost::asio::read_until(_stream, *_buffer, _boundary, error_code);

                if (error_code)
                {
                    _output_file_paths = std::vector<std::filesystem::path>{};

                    return;
                }

                // Consume read bytes as it is just the boundary
                _buffer->consume(bytes_transferred);

                // Read the first file header obtaining bytes until the empty string 
                // that represents the delimiter between file header and data itself
                bytes_transferred = boost::asio::read_until(_stream, *_buffer, "\r\n\r\n", error_code);

                sync_process_file_header(error_code, bytes_transferred);
            }

            void sync_process_file_header(
                boost::beast::error_code& error_code, 
                std::size_t bytes_transferred)
            {
                if (error_code)
                {
                    return;
                }

                // Construct the string representation of obtained file header
                std::string_view file_header_data{
                    _buffer_storage.data(),
                    bytes_transferred};

                // Position of the filename field in the file header
                size_t file_name_position = file_header_data.find("filename=\"");

                // filename field is absent
                if (file_name_position == std::string::npos)
                {
                    error_code = boost::asio::error::not_found;

                    return;
                }

                // Remove the data before the actual file name
                file_header_data.remove_prefix(file_name_position + 10);

                // Look for the end of the file name
                // Find from the end because actual file name can contain double quotes
                file_name_position = file_header_data.rfind('"');

                // No closing double quote in filename
                if (file_name_position == std::string::npos)
                {
                    error_code = boost::asio::error::not_found;

                    return;
                }

                // Get the actual file name
                file_header_data.remove_suffix(file_header_data.size() - file_name_position);

                if (_settings.on_read_file_header_handler)
                {
                    _file_path = _settings.on_read_file_header_handler(file_header_data);

                    if (_file_path.empty())
                    {
                        if (!generate_file_path(file_header_data))
                        {
                            error_code = boost::asio::error::bad_descriptor;

                            return;
                        }
                    }
                }
                else
                {
                    if (!generate_file_path(file_header_data))
                    {
                        error_code = boost::asio::error::bad_descriptor;

                        return;
                    }
                }

                // Open the file to write the obtaining data
                _file.open(_file_path, std::ios::binary);

                // Invalid file path was provided
                if (!_file.is_open())
                {
                    error_code = boost::asio::error::invalid_argument;

                    return;
                }
                
                // Store provided file path
                _output_file_paths.emplace_back(_file_path);

                // Consume the file header bytes 
                _buffer->consume(bytes_transferred);

                // Read the file body obtaining bytes until the boundary that represents the end of file
                bytes_transferred = boost::asio::read_until(_stream, *_buffer, _boundary, error_code);

                sync_process_file_body(error_code, bytes_transferred);
            }

            void sync_process_file_body(
                boost::beast::error_code& error_code, 
                std::size_t bytes_transferred)
            {
                // File can't be read at once as it is too big(more than _settings.packets_size bytes)
                // Process obtained packet and go on reading
                if (error_code == boost::asio::error::not_found)
                {
                    // Write obtained packet to the file
                    // Don't touch last symbols with boundary length as we could stop in the middle of boundary
                    // so we would write the part of boundary to the file
                    _file.write(
                        _buffer_storage.data(),
                        _buffer_storage.size() - _boundary.size());

                    // Consume written bytes
                    _buffer->consume(_buffer_storage.size() - _boundary.size());

                    // Read the next data until either we find a boundary or read the packet of maximum size again 
                    bytes_transferred = boost::asio::read_until(_stream, *_buffer, _boundary, error_code);

                    return sync_process_file_body(error_code, bytes_transferred);
                }

                // Unexpected error occured so clean up everything about not uploaded file
                if (error_code)
                {
                    _file.close();

                    // Remove the file from the file system
                    try
                    {
                        std::filesystem::remove(_output_file_paths.back());
                    }
                    catch (const std::exception& ex)
                    {}

                    // Remove the file from the list of uploaded files 
                    _output_file_paths.pop_back();

                    return;
                }

                // Write obtained bytes to the file excluding CRLF after the file data and -- followed by boundary
                // -- is the part of the boundary, used only in body, so we have to consider this -- length because
                // _boudary variable doesn't contain it
                _file.write(_buffer_storage.data(), bytes_transferred - _boundary.size() - 4);

                // Close the file as its uploading is over
                _file.close();

                // Invoke handler after reading the whole file body if it is defined
                if (_settings.on_read_file_body_handler)
                {
                    _settings.on_read_file_body_handler(_output_file_paths.back());
                }

                // Consume obtained bytes
                _buffer->consume(bytes_transferred); 

                // If there is "--" after the boundary then there are no more files and request body is over
                if (std::string_view{_buffer_storage.data(), _buffer_storage.size()} == "--\r\n")
                {
                    return;
                }
                
                // Read the next file header
                bytes_transferred = boost::asio::read_until(_stream, *_buffer, "\r\n\r\n", error_code);

                sync_process_file_header(error_code, bytes_transferred);
            }

            inline bool generate_file_path(std::string_view file_name)
            {
                _file_path = _settings.output_directory / file_name;

                try
                {
                    if (std::filesystem::exists(_file_path))
                    {
                        std::string new_file_name = _file_path.stem().string() + "(1)";

                        size_t copy_number = 1, copy_number_start_position = new_file_name.size() - 2;

                        _file_path.replace_filename(new_file_name + _file_path.extension().c_str());
                        
                        while (std::filesystem::exists(_file_path))
                        {
                            new_file_name.replace(
                                copy_number_start_position, 
                                new_file_name.size() - copy_number_start_position - 1,
                                std::to_string(++copy_number));

                            _file_path.replace_filename(new_file_name + _file_path.extension().c_str());
                        }
                    }
                }
                catch (const std::exception&)
                {
                    return false;
                }

                return true;
            }

            read_stream& _stream;
            // Buffer that is used to read requests outside this class
            // It is necessary because it can already store some part of the request body
            const dynamic_buffer& _input_buffer;
            // String buffer storage that actually contains read data
            std::string _buffer_storage{};
            // Main buffer that is wrapper around the string to use it in asio operations
            std::optional<boost::asio::dynamic_string_buffer<char, std::char_traits<char>, std::allocator<char>>> _buffer{};
            settings _settings{};
            std::string_view _boundary{};
            std::filesystem::path _file_path{};
            std::ofstream _file{};
            std::vector<std::filesystem::path> _output_file_paths{};
    };
};

#endif