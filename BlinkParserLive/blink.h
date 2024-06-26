#pragma once

#include "pdb_reader.h"
#include "scoped_handle.h"
#include <vector>
#include <string>
#include <filesystem>
#include <unordered_map>


void print(const char* message, size_t length);
inline void print(std::string message)
{
	message += '\n';
	print(message.data(), message.size());
}


namespace  blink_parser
{
	class Application
	{
	public:
		Application();

		void  Run(void* const blink_handle, const wchar_t* blink_environment = nullptr, const wchar_t* blink_working_directory = nullptr);
		bool  link(const std::filesystem::path &object_file);

		template<typename T>
		T Read_Symbol(const  std::string &name) const
		{
			if (const auto it = _symbols.find(name);  it != _symbols.end())
				return *reinterpret_cast<T*>(it->second);
			return T();
		}

		template <typename T = void, typename... Args>
		T call_symbol(const std::string &name, Args...  args) const
		{
			if (const auto it = _symbols.find(name); it != _symbols.end())
				return reinterpret_cast<T(*)(Args...)>(it->second)(std::forward<Args>(args)...);
			return T();
		}

	private:
		struct Notification_Info
		{
			static const size_t buffer_size = 4096;

			OVERLAPPED overlapped = {};
			std::vector<BYTE> p_info = std::vector<BYTE>(buffer_size);
		};


		template <typename SYMBOL_TYPE, typename  HEADER_TYPE>
		bool link(void* const object_file, const HEADER_TYPE& header);

		bool read_debug_info(const uint8_t *image_base);
		void read_import_address_table(const uint8_t *image_base);


		bool set_watch(void *const  dir_handle, Scoped_Handle &event_handle, Notification_Info &target_info);

		std::string build_compile_command_line(const std::filesystem::path &source_file, std::filesystem::path &object_file ) const;


		uint8_t* _image_base = nullptr;
		std::vector<std::filesystem::path> _source_dirs;
		std::vector<std::filesystem::path> _object_files;
		std::vector<std::vector<std::filesystem::path>> _source_files;
		source_file_map _source_file_map;
		std::unordered_map<std::string, void*> _symbols;
		std::unordered_map<std::string, uint32_t> _last_modifications;
	};







}


