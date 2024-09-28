#include "stdafx.h"
#include "windows.h"
#include "pe_header.h"
#include <tlhelp32.h>
#include <cstdio>
#include "pe_hash_database.h"
#include "dump_process.h"
#include "simple.h"
#include "work_queue.h"
#include <thread>
#include "close_watcher.h"


BOOL is_win64()
{
	#if defined(_WIN64)
		return TRUE;  // 64-bit programs run only on Win64
	#elif defined(_WIN32)
		// 32-bit programs run on both 32-bit and 64-bit Windows
		// so must sniff
		BOOL f64 = FALSE;
		return IsWow64Process(GetCurrentProcess(), &f64) && f64;
	#else
		return FALSE; // Win64 does not support Win16
	#endif
}

bool is_elevated(HANDLE h_Process)
{
	HANDLE h_Token;
	TOKEN_ELEVATION t_TokenElevation;
    TOKEN_ELEVATION_TYPE e_ElevationType;
	DWORD dw_TokenLength;
	
	if( OpenProcessToken(h_Process, TOKEN_READ | TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES , &h_Token) )
	{
		if(GetTokenInformation(h_Token,TokenElevation,&t_TokenElevation,sizeof(t_TokenElevation),&dw_TokenLength))
		{
			if(t_TokenElevation.TokenIsElevated != 0)
			{
				if(GetTokenInformation(h_Token,TokenElevationType,&e_ElevationType,sizeof(e_ElevationType),&dw_TokenLength))
				{
					if(e_ElevationType == TokenElevationTypeFull || e_ElevationType == TokenElevationTypeDefault)
					{
						return true;
					}
				}
			}
		}
	}

    return false;
}


bool get_privileges(HANDLE h_Process)
{
	HANDLE h_Token;
	DWORD dw_TokenLength;
	if( OpenProcessToken(h_Process, TOKEN_READ | TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES , &h_Token) )
	{
		// Read the old token privileges
		TOKEN_PRIVILEGES* privilages = new TOKEN_PRIVILEGES[100];
		if( GetTokenInformation(h_Token, TokenPrivileges, privilages,sizeof(TOKEN_PRIVILEGES)*100,&dw_TokenLength) )
		{
			// Enable all privileges
			for( int i = 0; i < privilages->PrivilegeCount; i++ )
			{
				privilages->Privileges[i].Attributes = SE_PRIVILEGE_ENABLED;
			}
			
			// Adjust the privilges
			if(AdjustTokenPrivileges( h_Token, false, privilages, sizeof(TOKEN_PRIVILEGES)*100, NULL, NULL  ))
			{
				delete[] privilages;
				return true;
			}
		}
		delete[] privilages;
	}
	return false;
}

bool ConsoleRequestingClose = false;
BOOL WINAPI ConsoleHandler(DWORD CEvent)
{
	char mesg[128];

	switch (CEvent)
	{
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
		// Cancel the event and set closing flag
		printf("Close request received.\r\n");
		ConsoleRequestingClose = true;
		break;
	case CTRL_CLOSE_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		// We need to cleanup before terminating since we can't cancel this event
		printf("Terminate request received.\r\n");
		ConsoleRequestingClose = true;
		Sleep(30000); // hackjob to make sure it cleans up
		break;
	}
	return TRUE;
}

void add_process_hashes( DWORD pid, pe_hash_database* db, PD_OPTIONS* options )
{
	// Build a list of the hashes from this process
	unordered_set<unsigned __int64> new_hashes;

	// Process this process
	dump_process* dumper = new dump_process( pid, db, options, true );
	dumper->get_all_hashes( &new_hashes );
	delete dumper;
	
	// Add all these hashes to the database
	db->add_hashes( new_hashes );
}

void add_process_hashes_worker(Queue<PROCESSENTRY32>* work_queue, pe_hash_database* db, PD_OPTIONS* options)
{
	// Gather hashes from the work queue process list
	while (!work_queue->empty())
	{
		// Process the hashes for this process
		PROCESSENTRY32 entry;
		if (work_queue->pop(entry))
		{
			add_process_hashes(entry.th32ProcessID, db, options);
		}
	}
}



void add_system_hashes( pe_hash_database* db, PD_OPTIONS* options )
{
	// Add clean hashes from all processes on the system right now

	// Build a list of the hashes from all processes
	options->ImportRec = false; // Force no import reconstruciton

	// Build the queue of work from the currently running processes
	Queue<PROCESSENTRY32> work_queue;
	int total_work_count = 0;

	PROCESSENTRY32 entry;
	entry.dwSize = sizeof(PROCESSENTRY32);
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);

	if( snapshot != INVALID_HANDLE_VALUE )
	{
		if (Process32First(snapshot, &entry) == TRUE)
		{
			while (Process32Next(snapshot, &entry) == TRUE)
			{
				printf("...adding process to work queue: pid 0x%x,%S\r\n", entry.th32ProcessID, entry.szExeFile);
				work_queue.push(entry);
				total_work_count++;
			}
		}
		CloseHandle(snapshot);
	}

	// Create threads processing the work queue
	thread** threads = new thread*[options->NumberOfThreads];
	for (int i = 0; i < options->NumberOfThreads; i++)
	{
		threads[i] = new thread(add_process_hashes_worker, &work_queue, db, options);
	}

	// Wait for queue depletion
	int count = 0;
	bool still_working = false;
	int running_count = 1;
	while (!work_queue.empty() || running_count > 0)
	{
		// Check threads state
		running_count = 0;
		for (int i = 0; i < options->NumberOfThreads; i++)
		{
			if( WaitForSingleObject(threads[i]->native_handle(), 1) == WAIT_TIMEOUT )
				running_count++;
		}

		if (count % 10 == 0)
		{
			// Print the status
			int waiting_count = work_queue.count();
			printf("Hash Queue -> Waiting: %i\tRunning: %i\tComplete: %i\r\n", waiting_count, running_count, total_work_count - (waiting_count + running_count));
		}

		// Wait
		Sleep(50);
		count++;
	}

	// Wait for thread completions
	for (int i = 0; i < options->NumberOfThreads; i++)
	{
		threads[i]->join(); // blocks until each thread is finished
	}

	// Cleanup
	printf("...cleaning up system memory hashes factory\r\n");
	for (int i = 0; i < options->NumberOfThreads; i++)
	{
		delete threads[i];
	}
	delete []threads;
}


void dump_process_worker(Queue<PROCESSENTRY32>* work_queue, pe_hash_database* db, PD_OPTIONS* options)
{
	// Gather hashes from the work queue process list
	unordered_set<unsigned __int64> new_hashes;
	while (!work_queue->empty())
	{
		// Process the hashes for this process
		PROCESSENTRY32 entry;
		if (work_queue->pop(entry))
		{
			// Process this process

			// Dump
			dump_process* dumper = new dump_process(entry.th32ProcessID, db, options, true);
			dumper->dump_all();

			// Exclude these hashes from the next dumps
			dumper->get_all_hashes(&new_hashes);
			db->add_hashes(new_hashes);
			new_hashes.clear();

			delete dumper;
		}
	}
}


void dump_system(pe_hash_database* db, PD_OPTIONS* options)
{
	// Dump modules from all running processes
	
	// Build the queue of work from the currently running processes
	Queue<PROCESSENTRY32> work_queue;
	unordered_set<DWORD> dumping_pids;

	PROCESSENTRY32 entry;
	entry.dwSize = sizeof(PROCESSENTRY32);
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
	int total_work_count = 0;

	if (snapshot != INVALID_HANDLE_VALUE)
	{
		if (Process32First(snapshot, &entry) == TRUE)
		{
			while (Process32Next(snapshot, &entry) == TRUE)
			{
				printf("...adding process to work queue: pid 0x%x,%S\r\n", entry.th32ProcessID, entry.szExeFile);
				work_queue.push(entry);
				dumping_pids.insert(entry.th32ProcessID);
				total_work_count++;
			}
		}
		CloseHandle(snapshot);
	}

	// Create threads processing the work queue
	thread** threads = new thread*[options->NumberOfThreads];
	for (int i = 0; i < options->NumberOfThreads; i++)
	{
		threads[i] = new thread(dump_process_worker, &work_queue, db, options);
	}

	// Wait for queue depletion
	int count = 0;
	bool still_working = false;
	int running_count = 1;
	bool added_new_processes = false;

	while (!work_queue.empty() || running_count > 0 || !added_new_processes)
	{
		// Check threads state
		running_count = 0;
		for (int i = 0; i < options->NumberOfThreads; i++)
		{
			if (WaitForSingleObject(threads[i]->native_handle(), 1) == WAIT_TIMEOUT)
				running_count++;
		}

		// Add any newly started processes at the very end
		if (!added_new_processes && work_queue.empty() && running_count )
		{
			printf("...adding new processes since we started this job\r\n");
			HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
			if (snapshot != INVALID_HANDLE_VALUE)
			{
				if (Process32First(snapshot, &entry) == TRUE)
				{
					while (Process32Next(snapshot, &entry) == TRUE)
					{
						if (dumping_pids.count(entry.th32ProcessID) == 0)
						{
							printf("...adding new process to work queue: pid 0x%x,%S\r\n", entry.th32ProcessID, entry.szExeFile);
							work_queue.push(entry);
							dumping_pids.insert(entry.th32ProcessID);
							total_work_count++;

							// Add a new worker thread if needed
							if (running_count < options->NumberOfThreads)
							{
								// Add a new worker thread
								for (int i = 0; i < options->NumberOfThreads; i++)
								{
									if (WaitForSingleObject(threads[i]->native_handle(), 1) != WAIT_TIMEOUT)
									{
										threads[i]->join(); // blocks until each thread is finished
										delete threads[i];
										threads[i] = new thread(dump_process_worker, &work_queue, db, options);
										break;
									}
								}
							}
						}
					}
				}
				CloseHandle(snapshot);
			}
			added_new_processes = true;
		}

		if (count % 10 == 0)
		{
			// Print the status
			int waiting_count = work_queue.count();
			printf("Dump Queue -> Waiting: %i\tRunning: %i\tComplete: %i\r\n", waiting_count, running_count, total_work_count - (waiting_count + running_count));
		}

		// Wait
		Sleep(50);
		count++;
	}

	// Wait for thread completions
	for (int i = 0; i < options->NumberOfThreads; i++)
	{
		threads[i]->join(); // blocks until each thread is finished
	}

	// Cleanup
	printf("...cleaning up system dump factory\r\n");
	for (int i = 0; i < options->NumberOfThreads; i++)
	{
		delete threads[i];
	}
	delete []threads;
}






bool global_flag_verbose = false;

int _tmain(int argc, _TCHAR* argv[])
{

	get_privileges( GetCurrentProcess() );

	// Process the flags	
	WCHAR* filter = NULL;
	char* processNameFilter = NULL;
	char* clean_database;
	string path = ExePath();
	clean_database = new char[ path.length() + strlen("clean.hashes") + 2 ];
	sprintf( clean_database, "%s\\%s", path.c_str() , "clean.hashes" );

	bool flagHelp = false;
	bool flagHeader = true;
	bool flagPidDump = false;
	bool flagProcessNameDump = false;
	bool flagSystemDump = false;
	bool flagAddressDump = false;
	bool flagDumpCloses = false;

	char* add_directory = NULL;
	bool flagDB_gen = false;
	bool flagDB_genQuick = false;
	bool flagDB_add = false;
	bool flagDB_clean = false;
	bool flagDB_ignore = false;
	bool flagDB_remove = false;
	bool flagRecursion = true;
	
	
	PD_OPTIONS options;
	options.ImportRec = true;
	options.ForceGenHeader = false;
	options.Verbose = false;
	options.EntryPointOverride = -1;
	options.ReconstructHeaderAsDll = false;
	options.DumpChunks = true;
	options.NumberOfThreads = 16; // Default 16 threads
	
	DWORD pid = -1;
	__int64 address = 0;

	if( argc <= 1 )
		flagHelp = true;

	for( int i = 1; i < argc; i++ )
	{
		if( lstrcmp(argv[i],L"--help") == 0 || lstrcmp(argv[i],L"-help") == 0 || lstrcmp(argv[i],L"-h") == 0 || lstrcmp(argv[i],L"--h") == 0)
			flagHelp = true;
		else if( lstrcmp(argv[i],L"-nh") == 0 )
			flagHeader = false;
		else if( lstrcmp(argv[i],L"-nr") == 0 )
			flagRecursion = false;
		else if( lstrcmp(argv[i],L"-ni") == 0 )
			options.ImportRec = false;
		else if( lstrcmp(argv[i],L"-nc") == 0 )
			options.DumpChunks = false;
		else if (lstrcmp(argv[i], L"-nt") == 0)
			options.NumberOfThreads = 1;
		else if (lstrcmp(argv[i], L"-closemon") == 0)
			flagDumpCloses = true;
		else if( lstrcmp(argv[i],L"-v") == 0 )
		{
			options.Verbose = true;
			global_flag_verbose = true;
		}
		else if( lstrcmp(argv[i],L"-g") == 0 )
			options.ForceGenHeader = true;
		else if( lstrcmp(argv[i],L"-pid") == 0 )
		{
			if( i + 1 < argc )
			{
				// Attempt to parse this second part
				filter = argv[i+1];

				// Check the prefix
				bool isHex = false;
				wchar_t* prefix = new wchar_t[3];
				memcpy(prefix, filter, 4);
				prefix[2] = 0;

				if( wcscmp(prefix, L"0x") == 0 )
				{
					filter = &filter[2];
					isHex = true;
				}
				delete[] prefix;
				
				// Extract the pid from the string
				if( (isHex && swscanf(filter, L"%x", &pid) > 0) ||
					(!isHex && swscanf(filter, L"%i", &pid) > 0))
				{
					// Successfully parsed the PID
					flagPidDump = true;
				}
				else
				{
					fprintf(stderr,"Failed to parse -pid argument. It must be followed by a number:\r\n\teg. 'pd -pid 0x10A'\r\n");
					exit(0);
				}

				// Skip next argument
				i++;
			}
			else
			{
				fprintf(stderr,"Failed to parse -pid argument. It must be followed by a number:\r\n\teg. 'pd -pid 0x10A'\r\n");
				exit(0);
			}
		}
		else if( lstrcmp(argv[i],L"-a") == 0 )
		{
			if( i + 1 < argc )
			{
				// Attempt to parse this second part
				filter = argv[i+1];

				// Check the prefix
				bool isHex = false;
				wchar_t* prefix = new wchar_t[3];
				memcpy(prefix, filter, 4);
				prefix[2] = 0;

				if( wcscmp(prefix, L"0x") == 0 )
				{
					filter = &filter[2];
					isHex = true;
				}
				delete[] prefix;
				
				// Extract the pid from the string
				if( (isHex && swscanf(filter, L"%llx", &address) > 0) ||
					(!isHex && swscanf(filter, L"%llu", &address) > 0))
				{
					// Successfully parsed the PID
					flagAddressDump = true;
				}
				else
				{
					fprintf(stderr,"Failed to parse -a address argument. It must be followed by a number:\r\n\teg. 'pd -a 0x401000 -pid 0x10A'\r\n");
					exit(0);
				}

				// Skip next argument
				i++;
			}
			else
			{
				fprintf(stderr,"Failed to parse -pid argument. It must be followed by a number:\r\n\teg. 'pd -pid 0x10A'\r\n");
				exit(0);
			}
		}
		else if( lstrcmp(argv[i],L"-p") == 0 )
		{
			if( i + 1 < argc )
			{
				// Extract the process name filter regex
				processNameFilter = new char[wcslen(argv[i+1]) + 1];
				sprintf( processNameFilter, "%S", argv[i+1] );
				
				flagProcessNameDump = true;
				
				// Skip next argument
				i++;
			}
			else
			{
				fprintf(stderr,"Failed to parse -p argument. It must be followed by a regex match statement:\r\n\teg. 'pd -p chrome.exe'\r\n");
				exit(0);
			}
		}
		else if( lstrcmp(argv[i],L"-t") == 0 )
		{
			if( i + 1 < argc )
			{
				// Extract the number of threads to use
				filter = argv[i+1];

				// Check the prefix
				bool isHex = false;
				wchar_t* prefix = new wchar_t[3];
				memcpy(prefix, filter, 4);
				prefix[2] = 0;

				if( wcscmp(prefix, L"0x") == 0 )
				{
					filter = &filter[2];
					isHex = true;
				}
				delete[] prefix;
				
				// Extract the number from the string
				if( (isHex && swscanf(filter, L"%x", &options.NumberOfThreads) > 0) ||
					(!isHex && swscanf(filter, L"%i", &options.NumberOfThreads) > 0))
				{
					// Successfully parsed the value
					if( options.NumberOfThreads < 1 )
					{
						fprintf(stderr,"Failed to parse -t argument. It must be followed by a number 1 or larger:\r\n\teg. 'pd -system -t 10'\r\n");
						exit(0);
					}
					printf("Set number of threads to %i.\r\n", options.NumberOfThreads);
				}
				else
				{
					fprintf(stderr,"Failed to parse -t argument. It must be followed by a number:\r\n\teg. 'pd -system -t 10'\r\n");
					exit(0);
				}
				
				// Skip next argument
				i++;
			}
			else
			{
				fprintf(stderr,"Failed to parse -t argument. It must be followed by a number:\r\n\teg. 'pd -system -t 10'\r\n");
				exit(0);
			}
		}
		else if( lstrcmp(argv[i],L"-c") == 0 )
		{
			if( i + 1 < argc )
			{
				// Extract the path to use as the clean file database
				clean_database = new char[wcslen(argv[i+1]) + 1];
				sprintf( clean_database, "%S", argv[i+1] );
				printf("Set clean database filepath to %s.\r\n", clean_database);
				
				// Skip next argument
				i++;
			}
			else
			{
				fprintf(stderr,"Failed to parse -c argument. It must be followed by a path to the clean file hash database to use.\r\n");
				exit(0);
			}
		}
		else if( lstrcmp(argv[i],L"-o") == 0 )
		{
			if( i + 1 < argc )
			{
				// Extract the default output path to use
				char* output_path = new char[wcslen(argv[i+1]) + 1];
				sprintf( output_path, "%S", argv[i+1] );
				options.set_output_path( output_path );
				printf("Set output path to %s.\r\n", output_path);
				delete [] output_path;
				
				// Skip next argument
				i++;
			}
			else
			{
				fprintf(stderr,"Failed to parse -c argument. It must be followed by a path to the clean file hash database to use.\r\n");
				exit(0);
			}
		}
		else if( lstrcmp(argv[i],L"-system") == 0 )
			flagSystemDump = true;
		else if( lstrcmp(argv[i],L"-db") == 0 )
		{
			// Process the db commands
			if(  i + 1 < argc )
			{
				// Process this db command
				// -db gen
				// -db genquick
				// -db add [directory]
				// -db clean
				// -db ignore
				if( lstrcmp(argv[i+1],L"gen") == 0 )
				{
					flagDB_gen = true;
				}else if( lstrcmp(argv[i+1],L"genquick") == 0 )
				{
					flagDB_genQuick = true;
				}else if( lstrcmp(argv[i+1],L"clean") == 0 )
				{
					flagDB_clean = true;
				}else if( lstrcmp(argv[i+1],L"ignore") == 0 )
				{
					flagDB_ignore = true;
				}else if( lstrcmp(argv[i+1],L"add") == 0 )
				{
					// Has yet another argument to specify the directory to add
					if(  i + 2 < argc )
					{
						// Extract the directory name to add
						add_directory = new char[wcslen(argv[i+2]) + 1];
						sprintf( add_directory, "%S", argv[i+2] );

						DIR* pdir = opendir( add_directory );
						if( pdir != NULL )
						{
							flagDB_add = true;
							closedir( pdir );
						}
						else
						{
							fprintf(stderr,"Failed to process '-db add' argument. The directory '%s' does not exist or is not accessible.\r\n", add_directory);
							exit(0);
						}
					}
					else
					{
						fprintf(stderr,"Failed to parse '-db add' argument. It must be followed by a directory to add:\r\n\teg. 'pd -db add C:\\Windows\\'\r\n");
						exit(0);
					}
					i+=1;
				}else if( lstrcmp(argv[i+1],L"remove") == 0 || lstrcmp(argv[i+1],L"rem") == 0 )
				{
					// Has yet another argument to specify the directory to remove
					if(  i + 2 < argc )
					{
						// Extract the directory name to remove
						add_directory = new char[wcslen(argv[i+2]) + 1];
						sprintf( add_directory, "%S", argv[i+2] );

						DIR* pdir = opendir( add_directory );
						if( pdir != NULL )
						{
							flagDB_remove = true;
							closedir( pdir );
						}
						else
						{
							fprintf(stderr,"Failed to process '-db remove' argument. The directory '%s' does not exist or is not accessible.\r\n", add_directory);
							exit(0);
						}
					}
					else
					{
						fprintf(stderr,"Failed to parse '-db remove' argument. It must be followed by a directory to remove:\r\n\teg. 'pd -db add C:\\Windows\\'\r\n");
						exit(0);
					}
					i+=1;
				}


				i+=1;
			}else{
				fprintf(stderr,"Failed to parse -db argument. It must be followed by a command:\r\n\teg. 'pd -db genquick'\r\n");
				exit(0);
			}
		}else{
			// This is an unassigned argument
			fprintf(stderr,"Failed to parse argument number %i, '%S'. Try 'pd --help' for usage instructions.\r\n", i, argv[i]);
			exit(0);
		}
	}

	if( flagHeader )
	{
		printf("Process Dump v2.1\r\n");
		printf("  Copyright © 2017, Geoff McDonald\r\n");
		printf("  http://www.split-code.com/\r\n");
		printf("  https://github.com/glmcdona/Process-Dump\r\n\r\n");
	}

	if( flagHelp )
	{
		// Print help page
		printf("Process Dump (pd.exe) is a tool used to dump both 32 and 64 bit executable modules back to disk from memory within a process address space. This tool is able to find and dump hidden modules as well as loose executable code chunks, and it uses a clean hash database to exclude dumping of known clean files. This tool uses an aggressive import reconstruction approach that links all DWORD/QWORDs that point to an export in the process to the corresponding export function. Process dump can be used to dump all unknown code from memory ('-system' flag), dump specific processes, or run in a monitoring mode that dumps all processes just before they terminate.\r\n\r\n");
		printf("Before first usage of this tool, when on the clean workstation the clean exclusing hash database can be generated by either:\r\n");
		printf("\tpd -db gen\r\n");
		printf("\tpd -db genquick\r\n\r\n");
		printf("Example Usage:\r\n");
		printf("\tpd -system\r\n");
		printf("\tpd -pid 419\r\n");
		printf("\tpd -pid 0x1a3\r\n");
		printf("\tpd -pid 0x1a3 -a 0x401000 -o c:\\dump\\ -c c:\\dump\\test\\clean.db\r\n");
		printf("\tpd -p chrome.exe\r\n");
		printf("\tpd -p \"(?i).*chrome.*\"\r\n");
		printf("\tpd -closemon\r\n\r\n");

		printf("Options:\r\n");
		printf("\t-system\t\tDumps all modules not matching the clean hash database\r\n\t\t\tfrom all accessible processes into the working\r\n\t\t\tdirectory.\r\n\r\n");
		printf("\t-pid <pid>\tDumps all modules not matching the clean hash database\r\n\t\t\tfrom the specified pid into the current working\r\n\t\t\tdirectory. Use a '0x' prefix to specify a hex PID.\r\n\r\n");
		printf("\t-closemon\t\tRuns in monitor mode. When any processes are terminating\r\n\t\t\tprocess dump will first dump the process.\r\n\r\n");
		printf("\t-p <regex>\tDumps all modules not matching the clean hash database\r\n\t\t\tfrom the process name found to match the filter into\r\n\t\t\tspecified pid into the current working directory.\r\n\r\n");
		printf("\t-g\t\tForces generation of PE headers from scratch, ignoring existing headers.\r\n\r\n");
		printf("\t-o <path>\tSets the default output root folder for dumped components.\r\n\r\n");
		//printf("\t-log\t\tRuns in log generation mode. No files are dumped, logfiles are generated for analysis. Specify a network path as the output and batch run to generate a report of multiple workstations.\r\n\r\n");
		//printf("\t-netcollect\t\tRuns in network sample collection mode. Specify a network path as the output, dumped files will be organized by hash.\r\n\r\n");
		printf("\t-v\t\tVerbose.\r\n\r\n");
		printf("\t-nh\t\tNo header is printed in the output.\r\n\r\n");
		printf("\t-nr\t\tDisable recursion on hash database directory add or\r\n\t\t\tremove commands.\r\n\r\n");
		printf("\t-ni\t\tDisable import reconstruction.\r\n\r\n");
		printf("\t-nc\t\tDisable dumping of loose code regions.\r\n\r\n");
		printf("\t-nt\t\tDisable multithreading.\r\n\r\n");
		printf("\t-t <thread count>\t\tSets the number of threads to use (default 16).\r\n\r\n");
		printf("\t-c <filepath>\t\tFull filepath to the clean hash database to use for this run.\r\n\r\n");
		printf("\t-db gen\t\tAutomatically processes a few common folders as well as\r\n\t\t\tall the currently running processes and adds the found\r\n\t\t\tmodule hashes to the clean hash database. It will add\r\n\t\t\tall files recursively in: \r\n\t\t\t\t%%WINDIR%% \r\n\t\t\t\t%%HOMEPATH%% \r\n\t\t\t\tC:\\Program Files\\ \r\n\t\t\t\tC:\\Program Files (x86)\\ \r\n\t\t\tAs well as all modules in all running processes \r\n\r\n");
		printf("\t-db genquick\tAdds the hashes from all modules in all processes to\r\n\t\t\tthe clean hash database. Run this on a clean system.\r\n\r\n");
		printf("\t-db add <dir>\tAdds all the files in the specified directory\r\n\t\t\trecursively to the clean hash database. \r\n\r\n");
		printf("\t-db rem <dir>\tRemoves all the files in the specified directory\r\n\t\t\trecursively from the clean hash database. \r\n\r\n");
		printf("\t-db clean\tClears the clean hash database.\r\n\r\n");
		printf("\t-db ignore\tIgnores the clean hash database when dumping a process\r\n\t\t\tthis time.  All modules will be dumped even if a match\r\n\t\t\tis found.\r\n\r\n");
	}
	
	// Sanity check on flags
	if( (int) flagPidDump + (int) flagProcessNameDump + (int) flagSystemDump +
		(int) flagDB_gen + (int) flagDB_genQuick + (int) flagDB_add + (int) flagDB_clean + (int) flagDumpCloses > 1 )
	{
		// Only one of these at a time
		fprintf(stderr,"Error. Only one process dump or hash database command should be issued per execution.\r\n");
		exit(0);
	}

	if( flagAddressDump && !flagPidDump )
	{
		// Only one of these at a time
		fprintf(stderr,"Error. Dumping a specific address only works with the -pid flag to specify the process.\r\n");
		exit(0);
	}


	// Warn if the process was not run as administrator
	HANDLE h_Process = GetCurrentProcess();
	if( !is_elevated(h_Process) )
	{
		printf("WARNING: This tool should be run with administrator rights for best results.\r\n\r\n");
	}

	// Request maximum thread token privileges
	if( !get_privileges(h_Process) )
	{
		printf("WARNING: Failed to adjust token privileges. This may result in not being able to access some processes due to insufficient privileges.\r\n\r\n");
	}

	// Warn if running in 32 bit mode on a 64 bit OS
	if( is_win64() && sizeof(void*) == 4 )
	{
		printf("WARNING: To properly access all processes on a 64 bit Windows version, the 64 bit version of this tool should be used. Currently Process Dump is running as a 32bit process under a 64bit operating system.\r\n\r\n");
	}

	

	pe_hash_database* db = new pe_hash_database(clean_database);


	if( flagDB_clean )
	{
		db->clear_database();
		printf("Cleared the clean hash database.\r\n");
		db->save();
	}else if( flagDB_add )
	{
		// Add the specified folder
		if( flagRecursion )
			printf("Adding all files in folder '%s' recursively to clean hash database...\r\n", add_directory);
		else
			printf("Adding all files in folder '%s' to clean hash database...\r\n", add_directory);

		int count_before = db->count();
		db->add_folder(add_directory, L"*", flagRecursion);
		printf("Added %i new hashes to the database. It now has %i hashes.\r\n", db->count() - count_before, db->count());
		db->save();
	}else if( flagDB_remove )
	{
		// Remove the specified folder
		if( flagRecursion )
			printf("Removing all files in folder '%s' recursively from the clean hash database...\r\n", add_directory);
		else
			printf("Removing all files in folder '%s' from the clean hash database...\r\n", add_directory);
		
		int count_before = db->count();
		db->remove_folder(add_directory, L"*", flagRecursion);
		printf("Removed %i hashes from the database. It now has %i hashes.\r\n", count_before - db->count(), db->count());
		db->save();
	}else if( flagDB_gen )
	{
		printf("Generating full clean database. This can take up to 30 minutes depending on the system.\r\n");

		// Add all the running processes to the clean hash database
		int count_before = db->count();
		printf("Adding modules from all running processes to clean hash database...\r\n");
		add_system_hashes( db, &options );
		printf("...added %i new hashes from running processes.\r\n", db->count() - count_before);
		db->save();

		// Add a bunch of folders to the database
		count_before = db->count();
		printf("Adding files in %%WINDIR%% to clean hash database...\r\n");
		db->add_folder("%WINDIR%", L"*", true);
		printf("...added %i new hashes from %%WINDIR%%.\r\n", db->count() - count_before);
		db->save();

		count_before = db->count();
		printf("Adding files in %%USERPROFILE%% to clean hash database...\r\n");
		db->add_folder("%USERPROFILE%", L"*", true);
		printf("...added %i new hashes from %%USERPROFILE%%.\r\n", db->count() - count_before);
		db->save();

		count_before = db->count();
		printf("Adding files in 'C:\\Program Files\\' to clean hash database...\r\n");
		db->add_folder("C:\\Program Files\\", L"*", true);
		printf("...added %i new hashes from 'C:\\Program Files\\'.\r\n", db->count() - count_before);
		db->save();

		count_before = db->count();
		printf("Adding files in C:\\Program Files (x86)\\ to clean hash database...\r\n");
		db->add_folder("C:\\Program Files (x86)\\", L"*", true);
		printf("...added %i new hashes from 'C:\\Program Files (x86)\\'.\r\n", db->count() - count_before);
		db->save();

		printf("\r\nFinished. The clean hash  database now has %i hashes.\r\n", db->count());
	}else if( flagDB_genQuick )
	{
		// Add all the running processes to the clean hash database
		int count_before = db->count();
		printf("Adding modules from all running processes to clean hash database...\r\n");
		add_system_hashes( db, &options );
		printf("...added %i new hashes from running processes.\r\n", db->count() - count_before);
		db->save();

		printf("\r\nFinished. The clean hash database now has %i hashes.\r\n", db->count());
	}

	// Clear the database if we set the flag to not use the database
	if( flagDB_ignore )
	{
		db->clear_database();
		printf("Ignoring the clean hash database for this execution.\r\n");
	}

	// Now process the dumping commands
	if( flagPidDump )
	{
		// Dump the specified PID
		dump_process* dumper = new dump_process( pid, db,  &options, false );

		if( flagAddressDump )
		{
			dumper->dump_region( address );
		}
		else
		{
			dumper->dump_all();
		}
		delete dumper;
	}
	else if( flagProcessNameDump )
	{
		// Dump the specified regex process name(s)

		// First gather the process matches
		DynArray<process_description*> matches;
		int count = process_find( processNameFilter, &matches );

		if( count > 1 )
		{
			// As the user if we should really dump all the found processes
			printf("\r\n\r\nPID\tProcess Name\r\n");
			for( int i = 0; i < count; i++ )
			{
				printf("0x%x\t%s\r\n", matches[i]->pid, matches[i]->process_name);
			}

			printf("\r\n\r\nAre you sure all of these processes should be dumped? (y/n): ");

			char* answer = new char[10];
			fgets( answer, 10, stdin );
			if( answer[0] != 'y' )
			{
				delete[] answer;
				exit(0);
			}
			delete[] answer;
		}

		// Loop through dumping the matching processes. Don't double-dump
		// modules with the same hash -- only dump them the first time
		// they are seen.
		unordered_set<unsigned __int64> new_hashes;
		for( int i = 0; i < count; i++ )
		{
			// Process this process
			dump_process* dumper = new dump_process( matches[i]->pid, db, &options, false );

			dumper->dump_all();

			// Exclude these hashes from the next dumps
			dumper->get_all_hashes( &new_hashes );
			db->add_hashes( new_hashes );
			new_hashes.clear();

			delete dumper;
		}
	}
	else if( flagSystemDump )
	{
		// Dump all processes running on the machine right now
		dump_system( db,  &options );
	}
	else if( flagPidDump )
	{
		// Dump the specified process
		dump_process* dumper = new dump_process( pid, db,  &options, false );
		dumper->dump_all();
		delete dumper;
	}
	else if (flagDumpCloses)
	{
		// Run in monitoring mode to dump all processes on close

		// Register the quit handler (CTRL-C to stop)
		if (SetConsoleCtrlHandler((PHANDLER_ROUTINE)ConsoleHandler, TRUE) == FALSE)
		{
			// unable to install handler... 
			// display message to the user
			printf("WARNING: Unable to install keyboard handler. This means that process dump will not be able to close or cleanup properly.\r\n");
		}

		// Start the hook monitor
		close_watcher* watcher = new close_watcher(db, &options);
		watcher->start_monitor();

		printf("------> Note: You may cleanly quit at any time by pressing CTRL-C. <------\r\n");

		// Wait until the user requests a close (by CTRL-C)
		while (!ConsoleRequestingClose)
		{
			Sleep(100);
		}
		
		// Cleanup properly
		printf("Cleaning up process terminate hooks cleanly...\r\n");
		watcher->stop_monitor();
		delete watcher;
	}

	printf("Finished running.\r\n");

	return 0;
}


