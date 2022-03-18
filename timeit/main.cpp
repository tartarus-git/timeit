#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <csignal>

#include <cstdlib>

#include <thread>
#include <chrono>

#include <io.h>

#include <cstring>
#include <string>

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

// Output coloring.
namespace color {
	char* red;
	void initRed() { red = new char[ANSI_RED_CODE_LENGTH]; memcpy(red, ANSI_ESC_CODE_PREFIX "31" ANSI_ESC_CODE_SUFFIX, ANSI_RED_CODE_LENGTH); }

	char* reset;
	void initReset() { reset = new char[ANSI_RESET_CODE_LENGTH]; memcpy(reset, ANSI_ESC_CODE_PREFIX "0" ANSI_ESC_CODE_SUFFIX, ANSI_RESET_CODE_LENGTH); }

	void initErrorColoring() { initRed(); initReset(); return; }

	void release() { delete[] color::red; delete[] color::reset; }
}

namespace flags {
	bool expandArgs = false;
	uint8_t timeUnit = 3;
	bool timeAccuracy = false;
}

// SIDE-NOTE: It is implementation defined whether global variables that are dynamically initialized (their value isn't compile-time calculated and stored in .data, it needs to be calculated at runtime) are lazy initialized or whether they are initialized before main(). Don't rely on one of those behaviours.

// NOTE: I think the below note is useful, so I'm going to keep the whole line in even though it currently has nothing to do with the codebase anymore.
//std::mutex reportError_mutex;	// NOTE: I know you want to destruct this mutex explicitly because the code looks better (arguable in this case), but the mutex class literally doesn't have any sort of release function, and calling the destructor directly is a terrible idea because then it'll probably get destructed twice.

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
		_write(STDERR_FILENO, buffer, sizeof(buffer));
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

				if (!strcmp(flagTextStart, "expand-args")) { flags::expandArgs = true; continue; }

				if (!strcmp(flagTextStart, "error-color")) {
					i++;
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

void signalHandler(int signum) { }						// NOTE: This is supposed to be empty. At no point in our program's execution do we ever need to react to a signal being sent, so the only function of this line is to stop specific signals from stopping our program and instead make them give us time.

std::chrono::nanoseconds runChildProcess(int argc, const char* const * argv) {
	if (argv[0][0] == '\0') {				// NOTE: I thought this was impossible, but apparently it isn't, so we have to check for it and report an error if it occurs.
											// NOTE: The reason we don't just let CreateProcessA detect this error is because it will probably just filter out the nothingness and use the first argument as the program name, which is very terrible.
											// NOTE: We don't care about the other arguments being zero because those get passed onto the child process anyway. It'll be the one to deal with any discrepencies in that regard.
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

	PROCESS_INFORMATION procInfo;				// NOTE: No need to set the fields to zero, since this is only for getting data out from CreateProcessA. The OS fills these, we don't need to zero them out.

	STARTUPINFOA startupInfo = { };
	startupInfo.cb = sizeof(STARTUPINFOA);

	std::chrono::high_resolution_clock::time_point startTime = std::chrono::high_resolution_clock::now();

	char* innerBuffer = buffer.data();						// NOTE: We have to use data() here instead of c_str() because it's undefined behaviour otherwise. The deal with data() is that you're allowed to change everything except the NUL termination character at the end of c-style string that is returned.
	// NOTE: The way it is now, CreateProcessA can change most of the contents, and if it changes the pointer to point to something else, we don't care because the pointer is just a copy. This means that we are as safe as we can be. The only danger comes from the possible modification of the NUL character, which CreateProcessA probably won't do because why should it.
	// NOTE: I should also point out that the docs say that CreateProcessW is the one that changes the string, not CreateProcessA, so we should be safe. The only reason I'm taking these precautions is because the parameter isn't marked with const and I don't totally trust the docs.

	if (!CreateProcessA(nullptr, innerBuffer, nullptr, nullptr, false, 0, nullptr, nullptr, &startupInfo, &procInfo)) {
		reportError("couldn't start target program");				// NOTE: Sadly, the win32 docs don't really specify the possible error codes for CreateProcessA, so I don't know which cases to check for here. This generic error message will have to do.
		exit(EXIT_SUCCESS);											// NOTE: The majority of errors in this spot are probably going to be caused by misspellings of the target program by the user, so we use EXIT_SUCCESS here.
	}

	if (!CloseHandle(procInfo.hThread)) {
		reportError("failed to close child process main thread handle");
		CloseHandle(procInfo.hProcess);
		exit(EXIT_FAILURE);
	}

	if (WaitForSingleObject(procInfo.hProcess, INFINITE) == WAIT_FAILED) {
		reportError("failed to wait for child process to finish execution");
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
	// TODO: You should make your own algorithm for doing this. These can get sort of complicated if you tune them to your hardware, but it's a good excersize, you should do it.
	if (_ui64toa_s(value, output, ELAPSED_TIME_STRING_SIZE, 10) != 0) {					// NOTE: An unsigned int64 can be 20 digits long at maximum, which is why we technically only have to write to the first 21 bytes of the output array (cuz NUL character). The output array is larger because it needs to work with doubleToString as well. We use ELAPSED_TIME_STRING here even though we don't need the full size just in case, and because any slow-down that it might cause isn't noticable and doesn't effect the functionality of the program in any way whatsoever.
		reportError("failed to convert elapsed time integer to string");
		exit(EXIT_FAILURE);
	}
}

void doubleToString(char (&output)[ELAPSED_TIME_STRING_SIZE], double value) {
	if (sprintf_s(output, ELAPSED_TIME_STRING_SIZE, "%f", value) == -1) {				// TODO: What is the minimum size required for the buffer for this to never overflow ever?
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
		if (_write(STDERR_FILENO, elapsedTimeString, strlen(elapsedTimeString)) == -1) {					// NOTE: AFAIK, there is no syscall available that takes in a null-terminated string for writing. Thus, we are forced to measure the string and use the standard _write syscall.
			reportError("failed to write elapsed time to parent stderr");
			exit(EXIT_FAILURE);
		}
	}
}

int main(int argc, char* const * argv) {
	signal(SIGINT, signalHandler);																							// These two lines make it so that the program ignores both SIGINT and SIGBREAK signals as best as possible. SIGBREAK can only be ignored for 5 seconds, but that's actually good.
	signal(SIGBREAK, signalHandler);

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