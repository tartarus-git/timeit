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

#include <cstring>

//#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// ANSI escape code helpers.
#define ANSI_ESC_CODE_PREFIX "\033["
#define ANSI_ESC_CODE_SUFFIX "m"
#define ANSI_ESC_CODE_MIN_SIZE ((sizeof(ANSI_ESC_CODE_PREFIX) - 1) + (sizeof(ANSI_ESC_CODE_SUFFIX) - 1))

#define ANSI_RED_CODE_LENGTH ANSI_ESC_CODE_MIN_SIZE + 2
#define ANSI_RESET_CODE_LENGTH ANSI_ESC_CODE_MIN_SIZE + 1

// NOTE: The difference between const char[] and const char* is that const char* is stored in .rodata and const char[] is stored in .data. That means you can edit this help text at runtime even though it's const.
const char helpText[] = "timeit runs the specified program with the specified arguments and prints the elapsed time until program completion to stderr. The stdin and stdout of timeit are both redirected to the stdin and stdout of the specified program, allowing " \
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
	void initRed() { red = new char[ANSI_RED_CODE_LENGTH]; memcpy(red, ANSI_ESC_CODE_PREFIX "31" ANSI_ESC_CODE_SUFFIX, ANSI_RED_CODE_LENGTH); }
	//void initPipedRed() { red = new char; *red = '\0'; }

	char* reset;
	void initReset() { reset = new char[ANSI_RESET_CODE_LENGTH]; memcpy(reset, ANSI_ESC_CODE_PREFIX "0" ANSI_ESC_CODE_SUFFIX, ANSI_RESET_CODE_LENGTH); }
	//void initPipedReset() { reset = new char; *reset = '\0'; }

	void initErrorColoring() { initRed(); initReset(); return; }

	void release() { delete[] color::red; delete[] color::reset; }
}

namespace flags {
	bool expandArgs = false;
	uint8_t timeUnit = 3;
	bool timeAccuracy = false;
}

template <size_t N>
void reportError(const char (&msg)[N]) {
	if (isErrorColored) {
		color::initErrorColoring();
		char buffer[ANSI_RED_CODE_LENGTH + sizeof("ERROR: ") - 1 + N - 1 + 1 + ANSI_RESET_CODE_LENGTH];							// NOTE: This code block is to create our own specific buffering for these substrings, to avoid syscalls.
		memcpy(buffer, color::red, ANSI_RED_CODE_LENGTH);
		memcpy(buffer + ANSI_RED_CODE_LENGTH, "ERROR: ", sizeof("ERROR: ") - 1);
		memcpy(buffer + ANSI_RED_CODE_LENGTH + sizeof("ERROR: ") - 1, msg, N - 1);
		buffer[ANSI_RED_CODE_LENGTH + sizeof("ERROR: ") - 1 + N - 1] = '\n';
		memcpy(buffer + ANSI_RED_CODE_LENGTH + sizeof("ERROR: ") - 1 + N - 1 + 1, color::reset, ANSI_RESET_CODE_LENGTH);
		_write(STDERR_FILENO, buffer, sizeof(buffer));				// TODO: Why is this line green. Intellisense mess-up. Somehow, the buffer's bounds aren't right or something, figure out why.
		color::release();
		return;
	}
		char buffer[sizeof("ERROR: ") - 1 + N - 1 + 1];
		memcpy(buffer, "ERROR: ", sizeof("ERROR: ") - 1);
		memcpy(buffer + sizeof("ERROR: ") - 1, msg, N - 1);
		buffer[sizeof("ERROR: ") - 1 + N - 1] = '\n';
		_write(STDERR_FILENO, buffer, sizeof(buffer));				// TODO: Why is this line green. Intellisense mess-up. Somehow, the buffer's bounds aren't right or something, figure out why.
}

// NOTE: I have previously only shown help when output is connected to TTY, so as not to pollute stdout when piping. Back then, help was shown sometimes when it wasn't requested, which made it prudent to include that feature. Now, you have to explicitly ask for help, making TTY branching unnecessary.
void showHelp() {
	if (_write(STDOUT_FILENO, helpText, sizeof(helpText) - 1) != -1) {			// NOTE: It's ok to use unbuffered IO here because we always exit after writing this, no point in buffering.
		reportError("failed to write help text to stdout");
		exit(EXIT_FAILURE);
	}
}

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
					if (i == argc) { reportError("the --error-color flag was not supplied with a value"); exit(EXIT_SUCCESS); }

					if (!strcmp(argv[i], "on")) { forcedErrorColoring = true; continue; }
					if (!strcmp(argv[i], "off")) { forcedErrorColoring = false; continue; }
					if (!strcmp(argv[i], "auto")) { forcedErrorColoring = isErrorColored; continue; }
					reportError("invalid value for --error-color flag"); exit(EXIT_SUCCESS);
				}

				if (!strcmp(flagTextStart, "unit")) {
					i++;
					if (i == argc) { reportError("the --unit flag was not supplied with a value"); exit(EXIT_SUCCESS); }

					if (!strcmp(argv[i], "nanoseconds")) { flags::timeUnit = 0; continue; }
					if (!strcmp(argv[i], "microseconds")) { flags::timeUnit = 1; continue; }
					if (!strcmp(argv[i], "milliseconds")) { flags::timeUnit = 2; continue; }
					if (!strcmp(argv[i], "seconds")) { flags::timeUnit = 3; continue; }
					if (!strcmp(argv[i], "minutes")) { flags::timeUnit = 4; continue; }
					if (!strcmp(argv[i], "hours")) { flags::timeUnit = 5; continue; }
					reportError("invalid value for --unit flag"); exit(EXIT_SUCCESS);
				}

				if (!strcmp(flagTextStart, "accuracy")) {
					i++;
					if (i == argc) { reportError("the --accuracy flag was not supplied with a value"); exit(EXIT_SUCCESS); }

					if (!strcmp(argv[i], "double")) { flags::timeAccuracy = false; continue; }				// NOTE: This might seem unnecessary, but we need it in case the --accuracy flag is used twice and has already changed the value.
					if (!strcmp(argv[i], "int")) { flags::timeAccuracy = true; continue; }
					reportError("invalid value for --accuracy flag"); exit(EXIT_SUCCESS);
				}

				if (!strcmp(flagTextStart, "help")) { showHelp(); exit(EXIT_SUCCESS); }
				if (!strcmp(flagTextStart, "h")) { showHelp(); exit(EXIT_SUCCESS); }

				//color::initErrorColoring();								// NOTE: Not necessary because of fall-through.
				//std::cerr << color::red << "ERROR: one or more flag arguments are invalid\n" << color::reset;
				//color::release();
				//exit(EXIT_SUCCESS);
			}
			reportError("one or more flag arguments are invalid"); exit(EXIT_SUCCESS);
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

void closeParentPipeHandles() {							// NOTE: No need to return whether or not we were successful because when we call this function, we don't actually ever care if we were successful or not.
	CloseHandle(parentOutputReadHandle);
	CloseHandle(parentInputWriteHandle);
	CloseHandle(parentErrorReadHandle);
}

void CreatePipes() {
	if (!CreatePipe(&parentOutputReadHandle, &childOutputHandle, nullptr, 0)) {
		reportError("failed to create pipe from child stdout to parent"); exit(EXIT_FAILURE);						// NOTE: We exit with EXIT_FAILURE here because this is presumably a system error and it shouldn't really be the users fault AFAIK.
	}
	if (!SetHandleInformation(childOutputHandle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
		reportError("failed to set child stdout handle to inheritable");
		CloseHandle(childOutputHandle);
		CloseHandle(parentOutputReadHandle);
		exit(EXIT_FAILURE);
	}

	if (!CreatePipe(&childInputHandle, &parentInputWriteHandle, nullptr, 0)) {
		reportError("failed to create pipe from parent to child stdin");
		CloseHandle(childOutputHandle);
		CloseHandle(parentOutputReadHandle);
		exit(EXIT_FAILURE);
	}
	if (!SetHandleInformation(childInputHandle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
		reportError("failed to set child stdin handle to inheritable");
		CloseHandle(childOutputHandle);
		CloseHandle(parentOutputReadHandle);
		CloseHandle(childInputHandle);
		CloseHandle(parentInputWriteHandle);
		exit(EXIT_FAILURE);
	}

	if (!CreatePipe(&parentErrorReadHandle, &childErrorHandle, nullptr, 0)) {
		reportError("failed to create pipe from child stderr to parent");
		CloseHandle(childOutputHandle);
		CloseHandle(parentOutputReadHandle);
		CloseHandle(childInputHandle);
		CloseHandle(parentInputWriteHandle);
		exit(EXIT_FAILURE);
	}
	if (!SetHandleInformation(childErrorHandle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
		reportError("failed to set child stderr handle to inheritable");
		CloseHandle(childOutputHandle);
		CloseHandle(childInputHandle);
		CloseHandle(childErrorHandle);
		closeParentPipeHandles();
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

	unsigned long byteAmount;
	
	BufferedHandleReader outputReader(parentOutputReadHandle);
	//BufferedHandleReader inputReader									// TODO: I don't see a way to do this in a non-blocking way. This is the same problem we had at the beginning of grep development. Just start a new thread and handle it in a blocking way on there. Don't worry about SIGINT stuff, that handles well because either EOF is sent or the syscall is cancelled, I'm not quite sure which one yet, you should test.
	BufferedHandleReader errorReader(parentErrorReadHandle);

	while (true) {
		if (PeekNamedPipe(parentOutputReadHandle, nullptr, 0, nullptr, &byteAmount, nullptr)) {
			if (byteAmount != 0) {
				byteAmount = outputReader.read();
				if (byteAmount != 0) {
					if (_write(STDOUT_FILENO, outputReader.buffer, byteAmount) == -1) {
						reportError("failed to write to parent stdout");
						/*outputReader.release();
						//inputReader
						errorReader.release();					// TODO: Make sure you release everything that you need to everywhere before you call exit.
						releasePipes();
						exit(EXIT_FAILURE);						// NOTE: exit doesn't call the destructors of your stack objects. It calls other things (including handlers registered with atexit and the destructors of static objects), but not the destructors of local stack objects. BE WARE!!!!
						*/
						break;
					}
				}
				else { reportError("failed to read from child stdout"); break; }
			}
		}
		else if (GetLastError() == ERROR_BROKEN_PIPE) { outputClosed = true; }
		else { reportError("failed to poll child output pipe"); break; }

		if (outputClosed && errorClosed) {
			outputReader.release();
			//inputReader
			errorReader.release();
			closeParentPipeHandles();
			return;
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
	closeParentPipeHandles();
	CloseHandle(procInfo.hThread);
	GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);
	WaitForSingleObject(procInfo.hProcess, INFINITE);				// TODO: Will interrupting this with a SIGINT handler cause it to abort or to retry. Will userland get executation back. AFAIK, there isn't a EINTR error code thing here, so those two options are the only ones.
	CloseHandle(procInfo.hProcess);
	exit(EXIT_FAILURE);												// NOTE: Reaching this area is most probably do to some system thing, so exit with failure.
}

std::chrono::nanoseconds runChildProcess(int argc, const char* const * argv) {
	if (argv[0][0] == '\0') {				// NOTE: I thought this was impossible, but apparently it isn't, so we have to check for it and report an error if it occurs.
											// NOTE: The reason we don't just let CreateProcessA detect this error is because it will probably just filter out the nothingness and use the first argument as the program name, which is very terrible.
		reportError("target program name cannot be empty"); exit(EXIT_SUCCESS);
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
		for (int i = 1; i < argc; i++) {
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
		switch (GetLastError()) {
			// TODO: Find the errors that are necessary here and conditionally throw different errors.
		}

		// TODO: Remove the following after you've done the above.
		std::cerr << "had some trouble starting child process\n";
		exit(EXIT_FAILURE);
	}

	if (!CloseHandle(childOutputHandle)) {
		reportError("failed to close child output pipe handle");
		CloseHandle(childInputHandle);
		CloseHandle(childErrorHandle);
		closeParentPipeHandles();
		CloseHandle(procInfo.hThread);
		CloseHandle(procInfo.hProcess);
		exit(EXIT_FAILURE);
	}
	if (!CloseHandle(childInputHandle)) {
		reportError("failed to close child input pipe handle");
		CloseHandle(childErrorHandle);
		closeParentPipeHandles();
		CloseHandle(procInfo.hThread);
		CloseHandle(procInfo.hProcess);
		exit(EXIT_FAILURE);
	}
	if (!CloseHandle(childErrorHandle)) {
		reportError("failed to close child error pipe handle");
		closeParentPipeHandles();
		CloseHandle(procInfo.hThread);
		CloseHandle(procInfo.hProcess);
		exit(EXIT_FAILURE);
	}

	managePipes();

	WaitForSingleObject(procInfo.hProcess, INFINITE);				// TODO: Handle error here.

	if (!CloseHandle(procInfo.hThread)) {
		reportError("failed to close child process main thread handle");
		CloseHandle(procInfo.hProcess);
		exit(EXIT_FAILURE);
	}
	if (!CloseHandle(procInfo.hProcess)) {
		reportError("failed to close child process handle");
		CloseHandle(procInfo.hProcess);
		exit(EXIT_FAILURE);
	}


	return std::chrono::high_resolution_clock::now() - startTime;
}

char* intToString(uint64_t value) {
	char* result = new char[20];			// TODO: You should probably pass these pointers in as variables so the calling code can have them on the stack, putting them on heap is pretty pointless.
	//_ui64toa(value, result, 10);					// TODO: This is temporary, you should make your own algorithm for doing this. These can get sort of complicated if you tune them to your hardware, but it's a good excersize, you should do it.
	if (_ui64toa_s(value, result, 20, 10) == 0) { return result; }
	return nullptr;
}

char* doubleToString(double value) {
	char* result = new char[128];
	if (sprintf_s(result, 128, "%f", value) == -1) { return nullptr; }
	return result;
}

void manageArgs(int argc, const char* const * argv) {
	unsigned int targetProgramIndex = parseFlags(argc, argv);														// Parse flags before doing anything else.
	unsigned int targetProgramArgCount = argc - targetProgramIndex;
	switch (targetProgramArgCount) {
	case 0:
		reportError("too few arguments"); exit(EXIT_SUCCESS);
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
			if (_write(STDERR_FILENO, elapsedTimeString, strlen(elapsedTimeString)) == -1) {			// TODO: Is this the best way to do this? The measuring might be unnecessary if there is some syscall that measures for you.
				reportError("failed to write elapsed time to parent stderr");
				delete[] elapsedTimeString;
				exit(EXIT_FAILURE);
			}
			delete[] elapsedTimeString;
		}
	}
}

int main(int argc, char* const * argv) {
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

	return 0;
}