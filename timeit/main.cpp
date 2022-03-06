#define _CRT_SECURE_NO_WARNINGS				// TODO: Make sure the places in the code that are affected by this aren't better off without it. Your number to string conversion might overflow if your not careful with your bounds. How big should the bounds be to fit all floats?

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <io.h>

#include <thread>
#include <chrono>

#include <iostream>

#include <string>



char childOutputBuffer[1024];
OVERLAPPED readOutputEvent = { };
void readChildOutput() {
		if (!ReadFile(parentOutputReadHandle, childOutputBuffer, sizeof(childOutputBuffer), nullptr, &readOutputEvent)) {
			std::cerr << "ReadFile from parentOutputReadHandle failed\n";
			std::cerr << strerror(GetLastError()) << '\n';
			exit(EXIT_FAILURE);
		}
}

char childErrorBuffer[1024];
OVERLAPPED readErrorEvent = { };
void readChildError() {
		if (!ReadFile(parentErrorReadHandle, childErrorBuffer, sizeof(childErrorBuffer), nullptr, &readErrorEvent)) {
			std::cerr << "ReadFile from parentErrorReadHandle failed\n";
			std::cerr << strerror(GetLastError()) << '\n';
			exit(EXIT_FAILURE);
		}
}

char parentInputBuffer[1024];
OVERLAPPED readInputEvent = { };
void readParentInput() {
		if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), parentInputBuffer, sizeof(parentInputBuffer), nullptr, &readInputEvent)) {
			std::cerr << "ReadFile from parent stdin failed\n";
			std::cerr << strerror(GetLastError()) << '\n';
			exit(EXIT_FAILURE);
		}
}

void managePipes() {
	//readChildOutput();
	//readChildError();
	//readParentInput();

	while (true) {
		unsigned long bytesAvailable;
		if (!PeekNamedPipe(parentOutputReadHandle, nullptr, 0, nullptr, &bytesAvailable, nullptr)) {
			if (GetLastError() == ERROR_BROKEN_PIPE) {
				return;
			}
			std::cerr << "failed while polling child output pipe\n";
			std::cerr << strerror(GetLastError()) << '\n';
			exit(EXIT_FAILURE);
		}
		if (bytesAvailable != 0) {
			unsigned long bytesRead;
			if (!ReadFile(parentOutputReadHandle, childOutputBuffer, sizeof(childOutputBuffer), &bytesRead, nullptr)) {
				std::cerr << "ReadFile from parentOutputReadHandle failed\n";
				std::cerr << strerror(GetLastError()) << '\n';
				exit(EXIT_FAILURE);
			}
			std::cout.write(childOutputBuffer, bytesRead);
		}
/*
		if (HasOverlappedIoCompleted(&readOutputEvent)) {
			unsigned long bytesRead;
			if (!GetOverlappedResult(parentOutputReadHandle, &readOutputEvent, &bytesRead, true)) {
				if (GetLastError() == ERROR_HANDLE_EOF) {  }
				else { std::cerr << "GetOverlappedResult failed with parentOutputReadHandle\n"; exit(EXIT_FAILURE); }
			} else {
				std::cout.write(childOutputBuffer, bytesRead);
				readChildOutput();
			}
		}
		if (HasOverlappedIoCompleted(&readErrorEvent)) {
			unsigned long bytesRead;
			if (!GetOverlappedResult(parentErrorReadHandle, &readErrorEvent, &bytesRead, true)) {
				if (GetLastError() == ERROR_HANDLE_EOF) { }
				else { std::cerr << "GetOverlappedResult failed with parentErrorReadHandle\n"; exit(EXIT_FAILURE); }
			} else {
				std::cerr.write(childErrorBuffer, bytesRead);
				readChildError();
			}
		}
		if (HasOverlappedIoCompleted(&readInputEvent)) {
			unsigned long bytesRead;
			if (!GetOverlappedResult(GetStdHandle(STD_INPUT_HANDLE), &readInputEvent, &bytesRead, true)) {
				if (GetLastError() == ERROR_HANDLE_EOF) {
					CloseHandle(parentInputWriteHandle); return;
				} else { std::cerr << "GetOverlappedResult failed with parent stdin\n"; exit(EXIT_FAILURE); }
			}
			else {
				unsigned long tempbytesRead = bytesRead;
				if (!WriteFile(parentInputWriteHandle, parentInputBuffer, tempbytesRead, &bytesRead, nullptr)) {
					std::cerr << "failed to write to child process input\n";
					exit(EXIT_FAILURE);
				}
				readParentInput();
			}
		}
		*/
	}
}

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// ANSI escape code helpers.
#define ANSI_ESC_CODE_PREFIX "\033["
#define ANSI_ESC_CODE_SUFFIX "m"
#define ANSI_ESC_CODE_MIN_SIZE ((sizeof(ANSI_ESC_CODE_PREFIX) - 1) + (sizeof(ANSI_ESC_CODE_SUFFIX) - 1))

const char* const helpText = "timeit runs the specified program with the specified arguments and prints the elapsed time until program completion to stderr. The stdin and stdout of timeit are both redirected to the stdin and stdout of the specified program, allowing " \
							 "the construct to be used seamlessly within piped chains of programs.\n" \
							 "\n" \
							 "usage: timeit [--expand-args || --error-color <auto|on|off> || --unit <nanoseconds|microseconds|milliseconds|seconds|minutes|hours> || --accuracy <double|int>] <program> <arguments>...\n" \
							 "       timeit <--help || --h>            -->            shows help text\n" \
							 "\n" \
							 "arguments:\n" \
								"\t--expand-args                 -->                  expand elements of <arguments>... that contain spaces into multiple arguments (default behaviour is to leave them as single arguments)\n" \
								"\t--error-color <auto|on|off>   -->                  force a specific coloring behaviour for error messages (which are always printed to stderr) (default behaviour is auto)\n" \
								"\t--unit <(see above)>          -->                  the unit to output the elapsed time in (default is seconds)\n" \
								"\t--accuracy <double|int>       -->                  specify whether elapsed time is outputted as a decimal (double) or as a round number (int) (default is double)\n"
	// TODO: You still need to explain the last two cmdline args in the arguments section.
							 "\n" \
							 "NOTE: It is possible to specify a flag more than once. If this is the case, only the last occurrence will influence program behaviour.\n";

// Flag to keep track of whether we should color errors or not.
bool isErrorColored;

// Coloring
namespace color {
	char* red = nullptr;										// NOTE: The nullptr is needed so that error reporting subroutines know if they need to allocate colors or if it has been done for them already.
	void unsafeInitRed() { red = new char[ANSI_ESC_CODE_MIN_SIZE + 2 + 1]; memcpy(red, ANSI_ESC_CODE_PREFIX "31" ANSI_ESC_CODE_SUFFIX, ANSI_ESC_CODE_MIN_SIZE + 2 + 1); }
	void unsafeInitPipedRed() { red = new char; *red = '\0'; }

	char* reset;
	void unsafeInitReset() { reset = new char[ANSI_ESC_CODE_MIN_SIZE + 1 + 1]; memcpy(reset, ANSI_ESC_CODE_PREFIX "0" ANSI_ESC_CODE_SUFFIX, ANSI_ESC_CODE_MIN_SIZE + 1 + 1); }
	void unsafeInitPipedReset() { reset = new char; *reset = '\0'; }

	void initErrorColoring() { if (isErrorColored) { unsafeInitRed(); unsafeInitReset(); return; } unsafeInitPipedRed(); unsafeInitPipedReset(); }

	void release() { delete[] color::red; delete[] color::reset; }
}

// TODO: Be super const correct on all the read-only strings and stuff, in case you haven't done that already.

namespace flags {
	bool expandArgs = false;
	uint8_t timeUnit = 3;
	bool timeAccuracy = false;
}

void showHelp() { std::cout << helpText; }				// NOTE: I have previously only shown help when output is connected to TTY, so as not to pollute stdout when piping. Back then, help was shown sometimes when it wasn't requested, which made it prudent to include that feature. Now, you have to explicitly ask for help, making this TTY branching unnecessary.

bool forcedErrorColoring;					// NOTE: GARANTEE: If something goes wrong while parsing the cmdline args and an error message is necessary, the error message will always be printed with the default coloring (based on TTY/piped mode).

// Parse flags at the argument level. Calls parseFlagGroup if it encounters flag groups and handles word flags (those with -- in front) separately.
unsigned int parseFlags(int argc, const char* const * argv) {																// NOTE: If you write --context twice or --color twice (or any additional flags that we may add), the value supplied to the rightmost instance will be the value that is used. Does not throw an error.
	for (int i = 1; i < argc; i++) {
		const char* arg = argv[i];
		if (arg[0] == '-') {
			if (arg[1] == '-') {
				const char* flagTextStart = arg + 2;
				if (*flagTextStart == '\0') { continue; }
				if (!strcmp(flagTextStart, "expand-args")) {
					flags::expandArgs = true;
					continue;
				}				// TODO: Make sure I don't have any std::cerrs without newline built into string lying around.
				if (!strcmp(flagTextStart, "error-color")) {
					i++;					// TODO: Change all the std::couts to std::cerrs.
					if (i == argc) {
						color::initErrorColoring();
						std::cout << color::red << format::error << "the --error-color flag was not supplied with a value\n" << color::reset;
						color::release();
						exit(EXIT_SUCCESS);
					}
					if (!strcmp(argv[i], "on")) { forcedErrorColoring = true; continue; }
					if (!strcmp(argv[i], "off")) { forcedErrorColoring = false; continue; }
					if (!strcmp(argv[i], "auto")) { forcedErrorColoring = isErrorColored; continue; }
					color::initErrorColoring();
					std::cout << color::red << format::error << "invalid value for --error-color flag\n" << color::reset;
					color::release();
					exit(EXIT_SUCCESS);
				}
				if (!strcmp(flagTextStart, "unit")) {
					i++;
					if (i == argc) {
						color::initErrorColoring();
						std::cout << color::red << format::error << "the --unit flag was not supplied with a value\n" << color::reset;
						color::release();
						exit(EXIT_SUCCESS);
					}
					if (!strcmp(argv[i], "nanoseconds")) { flags::timeUnit = 0; continue; }
					if (!strcmp(argv[i], "microseconds")) { flags::timeUnit = 1; continue; }
					if (!strcmp(argv[i], "milliseconds")) { flags::timeUnit = 2; continue; }
					if (!strcmp(argv[i], "seconds")) { flags::timeUnit = 3; continue; }
					if (!strcmp(argv[i], "minutes")) { flags::timeUnit = 4; continue; }
					if (!strcmp(argv[i], "hours")) { flags::timeUnit = 5; continue; }
					color::initErrorColoring();
					std::cout << color::red << format::error << "invalid value for --unit flag\n" << color::reset;
					color::release();
					exit(EXIT_SUCCESS);
				}
				if (!strcmp(flagTextStart, "accuracy")) {
					i++;
					if (i == argc) {
						color::initErrorColoring();
						std::cerr << color::red << format::error << "the --accuracy flag was not supplied with a value\n" << color::reset;
						color::release();
						exit(EXIT_SUCCESS);
					}
					if (!strcmp(argv[i], "double")) { flags::timeAccuracy = false; continue; }				// NOTE: This might seem unnecessary, but we need it in case the --accuracy flag is used twice and has already changed the value.
					if (!strcmp(argv[i], "int")) { flags::timeAccuracy = true; continue; }
					color::initErrorColoring();
					std::cout << color::red << format::error << "invalid value for --accuracy flag\n" << color::reset;
					color::release();
					exit(EXIT_SUCCESS);
				}
				if (!strcmp(flagTextStart, "help")) { showHelp(); exit(EXIT_SUCCESS); }
				if (!strcmp(flagTextStart, "h")) { showHelp(); exit(EXIT_SUCCESS); }
				color::initErrorColoring();
				std::cout << color::red << format::error << "one or more flag arguments are invalid\n" << color::reset;
				color::release();
				exit(EXIT_SUCCESS);
			}
			color::initErrorColoring();
			std::cout << color::red << format::error << "one or more flag arguments are invalid\n" << color::reset;
			color::release();
			exit(EXIT_SUCCESS);
		}
		return i;																									// Return index of first arg that isn't flag arg. Helps calling code parse args.
	}
	return argc;																									// No non-flag argument was found. Return argc because it works nicely with calling code.
}

HANDLE childOutputHandle;
HANDLE childInputHandle;
HANDLE childErrorHandle;

HANDLE parentOutputReadHandle;
HANDLE parentInputWriteHandle;
HANDLE parentErrorReadHandle;

void CreatePipes() {
	if (!CreatePipe(&parentOutputReadHandle, &childOutputHandle, nullptr, 0)) {
		color::initErrorColoring();			// TODO: Make sure there are no extra punctuation in your error messages.
		std::cerr << color::red << "ERROR: failed to create pipe from child stdout to parent\n" << color::reset;
		color::release();
		exit(EXIT_FAILURE);						// NOTE: We exit with EXIT_FAILURE here because this is presumably a system error and it shouldn't really be the users fault AFAIK.
	}
	if (!SetHandleInformation(childOutputHandle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
		color::initErrorColoring();
		std::cerr << color::red << "ERROR: failed to set child stdout handle to inheritable\n" << color::reset;
		color::release();
		exit(EXIT_FAILURE);
	}

	if (!CreatePipe(&childInputHandle, &parentInputWriteHandle, nullptr, 0)) {
		color::initErrorColoring();
		std::cerr << color::red << "ERROR: failed to create pipe from parent to child stdin\n" << color::reset;
		color::release();
		exit(EXIT_FAILURE);
	}
	if (!SetHandleInformation(childInputHandle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
		color::initErrorColoring();
		std::cerr << color::red << "ERROR: failed to set child stdin handle to inheritable\n" << color::reset;
		color::release();
		exit(EXIT_FAILURE);
	}

	if (!CreatePipe(&parentErrorReadHandle, &childErrorHandle, nullptr, 0)) {
		color::initErrorColoring();
		std::cerr << color::red << "ERROR: failed to create pipe from child stderr to parent\n" << color::reset;
		color::release();
		exit(EXIT_FAILURE);
	}
	if (!SetHandleInformation(childErrorHandle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
		color::initErrorColoring();
		std::cerr << color::red << "ERROR: failed to set child stderr handle to inheritable\n" << color::reset;
		color::release();
		exit(EXIT_FAILURE);
	}
}

std::chrono::nanoseconds runChildProcess(int argc, const char* const * argv) {
	if (argv[0][0] == '\0') {				// NOTE: I thought this was impossible, but apparently it isn't, so we have to check for it and report an error if it occurs.
											// NOTE: The reason we don't just let CreateProcessA detect this error is because it will probably just filter out the nothingness and use the first argument as the program name, which is very terrible.
		color::initErrorColoring();
		std::cerr << color::red << "ERROR: target program name cannot be empty\n" << color::reset;
		color::release();
		exit(EXIT_SUCCESS);
	}

	std::string buffer;
	buffer += '\"';									// IMPORTANT NOTE: These are super necessary because you wouldn't otherwise be able to run programs that have names with spaces in them. Even worse, you could accidentally run a completely different program than the one you wanted, which is terrible behaviour. That is why we add the quotes, to prevent against those two things.
	buffer += argv[0];
	buffer += '\" ';
	if (flags::expandArgs) {						// NOTE: The difference in behviour here is achieved by adding quotes around the arguments. When quotes are present, arguments with spaces are still considered as one argument. Without quotes, the given arguments will be split up into many arguments when CreateProcessA creates the child process.
		for (int i = 1; i < argc; i++) {
			buffer += argv[i];
			buffer += ' ';
		}
	}
	else {
		for (int i = 0; i < argc; i++) {
			buffer += '\"';
			buffer += argv[i];
			buffer += '\" ';
		}
	}

	std::cerr << buffer << '\n';					// TODO: Temp code, remove this later.

	PROCESS_INFORMATION procInfo = { };

	STARTUPINFOA startupInfo = { };
	startupInfo.cb = sizeof(STARTUPINFOA);
	startupInfo.hStdOutput = childOutputHandle;
	startupInfo.hStdInput = childInputHandle;				// TODO: Make sure that the program name thing can't be put in the first argument of the below function. Maybe I was just doing it wrong before? Does it still discover the program in the same way?
	startupInfo.hStdError = childErrorHandle;
	startupInfo.dwFlags = STARTF_USESTDHANDLES;

	std::chrono::high_resolution_clock::time_point startTime = std::chrono::high_resolution_clock::now();

	char* innerBuffer = buffer.data();						// NOTE: We have to use data() here instead of c_str() because it's undefined behaviour otherwise. The deal with data() is that you're allowed to change everything except the NUL termination character at the end of c-style string that is returned.
	// NOTE: The way it is now, CreateProcessA can change most of the contents, and if it changes the pointer to point to something else, we don't care because the pointer is just a copy. This means that we are as safe as we can be. The only danger comes from the possible modification of the NUL character, which CreateProcessA probably won't do because why should it.
	// NOTE: I should also point out that the docs say that CreateProcessW is the one that changes the string, not CreateProcessA, so we should be safe. The only reason I'm taking these precautions is because the parameter isn't marked with const and I don't totally trust the docs.
	if (!CreateProcessA(nullptr, innerBuffer, nullptr, nullptr, true, 0, nullptr, nullptr, &startupInfo, &procInfo)) {
		std::cerr << "had some trouble starting child process\n";
		std::cerr << strerror(GetLastError()) << '\n';
		exit(EXIT_FAILURE);
		// TODO: Be more specific here and branch over what the error actually is. If it's user-made you should exit with success and if it's a system issue you should exit with failure.
	}

	WaitForSingleObject(procInfo.hProcess, INFINITE);				// TODO: Handle error here.
	unsigned long exitCode;
	GetExitCodeProcess(procInfo.hProcess, &exitCode);
	std::cerr << exitCode << '\n';

	CloseHandle(procInfo.hProcess);						// TODO: Handle errors here too.
	CloseHandle(procInfo.hThread);

	CloseHandle(childOutputHandle);				// TODO: See if you can cause the other thread to somehow read an EOF instead of breaking the pipe. It has more to do with the code of the other thread than the code of this thread.
	CloseHandle(childInputHandle);
	CloseHandle(childErrorHandle);

	return std::chrono::high_resolution_clock::now() - startTime;

	char buffer2[128];
	sprintf(buffer2, "%f", duration.count());
	return std::string(buffer2);
}

std::string intToString(uint64_t value) {
	return std::string("hi");
}

std::string doubleToString(double value) {

}

void manageArgs(int argc, const char* const * argv) {
	unsigned int targetProgramIndex = parseFlags(argc, argv);														// Parse flags before doing anything else.
	unsigned int targetProgramArgCount = argc - targetProgramIndex;
	switch (targetProgramArgCount) {
	case 0:
		color::initErrorColoring();
		std::cout << color::red << format::error << "too few arguments\n" << color::reset;
		color::release();
		exit(EXIT_SUCCESS);
	default:
		isErrorColored = forcedErrorColoring;																		// If everything went great with parsing the cmdline args, finally set output coloring to what the user wants it to be. It is necessary to do this here because of the garantee that we wrote above.
		CreatePipes();
		std::chrono::nanoseconds elapsedTime = RunChildProcess(targetProgramArgCount, argv + targetProgramIndex);
		std::string elapsedTimeString;
		switch (flags::timeUnit) {
		case 0:
			if (flags::timeAccuracy) { elapsedTimeString = intToString(std::chrono::duration_cast<std::chrono::duration<uint64_t, std::nano>>(elapsedTime).count()); break; }
			elapsedTimeString = doubleToString(std::chrono::duration_cast<std::chrono::duration<double, std::nano>>(elapsedTime).count()); break;
		case 1:
			if (flags::timeAccuracy) { elapsedTimeString = intToString(std::chrono::duration_cast<std::chrono::duration<uint64_t, std::micro>>(elapsedTime).count()); break; }
			elapsedTimeString = doubleToString(std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(elapsedTime).count()); break;
		case 2:
			if (flags::timeAccuracy) { elapsedTimeString = intToString(std::chrono::duration_cast<std::chrono::duration<uint64_t, std::milli>>(elapsedTime).count()); break; }
			elapsedTimeString = doubleToString(std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(elapsedTime).count()); break;
		case 3:
			if (flags::timeAccuracy) { elapsedTimeString = intToString(std::chrono::duration_cast<std::chrono::duration<uint64_t>>(elapsedTime).count()); break; }
			elapsedTimeString = doubleToString(std::chrono::duration_cast<std::chrono::duration<double>>(elapsedTime).count()); break;
		case 4:
			if (flags::timeAccuracy) { elapsedTimeString = intToString(std::chrono::duration_cast<std::chrono::duration<uint64_t, std::ratio<60, 1>>>(elapsedTime).count()); break; }
			elapsedTimeString = doubleToString(std::chrono::duration_cast<std::chrono::duration<double, std::ratio<60, 1>>>(elapsedTime).count()); break;
		case 5:
			if (flags::timeAccuracy) { elapsedTimeString = intToString(std::chrono::duration_cast<std::chrono::duration<uint64_t, std::ratio<3600, 1>>>(elapsedTime).count()); break; }
			elapsedTimeString = doubleToString(std::chrono::duration_cast<std::chrono::duration<double, std::ratio<3600, 1>>>(elapsedTime).count()); break;
		}
		std::cerr << elapsedTimeString << '\n';
	}
}

int main(int argc, char* const * argv) {
	std::cerr.sync_with_stdio(false);
	// TODO: Don't use std::cout, write it to stdout unbuffered.
	// TODO: Don't use std::cin, you gotta do some ReadFile stuff to allow async and signal catching.

	if (_isatty(STDERR_FILENO)) {
		HANDLE stdErrHandle = GetStdHandle(STD_ERROR_HANDLE);
		if (!stdErrHandle || stdErrHandle == INVALID_HANDLE_VALUE) { goto ANSISetupFailure; }
		DWORD mode;
		if (!GetConsoleMode(stdErrHandle, &mode)) { goto ANSISetupFailure; }
		if (!SetConsoleMode(stdErrHandle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) { goto ANSISetupFailure; }

		isErrorColored = true;
		forcedErrorColoring = true;
	}
	else { ANSISetupFailure: isErrorColored = false; forcedErrorColoring = false; }

	manageArgs(argc, argv);

	CloseHandle(parentOutputReadHandle);
	CloseHandle(parentInputWriteHandle);
	CloseHandle(parentErrorReadHandle);

	return 0;

	std::thread pipeManagerThread(managePipes);
	std::string duration = RunChildProcess(argv[1], argv[2]);
	pipeManagerThread.join();
}