#define BUFFERED_HANDLE_READER_BUFFER_START_SIZE 2048			// TODO: Make this number the correct one, look at grep or something, somewhere I have the right number here.
#define BUFFERED_HANDLE_READER_BUFFER_STEP_SIZE 2048			// TODO: Same for this one.
#define BUFFERED_HANDLE_READER_BUFFER_MAX_SIZE 2048				// TODO: Same for this one.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <csignal>

#include <stdlib.h>
#include <malloc.h>

#include <thread>
#include <mutex>
#include <chrono>

#include <iostream>
#include <io.h>

#include <cstring>
#include <string>

#define STDIN_FILENO 0
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

// SIDE-NOTE: It is implementation defined whether global variables that are dynamically initialized (their value isn't compile-time calculated and stored in .data, it needs to be calculated at runtime) are lazy initialized or whether they are initialized before main(). Don't rely on one of those behaviours.

std::mutex reportError_mutex;	// NOTE: I know you want to destruct this mutex explicitly because the code looks better (arguable in this case), but the mutex class literally doesn't have any sort of release function, and calling the destructor directly is a terrible idea because then it'll probably get destructed twice.

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
		std::lock_guard lock(reportError_mutex);
		_write(STDERR_FILENO, buffer, sizeof(buffer));				// TODO: Why is this line green. Intellisense mess-up. Somehow, the buffer's bounds aren't right or something, figure out why.
		color::release();
		return;
	}
	char buffer[sizeof("ERROR: ") - 1 + N - 1 + 1];
	memcpy(buffer, "ERROR: ", sizeof("ERROR: ") - 1);
	memcpy(buffer + sizeof("ERROR: ") - 1, msg, N - 1);
	buffer[sizeof("ERROR: ") - 1 + N - 1] = '\n';
	std::lock_guard lock(reportError_mutex);
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
protected:
	HANDLE handle;

	unsigned long bufferSize;

	void increaseBufferSize() {
		unsigned long newBufferSize = bufferSize + BUFFERED_HANDLE_READER_BUFFER_STEP_SIZE;				// TODO: Probably rename these defines because now we have the derived class that also indirectly uses them. Make it more general.
		if (newBufferSize > BUFFERED_HANDLE_READER_BUFFER_MAX_SIZE) { return; }
		char* temp = (char*)realloc(buffer, newBufferSize);
		if (temp) { buffer = temp; bufferSize = newBufferSize; }
	}

public:
	char* buffer;

	BufferedHandleReader(HANDLE handle) : handle(handle), bufferSize(BUFFERED_HANDLE_READER_BUFFER_START_SIZE) { buffer = new char[BUFFERED_HANDLE_READER_BUFFER_START_SIZE]; }

	unsigned long read() {
		unsigned long bytesRead;
		if (ReadFile(handle, buffer, bufferSize, &bytesRead, nullptr)) { if (bytesRead == bufferSize) { increaseBufferSize(); } return bytesRead; }
		return 0;
	}

	void release() { delete[] buffer; buffer = nullptr; }					// NOTE: Just remember, delete[] and delete can't set the pointer to nullptr because you haven't passed it by reference. It would go against the language rules.
	
	~BufferedHandleReader() { if (buffer) { delete[] buffer; } }	// SIDE-NOTE FOR FUTURE REFERENCE: Every time, you google if new and delete can throw, and you keep forgetting every time. Just remember, they can both throw. EXCEPT if you use a special form of new, which you should remember to use from now on because it's useful.
};

class BufferedStdinReader : public BufferedHandleReader {
	using BufferedHandleReader::read;			// NOTE: This makes an exception to the rule of making the base class's public members public. You can also make an exception to the rule of making base class's public members private (when in that situation), if you write the using statement in a public block.

public:
	BufferedStdinReader() : BufferedHandleReader((HANDLE)true) { }

	int read() {
		int bytesRead = _read(STDIN_FILENO, buffer, bufferSize);
		if (bytesRead == bufferSize) { increaseBufferSize(); }
		return bytesRead;												// NOTE: Will return -1 if _read failed and returned -1. This is important behaviour and isn't a mistake.
	}

	using BufferedHandleReader::release;
};

PROCESS_INFORMATION procInfo;

bool shouldRun = true;

void signalHandler(int signum) { shouldRun = false; }		// NOTE: SIGILL and SIGTERM aren't generated in windows, you can generate them yourself though and pass them around. Also, signals do cause interrupts and aren't run on separate threads, EXCEPT SIGINT. SIGINT starts new thread and makes it asynchronous. BE WARE!!

bool childInputPipeManagerThreadSuccess = false;

void manageInputPipe() {
	BufferedStdinReader inputReader;

	int bytesRead;
	unsigned long bytesWritten;					// NOTE: We don't technically even want this, but we have to include and use it for the syscall because it is required by win32 documentation.

	while (shouldRun) {
		bytesRead = inputReader.read();
		switch (bytesRead) {
		case 0: reportError("DEBUG: got EOF on parent stdin reader"); inputReader.release(); childInputPipeManagerThreadSuccess = true; return;
		case -1: reportError("failed to read from parent stdin"); childInputPipeManagerThreadSuccess = true; return;
		default:
			if (!WriteFile(parentInputWriteHandle, inputReader.buffer, bytesRead, &bytesWritten, nullptr)) {
				if (GetLastError() == ERROR_BROKEN_PIPE) { return; }
				reportError("failed to write to child stdin");
				childInputPipeManagerThreadSuccess = true;
				return;
			}
		}
	}
}

bool waitForChildInputManagerThread(std::thread& thread) { thread.join(); return childInputPipeManagerThreadSuccess; }

void managePipes() {
	bool outputClosed = false;
	bool inputClosed = false;
	bool errorClosed = false;

	unsigned long byteAmount;
	
	BufferedHandleReader outputReader(parentOutputReadHandle);
	//BufferedHandleReader inputReader									// TODO: I don't see a way to do this in a non-blocking way. This is the same problem we had at the beginning of grep development. Just start a new thread and handle it in a blocking way on there. Don't worry about SIGINT stuff, that handles well because either EOF is sent or the syscall is cancelled, I'm not quite sure which one yet, you should test.
	BufferedHandleReader errorReader(parentErrorReadHandle);				// TODO: You should move every instantiation and processing thing you can to before the CreateProcess thing. So that we get into reading the stuff as soon as possible. Use globals probably.

	//std::thread childInputPipeManagerThread(manageInputPipe);

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
						exit(EXIT_FAILURE);
						*/
						break;
					}
				}
				else { reportError("failed to read from child stdout"); break; }
			}
		}
		else if (GetLastError() == ERROR_BROKEN_PIPE) {
			if (errorClosed) {
				outputReader.release();
				//inputReader
				errorReader.release();
				//WaitForSingleObject(procInfo.hProcess, INFINITE);
				// TODO: When the child process exits and this code is reached, the input reader is still waiting for input.
				// Artificially sending interrupt doesn't work since the terminal is normally the one to send the EOF when the program gets interrupted (which isn't standard behaviour anyway AFAIK)
				// Closing your own stdin doesn't work because it'll wait until the read operation is finished, which will never finish. You're in a real pickle here, figure it out.
				//if (!waitForChildInputManagerThread(childInputPipeManagerThread)) { }			// TODO: Find a way to handle the errors here in a smooth way, as little code duplication as possible, while also not including needless inefficiencies for the sake of nice-looking code.
				closeParentPipeHandles();				// TODO: There is no reason to hold off on two of the three handles until this statement, if you break it up, it'll make more sense. Also, you need to handle errors here, since this isn't a last ditch effort.
				return;
			}
			outputClosed = true;
		}
		else { reportError("failed to poll child output pipe"); break; }

		if (PeekNamedPipe(parentErrorReadHandle, nullptr, 0, nullptr, &byteAmount, nullptr)) {
			if (byteAmount != 0) {
				byteAmount = errorReader.read();
				// TODO: This needs to be locked with the same lock as the reportError thing, or else multithreading isn't going to work right.
				if (byteAmount != 0) { if (_write(STDERR_FILENO, errorReader.buffer, byteAmount) == -1) { reportError("failed to write to parent stderr"); break; } }
				else { reportError("failed to read from child stderr"); break; }
			}
		}
		else if (GetLastError() == ERROR_BROKEN_PIPE) {
			if (outputClosed) {
				outputReader.release();
				//inputReader
				errorReader.release();
				//WaitForSingleObject(procInfo.hProcess, INFINITE);
				//if (!waitForChildInputManagerThread(childInputPipeManagerThread)) { }
				closeParentPipeHandles();
				return;
			}
			errorClosed = true;
		}
		else { reportError("failed to poll child error pipe"); break; }

		if (!shouldRun) {
			// NOTE: The child process also received the Ctrl + C, so we should be able to return without worry.
			// NOTE: If the child process hangs and doesn't want to die, so do we. This is good, because then the user can see that something is wrong. If we were to close even though the child process hangs, the user might never know there is still a hanging process in the background.
			outputReader.release();
			errorReader.release();
			closeParentPipeHandles();			// NOTE: Closing the parentInputWrite pipe handle here sends EOF to child program, which it might use to know when it got a Ctrl + C, so we send it to them to be nice (this is usually a job my terminal does, but we have to do it here).
			//if (!waitForChildInputManagerThread(childInputPipeManagerThread)) { }			// NOTE: If we happen to close the pipe while the input manager thread is writing to it, our CloseHandle syscall will just wait, so no big deal.
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

	//shouldRun = false;
	outputReader.release();
	errorReader.release();
	CloseHandle(parentOutputReadHandle);
	CloseHandle(parentErrorReadHandle);
	CloseHandle(procInfo.hThread);
	GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);				// TODO: Is this really the only way to send the signal to the child process? Will the child process even get it like this? Research.
	CloseHandle(parentInputWriteHandle);			// NOTE: Same as way above, sends EOF to child process, which it might need to register Ctrl + C.
	//childInputPipeManagerThread.join();
	WaitForSingleObject(procInfo.hProcess, INFINITE);				// TODO: Will interrupting this with a SIGINT handler cause it to abort or to retry. Will userland get executation back. AFAIK, there isn't a EINTR error code thing here, so those two options are the only ones.
	CloseHandle(procInfo.hProcess);
	exit(EXIT_FAILURE);												// NOTE: Reaching this area is most probably due to some system thing, so exit with failure.
	// NOTE: exit doesn't call the destructors of your stack objects. It calls other things (including handlers registered with atexit and the destructors of static objects), but not the destructors of local stack objects. BE WARE!!!!
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

	//std::cerr << buffer << '\n';					// TODO: Temp code, remove this later.
	_write(STDERR_FILENO, buffer.c_str(), buffer.length() + 1);			// TODO: Applies to this as well.

	procInfo = { };

	STARTUPINFOA startupInfo = { };
	startupInfo.cb = sizeof(STARTUPINFOA);
	//startupInfo.hStdOutput = childOutputHandle;
	//startupInfo.hStdInput = childInputHandle;				// TODO: Make sure that the program name thing can't be put in the first argument of the below function. Maybe I was just doing it wrong before? Does it still discover the program in the same way?
	//startupInfo.hStdError = childErrorHandle;
	//startupInfo.dwFlags = STARTF_USESTDHANDLES;

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
		exit(EXIT_FAILURE);
	}


	return std::chrono::high_resolution_clock::now() - startTime;
}

#define ELAPSED_TIME_STRING_SIZE 128

// NOTE: type (&name)[size] is the syntax for an array reference, which is super useful in some cases. Why? Because it enforces the size of the array at compile-time. So you can't pass anything into the function except an array of the correct size. Compiler has no way of checking this properly if you use pointers, which means those aren't allowed either, which is also useful in some cases.
void intToString(char (&output)[ELAPSED_TIME_STRING_SIZE], uint64_t value) {
	//_ui64toa(value, result, 10);					// TODO: This is temporary, you should make your own algorithm for doing this. These can get sort of complicated if you tune them to your hardware, but it's a good excersize, you should do it.
	if (_ui64toa_s(value, output, 20, 10) != 0) {					// NOTE: An unsigned int64 can be 20 digits long at maximum, which is why we only write to the first 20 bytes of the output array. The output array is larger because it needs to work with doubleToString as well.
		reportError("failed to convert elapsed time integer to string");
		exit(EXIT_FAILURE);				// TODO: The number conversion function above might actually fail if it doesn't have enough space to write NUL character, which it doesn't right now if we use the biggest possible numbers. Definitely check that and debug it if it is true.
	}									// TODO: Also, the fact that there might not be a NUL character means trouble for the later code that measures the string, since it doesn't know when to stop. You need to fix all this some how.
}

void doubleToString(char (&output)[ELAPSED_TIME_STRING_SIZE], double value) {
	if (sprintf_s(output, ELAPSED_TIME_STRING_SIZE, "%f", value) == -1) {
		reportError("failed to convert elapsed time double to string");
		exit(EXIT_FAILURE);
	}
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
		char elapsedTimeString[ELAPSED_TIME_STRING_SIZE];
		switch (flags::timeUnit) {
		case 0:
			if (flags::timeAccuracy) { intToString(elapsedTimeString, std::chrono::duration_cast<std::chrono::duration<uint64_t, std::nano>>(elapsedTime).count()); break; }
			doubleToString(elapsedTimeString, std::chrono::duration_cast<std::chrono::duration<double, std::nano>>(elapsedTime).count()); break;
		case 1:
			if (flags::timeAccuracy) { intToString(elapsedTimeString, std::chrono::duration_cast<std::chrono::duration<uint64_t, std::micro>>(elapsedTime).count()); break; }
			doubleToString(elapsedTimeString, std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(elapsedTime).count()); break;
		case 2:
			if (flags::timeAccuracy) { intToString(elapsedTimeString, std::chrono::duration_cast<std::chrono::duration<uint64_t, std::milli>>(elapsedTime).count()); break; }
			doubleToString(elapsedTimeString, std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(elapsedTime).count()); break;
		case 3:
			if (flags::timeAccuracy) { intToString(elapsedTimeString, std::chrono::duration_cast<std::chrono::duration<uint64_t>>(elapsedTime).count()); break; }
			doubleToString(elapsedTimeString, std::chrono::duration_cast<std::chrono::duration<double>>(elapsedTime).count()); break;
		case 4:
			if (flags::timeAccuracy) { intToString(elapsedTimeString, std::chrono::duration_cast<std::chrono::duration<uint64_t, std::ratio<60, 1>>>(elapsedTime).count()); break; }
			doubleToString(elapsedTimeString, std::chrono::duration_cast<std::chrono::duration<double, std::ratio<60, 1>>>(elapsedTime).count()); break;
		case 5:
			if (flags::timeAccuracy) { intToString(elapsedTimeString, std::chrono::duration_cast<std::chrono::duration<uint64_t, std::ratio<3600, 1>>>(elapsedTime).count()); break; }
			doubleToString(elapsedTimeString, std::chrono::duration_cast<std::chrono::duration<double, std::ratio<3600, 1>>>(elapsedTime).count()); break;
		}
		if (_write(STDERR_FILENO, elapsedTimeString, strlen(elapsedTimeString)) == -1) {			// TODO: Is this the best way to do this? The measuring might be unnecessary if there is some syscall that measures for you.
			reportError("failed to write elapsed time to parent stderr");
			exit(EXIT_FAILURE);
		}
	}
}

int main(int argc, char* const * argv) {
	if (signal(SIGINT, signalHandler) == SIG_ERR) { reportError("failed to set up SIGINT signal handling"); return 1; }
	if (signal(SIGBREAK, signalHandler) == SIG_ERR) { reportError("failed to set up SIGBREAK signal handling"); return 1; }
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