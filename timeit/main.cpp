#define BUFFERED_HANDLE_READER_BUFFER_START_SIZE 2048			// TODO: Make this number the correct one, look at grep or something, somewhere I have the right number here.
#define BUFFERED_HANDLE_READER_BUFFER_STEP_SIZE 2048			// TODO: Same for this one.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <stdlib.h>
#include <malloc.h>

#include <thread>
#include <chrono>

#include <iostream>
#include <io.h>

#include <string>

//#define STDIN_FILENO 0
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
								"\t--accuracy <double|int>       -->                  specify whether elapsed time is outputted as a decimal (double) or as a round number (int) (default is double)\n" \
								"\t<program>                     -->                  the program which is to be timed\n" \
								"\t<arguments>...                -->                  the arguments to pass to the target program\n" \
							 "\n" \
							 "NOTE: It is possible to specify a flag more than once. If this is the case, only the last occurrence will influence program behaviour.\n";

// Flag to keep track of whether we should color errors or not.
bool isErrorColored;

// Coloring
namespace color {
	char* red;			// TODO: Make sure nothing relied on the previous nullptr tactic that carried over from grep.
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

// NOTE: I have previously only shown help when output is connected to TTY, so as not to pollute stdout when piping. Back then, help was shown sometimes when it wasn't requested, which made it prudent to include that feature. Now, you have to explicitly ask for help, making TTY branching unnecessary.
void showHelp() { std::cout << helpText; }

bool forcedErrorColoring;					// NOTE: GARANTEE: If something goes wrong while parsing the cmdline args and an error message is necessary, the error message will always be printed with the default coloring (based on TTY/piped mode).

// Parse flags at the argument level. Calls parseFlagGroup if it encounters flag groups and handles word flags (those with -- in front) separately.
unsigned int parseFlags(int argc, const char* const * argv) {																// NOTE: If you write --context twice or --color twice (or any additional flags that we may add), the value supplied to the rightmost instance will be the value that is used. Does not throw an error.
	for (int i = 1; i < argc; i++) {
		const char* arg = argv[i];
		if (arg[0] == '-') {
			if (arg[1] == '-') {
				const char* flagTextStart = arg + 2;
				if (*flagTextStart == '\0') { continue; }

				if (!strcmp(flagTextStart, "expand-args")) { flags::expandArgs = true; continue; }				// TODO: Make sure I don't have any std::cerrs without newline built into string lying around.

				if (!strcmp(flagTextStart, "error-color")) {
					i++;					// TODO: Change all the std::couts to std::cerrs.
					if (i == argc) {
						color::initErrorColoring();
						std::cerr << color::red << "ERROR: the --error-color flag was not supplied with a value\n" << color::reset;
						color::release();
						exit(EXIT_SUCCESS);
					}
					if (!strcmp(argv[i], "on")) { forcedErrorColoring = true; continue; }
					if (!strcmp(argv[i], "off")) { forcedErrorColoring = false; continue; }
					if (!strcmp(argv[i], "auto")) { forcedErrorColoring = isErrorColored; continue; }
					color::initErrorColoring();
					std::cerr << color::red << "ERROR: invalid value for --error-color flag\n" << color::reset;
					color::release();
					exit(EXIT_SUCCESS);
				}

				if (!strcmp(flagTextStart, "unit")) {
					i++;
					if (i == argc) {
						color::initErrorColoring();
						std::cerr << color::red << "ERROR: the --unit flag was not supplied with a value\n" << color::reset;
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
					std::cerr << color::red << "ERROR: invalid value for --unit flag\n" << color::reset;
					color::release();
					exit(EXIT_SUCCESS);
				}

				if (!strcmp(flagTextStart, "accuracy")) {
					i++;
					if (i == argc) {
						color::initErrorColoring();
						std::cerr << color::red << "ERROR: the --accuracy flag was not supplied with a value\n" << color::reset;
						color::release();
						exit(EXIT_SUCCESS);
					}
					if (!strcmp(argv[i], "double")) { flags::timeAccuracy = false; continue; }				// NOTE: This might seem unnecessary, but we need it in case the --accuracy flag is used twice and has already changed the value.
					if (!strcmp(argv[i], "int")) { flags::timeAccuracy = true; continue; }
					color::initErrorColoring();
					std::cerr << color::red << "ERROR: invalid value for --accuracy flag\n" << color::reset;
					color::release();
					exit(EXIT_SUCCESS);
				}

				if (!strcmp(flagTextStart, "help")) { showHelp(); exit(EXIT_SUCCESS); }
				if (!strcmp(flagTextStart, "h")) { showHelp(); exit(EXIT_SUCCESS); }

				//color::initErrorColoring();								// NOTE: Not necessary because of fall-through.
				//std::cerr << color::red << "ERROR: one or more flag arguments are invalid\n" << color::reset;
				//color::release();
				//exit(EXIT_SUCCESS);
			}
			color::initErrorColoring();
			std::cerr << color::red << "ERROR: one or more flag arguments are invalid\n" << color::reset;
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

void releasePipes() {							// NOTE: No need to return whether or not we were successful because when we call this function, we don't actually ever care if we were successful or not.
	CloseHandle(childOutputHandle);
	CloseHandle(parentOutputReadHandle);

	CloseHandle(childInputHandle);
	CloseHandle(parentInputWriteHandle);
	
	CloseHandle(childErrorHandle);
	CloseHandle(parentErrorReadHandle);
}

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
		CloseHandle(childOutputHandle);
		CloseHandle(parentOutputReadHandle);
		exit(EXIT_FAILURE);
	}

	if (!CreatePipe(&childInputHandle, &parentInputWriteHandle, nullptr, 0)) {
		color::initErrorColoring();
		std::cerr << color::red << "ERROR: failed to create pipe from parent to child stdin\n" << color::reset;
		color::release();
		CloseHandle(childOutputHandle);
		CloseHandle(parentOutputReadHandle);
		exit(EXIT_FAILURE);
	}
	if (!SetHandleInformation(childInputHandle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
		color::initErrorColoring();
		std::cerr << color::red << "ERROR: failed to set child stdin handle to inheritable\n" << color::reset;
		color::release();
		CloseHandle(childOutputHandle);
		CloseHandle(parentOutputReadHandle);
		CloseHandle(childInputHandle);
		CloseHandle(parentInputWriteHandle);
		exit(EXIT_FAILURE);
	}

	if (!CreatePipe(&parentErrorReadHandle, &childErrorHandle, nullptr, 0)) {
		color::initErrorColoring();
		std::cerr << color::red << "ERROR: failed to create pipe from child stderr to parent\n" << color::reset;
		color::release();
		CloseHandle(childOutputHandle);
		CloseHandle(parentOutputReadHandle);
		CloseHandle(childInputHandle);
		CloseHandle(parentInputWriteHandle);
		exit(EXIT_FAILURE);
	}
	if (!SetHandleInformation(childErrorHandle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
		color::initErrorColoring();
		std::cerr << color::red << "ERROR: failed to set child stderr handle to inheritable\n" << color::reset;
		color::release();
		releasePipes();
		exit(EXIT_FAILURE);
	}
}

class BufferedHandleReader {
public:
	HANDLE handle;

	char* buffer;
	unsigned long bufferSize;

	BufferedHandleReader(HANDLE handle) : handle(handle), bufferSize(BUFFERED_HANDLE_READER_BUFFER_START_SIZE) { buffer = new char[BUFFERED_HANDLE_READER_BUFFER_START_SIZE]; }

	void increaseBufferSize() {
		unsigned long newBufferSize = bufferSize + BUFFERED_HANDLE_READER_BUFFER_STEP_SIZE;
		char* temp = (char*)realloc(buffer, newBufferSize);
		if (temp) { buffer = temp; bufferSize = newBufferSize; }
	}

	unsigned long read() {
		unsigned long bytesRead;
		if (ReadFile(handle, buffer, bufferSize, &bytesRead, nullptr)) { if (bytesRead == bufferSize) { increaseBufferSize(); } return bytesRead; }
		return 0;
	}

	void release() { delete[] buffer; buffer = nullptr; }					// NOTE: Just remember, delete[] and delete can't set the pointer to nullptr because you haven't passed it by reference. It would go against the language rules.
	
	~BufferedHandleReader() { if (buffer) { delete[] buffer; } }	// SIDE-NOTE FOR FUTURE REFERENCE: Every time, you google if new and delete can throw, and you keep forgetting every time. Just remember, they can both throw. EXCEPT if you use a special form of new, which you should remember to use from now on because it's useful.
};

PROCESS_INFORMATION procInfo;

void managePipes() {
	bool outputClosed = false;
	bool inputClosed = false;
	bool errorClosed = false;

	unsigned long bytesAvailable;
	
	BufferedHandleReader outputReader(parentOutputReadHandle);
	//BufferedHandleReader inputReader									// TODO: I don't see a way to do this in a non-blocking way. This is the same problem we had at the beginning of grep development. Just start a new thread and handle it in a blocking way on there. Don't worry about SIGINT stuff, that handles well because either EOF is sent or the syscall is cancelled, I'm not quite sure which one yet, you should test.
	BufferedHandleReader errorReader(parentErrorReadHandle);

	while (true) {
		if (PeekNamedPipe(parentOutputReadHandle, nullptr, 0, nullptr, &bytesAvailable, nullptr)) {
			if (bytesAvailable != 0) {
				unsigned long bytesRead = outputReader.read();
				if (bytesRead != 0) {
					if (_write(STDOUT_FILENO, outputReader.buffer, bytesRead) == -1) {
						color::initErrorColoring();
						std::cerr << color::red << "ERROR: failed to write to parent stdout\n" << color::reset;
						color::release();
						outputReader.release();
						//inputReader
						errorReader.release();					// TODO: Make sure you release everything that you need to everywhere before you call exit.
						releasePipes();
						exit(EXIT_FAILURE);						// NOTE: exit doesn't call the destructors of your stack objects. It calls other things (including handlers registered with atexit and the destructors of static objects), but not the destructors of local stack objects. BE WARE!!!!
					}
				}
				else {
					color::initErrorColoring();
					std::cerr << color::red << "ERROR: failed to read from child stdout\n" << color::reset;
					color::release();
					outputReader.release();
					//inputReader
					errorReader.release();
					releasePipes();					// TODO: Replace these with break, just like the below code does.
					exit(EXIT_FAILURE);
				}
			}
		}
		else if (GetLastError() == ERROR_BROKEN_PIPE) { outputClosed = true; }
		else {
			color::initErrorColoring();
			std::cerr << color::red << "ERROR: failed to poll child output pipe\n" << color::reset;
			color::release();
			break;
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
		}*/

		// TODO: You're gonna have to put some sort of sleep thing in here so you don't take up 100% of a core with this thread. Or maybe just use 100% of the core, idk.
	}

	outputReader.release();
	//inputReader
	errorReader.release();
	releasePipes();
	CloseHandle(procInfo.hThread);
	GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);
	WaitForSingleObject(procInfo.hProcess, INFINITE);				// TODO: Will interrupting this with a SIGINT handler cause it to abort or to retry. Will userland get executation back. AFAIK, there isn't a EINTR error code thing here, so those two options are the only ones.
	CloseHandle(procInfo.hProcess);
	exit(EXIT_FAILURE);												// NOTE: Reaching this area is most probably do to some system thing, so exit with failure.
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
	buffer += "\" ";
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
			buffer += "\" ";
		}
	}

	std::cerr << buffer << '\n';					// TODO: Temp code, remove this later.

	procInfo = { };

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

	managePipes();

	WaitForSingleObject(procInfo.hProcess, INFINITE);				// TODO: Handle error here.
	unsigned long exitCode;
	GetExitCodeProcess(procInfo.hProcess, &exitCode);
	std::cerr << exitCode << '\n';

	CloseHandle(procInfo.hThread);
	CloseHandle(procInfo.hProcess);						// TODO: Handle errors here too.

	CloseHandle(childOutputHandle);				// TODO: See if you can cause the other thread to somehow read an EOF instead of breaking the pipe. It has more to do with the code of the other thread than the code of this thread.
	CloseHandle(childInputHandle);
	CloseHandle(childErrorHandle);

	return std::chrono::high_resolution_clock::now() - startTime;
}

char* intToString(uint64_t value) {
	char* result = new char[20];
	//_ui64toa(value, result, 10);					// TODO: This is temporary, you should make your own algorithm for doing this. These can get sort of complicated if you tune them to your hardware, but it's a good excersize, you should do it.
	if (_ui64toa_s(value, result, sizeof(result), 10) == 0) { return result; }
	return nullptr;
}

char* doubleToString(double value) {
	char* result = new char[128];
	if (sprintf_s(result, sizeof(result), "%f", value) == -1) { return nullptr; }
	return result;
}

void manageArgs(int argc, const char* const * argv) {
	unsigned int targetProgramIndex = parseFlags(argc, argv);														// Parse flags before doing anything else.
	unsigned int targetProgramArgCount = argc - targetProgramIndex;
	switch (targetProgramArgCount) {
	case 0:
		color::initErrorColoring();
		std::cout << color::red << "ERROR: too few arguments\n" << color::reset;
		color::release();
		exit(EXIT_SUCCESS);
	default:
		isErrorColored = forcedErrorColoring;																		// If everything went great with parsing the cmdline args, finally set output coloring to what the user wants it to be. It is necessary to do this here because of the garantee that we wrote above.
		CreatePipes();
		std::chrono::nanoseconds elapsedTime = runChildProcess(targetProgramArgCount, argv + targetProgramIndex);
		char* elapsedTimeString;
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
		if (elapsedTimeString) {
			std::cerr << elapsedTimeString << '\n';
			delete[] elapsedTimeString;
		}
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
}