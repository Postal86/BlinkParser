﻿#include "blink.h"
#include "coff_reader.h"
#include <algorithm>





static void add_unique_path(std::vector<std::filesystem::path> &paths, const std::filesystem::path &path)
{
	if (path.empty())
		return;
	if (std::find(paths.begin(), paths.end(), path) == paths.end())
		paths.push_back(path);
}


static void find_common_paths(const std::vector<std::filesystem::path> &paths, std::vector<std::filesystem::path> &source_dirs)
{
	if (paths.empty())
		return;

	add_unique_path(source_dirs, paths[0].parent_path());

	for (auto path_it = paths.begin() + 1; path_it != paths.end(); ++path_it)
	{
		// Only  consider  files  that  do actually exist
		if (!std::filesystem::exists(*path_it))
			continue;

		std::filesystem::path common_path;
		std::filesystem::path file_directory = path_it->parent_path();
		for  (auto dir_it = source_dirs.begin();  dir_it != source_dirs.end(); ++dir_it)
		{
			common_path.clear();
			for (auto it2 = file_directory.begin(), it3 = dir_it->begin(); it2 != file_directory.end() && it3 != dir_it->end() &&
				*it2 == *it3; ++it2, ++it3)
				common_path /= *it2;
			if (!common_path.empty())
				*dir_it = common_path;
		}

		if (!common_path.empty() && !file_directory.empty())
			add_unique_path(source_dirs, file_directory);
	}
}

blink_parser::Application::Application()
{
	_image_base = reinterpret_cast<BYTE*>(GetModuleHandle(nullptr));

	_symbols.insert({ "__ImageBase", _image_base });
}


void blink_parser::Application::Run(void* const blink_handle, const wchar_t* blink_environment, const wchar_t* blink_working_directory)
{
	std::vector<const BYTE*> dlls;

	{
		print("Reading  PE  import  directory ...");

		read_import_address_table(_image_base);
	}

	{
		print("Reading  PE debug info directory ...");

		if (!read_debug_info(_image_base))
		{
			print(" Error: Could not  find  path to matching  program  debug database  in executable image.");
			return;
		}

		std::vector<std::filesystem::path> cpp_files;

		for (size_t i = 0; i < _object_files.size(); ++i)
		{
			if (std::error_code ec; _object_files[i].extension() != ".obj" ||  !std::filesystem::exists(_object_files[i], ec))
				continue;

			const auto it = std::find_if(_source_files[i].begin(), _source_files[i].end(),
				[](const auto& path) {const auto ext = path.extension(); return ext == ".c" || ext == ".cpp" || ext == ".cxx"; });

			if (it != _source_files[i].end())
			{
				print(" Found  source file: " + it->string());

				cpp_files.push_back(*it);
			}

		}

		//  The linker  is invoked  in solution  directory,  which  may  be out of  source  directory. Use  source common paths  instead.
		find_common_paths(cpp_files, _source_dirs);
	}

	if (_source_dirs.empty())
	{
		print( " Error: Could not  determine  source  directories.  Check  your program ");
	}

	Scoped_Handle compiler_stdin, compiler_stdout;

	{
		print("Starting  compiler  process ...");

		// Launch compiler  process
		STARTUPINFO si = { sizeof(si) };
		si.dwFlags = STARTF_USESTDHANDLES;
		SECURITY_ATTRIBUTES sa = { sizeof(sa) };
		sa.bInheritHandle = TRUE;


		if (!CreatePipe(&si.hStdInput, &compiler_stdin, &sa, 0))
		{
			print(" Error: Could not  create input communication pipe.");
			return; 
		}

		SetHandleInformation(compiler_stdin, HANDLE_FLAG_INHERIT, FALSE);

		if (!CreatePipe(&compiler_stdout, &si.hStdOutput, &sa, 0))
		{
			print(" Error: Could not create  output  communication pipe.");

			CloseHandle(si.hStdInput);
			return;
		}

		SetHandleInformation(compiler_stdout, HANDLE_FLAG_INHERIT, FALSE);

		si.hStdError = si.hStdOutput;

		wchar_t cmdLine[] = L"cmd.exe /q /d /k @echo off";
		PROCESS_INFORMATION pi;

		// Use environment and working directory of the blink application for the compiler process
		// This way, the user can run blink from their build prompt and the compiler process will compile similar to how it would when run from the user prompt directly
		if (!CreateProcessW(nullptr, cmdLine, nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, reinterpret_cast<LPVOID>(const_cast<wchar_t *>(blink_environment)),  
			blink_working_directory, &si, &pi))
		{
			print(" Error: Could not  create process.");

			CloseHandle(si.hStdInput);
			CloseHandle(si.hStdOutput);
			return;
		}

		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		CloseHandle(si.hStdInput);
		CloseHandle(si.hStdOutput);

		print(" Started  process with PID " +  std::to_string(pi.dwProcessId));
	}

	std::vector<Scoped_Handle> dir_handles;
	std::vector<Scoped_Handle> event_handles;
	std::vector<Notification_Info>  notification_infos;
	for (auto it  = _source_dirs.begin(); it != _source_dirs.end(); ++it)
	{
		print( "Starting  file system  watcher  for '" + it->string() + "' ...");

		Scoped_Handle& dir_handle = dir_handles.emplace_back();
		event_handles.emplace_back();
		notification_infos.emplace_back();

		dir_handle = CreateFileW(it->c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);

		if (dir_handle == INVALID_HANDLE_VALUE)
		{
			print("Error: Could not  open  directory handle.");
			return; 
		}

		if (!set_watch(dir_handle, event_handles.back(), notification_infos.back()))
			return;
	}

	DWORD  size = 0;
	DWORD  bytes_written = 0;
	DWORD  bytes_transferred = 0;
	//  Check  that both  the compiler  and  blink  application  are still    running
	while (PeekNamedPipe(compiler_stdout, nullptr, 0, nullptr, &size, nullptr) &&  PeekNamedPipe(blink_handle, nullptr, 0, nullptr, &size, nullptr))
	{
		const DWORD  wait_result = WaitForMultipleObjects(static_cast<DWORD>(event_handles.size()), reinterpret_cast<const HANDLE*>(event_handles.data()), FALSE, 1000);

		if (wait_result == WAIT_FAILED)
			break;
		if (wait_result == WAIT_TIMEOUT)
			continue;

		const size_t  dir_index = wait_result;
		if (!GetOverlappedResult(dir_handles[dir_index], &notification_infos[dir_index].overlapped, &bytes_transferred, TRUE))
			break;

		bool first_notification = true;
		// Iterate  over all  notification  items
		for (auto info = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(notification_infos[dir_index].p_info.data()); first_notification ||  info->NextEntryOffset != 0;
			first_notification = false,  info = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(reinterpret_cast<BYTE *>(info) +  info->NextEntryOffset))
		{
			std::filesystem::path object_file, source_file =
				_source_dirs[dir_index] / std::wstring(info->FileName, info->FileNameLength / sizeof(WCHAR));

			// Ignore  changes  to files  that  are not  C++  source files
			if (const auto ext = source_file.extension(); ext != ".c" && ext != ".cxx")
				continue;

			// Ignore duplicated  notifications  by  comparing  times  and skipping  any changes  that  are not  older  than 3 seconds
			if (const auto current_time = GetTickCount(); _last_modifications[source_file.string()] + 3000 > current_time)
				continue;
			else
				_last_modifications[source_file.string()] = current_time;

			print("Detected  modification to: " + source_file.string());

			// Build  compiler  command line
			std::string  cmdline = build_compile_command_line(source_file, object_file);

			// Append special  completion  message
			cmdline += "\necho  Finished  compiling \"" + object_file.string() + "\" with code %errorlevel%.\n"; // Message  used to confirm  that compile finished  in message  loop  above 

			//  Execute  compiler  command  line
			WriteFile(compiler_stdin, cmdline.c_str(), static_cast<DWORD>(cmdline.size()), &size, nullptr );


			// Read  and react  to  compiler  output  messages
			while (WaitForSingleObject(compiler_stdout, INFINITE) == WAIT_OBJECT_0 && PeekNamedPipe(compiler_stdout,  nullptr, 0, nullptr, &size,  nullptr))
			{
				if (size == 0)
					continue;

				std::string message(size, '\0');
				ReadFile(compiler_stdout, message.data(), size, &size, nullptr);

				size_t offset = 0, next;

				do
				{
					next = message.find('\n', offset);
					print(message.data() + offset, next != std::string::npos ? next - offset + 1 : message.size() - offset);
					offset = next + 1;

				} while (next != std::string::npos);


				//  Listen  for special completion message
				if (offset = message.find( " with code "); offset != std::string::npos)
				{
					//  Only  load  the complicated  module  if compilation was successful
					if (const long  exit_code =  strtol(message.data() + 11, nullptr, 10); exit_code == 0)
					{
						call_symbol("__blink_sync", source_file.string().c_str()); // Notify  application that we want  to link an object file.
						const bool link_success = link(object_file);
						call_symbol("__blink_release",  source_file.string().c_str(), link_success);
                    }
					break;
				}

			}


			// The  OBJ  file  does not  need   anymore
			DeleteFileW(object_file.c_str());
		}
		
		if (!set_watch(dir_handles[dir_index], event_handles[dir_index], notification_infos[dir_index]))
			break;
	}
}

bool blink_parser::Application::read_debug_info(const BYTE* image_base)
{
	struct RSDS_DEBUG_FORMAT
	{

		uint32_t signature; 
		guid guid;
		uint32_t age; 
		char path[1];

	} const *debug_data = nullptr;

	const auto headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(
		image_base + reinterpret_cast<const IMAGE_DOS_HEADER*>(image_base)->e_lfanew);

	// Search debug directory for program  debug  database  file  name
	const IMAGE_DATA_DIRECTORY& debug_directory = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
	const auto debug_directory_entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(
		image_base + debug_directory.VirtualAddress);

	for (unsigned int i = 0; i < debug_directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++i)
	{
		if (debug_directory_entries[i].Type == IMAGE_DEBUG_TYPE_CODEVIEW)
		{
			debug_data = reinterpret_cast<const RSDS_DEBUG_FORMAT*>(
				image_base + debug_directory_entries[i].AddressOfRawData);
			if (debug_data->signature == 0x53445352) // RSDS
				break;
		}
	}

	if (debug_data == nullptr)
		return false;

	pdb_reader pdb(debug_data->path);

	print(" Found program debug database: " + std::string(debug_data->path));

	// The linker working directory should equal the project root directory
	std::string linker_cmd;
	std::filesystem::path cwd;
	pdb.read_link_info(cwd, linker_cmd);
	if (!cwd.empty())
		add_unique_path(_source_dirs, cwd);

	pdb.read_symbol_table(_image_base, _symbols);
	pdb.read_object_files(_object_files);
	pdb.read_source_files(_source_files, _source_file_map);

   return true;
}

void blink_parser::Application::read_import_address_table(const BYTE* image_base) {

	const auto headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(
		_image_base + reinterpret_cast<const IMAGE_DOS_HEADER*> (_image_base)->e_lfanew);

	// Search import directory for additional symbols 
	const IMAGE_DATA_DIRECTORY& import_directory = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	const auto  import_directory_entries = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(_image_base + import_directory.VirtualAddress);

	for (unsigned int i = 0; import_directory_entries[i].FirstThunk != 0; i++)
	{
		const auto  name = reinterpret_cast<const char*>(_image_base + import_directory_entries[i].Name);
		const auto  import_name_table = reinterpret_cast<const IMAGE_THUNK_DATA*>(_image_base + import_directory_entries[i].Characteristics);
		const auto  import_address_table = reinterpret_cast<const IMAGE_THUNK_DATA*>(_image_base + import_directory_entries[i].FirstThunk);

		// The  module  should have already  been  loaded  by Windows  when the application was launched, so  just  get its  handle  here
		const auto  target_base = reinterpret_cast<const  BYTE*>(GetModuleHandleA(name));
		if (target_base == nullptr)
			continue; // Bail out if that  is  not the case  to be safe

		for (unsigned int k = 0; import_name_table[k].u1.AddressOfData != 0; k++)
		{
			const char* import_name = nullptr;

			// We need  to figure out the name of symbols imported  by ordinal  by going through the export  table of the  target module
			if (IMAGE_SNAP_BY_ORDINAL(import_name_table[k].u1.Ordinal))
			{
				const auto target_headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(target_base + reinterpret_cast<const IMAGE_DOS_HEADER*>(target_base)->e_lfanew);
				const auto export_directory = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(target_base + target_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
				const auto export_name_strings = reinterpret_cast<const DWORD *>(target_base + export_directory->AddressOfNames);
				const auto export_name_ordinals = reinterpret_cast<const DWORD*>(target_base + export_directory->AddressOfNameOrdinals);

				const auto ordinal = std::find(export_name_ordinals, export_name_ordinals + export_directory->NumberOfNames, IMAGE_ORDINAL(import_name_table[k].u1.Ordinal));
				if (ordinal != export_name_ordinals + export_directory->NumberOfNames)
					import_name = reinterpret_cast<const  char*>(target_base + export_name_strings[std::distance(export_name_ordinals, ordinal)]);
				else
					continue;

			}
			else
			{
				import_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(_image_base + import_name_table[k].u1.AddressOfData)->Name;
			}

			_symbols.insert({import_name, reinterpret_cast<void *>(import_address_table[k].u1.AddressOfData)});

		}

		read_debug_info(target_base);
	}
}

bool blink_parser::Application::set_watch(const HANDLE dir_handle, Scoped_Handle& event_handle, Notification_Info& target_info)
{
	event_handle = target_info.overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	if (event_handle == NULL)
	{
		print(" Error: CreateEvent failed.");
		return false;
	}

	DWORD  size = 0; 
	if (!ReadDirectoryChangesW(dir_handle, reinterpret_cast<FILE_NOTIFY_INFORMATION*>(target_info.p_info.data()), static_cast<DWORD>(target_info.p_info.size()), TRUE,
		FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME, &size, &target_info.overlapped, nullptr))
	{
		print(" Error: ReadDirectoryChangesW failed.");
		return false;
	}

	return true;
}

std::string blink_parser::Application::build_compile_command_line(const std::filesystem::path& source_file, std::filesystem::path& object_file) const
{
	std::string  cmdline; 

	Sleep(100); // Prevent  file  system error  in the  next few  code lines, TODO: figure out what causes this

	//  Check if this  source  file already  exists  in the  application in which  case we can  read some information from the  original object  file 
	if (const auto it = _source_file_map.find(source_file);
		it != _source_file_map.end())
	{
		object_file = _object_files[it->second.module];

		//  Read  original object file 
		COFF_HEADER  header; 
		const Scoped_Handle file = open_coff_file(object_file, header);
		if (file != INVALID_HANDLE_VALUE)
		{
			DWORD read = header.is_extended() ? header.bigobj.NumberOfSections : header.obj.NumberOfSections;
			std::vector<IMAGE_SECTION_HEADER> sections(read);
			ReadFile(file, sections.data(), read * sizeof(IMAGE_SECTION_HEADER), &read, nullptr);

			// Find  first  debug  symbol section  and read it 
			const auto  section = std::find_if(sections.begin(), sections.end(), [](const  auto& s) {
				return strcmp(reinterpret_cast<const  char(&)[]>(s.Name), ".debug&S") == 0; });

			if (section != sections.end())
			{
				std::vector<char>  debug_data(section->SizeOfRawData);
				SetFilePointer(file, section->PointerToRawData, nullptr, FILE_BEGIN);
				ReadFile(file, debug_data.data(), section->SizeOfRawData, &read, nullptr);

				//  Skip  header  in front of CodeView records (version, ...)
				Stream_Reader stream(std::move(debug_data)); 
				stream.skip(4); // Skip 32-bit  signature  (this  should be CV_SIGNATURE_C13, aka 4)

				while (stream.tell() < stream.size() && cmdline.empty())
				{
					// CV_DebugSSubsectionHeader_t
					const auto  subsection_type = stream.read<uint32_t>();
					const auto  subsection_length = stream.read<uint32_t>();
					if (subsection_type != 0xf1) // DEBUG_S_SYMBOLS
					{
						stream.skip(subsection_length);
						stream.align(4);
						continue;
					}

					parse_code_view_records(stream, subsection_length, [&](uint16_t tag) {
						if (tag != 0x113d) // S_ENVBLOCK
							return; // Skip  all  records  that  are  not  about  compiler environment 
						stream.skip(1);
						while (stream.tell() < stream.size() && *stream.data() != '\0')
						{
							const auto key = stream.read_string();
							const std::string value(stream.read_string());

							if (key == "cwd")
								cmdline += "cd /D \"" + value + "\"\n";
							else if (key == "cl") // Add  compiler  directories to path , so that 'mspdbcore.dll' is found
								cmdline += "set PATH=%PATH%;" + value + "\\..\\..\\x86;" + value + "\\..\\..\\x64\n\"" + value + "\" ";
							else  if (key == "cmd")
								cmdline += value;
						 }
						});

					stream.align(4); // Subsection  headers  are 4-byte  aligned
				}

			}
		}
	}

	//  Fall  back to default  command-line  if unable to extract it
	if (cmdline.empty())
	{
		cmdline = "cl.exe "
			"/nologo " // Suppress copyright message
			"/Z7 " // Enable COFF debug information
			"/MDd " // Link with 'MSVCRTD.lib'
			"/Od " // Disable optimizations
			"/EHsc " // Enable C++ exceptions
			"/std:c++latest " // C++ standard version
			"/Zc:wchar_t /Zc:forScope /Zc:inline "; // C++ language conformance


		cmdline = R"(cl.exe /c /ZI /JMC /nologo /W3 /WX- /diagnostics:column /sdl /Od /D _DEBUG /D _CONSOLE /D _CRT_OBSOLETE_NO_WARNINGS /D _UNICODE /D UNICODE /Gm- /EHsc /RTC1 /MDd /GS /fp:precise /Zc:wchar_t /Zc:forScope /Zc:inline /permissive- /Fo"x64\Debug\\" /Fd"x64\Debug\vc143.pdb" /external:W3 /Gd /TP /FC /errorReport:prompt )";
	}

	// Make sure to only compile and not link too
	//cmdline += " /c ";

	// Remove some arguments from the command-line since they are set to different values below
	const  auto remove_arg = [&cmdline](std::string arg) {
		for (unsigned int k = 0; k < 2; ++k)
			if (size_t offset = cmdline.find("-/"[k] + arg); offset != std::string::npos)
			{
				if (cmdline[offset + 1 + arg.size()] != '\"')
					cmdline.erase(offset, cmdline.find(' ', offset) - offset);
				else
					cmdline.erase(offset, cmdline.find('\"', offset + 2 + arg.size()) + 2 - offset);
				break;
			}
		};

	remove_arg("Fo");
	remove_arg("Fd"); // The program debug database is currently in use by the running application, so cannot write to it
	remove_arg("ZI"); // Do not create a program debug database, since all required debug information can be stored in the object file instead
	remove_arg("Yu"); // Disable pre-compiled headers, since the data is not accessible here
	remove_arg("Yc");
	remove_arg("JMC");


	// Always write to a separate object file since the original one may be in user by a debugger
	object_file = source_file; object_file.replace_extension("temp.obj");

	// Append input source file to command-line
	cmdline += '\"' + source_file.string() + '\"';

	// Append output object file to command-line
	cmdline += " /Fo\"" + object_file.string() + '\"';

	return cmdline;

}



