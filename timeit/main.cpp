#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <csignal>													// Needed for signal handling.

#include <cstdlib>													// Needed for _ui64toa_s (safe conversion function from unsigned int to ascii string) and exit function.

#include <chrono>													// Needed for timing the child process.

#include <cstdio>													// Needed for sprintf_s function.
#include <io.h>														// Needed for _isatty function and _write function.

#include <cstring>													// Needed for strcmp function and strlen function and memcpy function.
#include <string>													// Needed for the one single spot in which we use std::string.

//#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define static_strlen(x) (sizeof(x) - 1)

// ANSI escape code helpers.
#define ANSI_ESC_CODE_PREFIX "\033["
#define ANSI_ESC_CODE_SUFFIX "m"

// NOTE: const char[] and const char* are both stored in .rodata (at least usually, C++ standard leaves it open to the implementor). That means this help text is in .rodata (usually).
// NOTE: You can store strings in .data, which means they will be changeable from runtime. To do that, use char[].
// SIDE-NOTE: One would think that you could use char* as well as char[] for .data strings. That is false. char* will still point to .rodata memory, making changing the string that resides there UB.
// REASON: C did it this way, so C++ did it this way as well.
// BUT: That has been changed, I don't know when, but it's gone now. You have to use const char* for string literals now in C++. You can't use char*, which makes this whole thing much easier to understand.
const char helpText[] = "timeit runs the specified program with the specified arguments and prints the elapsed time until program completion to stderr.\n" \
							"Standard input/output/error all flow through the encapsulating timeit process to and from the target program, allowing \n" \
							"the construct to be used seamlessly within piped chains of programs.\n" \
							"\n" \
							"usage: timeit [--expand-args || --error-color <auto|on|off> ||\n" \
							"           --unit <nanoseconds | microseconds | milliseconds | seconds | minutes | hours> || --accuracy <double | int>]\n" \
							"           <program> [arguments]...\n" \
							"       timeit <--help || --h>            -->            shows help text\n" \
							"\n" \
							"arguments:\n" \
								"\t--expand-args                 -->                  expand elements of [arguments]... that contain spaces into\n" \
								"\t                                                   multiple arguments (default behaviour is to leave them as single arguments)\n" \
								"\t--error-color <auto|on|off>   -->                  force a specific coloring behaviour for error messages \n" \
								"\t                                                   (which are always printed to stderr) (default behaviour is auto)\n" \
								"\t--unit <(see above)>          -->                  the unit to output the elapsed time in (default is seconds)\n" \
								"\t--accuracy <double|int>       -->                  specify whether elapsed time is outputted as a decimal (double)\n" \
								"\t                                                   or as a round number (int) (default is double)\n" \
								"\t<program>                     -->                  the program which is to be timed\n" \
								"\t[arguments]...                -->                  the arguments to pass to the target program\n" \
							"\n" \
							"NOTE: It is possible to specify a flag more than once. If this is the case, only the last occurrence will influence program behaviour.\n";

// Flag to keep track of whether we should color errors or not.
bool isErrorColored;

// Text coloring, this is only used for error messages in the case of this program.
namespace color {
	const char red[] = ANSI_ESC_CODE_PREFIX "31" ANSI_ESC_CODE_SUFFIX;
	const char reset[] = ANSI_ESC_CODE_PREFIX "0" ANSI_ESC_CODE_SUFFIX;
}

// Some of the command-line flags that one can set. The rest don't require global variables, so they're not in here.
namespace flags {
	bool expandArgs = false;
	uint8_t timeUnit = 3;
	bool timeAccuracy = false;
}

// SIDE-NOTE: It is implementation defined whether global variables that are dynamically initialized (their value isn't compile-time calculated and stored in .data, it needs to be calculated at runtime) are lazy initialized or whether they are initialized before main(). Don't rely on one of those behaviours.

// NOTE: I think the below note is useful, so I'm going to keep the whole line in even though it currently has nothing to do with the codebase anymore.
//std::mutex reportError_mutex;	// NOTE: I know you want to destruct this mutex explicitly because the code looks better (arguable in this case), but the mutex class literally doesn't have any sort of release function, and calling the destructor directly is a terrible idea because then it'll probably get destructed twice.

// This function makes it easy to report errors. It handles the coloring for you, as well as the formatting of the error string.
template <size_t N>
void reportError(const char (&msg)[N]) {		// NOTE: Technically, it would be more efficient to store completed, colored and uncolored versions of the full error texts as const chars, and that is totally possible with cool preprocessor and template tricks, but it isn't useful or necessary. It's actually harmful honestly.
	if (isErrorColored) {						// NOTE: Having the error strings split up into coloring, ERROR: tag, and message like this, allows the final ELF to store less .rodata in total. The opposite choice, making the error messages faster, is useless, who cares if the error messages are a little tiny bit faster at expense of memory.
		// Construct appropriately sized buffer.
		char buffer[static_strlen(color::red) + static_strlen("ERROR: ") + N - 1 + static_strlen(color::reset) + 1];

		// Copy all the necessary data to the buffer. This would be easier and more efficient if C++ let us do >>>> color::red "ERROR: " <<<< like it lets us do for string literals.
		std::memcpy(buffer, color::red, static_strlen(color::red));
		std::memcpy(buffer + static_strlen(color::red), "ERROR: ", static_strlen("ERROR: "));											// NOTE: We could use the C-style version here (without sdt::), but this is the "correct" way.
		std::memcpy(buffer + static_strlen(color::red) + static_strlen("ERROR: "), msg, N - 1);
		std::memcpy(buffer + static_strlen(color::red) + static_strlen("ERROR: ") + N - 1, color::reset, static_strlen(color::reset));

		// Add a newline to the end.
		buffer[static_strlen(color::red) + static_strlen("ERROR: ") + N - 1 + static_strlen(color::reset)] = '\n';

		// Output the buffer to sterr.
		_write(STDERR_FILENO, buffer, sizeof(buffer));
		return;
	}

	// The uncolored version of the above code.
	char buffer[static_strlen("ERROR: ") + N - 1 + 1];

	std::memcpy(buffer, "ERROR: ", static_strlen("ERROR: "));
	std::memcpy(buffer + static_strlen("ERROR: "), msg, N - 1);

	buffer[static_strlen("ERROR: ") + N - 1] = '\n';

	_write(STDERR_FILENO, buffer, sizeof(buffer));										// NOTE: Intellisense is complaining about this line, but PVS-Studio can't find anything. I can't see anything wrong either, so I'm leaving it like this.
}

// NOTE: I have previously only shown help when output is connected to TTY, so as not to pollute stdout when piping. Back then, help was shown sometimes when it wasn't requested, which made it prudent to include that feature. Now, you have to explicitly ask for help, making TTY branching unnecessary.
void showHelp() {
	if (_write(STDOUT_FILENO, helpText, static_strlen(helpText)) == -1) {				// NOTE: It's ok to use unbuffered IO here because we always exit after writing this, no point in buffering.
		reportError("failed to write help text to stdout");
		exit(EXIT_FAILURE);
	}
}

// This variable keeps track of the error coloring that the user requested from the command-line.
bool forcedErrorColoring;					// NOTE: GARANTEE: If something goes wrong while parsing the cmdline args and an error message is necessary, the error message will always be printed with the default coloring (based on TTY/piped mode).

// Parse flag arguments. Only handles word flags (those with -- in front), since we don't have any single letter flags (those with - in front).
unsigned int parseFlags(int argc, const char* const * argv) {																// NOTE: If you write --error-color, --unit, etc... twice, the value supplied to the rightmost instance will be the value that is used. Does not throw an error.
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

					if (!strcmp(argv[i], "double")) { flags::timeAccuracy = false; continue; }								// NOTE: This might seem unnecessary, but (as stated above already) we need it in case the --accuracy flag is used twice and has already changed the value.
					if (!strcmp(argv[i], "int")) { flags::timeAccuracy = true; continue; }
					reportError("invalid value for --accuracy flag"); exit(EXIT_SUCCESS);
				}

				if (!strcmp(flagTextStart, "help")) { showHelp(); exit(EXIT_SUCCESS); }
				if (!strcmp(flagTextStart, "h")) { showHelp(); exit(EXIT_SUCCESS); }

				// NOTE: Usually, I would report an error here, but since there are no valid flags with single "-", and since the following error message is very applicable to this case as well, we can just fall through to the following error handling code and everything is a-okay.
			}
			reportError("one or more flag arguments are invalid"); exit(EXIT_SUCCESS);
		}
		return i;																									// Return index of first arg that isn't flag arg. Helps calling code parse args.
	}
	return argc;																									// No non-flag argument was found. Return argc because it works nicely with calling code.
}

void signalHandler(int signum) { }						// NOTE: This is supposed to be empty. At no point in our program's execution do we ever need to react to a signal being sent, so the only function of this line is to stop specific signals from stopping our program and instead make them give us time.

// This function is responsible for actually running the program which is to be timed. The program is run as a child process of this program. It is given our stdout, stdin and stderr handles and it is able to freely interact with the console and the user, or with pipes, should some be set up.
std::chrono::nanoseconds runChildProcess(int argc, const char* const * argv) {
	// NOTE: I thought this was impossible, but apparently it isn't, so we have to check for it and report an error if it occurs.
	// NOTE: The reason we don't just let CreateProcessA detect this error is because it will probably just filter out the nothingness and use the first argument as the program name, which is very terrible.
	// NOTE: We don't care about the other arguments being zero because those get passed onto the child process anyway. It'll be the one to deal with any discrepencies in that regard.
	if (argv[0][0] == '\0') { reportError("target program name cannot be empty"); exit(EXIT_SUCCESS); }

	// Fill a buffer with the required command-line to invoke the target program with all the specified arguments.
	std::string buffer;
	buffer += '\"';	// IMPORTANT NOTE: These are super necessary because you wouldn't otherwise be able to run programs that have names with spaces in them. Even worse, you could accidentally run a completely different program than the one you wanted, which is terrible behaviour. That is why we add the quotes, to prevent against those two things.
	buffer += argv[0];
	buffer += "\" ";
	if (flags::expandArgs) {					// NOTE: The difference in behviour here is achieved by adding quotes around the arguments. When quotes are present, arguments with spaces are still considered as one argument. Without quotes, the given arguments will be split up into many arguments when CreateProcessA creates the child process.
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

	PROCESS_INFORMATION procInfo;							// NOTE: No need to set the fields to zero, since this is only for getting data out from CreateProcessA. The OS fills these, we don't need to zero them out.

	STARTUPINFOA startupInfo = { };							// We could fill this struct with information about how exactly the target program is to be started, which permissions it has, etc..., but all that is unnecessary for this application, so we just zero everything out and set the mandatory cb field.
	startupInfo.cb = sizeof(STARTUPINFOA);

	std::chrono::high_resolution_clock::time_point startTime = std::chrono::high_resolution_clock::now();					// Record the current point in time. This will later be used to create a difference, which will then be used to calculate the execution-time of the target program.

	// Finally actually create the child process.
	// IMPORTANT NOTE: We have to use buffer.data() instead of buffer.c_str() because data() returns a modifiable c-string, which is necessary for CreateProcessA. The deal is that you're allowed to modify everything except the trailing NUL character, doing so would be UB.
	// CreateProcessA almost definitely won't change the NUL character because why should it, and it won't change the c-string pointer because it just accepts a copy, meaning it literally can't. So the only thing it can really change is the meat of the c-string, which we don't care about.
	// Also, I think (if I'm reading them right) the docs said that CreateProcessW is the one that changes the string, not CreateProcessA, so we should be safe anyway. Strange that the argument isn't marked with const in CreateProcessA then.
	if (!CreateProcessA(nullptr, buffer.data(), nullptr, nullptr, false, 0, nullptr, nullptr, &startupInfo, &procInfo)) {
		reportError("couldn't start target program");				// NOTE: Sadly, the win32 docs don't really specify the possible error codes for CreateProcessA, so I don't know which cases to check for here. This generic error message will have to do.
		exit(EXIT_SUCCESS);											// NOTE: The majority of errors in this spot are probably going to be caused by misspellings of the target program by the user, so we use EXIT_SUCCESS here.
	}

	// Close the hThread handle as soon as possible since we never actually use it for anything. NOTE: Don't worry about something depending on it. All handles in Windows are ref-counted AFAIK, so as long as the child process is active and is using this handle, which it almost definitely is, it won't actually be closed until our child exits.
	if (!CloseHandle(procInfo.hThread)) {
		reportError("failed to close child process main thread handle");
		CloseHandle(procInfo.hProcess);
		exit(EXIT_FAILURE);
	}

	// Wait for the child process to exit. NOTE: If SIGINT or SIGBREAK is sent, we assume that the child process will handle it and exit, so that we can exit as well. If that doesn't happen and the child hangs, we hang with it so the user knows that something is hanging.
	// If we didn't hang when the child hung, we could exit and the child would still exist, possibly without the user ever knowing. Since the child still has access to the console, maybe the console wouldn't continue until the child also exits, but we do it like this just in case (and also because it is easier).
	// Plus, if we hang and the OS breaks us off, it's not really that bad. We don't have any unfreed resources (except hProcess, which isn't a big deal) at this point.
	if (WaitForSingleObject(procInfo.hProcess, INFINITE) == WAIT_FAILED) {
		reportError("failed to wait for child process to finish execution");
		CloseHandle(procInfo.hProcess);
		exit(EXIT_FAILURE);
	}

	if (!CloseHandle(procInfo.hProcess)) {
		reportError("failed to close child process handle");
		exit(EXIT_FAILURE);
	}

	return std::chrono::high_resolution_clock::now() - startTime;									// Calculate the elapsed time for the target program and return it.
}

// Number to string conversion functions.

#define ELAPSED_TIME_STRING_SIZE 128

// NOTE: type (&name)[size] is the syntax for an array reference, which is super useful in some cases. Why? Because it enforces the size of the array at compile-time. So you can't pass anything into the function except an array of the correct size.
// NOTE: Compiler has no way of checking this properly if you use pointers, which means those aren't allowed either, which is also useful in some cases.
void intToString(char (&output)[ELAPSED_TIME_STRING_SIZE], uint64_t value) {
	// TODO: You should make your own algorithm for doing this. These can get sort of complicated if you tune them to your hardware, but it's a good excersize, you should do it.
	if (_ui64toa_s(value, output, ELAPSED_TIME_STRING_SIZE, 10) != 0) {					// NOTE: An unsigned int64 can be 20 digits long at maximum, which is why we technically only have to write to the first 21 bytes of the output array (cuz NUL character).
		reportError("failed to convert elapsed time integer to string");				// NOTE: The output array is larger because it needs to work with doubleToString as well. We use ELAPSED_TIME_STRING here even though we don't need the full size just in case,
		exit(EXIT_FAILURE);																// and because any slow-down that it might cause isn't noticable and doesn't effect the functionality of the program in any way whatsoever.
	}
}

void doubleToString(char (&output)[ELAPSED_TIME_STRING_SIZE], double value) {
	if (sprintf_s(output, ELAPSED_TIME_STRING_SIZE, "%f", value) == -1) {				// NOTE: The buffer should be large enough for this never to overflow. Even if it isn't, sprintf_s will return -1 before it causes any sort of overflow, since it's a safe function.
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
		switch (flags::timeUnit) {																					// Make sure we output the elapsed time in the unit that the user wants.
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

		size_t elapsedTimeStringLength = strlen(elapsedTimeString);
		elapsedTimeString[elapsedTimeStringLength] = '\n';															// Replace NUL termination character with newline. We don't need the NUL character anyway, so it's totally fine.
		if (_write(STDERR_FILENO, elapsedTimeString, elapsedTimeStringLength + 1) == -1) {
			reportError("failed to write elapsed time to parent stderr");
			exit(EXIT_FAILURE);
		}
	}
}

int main(int argc, char* const * argv) {
	signal(SIGINT, signalHandler);																							// These two lines make it so that the program ignores both SIGINT and SIGBREAK signals as best as possible. SIGBREAK can only be ignored for 5 seconds, but that's actually good.
	signal(SIGBREAK, signalHandler);

	if (_isatty(STDERR_FILENO)) {																							// If stderr is connected to a tty, we need to enable ANSI escape codes so that we can output error messages in color. We also set a pair of flags so the rest of the program knows about the tty-status.
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
}